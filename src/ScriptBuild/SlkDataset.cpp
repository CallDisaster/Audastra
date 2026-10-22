#include "War3MapCore/Script/SlkDataset.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/VerifiedFile.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace War3Map::Script {
namespace {
constexpr std::array<std::string_view, 7> kClasses{"unit", "ability", "item", "buff", "upgrade", "doodad", "destructable"};
constexpr std::array<std::string_view, 9> kPrebuilt{"ability", "buff", "unit", "item", "upgrade", "doodad", "destructable", "txt", "misc"};
constexpr std::string_view kModel = "SlkDataset/typed-prebuilt-v1";
struct Failure { std::string code, message; OriginSpan source; };
[[noreturn]] void Reject(std::string code, std::string message, const OriginSpan& source) {
    throw Failure{std::move(code), std::move(message), source};
}
std::string PathText(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string Digest(std::span<const std::byte> bytes, const OriginSpan& source) {
    const auto hash = Sha256(bytes);
    if (!hash.Succeeded()) Reject("SLK_DATASET_HASH", "无法生成来源摘要。", source);
    return *hash.value;
}
std::string_view Trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.remove_suffix(1);
    return value;
}
bool PlainSection(std::string_view text) {
    return !text.empty() && text.size() <= 256 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}
bool Identifier(std::string_view text) {
    // LNI 数字起首 key 会进入数字解析，不能假设为字符串键；纯 section 的数字起首另行保留。
    return PlainSection(text) && !(text.front() >= '0' && text.front() <= '9');
}
// 旧版 w3x2lni 把多分量字段写成带引号的 "名称:序号"（metadata 的 field 值为不带引号的 名称:序号），
// 新版写作 名称_序号，含义相同；统一成新版键，其余引号键仍拒绝。
std::optional<std::string> IndexedKey(std::string_view text, bool quoted) {
    if (quoted) {
        if (text.size() < 2 || text.front() != '"' || text.back() != '"') return std::nullopt;
        text = text.substr(1, text.size() - 2);
    }
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon + 1 == text.size() || !Identifier(text.substr(0, colon))) return std::nullopt;
    const auto index = text.substr(colon + 1);
    if (index.size() > 3 || !std::all_of(index.begin(), index.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    return std::string(text.substr(0, colon)) + "_" + std::string(index);
}
bool Rawcode(std::string_view text) {
    return text.size() == 4 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e && c != '[' && c != ']'; });
}
bool MetadataId(std::string_view text) {
    // 目标 frontend_obj 把 c4 字段 ID 截到首 NUL，backend_obj 再补零至 c4；
    // metadata 保留未补零的 1–4 字节名，不能套用对象 rawcode 的四字节约束。
    return !text.empty() && text.size() <= 4 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return c >= 0x21 && c <= 0x7e && c != '[' && c != ']';
    });
}
struct Budget {
    const SlkDatasetLimits& limits;
    std::size_t bytes = 0, records = 0, fields = 0, values = 0, strings = 0, metadata = 0;
    void Spend(std::size_t& used, std::size_t count, std::size_t maximum, const OriginSpan& source) {
        if (used > maximum || count > maximum - used) Reject("SLK_DATASET_LIMIT", "数据集读取/展开超过预算。", source);
        used += count;
    }
    void Text(std::size_t count, const OriginSpan& source) {
        if (count > 1024 * 1024) Reject("SLK_DATASET_LIMIT", "单字符串超过 1 MiB。", source);
        Spend(strings, count, limits.stringBytes, source);
    }
    void Value(const OriginSpan& source) { Spend(values, 1, limits.values, source); }
    void Key(std::string_view key, const OriginSpan& source) {
        if (key.empty() || key.size() > 256) Reject("SLK_DATASET_LIMIT", "数据键长度不在 1–256 字节范围。", source);
        Text(key.size(), source);
    }
};

