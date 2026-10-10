#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_set>

namespace War3Map::MapData {
namespace {

// 布局依据独立格式说明核对；这里的读取、边界检查和文本语法处理为本工程实现。
// https://github.com/ChiefOfGxBxL/WC3MapSpecification/tree/master/Regions
// https://github.com/ChiefOfGxBxL/WC3MapSpecification/tree/master/Cameras
// https://github.com/ChiefOfGxBxL/WC3MapSpecification/tree/master/Strings
// W3S 字段语义交叉核对 War3Net 的 Audio/Sound 定义（MIT），没有引入其源码。
constexpr std::size_t kMaxBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxRecords = 100000u;
constexpr std::size_t kMaxStringBytes = 1024u * 1024u;

struct ReadFailure {
    std::string code;
    std::string message;
    std::size_t offset;
};

[[noreturn]] void Fail(std::string code, std::string message, std::size_t offset) {
    throw ReadFailure{std::move(code), std::move(message), offset};
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {
        if (bytes.size() > kMaxBytes) Fail("AUX_BUDGET_EXCEEDED", "成员超过 64 MiB 解析预算", 0);
    }

    std::size_t Position() const { return position_; }

    std::uint8_t Byte() {
        Require(1);
        return std::to_integer<std::uint8_t>(bytes_[position_++]);
    }

    std::uint32_t U32() {
        Require(4);
        std::uint32_t result = 0;
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            result |= static_cast<std::uint32_t>(Byte()) << shift;
        }
        return result;
    }

    std::int32_t I32() { return std::bit_cast<std::int32_t>(U32()); }

    float Real() {
        const auto offset = position_;
        const auto value = std::bit_cast<float>(U32());
        if (!std::isfinite(value)) Fail("AUX_NONFINITE_REAL", "字段含非有限浮点数", offset);
        return value;
    }

    std::string String() {
        const auto begin = position_;
        while (position_ < bytes_.size() && bytes_[position_] != std::byte{0}) {
            if (position_ - begin >= kMaxStringBytes) {
                Fail("AUX_STRING_BUDGET", "单个字符串超过解析预算", begin);
            }
            ++position_;
        }
        if (position_ == bytes_.size()) Fail("AUX_STRING_UNTERMINATED", "字符串缺少结束符", begin);
        std::string result(reinterpret_cast<const char*>(bytes_.data() + begin), position_ - begin);
        ++position_;
        return result;
    }

    std::uint32_t Header(std::uint32_t expectedVersion, std::size_t minimumRecordBytes) {
        std::uint32_t version = 0;
        return HeaderBetween(expectedVersion, expectedVersion, minimumRecordBytes, version);
    }

    std::uint32_t HeaderBetween(std::uint32_t minimumVersion, std::uint32_t maximumVersion,
        std::size_t minimumRecordBytes, std::uint32_t& version) {
        version = U32();
        if (version < minimumVersion || version > maximumVersion) {
            Fail("AUX_VERSION_UNSUPPORTED", "不支持的成员版本 " + std::to_string(version) +
                "，本轮支持 " + std::to_string(minimumVersion) + ".." + std::to_string(maximumVersion), 0);
        }
        const auto count = U32();
        if (count > kMaxRecords || count > (bytes_.size() - position_) / minimumRecordBytes) {
            Fail("AUX_COUNT_INVALID", "记录数量超出剩余成员边界或解析预算", 4);
        }
        return count;
    }

    void Finish() const {
        if (position_ != bytes_.size()) {
            Fail("AUX_TRAILING_DATA", "成员尾部含未识别数据，不能声明已完整解析", position_);
        }
    }

private:
    void Require(std::size_t amount) const {
        if (amount > bytes_.size() - position_) Fail("AUX_TRUNCATED", "成员字段不完整", position_);
    }
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

Diagnostic MakeDiagnostic(std::string_view member, const ReadFailure& failure,
    DiagnosticSeverity severity = DiagnosticSeverity::Error) {
    Diagnostic diagnostic{severity, "MapAuxiliary", failure.code,
        std::string(member) + "：" + failure.message, std::nullopt};
    diagnostic.origin = OriginSpan{std::filesystem::path(member), failure.offset, 0, {}};
    return diagnostic;
}

template <typename T>
Result<T> AllocationFailure(std::string_view member, bool sizeLimit) {
    return Result<T>::Failure("MapAuxiliary",
        sizeLimit ? "AUX_CONTAINER_LIMIT" : "AUX_ALLOCATION_FAILED",
        std::string(member) + (sizeLimit ? "：解析数据超过容器容量限制，未产生解析结果" :
            "：解析内存分配失败，未产生解析结果"));
}

template <typename T, typename Parse>
Result<T> ParseBinary(std::span<const std::byte> bytes, std::string_view member, Parse parse) {
    try {
        Reader reader(bytes);
        auto document = parse(reader);
        reader.Finish();
        document.originalBytes.assign(bytes.begin(), bytes.end());
        return Result<T>::Success(std::move(document));
    } catch (const ReadFailure& failure) {
        Result<T> result;
        result.diagnostics.push_back(MakeDiagnostic(member, failure));
        return result;
    } catch (const std::bad_alloc&) {
        return AllocationFailure<T>(member, false);
    } catch (const std::length_error&) {
        return AllocationFailure<T>(member, true);
    }
}

bool HorizontalSpace(char character) { return character == ' ' || character == '\t'; }
bool WhiteSpace(char character) {
    return HorizontalSpace(character) || character == '\r' || character == '\n';
}

bool ParseId(std::string_view text, std::uint32_t& id) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](char character) {
        return character >= '0' && character <= '9';
    })) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