class ValueParser {
public:
    ValueParser(std::string_view input, bool bare, Budget& budget, OriginSpan source, bool indexed = false)
        : input_(input), bare_(bare), indexed_(indexed), budget_(budget), source_(std::move(source)) {}
    SlkSessionValue Parse() {
        Space();
        if (at_ == input_.size()) Bad("缺少值，不能猜测为空字符串。");
        SlkSessionValue result;
        if (input_[at_] == '{') {
            ++at_; budget_.Value(source_);
            std::vector<SlkSessionScalar> values;
            Space();
            if (!Take('}')) {
                while (true) {
                    if (values.size() >= budget_.limits.arrayElements) Bad("数组超过元素预算。", "SLK_DATASET_LIMIT");
                    values.push_back(Scalar(false));
                    Space();
                    if (Take('}')) break;
                    if (!Take(',')) Bad("数组只支持逗号分隔的稠密标量。");
                    Space();
                    if (at_ < input_.size() && input_[at_] == '}') Bad("本子集不猜测数组尾逗号语义。");
                }
            }
            result = std::move(values);
        } else result = Scalar(bare_);
        Space();
        if (at_ != input_.size()) Bad("值后存在未支持的语法；不执行表达式或注释。");
        return result;
    }
private:
    [[noreturn]] void Bad(std::string message, std::string code = "SLK_DATASET_VALUE_UNSUPPORTED") const {
        auto where = source_; where.offset += at_; where.length = 1;
        Reject(std::move(code), std::move(message), where);
    }
    void Space() { while (at_ < input_.size() && (input_[at_] == ' ' || input_[at_] == '\t')) ++at_; }
    bool Take(char c) { if (at_ < input_.size() && input_[at_] == c) { ++at_; return true; } return false; }
    SlkSessionScalar Scalar(bool bare) {
        Space(); budget_.Value(source_);
        if (at_ == input_.size()) Bad("缺少数组元素或字符串。");
        if (input_[at_] == '"' || input_[at_] == '\'') {
            const char quote = input_[at_++];
            std::string result;
            while (at_ < input_.size() && input_[at_] != quote) {
                char c = input_[at_++];
                if (c == '\\') {
                    if (at_ == input_.size()) Bad("字符串转义不完整。");
                    c = input_[at_++];
                    switch (c) {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case '\\': case '\'': case '"': break;
                    default: Bad("字符串转义尚未证明。");
                    }
                } else if (static_cast<unsigned char>(c) < 0x20) Bad("字符串包含未转义控制字节。");
                if (result.size() >= 1024 * 1024) Bad("字符串超过 1 MiB。", "SLK_DATASET_LIMIT");
                budget_.Text(1, source_); result.push_back(c);
            }
            if (!Take(quote)) Bad("字符串未闭合。");
            return result;
        }
        const auto begin = at_;
        while (at_ < input_.size() && input_[at_] != ',' && (bare || input_[at_] != '}') && input_[at_] != ' ' && input_[at_] != '\t') ++at_;
        const auto token = input_.substr(begin, at_ - begin);
        if (token.empty()) Bad("不支持空元素或嵌套表。");
        if (token.find("--") != std::string_view::npos) Bad("裸值含 LNI 注释起始符；当前子集不解析注释。");
        if (token == "true") return true;
        if (token == "false") return false;
        if (token == "nil" || token == "nan" || token == "inf") Bad("nil/非有限数不能作为 typed 数据猜测。");
        const char first = token.front();
        if ((first >= '0' && first <= '9') || first == '-' || first == '.') {
            if (token.find_first_of(".eE") == std::string_view::npos) {
                std::int64_t number = 0;
                const auto parsed = std::from_chars(token.data(), token.data() + token.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) Bad("整数不是可靠的 int64 十进制字面量。");
                return number;
            }
            double number = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), number, std::chars_format::general);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || !std::isfinite(number)) Bad("实数不是有限 double 字面量。");
            return number;
        }
        if (!bare || !std::all_of(token.begin(), token.end(), [this](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-' || c == '.' || c == '$' || c == '{' || c == '}' || (indexed_ && c == ':');
        })) Bad("仅 metadata/config 接受已界定的裸字符串；不执行 Lua。");
        budget_.Text(token.size(), source_);
        return std::string(token);
    }
    std::string_view input_;
    bool bare_, indexed_;
    Budget& budget_;
    OriginSpan source_;
    std::size_t at_ = 0;
};

using Document = std::map<std::string, SlkDatasetMetadataScope>;
Document Parse(std::span<const std::byte> bytes, std::size_t sourceIndex, const SlkDatasetSourceReceipt& receipt,
    bool sections, bool bare, bool indexed, Budget& budget) {
    const auto input = bytes.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    OriginSpan source{receipt.path, 0, 0, receipt.sha256};
    if (input.find('\0') != std::string_view::npos) Reject("SLK_DATASET_SYNTAX", "数据文件含 NUL。", source);
    Document result;
    SlkDatasetMetadataScope* scope = nullptr;
    SlkDatasetFields* fields = nullptr;
    std::size_t lineStart = 0;
    while (lineStart < input.size()) {
        const auto end = input.find('\n', lineStart);
        const auto lineEnd = end == std::string_view::npos ? input.size() : end;
        const auto line = Trim(input.substr(lineStart, lineEnd - lineStart));
        source.offset = lineStart; source.length = lineEnd - lineStart;
        if (!line.empty()) {
            if (line.front() == '[') {
                if (line.back() != ']') Reject("SLK_DATASET_SYNTAX", "section 未闭合。", source);
                auto rawName = line.substr(1, line.size() - 2);
                const bool child = rawName.starts_with('.');
                if (child) rawName.remove_prefix(1);
                // ':' 是继承语法，'.'/引号等另有 LNI 含义；只接受已确认的纯名称（旧布局的引号分量字段除外）。
                const auto indexedName = indexed && child ? IndexedKey(rawName, true) : std::nullopt;
                const std::string name = indexedName ? *indexedName : std::string(rawName);
                if (!PlainSection(name))
                    Reject("SLK_DATASET_SYNTAX", "section 名称不在已确认范围。", source);
                budget.Key(name, source); budget.Spend(budget.records, 1, budget.limits.records, source);
                const SlkDatasetPosition position{sourceIndex, lineStart, lineEnd - lineStart};
                if (child) {
                    if (!sections || !scope || !Identifier(name)) Reject("SLK_DATASET_SYNTAX", "不支持此相对 section。", source);
                    if (scope->fields.contains(name) || scope->properties.contains(name))
                        Reject("SLK_DATASET_DUPLICATE", "重复 canonical metadata 字段，或与同 scope 属性重名。", source);
                    budget.Spend(budget.metadata, 1, budget.limits.metadataFields, source);
                    auto [at, inserted] = scope->fields.emplace(name, SlkDatasetRecord{{}, position});
                    (void)inserted; fields = &at->second.fields;
                } else {
                    if (result.contains(name)) Reject("SLK_DATASET_DUPLICATE", "重复数据 section。", source);
                    auto [at, inserted] = result.emplace(name, SlkDatasetMetadataScope{{}, {}, position});
                    (void)inserted; scope = &at->second; fields = &scope->properties;
                }
            } else {
                const auto equal = line.find('=');
                if (!fields || equal == std::string_view::npos) Reject("SLK_DATASET_SYNTAX", "只支持 section 内的键值数据。", source);
                const auto rawKey = Trim(line.substr(0, equal));
                const auto indexedKey = indexed ? IndexedKey(rawKey, true) : std::nullopt;
                const std::string key = indexedKey ? *indexedKey : std::string(rawKey);
                if (!Identifier(key)) Reject("SLK_DATASET_SYNTAX", "字段名不在已确认范围。", source);
                if (fields->contains(key)) Reject("SLK_DATASET_DUPLICATE", "重复字段，不能按最后值吞掉来源。", source);
                budget.Key(key, source); budget.Spend(budget.fields, 1, budget.limits.fields, source); budget.Value(source);
                const auto value = Trim(line.substr(equal + 1));
                const auto valueOffset = static_cast<std::size_t>(value.data() - input.data());
                auto valueSource = source; valueSource.offset = valueOffset; valueSource.length = value.size();
                const bool indexedField = indexed && sections && scope && fields != &scope->properties && key == "field";
                auto parsed = ValueParser(value, bare, budget, valueSource, indexedField).Parse();
                if (indexedField) {
                    // metadata 的 field 与节名对应，同样统一成 名称_序号。
                    auto* scalar = std::get_if<SlkSessionScalar>(&parsed);
                    auto* text = scalar ? std::get_if<std::string>(scalar) : nullptr;
                    if (text && text->find(':') != std::string::npos) {
                        const auto canonical = IndexedKey(*text, false);
                        if (!canonical) Reject("SLK_DATASET_SYNTAX", "metadata field 的分量写法不在已确认范围。", valueSource);
                        *text = *canonical;
                    }
                }
                fields->emplace(key, SlkDatasetValue{std::move(parsed), {sourceIndex, valueOffset, value.size()}});
            }
        }
        if (end == std::string_view::npos) break;
        lineStart = end + 1;
    }
    return result;
}