// 不转码旧地图的本地编码字符串；UTF-8 BOM 仅作为原文的一部分识别并保留。
class WtsReader {
public:
    explicit WtsReader(std::string_view text) : text_(text) {
        if (text.starts_with("\xef\xbb\xbf")) position_ = 3;
    }

    StringTableDocument Parse(std::vector<Diagnostic>& diagnostics) {
        StringTableDocument document;
        std::unordered_set<std::uint32_t> identifiers;
        while (SkipTrivia()) {
            if (document.entries.size() >= kMaxRecords) {
                Fail("WTS_COUNT_EXCEEDED", "字符串记录数量超过解析预算", position_);
            }
            const auto begin = position_;
            if (!text_.substr(position_).starts_with("STRING")) {
                Fail("WTS_DECLARATION_INVALID", "期望 STRING 声明", position_);
            }
            position_ += 6;
            if (position_ == text_.size() || !HorizontalSpace(text_[position_])) {
                Fail("WTS_DECLARATION_INVALID", "STRING 后必须有空格及十进制 ID", begin);
            }
            while (position_ < text_.size() && HorizontalSpace(text_[position_])) ++position_;
            const auto idBegin = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            std::uint32_t id;
            if (!ParseId(text_.substr(idBegin, position_ - idBegin), id)) {
                Fail("WTS_ID_INVALID", "字符串 ID 不是有效 uint32 十进制整数", idBegin);
            }
            if (!SkipTrivia() || text_[position_] != '{') {
                Fail("WTS_OPEN_BRACE_MISSING", "字符串声明缺少左花括号", position_);
            }
            ++position_;
            // 花括号必须独占内容行，避免把格式符和正文之间的空白误并入值。
            while (position_ < text_.size() && HorizontalSpace(text_[position_])) ++position_;
            if (position_ == text_.size() || (text_[position_] != '\r' && text_[position_] != '\n')) {
                Fail("WTS_CONTENT_LINE_INVALID", "左花括号后应为换行", position_);
            }
            Newline();
            const auto contentBegin = position_;
            std::size_t contentEnd = contentBegin;
            bool closed = false;
            while (position_ < text_.size()) {
                const auto lineBegin = position_;
                while (position_ < text_.size() && text_[position_] != '\r' && text_[position_] != '\n') ++position_;
                const auto line = text_.substr(lineBegin, position_ - lineBegin);
                if (War3Map::Text::TrimHorizontal(line) == "}") {
                    // 仅去掉正文最后一行与结构右花括号之间的换行；正文空行和缩进原样保留。
                    contentEnd = lineBegin;
                    if (contentEnd > contentBegin && text_[contentEnd - 1] == '\n') --contentEnd;
                    if (contentEnd > contentBegin && text_[contentEnd - 1] == '\r') --contentEnd;
                    closed = true;
                    if (position_ < text_.size()) Newline();
                    break;
                }
                if (position_ - contentBegin > kMaxStringBytes) {
                    Fail("WTS_STRING_BUDGET", "单个字符串超过解析预算", contentBegin);
                }
                if (position_ < text_.size()) Newline();
            }
            if (!closed) Fail("WTS_CLOSE_BRACE_MISSING", "字符串声明缺少右花括号", contentBegin);
            document.entries.push_back({id, std::string(text_.substr(contentBegin, contentEnd - contentBegin)),
                begin, contentBegin, contentEnd - contentBegin});
            if (!identifiers.insert(id).second) {
                diagnostics.push_back(MakeDiagnostic("war3map.wts",
                    {"WTS_DUPLICATE_ID", "重复字符串 ID " + std::to_string(id) + "，引用解析将拒绝猜测", begin},
                    DiagnosticSeverity::Warning));
            }
        }
        return document;
    }

private:
    bool SkipTrivia() {
        while (position_ < text_.size()) {
            if (WhiteSpace(text_[position_])) {
                ++position_;
            } else if (text_.substr(position_).starts_with("//")) {
                while (position_ < text_.size() && text_[position_] != '\r' && text_[position_] != '\n') ++position_;
            } else {
                return true;
            }
        }
        return false;
    }