const SlkSessionScalar* Scalar(const SlkDatasetFields& fields, std::string_view name) {
    const auto found = fields.find(std::string(name));
    return found == fields.end() ? nullptr : std::get_if<SlkSessionScalar>(&found->second.value);
}
const std::string* String(const SlkDatasetFields& fields, std::string_view name) {
    const auto* value = Scalar(fields, name); return value ? std::get_if<std::string>(value) : nullptr;
}
void CheckMetadata(const Document& document, const OriginSpan& origin) {
    static constexpr std::string_view allowed[]{"appendindex", "cantempty", "concat", "data", "default", "displayname", "field", "id", "index", "key", "profile", "repeat", "splite", "type"};
    for (const auto& [scopeName, scope] : document) {
        (void)scopeName;
        for (const auto& [key, value] : scope.properties) {
            auto where = origin; where.offset = value.source.offset; where.length = value.source.length;
            if (key != "type" || !String(scope.properties, "type") || *String(scope.properties, "type") != "misc")
                Reject("SLK_DATASET_METADATA_UNSUPPORTED", "metadata scope 属性尚未覆盖。", where);
        }
        for (const auto& [name, field] : scope.fields) {
            auto fieldOrigin = origin; fieldOrigin.offset = field.source.offset; fieldOrigin.length = field.source.length;
            const auto* id = String(field.fields, "id");
            const auto* rawField = String(field.fields, "field");
            const auto* key = String(field.fields, "key");
            const auto* typeValue = Scalar(field.fields, "type");
            const auto* type = typeValue ? std::get_if<std::int64_t>(typeValue) : nullptr;
            if (!id || !MetadataId(*id) || !rawField || Text::LowerAscii(*rawField) != name || !key || !type || *type < 0 || *type > 3)
                Reject("SLK_DATASET_METADATA_UNSUPPORTED", "canonical metadata 的 id/field/key/type 不完整或不一致。", fieldOrigin);
            for (const auto& [property, value] : field.fields) {
                auto where = origin; where.offset = value.source.offset; where.length = value.source.length;
                if (std::find(std::begin(allowed), std::end(allowed), property) == std::end(allowed))
                    Reject("SLK_DATASET_METADATA_UNSUPPORTED", "未知 canonical metadata 属性：" + property, where);
                if (property == "appendindex" || property == "cantempty" || property == "concat" || property == "profile" || property == "splite") {
                    const auto* scalar = std::get_if<SlkSessionScalar>(&value.value);
                    if (!scalar || !std::holds_alternative<bool>(*scalar)) Reject("SLK_DATASET_METADATA_UNSUPPORTED", "metadata 标记必须保留 boolean 类型。", where);
                }
                if (property == "data" || property == "index" || property == "repeat") {
                    const auto* scalar = std::get_if<SlkSessionScalar>(&value.value);
                    const auto* number = scalar ? std::get_if<std::int64_t>(scalar) : nullptr;
                    if (!number || *number < 0 || *number > 256) Reject("SLK_DATASET_METADATA_UNSUPPORTED", "metadata 索引必须是有界整数。", where);
                }
            }
        }
    }
}

bool Under(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
std::filesystem::path Root(const std::filesystem::path& root, bool physical) {
    // 在 UTF-8 转换和 OriginSpan 复制前检查原生路径长度。
    if (root.native().size() > 4096) Reject("SLK_DATASET_ROOT", "来源根路径过长。", {"slk-dataset"});
    const OriginSpan origin{root};
    if (!root.is_absolute() || PathText(root).size() > 4096 || PathText(root).find('\0') != std::string::npos)
        Reject("SLK_DATASET_ROOT", "来源根必须为明确的有界绝对目录。", origin);
    auto result = root.lexically_normal();
    if (physical) {
        std::error_code error;
        result = std::filesystem::canonical(result, error);
        if (error || !std::filesystem::is_directory(result)) Reject("SLK_DATASET_ROOT", "专用来源根目录不可读取。", origin);
    }
    return result;
}
Result<std::vector<std::byte>> File(const std::filesystem::path& path, std::size_t maximum) {
    // 先有界快照求 SHA，再由已有只读租约接口二次核验；并发改写不接受混合字节。
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return Result<std::vector<std::byte>>::Failure("Script.SlkDataset", "SOURCE_OPEN", "无法打开专用文件。");
    const std::streamoff length = file.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > maximum)
        return Result<std::vector<std::byte>>::Failure("Script.SlkDataset", "SOURCE_SIZE", "文件超过读取预算。");
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!bytes.empty()) file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || file.peek() != std::char_traits<char>::eof())
        return Result<std::vector<std::byte>>::Failure("Script.SlkDataset", "SOURCE_CHANGED", "文件长度在读取时改变。");
    file.close();
    const auto hash = Sha256(bytes);
    if (!hash.Succeeded()) return Result<std::vector<std::byte>>::Failure("Script.SlkDataset", "SOURCE_HASH", "无法计算文件摘要。");
    return ReadVerifiedFile(path, *hash.value, maximum);
}
bool ValidLimits(const SlkDatasetLimits& limits) {
    const SlkDatasetLimits hard;
    return limits.fileBytes <= hard.fileBytes && limits.totalBytes <= hard.totalBytes && limits.records <= hard.records &&
        limits.fields <= hard.fields && limits.values <= hard.values && limits.stringBytes <= hard.stringBytes &&
        limits.arrayElements <= hard.arrayElements && limits.metadataFields <= hard.metadataFields;
}
} // namespace