    void Newline() {
        if (text_[position_] == '\r') {
            ++position_;
            if (position_ < text_.size() && text_[position_] == '\n') ++position_;
        } else {
            ++position_;
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

} // namespace

Result<RegionsDocument> ParseRegions(std::span<const std::byte> bytes) {
    return ParseBinary<RegionsDocument>(bytes, "war3map.w3r", [](Reader& reader) {
        RegionsDocument document;
        document.version = 5;
        const auto count = reader.Header(document.version, 30);
        document.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            RegionRecord record;
            record.left = reader.Real();
            record.bottom = reader.Real();
            record.right = reader.Real();
            record.top = reader.Real();
            record.name = reader.String();
            record.creationNumber = reader.I32();
            for (auto& value : record.weatherId) value = static_cast<std::byte>(reader.Byte());
            record.ambientSound = reader.String();
            for (auto& value : record.colorBgra) value = reader.Byte();
            document.entries.push_back(std::move(record));
        }
        return document;
    });
}

namespace {
Result<CamerasDocument> ParseCameraLayout(std::span<const std::byte> bytes, bool extended) {
    return ParseBinary<CamerasDocument>(bytes, "war3map.w3c", [extended](Reader& reader) {
        CamerasDocument document;
        document.version = 0;
        document.layout = extended ? CameraLayout::Extended : CameraLayout::Classic;
        const auto count = reader.Header(document.version, extended ? 53 : 41);
        document.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            CameraRecord record;
            record.targetX = reader.Real();
            record.targetY = reader.Real();
            record.zOffset = reader.Real();
            record.rotation = reader.Real();
            record.angleOfAttack = reader.Real();
            record.targetDistance = reader.Real();
            record.roll = reader.Real();
            record.fieldOfView = reader.Real();
            record.farClippingPlane = reader.Real();
            record.nearClippingPlane = reader.Real();
            if (extended) {
                std::array<float, 3> local{};
                for (auto& value : local) value = reader.Real();
                record.localRotation = local;
            }
            record.name = reader.String();
            document.entries.push_back(std::move(record));
        }
        return document;
    });
}
} // namespace

Result<CamerasDocument> ParseCameras(std::span<const std::byte> bytes) {
    return ParseCameraLayout(bytes, false);
}

Result<CamerasDocument> ReadCamerasVersioned(std::span<const std::byte> bytes) {
    auto classic = ParseCameraLayout(bytes, false);
    auto extended = ParseCameraLayout(bytes, true);
    if (classic.Succeeded() && extended.Succeeded()) {
        if (!classic.value->entries.empty()) {
            return Result<CamerasDocument>::Failure("MapAuxiliary", "W3C_LAYOUT_AMBIGUOUS",
                "war3map.w3c：经典与扩展镜头布局均完整消费成员，不能选择其一；原字节留在地图成员中。");
        }
        classic.value->layout = CameraLayout::UnknownEmpty;
        classic.diagnostics.push_back({DiagnosticSeverity::Warning, "MapAuxiliary", "W3C_LAYOUT_UNKNOWN_EMPTY",
            "war3map.w3c：空表没有记录布局证据，只允许原字节透传；新增镜头须先确定目标格式。"});
        return classic;
    }
    if (extended.Succeeded()) {
        extended.diagnostics.push_back({DiagnosticSeverity::Warning, "MapAuxiliary", "W3C_EXTENDED_READ_ONLY",
            "war3map.w3c：扩展镜头布局已只读解析并保留原字节，写出仍拒绝。"});
        return extended;
    }
    if (classic.Succeeded()) return classic;
    classic.diagnostics.push_back({DiagnosticSeverity::Error, "MapAuxiliary", "W3C_LAYOUT_UNRECOGNIZED",
        "war3map.w3c：经典与扩展镜头布局均未完整解析，未猜测记录边界。"});
    classic.diagnostics.insert(classic.diagnostics.end(), extended.diagnostics.begin(), extended.diagnostics.end());
    return classic;
}