Result<SlkTypedDataset> ReadSlkTypedDataset(const SlkDatasetRequest& request, const SlkDatasetFileReader& reader,
    const SlkDatasetLimits& limits) {
    try {
        OriginSpan origin{"slk-dataset"};
        if (!ValidLimits(limits)) Reject("SLK_DATASET_LIMIT", "调用方只能收紧硬预算。", origin);
        Budget budget{limits};
        if (request.finalW3i.size() > limits.fileBytes) Reject("SLK_DATASET_LIMIT", "W3I 超过读取预算。", origin);
        budget.Spend(budget.bytes, request.finalW3i.size(), limits.totalBytes, origin);
        SlkTypedDataset output;
        output.w3iSha256 = Digest(request.finalW3i, origin);
        output.documentRevision = request.documentRevision;
        origin = {"war3map.w3i", 0, request.finalW3i.size(), output.w3iSha256};
        const auto w3i = MapData::MapSettings::Parse(request.finalW3i);
        if (!w3i.Succeeded() || !w3i.diagnostics.empty()) Reject("SLK_DATASET_W3I_UNSUPPORTED", "需要可可靠读取数据集字段的 classic v25 W3I。", origin);
        if (w3i.value->gameDataSet == 1) output.family = SlkDatasetFamily::Custom;
        else if (w3i.value->gameDataSet == 2) output.family = SlkDatasetFamily::Melee;
        else if (w3i.value->gameDataSet == 0 || w3i.value->gameDataSet == -1)
            output.family = w3i.value->flags & 4 ? SlkDatasetFamily::Melee : SlkDatasetFamily::Custom;
        else Reject("SLK_DATASET_W3I_UNSUPPORTED", "未知 W3I 数据集选择，不能猜测版本。", origin);
        output.layout = request.layout;
        const bool legacy = request.layout == SlkDatasetLayout::ScriptPrebuiltDefault;
        if (!legacy && request.layout != SlkDatasetLayout::DataSetPrebuilt) Reject("SLK_DATASET_SELECTION_UNSUPPORTED", "未知 w3x2lni 布局。", origin);
        output.coreRoot = Root(request.coreRoot, !reader);
        output.installedYdweRoot = Root(request.installedYdweRoot, !reader);
        const auto read = [&](const std::filesystem::path& root, const std::filesystem::path& relative,
            std::string role, bool prebuilt, bool children, bool bare) {
            auto path = (root / relative).lexically_normal();
            OriginSpan source{path};
            if (!Under(root, path)) Reject("SLK_DATASET_ROOT", "专用文件路径越出已选根。", source);
            if (!reader) {
                std::error_code error;
                const auto canonical = std::filesystem::canonical(path, error);
                if (error) Reject(prebuilt ? "SLK_DATASET_FALLBACK_UNSUPPORTED" : "SLK_DATASET_SOURCE_READ",
                    prebuilt ? "prebuilt 文件不可读取；拒绝未实现的 frontend_buildslk fallback。" : "缺少专用配置或 canonical metadata。", source);
                if (!Under(root, canonical) || !std::filesystem::is_regular_file(canonical))
                    Reject("SLK_DATASET_ROOT", "文件不是根内普通文件。", source);
                path = canonical;
            }
            const auto maximum = std::min(limits.fileBytes, limits.totalBytes - budget.bytes);
            const auto bytes = reader ? reader(path, maximum) : File(path, maximum);
            if (!bytes.Succeeded() || !bytes.diagnostics.empty()) Reject(prebuilt ? "SLK_DATASET_FALLBACK_UNSUPPORTED" : "SLK_DATASET_SOURCE_READ",
                prebuilt ? "prebuilt 读取失败；没有回退构建默认数据。" : "专用配置/metadata 读取失败。", source);
            if (bytes.value->size() > maximum) Reject("SLK_DATASET_LIMIT", "来源回调返回了超过预算的文件。", source);
            budget.Spend(budget.bytes, bytes.value->size(), limits.totalBytes, source);
            const auto index = output.sources.size();
            output.sources.push_back({std::move(role), path, Digest(*bytes.value, source), bytes.value->size()});
            return Parse(*bytes.value, index, output.sources.back(), children, bare, legacy, budget);
        };
        // 旧入口只有一份默认数据，其 config.ini 不选数据集，不读取。
        if (!legacy) {
            auto config = read(output.coreRoot, "config.ini", "core-config", false, false, true);
            const auto global = config.find("global");
            const auto* data = global == config.end() ? nullptr : String(global->second.properties, "data");
            if (!data || data->empty() || data->size() > 128 || data->front() == '.' || data->find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos)
                Reject("SLK_DATASET_SELECTION_UNSUPPORTED", "config global.data 必须是明确的单级数据集目录。",
                    {output.sources.back().path, 0, output.sources.back().byteCount, output.sources.back().sha256});
            budget.Text(data->size(), origin); output.dataSetId = *data;
        }
        auto metadata = read(output.coreRoot, "defined/metadata.ini", "canonical-metadata", false, true, true);
        const OriginSpan metadataOrigin{output.sources.back().path, 0, output.sources.back().byteCount, output.sources.back().sha256};
        CheckMetadata(metadata, metadataOrigin);
        for (std::size_t i = 0; i < kClasses.size(); ++i) {
            auto node = metadata.extract(std::string(kClasses[i]));
            if (node.empty() || node.mapped().fields.empty()) Reject("SLK_DATASET_METADATA_UNSUPPORTED", "canonical metadata 缺少七类通用定义。", metadataOrigin);
            output.metadata.common[i] = std::move(node.mapped());
        }
        if (auto node = metadata.extract("misc"); !node.empty()) output.metadata.misc = std::move(node.mapped());
        output.metadata.byCode = std::move(metadata);
        const std::string family = legacy ? "default" : output.family == SlkDatasetFamily::Custom ? "Custom" : "Melee";
        for (const auto type : kPrebuilt) {
            const auto file = std::string(type) + ".ini";
            auto parsed = legacy ? read(output.coreRoot, std::filesystem::path("default") / file, "prebuilt-" + std::string(type), true, false, false)
                : read(output.installedYdweRoot, std::filesystem::path("plugin/w3x2lni/data") / output.dataSetId / "prebuilt" / family / file,
                    "prebuilt-" + std::string(type), true, false, false);
            SlkDatasetTable* target = type == "txt" ? &output.txt : type == "misc" ? &output.misc : nullptr;
            if (!target) target = &output.defaults[static_cast<std::size_t>(std::find(kClasses.begin(), kClasses.end(), type) - kClasses.begin())];
            for (auto& [id, record] : parsed) {
                const auto& receipt = output.sources[record.source.sourceIndex];
                const OriginSpan recordOrigin{receipt.path, record.source.offset, record.source.length, receipt.sha256};
                if (type != "txt" && type != "misc" && !Rawcode(id)) Reject("SLK_DATASET_OBJECT_ID", "标准物编节不是可靠四字节 ID。", recordOrigin);
                if (record.properties.contains("_id")) {
                    const auto* identity = String(record.properties, "_id");
                    if (!identity || *identity != id) Reject("SLK_DATASET_OBJECT_ID", "prebuilt _id 类型不正确或与节名不同。", recordOrigin);
                }
                if (record.properties.contains("_type")) {
                    const auto* kind = String(record.properties, "_type");
                    if (!kind || *kind != type) Reject("SLK_DATASET_OBJECT_ID", "prebuilt _type 类型不正确或与来源类别不同。", recordOrigin);
                }
                budget.Key(id, recordOrigin); // 转移 records 的字段，新增目标 map 的 ID 键先记账。
                target->emplace(id, SlkDatasetRecord{std::move(record.properties), record.source});
            }
        }
        std::string manifest;
        const auto append = [&](std::string_view value) {
            budget.Text(value.size(), origin);
            manifest += std::to_string(value.size()); manifest += ':'; manifest += value;
        };
        append(kModel); append(PathText(output.coreRoot)); append(PathText(output.installedYdweRoot));
        append(output.w3iSha256); append(std::to_string(output.documentRevision)); append(output.dataSetId); append(family);
        for (const auto& source : output.sources) {
            append(source.role); append(PathText(source.path)); append(source.sha256); append(std::to_string(source.byteCount));
        }
        const auto hash = Sha256Text(manifest);
        if (!hash.Succeeded()) Reject("SLK_DATASET_HASH", "无法生成来源清单摘要。", origin);
        output.manifestSha256 = *hash.value;
        return Result<SlkTypedDataset>::Success(std::move(output));
    } catch (const Failure& failure) {
        auto result = Result<SlkTypedDataset>::Failure("Script.SlkDataset", failure.code, failure.message);
        result.diagnostics.front().origin = failure.source;
        return result;
    } catch (const std::exception&) {
        return Result<SlkTypedDataset>::Failure("Script.SlkDataset", "SLK_DATASET_RESOURCE_FAILURE", "数据集读取未完成，未返回部分结果。");
    }
}

Result<SlkDatasetLayout> DetectSlkDatasetLayout(const std::filesystem::path& w3x2lniRoot) {
    const auto plugin = w3x2lniRoot / "plugin" / "w3x2lni";
    std::error_code error;
    const bool dataSets = std::filesystem::is_directory(plugin / "data", error);
    const bool legacyEntry = std::filesystem::is_regular_file(plugin / "script" / "w3x2lni.lua", error) &&
        std::filesystem::is_directory(plugin / "script" / "prebuilt" / "default", error);
    if (dataSets && legacyEntry)
        return Result<SlkDatasetLayout>::Failure("Script.SlkDataset", "SLK_DATASET_LAYOUT_AMBIGUOUS",
            "w3x2lni 同时有 data 数据集目录与旧入口 script/prebuilt/default，不能确定框架使用哪一套。");
    if (dataSets) return Result<SlkDatasetLayout>::Success(SlkDatasetLayout::DataSetPrebuilt);
    if (legacyEntry) return Result<SlkDatasetLayout>::Success(SlkDatasetLayout::ScriptPrebuiltDefault);
    return Result<SlkDatasetLayout>::Failure("Script.SlkDataset", "SLK_DATASET_LAYOUT_MISSING",
        "w3x2lni 既没有 data 数据集目录，也没有旧入口的 script/prebuilt/default。");
}

std::filesystem::path SlkDatasetCoreRoot(const std::filesystem::path& w3x2lniRoot, SlkDatasetLayout layout) {
    const auto script = w3x2lniRoot / "plugin" / "w3x2lni" / "script";
    return layout == SlkDatasetLayout::ScriptPrebuiltDefault ? script / "prebuilt" : script / "core";
}
} // namespace War3Map::Script