Result<SoundsDocument> ParseSounds(std::span<const std::byte> bytes) {
    auto result = ParseBinary<SoundsDocument>(bytes, "war3map.w3s", [](Reader& reader) {
        SoundsDocument document;
        const auto count = reader.HeaderBetween(1, 3, 71, document.version);
        document.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            SoundRecord record;
            record.name = reader.String();
            record.filePath = reader.String();
            record.eaxSetting = reader.String();
            record.flags = reader.U32();
            record.fadeInRate = reader.I32();
            record.fadeOutRate = reader.I32();
            record.volume = reader.I32();
            record.pitch = reader.Real();
            record.pitchVariance = reader.Real();
            record.priority = reader.I32();
            record.channel = reader.I32();
            record.minDistance = reader.Real();
            record.maxDistance = reader.Real();
            record.distanceCutoff = reader.Real();
            record.coneAngleInside = reader.Real();
            record.coneAngleOutside = reader.Real();
            record.coneOutsideVolume = reader.I32();
            for (auto& value : record.coneOrientation) value = reader.Real();
            if (document.version >= 2) {
                SoundExtendedRecord extended;
                extended.repeatVariableName = reader.String();
                extended.soundName = reader.String();
                extended.repeatSoundPath = reader.String();
                extended.dialogueTextKey = reader.I32();
                extended.unknown2 = reader.String();
                extended.dialogueSpeakerNameKey = reader.I32();
                extended.unknown4 = reader.I32();
                if (extended.unknown4 != 0) extended.unknown5 = reader.String();
                extended.unknown6 = reader.String();
                extended.unknown7 = reader.String();
                extended.facialAnimationLabel = reader.String();
                extended.facialAnimationGroupLabel = reader.String();
                extended.facialAnimationSetFilepath = reader.String();
                if (document.version >= 3) extended.unknown11 = reader.I32();
                record.extended = std::move(extended);
            }
            document.entries.push_back(std::move(record));
        }
        return document;
    });
    if (result.value && result.value->version >= 2) {
        result.diagnostics.push_back({DiagnosticSeverity::Warning, "MapAuxiliary", "W3S_EXTENDED_READ_ONLY",
            "war3map.w3s：v2/v3 扩展已只读解析并保留原字节，写出仍拒绝。"});
        for (std::size_t index = 0; index < result.value->entries.size(); ++index) {
            const auto& record = result.value->entries[index];
            const auto& extended = *record.extended;
            if (extended.repeatVariableName != record.name || extended.repeatSoundPath != record.filePath) {
                result.diagnostics.push_back({DiagnosticSeverity::Warning, "MapAuxiliary", "W3S_REPEAT_MISMATCH",
                    "war3map.w3s：声音 #" + std::to_string(index) + " 的重复变量名或路径与前缀不一致；原值保留。"});
            }
        }
    }
    return result;
}

Result<StringTableDocument> ParseStringTable(std::span<const std::byte> bytes) {
    Result<StringTableDocument> result;
    try {
        if (bytes.size() > kMaxBytes) Fail("WTS_BUDGET_EXCEEDED", "字符串表超过 64 MiB 解析预算", 0);
        const auto zero = std::find(bytes.begin(), bytes.end(), std::byte{0});
        if (zero != bytes.end()) Fail("WTS_NUL_INVALID", "字符串表含 NUL 字节", static_cast<std::size_t>(zero - bytes.begin()));
        const std::string_view text = bytes.empty() ? std::string_view{} :
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        WtsReader reader(text);
        auto document = reader.Parse(result.diagnostics);
        document.originalBytes.assign(bytes.begin(), bytes.end());
        result.value.emplace(std::move(document));
    } catch (const ReadFailure& failure) {
        result.diagnostics.push_back(MakeDiagnostic("war3map.wts", failure));
    } catch (const std::bad_alloc&) {
        return AllocationFailure<StringTableDocument>("war3map.wts", false);
    } catch (const std::length_error&) {
        return AllocationFailure<StringTableDocument>("war3map.wts", true);
    }
    return result;
}

Result<std::string> StringTableDocument::Resolve(std::string_view value) const {
    try {
        if (!value.starts_with("TRIGSTR_")) return Result<std::string>::Success(std::string(value));
        std::uint32_t id;
        if (!ParseId(value.substr(8), id)) {
            return Result<std::string>::Failure("MapAuxiliary", "WTS_REFERENCE_INVALID",
                "war3map.wts：TRIGSTR 引用必须包含完整十进制 ID");
        }
        const StringTableEntry* matched = nullptr;
        for (const auto& entry : entries) {
            if (entry.id != id) continue;
            if (matched) {
                return Result<std::string>::Failure("MapAuxiliary", "WTS_REFERENCE_AMBIGUOUS",
                    "war3map.wts：字符串 ID 存在多个定义：" + std::to_string(id));
            }
            matched = &entry;
        }
        if (!matched) {
            return Result<std::string>::Failure("MapAuxiliary", "WTS_REFERENCE_MISSING",
                "war3map.wts：字符串 ID 不存在：" + std::to_string(id));
        }
        return Result<std::string>::Success(matched->value);
    } catch (const std::bad_alloc&) {
        return AllocationFailure<std::string>("war3map.wts", false);
    } catch (const std::length_error&) {
        return AllocationFailure<std::string>("war3map.wts", true);
    }
}

} // namespace War3Map::MapData
