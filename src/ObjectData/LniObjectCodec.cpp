#include "War3MapCore/ObjectData/LniObjectCodec.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace War3Map::ObjectData {
namespace {

constexpr std::string_view kSubsystem = "ObjectData.LNI";
// 历史 `{ ... }` 块的值类型及行尾语义不明确，仍以 opaque 原文透传。
// 旧版工程的私有标记仅用于兼容读取；新写出使用 w3x2lni 的 Lua 长括号。
constexpr std::string_view kEscapedMultilineMarker = "-- FORGE_MULTILINE_ESCAPED_V1";

struct ParsedAssignment {
    std::string key;
    std::string prefix;
    std::string valueToken;
    std::string suffix;
};

[[nodiscard]] bool IsSpace(char value) noexcept {
    return value == ' ' || value == '\t';
}

[[nodiscard]] bool StartsComment(std::string_view trimmed) noexcept {
    return trimmed.starts_with(';')
        || trimmed.starts_with('#')
        || trimmed.starts_with("//")
        || trimmed.starts_with("--");
}

[[nodiscard]] bool ValidRawcode(std::string_view rawcode) noexcept {
    if (rawcode.size() != 4) {
        return false;
    }
    return std::all_of(rawcode.begin(), rawcode.end(), [](unsigned char value) {
        return value >= 0x21 && value <= 0x7e && value != '[' && value != ']';
    });
}

[[nodiscard]] bool ValidFieldId(std::string_view field) noexcept {
    if (field.empty()) {
        return false;
    }
    return std::none_of(field.begin(), field.end(), [](unsigned char value) {
        return value < 0x20 || value == '=' || value == '[' || value == ']'
            || value == ';' || value == '#' || value == '"';
    });
}

[[nodiscard]] bool ValidSourceKey(std::string_view key) noexcept {
    if (key.empty()) {
        return false;
    }
    return std::none_of(key.begin(), key.end(), [](unsigned char value) {
        return value < 0x20 || value == '=' || value == ';' || value == '#'
            || value == '"';
    });
}

[[nodiscard]] Diagnostic MakeDiagnostic(
    std::string code,
    std::string message,
    std::uint64_t offset = 0,
    std::uint64_t length = 0) {
    Diagnostic diagnostic{
        DiagnosticSeverity::Error,
        std::string(kSubsystem),
        std::move(code),
        std::move(message),
        std::nullopt,
    };
    if (length != 0) {
        diagnostic.origin = OriginSpan{{}, offset, length, {}};
    }
    return diagnostic;
}

[[nodiscard]] std::optional<ParsedAssignment> ParseAssignmentLine(
    std::string_view line) {
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view keyView = War3Map::Text::TrimHorizontal(line.substr(0, equals));
    if (!ValidSourceKey(keyView)) {
        return std::nullopt;
    }

    std::size_t valueStart = equals + 1;
    while (valueStart < line.size() && IsSpace(line[valueStart])) {
        ++valueStart;
    }

    bool quoted = false;
    bool escaped = false;
    std::size_t commentStart = std::string_view::npos;
    for (std::size_t index = valueStart; index < line.size(); ++index) {
        const char current = line[index];
        if (current == '"' && !escaped) {
            quoted = !quoted;
        }
        if (!quoted && (index == valueStart || IsSpace(line[index - 1]))) {
            if (current == ';' || current == '#'
                || (current == '-' && index + 1 < line.size() && line[index + 1] == '-')) {
                commentStart = index;
                break;
            }
        }
        escaped = current == '\\' && !escaped;
        if (current != '\\') {
            escaped = false;
        }
    }
    if (quoted) {
        return std::nullopt;
    }

    std::size_t valueRegionEnd = commentStart == std::string_view::npos
        ? line.size()
        : commentStart;
    std::size_t valueEnd = valueRegionEnd;
    while (valueEnd > valueStart && IsSpace(line[valueEnd - 1])) {
        --valueEnd;
    }

    return ParsedAssignment{
        std::string(keyView),
        std::string(line.substr(0, valueStart)),
        std::string(line.substr(valueStart, valueEnd - valueStart)),
        std::string(line.substr(valueEnd)),
    };
}

[[nodiscard]] std::optional<std::string> BlockTerminator(
    std::string_view valueToken) {
    const std::string_view trimmed = War3Map::Text::TrimHorizontal(valueToken);
    if (trimmed == "{") {
        return std::string("}");
    }
    if (trimmed.size() < 2 || trimmed.front() != '[') {
        return std::nullopt;
    }
    std::size_t cursor = 1;
    while (cursor < trimmed.size() && trimmed[cursor] == '=') {
        ++cursor;
    }
    if (cursor + 1 != trimmed.size() || trimmed[cursor] != '[') {
        return std::nullopt;
    }
    return "]" + std::string(trimmed.substr(1, cursor - 1)) + "]";
}

struct FramedLongBracket {
    std::size_t endLine = 0;
    std::string value;
    std::string closingSuffix;
};

// Lua 丢弃长括号开头紧邻的第一个换行。只解码可明确确认开头、闭合
// 和尾部的块；其余长括号仍以原文透传，不能通过字段编辑重写。
template <typename Lines>
[[nodiscard]] std::optional<FramedLongBracket> DecodeFramedLongBracket(
    const Lines& lines, std::size_t start) {
    const auto& opening = lines[start];
    const auto terminator = BlockTerminator(opening.renderedValue);
    if (!terminator || *terminator == "}" || !opening.suffix.empty()
        || opening.ending.empty()) {
        return std::nullopt;
    }

    std::string value;
    for (std::size_t index = start + 1; index < lines.size(); ++index) {
        const std::string_view content = lines[index].text;
        const std::size_t close = content.find(*terminator);
        if (close != std::string_view::npos) {
            const std::string_view trailing = content.substr(close + terminator->size());
            const std::string_view trimmed = War3Map::Text::TrimHorizontal(trailing);
            if (!trimmed.empty() && !trimmed.starts_with("--")) {
                return std::nullopt;
            }
            value.append(content.substr(0, close));
            return FramedLongBracket{index, std::move(value), std::string(trailing)};
        }
        value.append(content);
        value.append(lines[index].ending);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> ParseUnsigned(
    std::string_view text) noexcept {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] ModificationKey ParseModificationKey(std::string_view sourceKey, ObjectType type) {
    ModificationKey result{std::string(sourceKey), std::nullopt, std::nullopt, std::nullopt};
    if (!sourceKey.ends_with(']')) {
        return result;
    }
    const std::size_t open = sourceKey.rfind('[');
    if (open == std::string_view::npos || open == 0) {
        return result;
    }

    std::array<std::optional<std::uint32_t>, 3> components{};
    std::size_t componentCount = 0;
    std::string_view remaining = sourceKey.substr(open + 1, sourceKey.size() - open - 2);
    while (!remaining.empty() && componentCount < components.size()) {
        const std::size_t comma = remaining.find(',');
        const std::string_view component = War3Map::Text::TrimHorizontal(remaining.substr(0, comma));
        components[componentCount] = ParseUnsigned(component);
        if (!components[componentCount].has_value()) {
            return result;
        }
        ++componentCount;
        if (comma == std::string_view::npos) {
            remaining = {};
        } else {
            remaining.remove_prefix(comma + 1);
        }
    }
    if (!remaining.empty() || componentCount == 0) {
        return result;
    }

    result.fieldId = std::string(War3Map::Text::TrimHorizontal(sourceKey.substr(0, open)));
    if (!ValidFieldId(result.fieldId)) {
        return ModificationKey{
            std::string(sourceKey),
            std::nullopt,
            std::nullopt,
            std::nullopt,
        };
    }
    result.level = components[0];
    if (componentCount >= 2) {
        result.dataIndex = components[1];
    }
    if (componentCount >= 3) {
        result.variation = components[2];
    }
    // Doodad 的三分量首项仅是 LNI 占位；原生键只有 dataIndex/variation。
    // 只还原零占位，非零 level 保持原样以便后续显式拒绝。
    // 单、双分量键不属于该原生投影格式，也不推测其含义。
    if (type == ObjectType::Doodad && componentCount == 3
        && result.level.has_value() && *result.level == 0u) {
        result.level.reset();
    }
    return result;
}

[[nodiscard]] std::string RenderModificationKey(const ModificationKey& key) {
    std::string result = key.fieldId;
    // level/dataIndex/variation 任一存在都必须渲染：否则同一字段的多条
    // modification（如可破坏物的多变体 dvb1/dvg1）会塌缩成重复键。
    if (!key.level.has_value() && !key.dataIndex.has_value() &&
        !key.variation.has_value()) {
        return result;
    }
    result += '[' + std::to_string(key.level.value_or(0));
    if (key.dataIndex.has_value() || key.variation.has_value()) {
        result += ',' + std::to_string(key.dataIndex.value_or(0));
    }
    if (key.variation.has_value()) {
        result += ',' + std::to_string(*key.variation);
    }
    result += ']';
    return result;
}

[[nodiscard]] std::string DecodeQuoted(std::string_view token) {
    std::string result;
    result.reserve(token.size() - 2);
    for (std::size_t index = 1; index + 1 < token.size(); ++index) {
        if (token[index] == '\\' && index + 2 < token.size()
            && (token[index + 1] == '\\' || token[index + 1] == '"')) {
            result.push_back(token[index + 1]);
            ++index;
        } else {
            result.push_back(token[index]);
        }
    }
    return result;
}

[[nodiscard]] bool HasEscapedMultilineMarker(std::string_view suffix) {
    suffix = War3Map::Text::TrimHorizontal(suffix);
    return suffix == kEscapedMultilineMarker;
}

[[nodiscard]] bool LooksLikeEscapedMultilineMarker(std::string_view suffix) {
    return War3Map::Text::TrimHorizontal(suffix).starts_with("-- FORGE_MULTILINE_");
}

[[nodiscard]] std::optional<std::string> DecodeEscapedMultiline(std::string_view token) {
    if (token.size() < 2 || token.front() != '"' || token.back() != '"') return std::nullopt;
    std::string result;
    result.reserve(token.size() - 2);
    bool hasLineBreak = false;
    for (std::size_t index = 1; index + 1 < token.size(); ++index) {
        const char current = token[index];
        if (current == '\r' || current == '\n' || current == '\0') return std::nullopt;
        if (current != '\\') {
            result.push_back(current);
            continue;
        }
        if (index + 2 >= token.size()) return std::nullopt;
        switch (token[++index]) {
        case '\\': result.push_back('\\'); break;
        case '"': result.push_back('"'); break;
        case 'r': result.push_back('\r'); hasLineBreak = true; break;
        case 'n': result.push_back('\n'); hasLineBreak = true; break;
        default: return std::nullopt;
        }
    }
    return hasLineBreak ? std::optional<std::string>{std::move(result)} : std::nullopt;
}

[[nodiscard]] std::string RemoveEscapedMultilineMarker(std::string suffix) {
    if (HasEscapedMultilineMarker(suffix)) {
        const std::size_t marker = suffix.find(kEscapedMultilineMarker);
        suffix.erase(0, marker + kEscapedMultilineMarker.size());
    }
    return suffix;
}

[[nodiscard]] std::pair<Value, bool> ParseValueToken(std::string_view token) {
    if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
        return {Value{DecodeQuoted(token)}, false};
    }
    if (token == "true") {
        return {Value{true}, false};
    }
    if (token == "false") {
        return {Value{false}, false};
    }

    std::int32_t integer = 0;
    const auto [integerEnd, integerError] = std::from_chars(
        token.data(),
        token.data() + token.size(),
        integer);
    if (!token.empty() && integerError == std::errc{}
        && integerEnd == token.data() + token.size()) {
        return {Value{integer}, false};
    }

    if (token.find_first_of(".eE") != std::string_view::npos) {
        float floating = 0.0f;
        const auto [floatingEnd, floatingError] = std::from_chars(
            token.data(),
            token.data() + token.size(),
            floating,
            std::chars_format::general);
        if (floatingError == std::errc{}
            && floatingEnd == token.data() + token.size()
            && std::isfinite(floating)) {
            return {Value{floating}, false};
        }
    }
    return {Value{std::string(token)}, true};
}

[[nodiscard]] std::string EncodeQuoted(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const char character : value) {
        if (character == '\\' || character == '"') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

// w3x2lni 使用 [=[\r\n%s]=]；值内若包含 ]=] 则提高等号层级，
// 避免 Lua 提前闭合。开头 CRLF 由 Lua 解析器丢弃，不属于实际值。
[[nodiscard]] std::optional<std::string> EncodeLongBracket(std::string_view value) {
    for (std::size_t depth = 1; depth <= 64; ++depth) {
        const std::string equals(depth, '=');
        const std::string closing = "]" + equals + "]";
        if (value.find(closing) != std::string_view::npos) continue;
        std::string result = "[" + equals + "[\r\n";
        result.append(value);
        result += closing;
        return result;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> EncodeLniString(std::string_view value) {
    if (value.find('\0') != std::string_view::npos) return std::nullopt;
    if (value.find_first_of("\r\n") != std::string_view::npos) {
        return EncodeLongBracket(value);
    }
    return EncodeQuoted(value);
}

[[nodiscard]] std::optional<std::string> EncodeValue(const Value& value) {
    return std::visit(
        [](const auto& current) -> std::optional<std::string> {
            using T = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<T, std::int32_t>) {
                return std::to_string(current);
            } else if constexpr (std::is_same_v<T, float>) {
                if (!std::isfinite(current)) {
                    return std::nullopt;
                }
                std::array<char, 64> buffer{};
                const auto [end, error] = std::to_chars(
                    buffer.data(),
                    buffer.data() + buffer.size(),
                    current,
                    std::chars_format::general,
                    std::numeric_limits<float>::max_digits10);
                if (error != std::errc{}) {
                    return std::nullopt;
                }
                std::string rendered(buffer.data(), end);
                // LNI 的整数 token 会解析为 int32；整值 float 仍需保留小数标记。
                // 科学计数法已有类型标记，-0 则会保留符号并写成 -0.0。
                if (rendered.find_first_of(".eE") == std::string::npos) {
                    rendered += ".0";
                }
                return rendered;
            } else if constexpr (std::is_same_v<T, std::string>) {
                return EncodeLniString(current);
            } else if constexpr (std::is_same_v<T, bool>) {
                return current ? "true" : "false";
            } else if constexpr (
                std::is_same_v<T, RawcodeValue>
                || std::is_same_v<T, EnumValue>
                || std::is_same_v<T, AssetPathValue>
                || std::is_same_v<T, LocalizedStringValue>) {
                return EncodeLniString(current.value);
            } else if constexpr (std::is_same_v<T, RawcodeListValue>) {
                std::string result = "{";
                for (std::size_t index = 0; index < current.values.size(); ++index) {
                    if (index != 0) {
                        result += ", ";
                    }
                    result += EncodeQuoted(current.values[index]);
                }
                result += '}';
                return result;
            } else {
                return std::nullopt;
            }
        },
        value);
}

[[nodiscard]] std::string ChangeSummary(const ObjectChange& change) {
    std::string summary;
    switch (change.kind) {
    case ObjectChangeKind::Added:
        summary = "新增";
        break;
    case ObjectChangeKind::Removed:
        summary = "删除";
        break;
    case ObjectChangeKind::Modified:
        summary = "修改";
        break;
    }
    summary += "对象 " + change.object.rawcode;
    if (change.modification.has_value()) {
        summary += " 字段 " + RenderModificationKey(*change.modification);
    }
    return summary;
}

} // namespace

std::string LniObjectDocument::FormatName() const {
    return "LNI Object Data (" + std::string(LniFileName(type_)) + ')';
}

ObjectType LniObjectDocument::Type() const noexcept {
    return type_;
}

std::span<const ObjectRecord> LniObjectDocument::Objects() const noexcept {
    return objects_;
}

const ObjectRecord* LniObjectDocument::FindObject(const ObjectKey& key) const noexcept {
    if (key.type != type_) {
        return nullptr;
    }
    const auto iterator = std::find_if(
        objects_.begin(),
        objects_.end(),
        [&key](const ObjectRecord& object) {
            return object.key == key;
        });
    return iterator == objects_.end() ? nullptr : &*iterator;
}

bool LniObjectDocument::IsStructurallySafe() const noexcept {
    return structurallySafe_;
}

std::string LniObjectDocument::Render() const {
    std::string result;
    if (hasUtf8Bom_) {
        result.append("\xEF\xBB\xBF", 3);
    }
    for (const SourceLine& line : lines_) {
        if (line.kind == LineKind::Assignment) {
            result += line.prefix;
            result += line.renderedValue;
            result += line.suffix;
        } else {
            result += line.text;
        }
        result += line.ending;
    }
    return result;
}

std::string LniObjectDocument::DocumentFingerprint() const {
    constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offsetBasis;
    const std::string rendered = Render();
    for (const unsigned char byte : rendered) {
        hash ^= byte;
        hash *= prime;
    }
    if (!cloneSources_.empty()) {
        std::vector<CloneSource> sources = cloneSources_;
        std::sort(sources.begin(), sources.end(), [](const auto& left, const auto& right) {
            return left.newRawcode < right.newRawcode;
        });
        const auto add = [&](std::string_view text) {
            for (const unsigned char byte : text) { hash ^= byte; hash *= prime; }
        };
        add("\n--FORGE-NATIVE-CLONE-SOURCES-V1--\n");
        for (const auto& source : sources) {
            add(source.newRawcode); add("/"); add(source.sourceRawcode); add("\n");
        }
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

Status LniObjectDocument::RequireEditable(std::string_view operation) const {
    if (!structurallySafe_) {
        return Status::Failure(
            std::string(kSubsystem),
            "lni-unsafe-document",
            "文档包含未知或歧义结构，禁止" + std::string(operation) + "。");
    }
    return Status::Ok();
}

Status LniObjectDocument::SetModification(
    const ObjectKey& object,
    const ModificationKey& modification,
    Value value) {
    if (type_ == ObjectType::Doodad && modification.level == 0u
        && modification.dataIndex.has_value() && modification.variation.has_value()) {
        ModificationKey canonical = modification;
        canonical.level.reset();
        return SetModification(object, canonical, std::move(value));
    }
    Status editable = RequireEditable("修改");
    if (!editable.Succeeded()) {
        return editable;
    }
    if (object.type != type_ || !ValidRawcode(object.rawcode)) {
        return Status::Failure(
            std::string(kSubsystem),
            "lni-object-key-invalid",
            "对象类型或 rawcode 与当前 LNI 文档不匹配。");
    }
    if (!ValidFieldId(modification.fieldId) || modification.fieldId == "_parent") {
        return Status::Failure(
            std::string(kSubsystem),
            "lni-field-key-invalid",
            "字段 ID 无效；_parent 必须通过专门的继承关系操作修改。");
    }
    const bool doodadIndex = type_ == ObjectType::Doodad && modification.dataIndex.has_value()
        && modification.variation.has_value() && !modification.level.has_value();
    if (modification.dataIndex.has_value() && !modification.level.has_value() && !doodadIndex) {
        return Status::Failure(
            std::string(kSubsystem),
            "lni-field-index-invalid",
            "dataIndex 必须与 level 一起提供。");
    }
    const std::optional<std::string> encoded = EncodeValue(value);
    if (!encoded.has_value()) {
        return Status::Failure(
            std::string(kSubsystem),
            "lni-value-not-serializable",
            "该值无法安全编码为 LNI；UnknownBytes 只能由原格式透传。");
    }

    const auto objectIterator = std::find_if(
        objects_.begin(),
        objects_.end(),
        [&object](const ObjectRecord& candidate) {
            return candidate.key == object;
        });
    if (objectIterator == objects_.end()) {
        return Status::Failure(
            std::string(kSubsystem),
            "lni-object-not-found",
            "目标对象不存在。");
    }

    ObjectRecord& record = *objectIterator;
    const auto modificationIterator = std::find_if(
        record.modifications.begin(),
        record.modifications.end(),
        [&modification](const ObjectModification& candidate) {
            return candidate.key == modification;
        });
    if (modificationIterator != record.modifications.end()) {
        if (modificationIterator->valueWasOpaque) {
            return Status::Failure(
                std::string(kSubsystem),
                "lni-opaque-field-read-only",
                "该字段使用无法确认边界的表、长字符串或未知 LNI 值语法，只能无损透传，禁止直接修改。");
        }
        const std::size_t firstLine = modificationIterator->sourceLine;
        const auto block = DecodeFramedLongBracket(lines_, firstLine);
        std::string suffix = block ? block->closingSuffix : lines_[firstLine].suffix;
        std::string ending = block ? lines_[block->endLine].ending : lines_[firstLine].ending;
        const std::string_view suffixText = War3Map::Text::TrimHorizontal(suffix);
        if (encoded->find_first_of("\r\n") != std::string::npos && !suffixText.empty()
            && !suffixText.starts_with("--")) {
            return Status::Failure(std::string(kSubsystem), "lni-long-bracket-comment-unsupported",
                "字段的行尾备注不能安全写在长字符串后，未修改该字段。");
        }
        if (block) {
            const std::size_t removed = block->endLine - firstLine;
            lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(firstLine + 1),
                lines_.begin() + static_cast<std::ptrdiff_t>(block->endLine + 1));
            for (ObjectRecord& candidate : objects_) {
                if (candidate.sectionLine > block->endLine) candidate.sectionLine -= removed;
                for (ObjectModification& field : candidate.modifications) {
                    if (field.sourceLine > block->endLine) field.sourceLine -= removed;
                }
            }
        }
        SourceLine& line = lines_[firstLine];
        line.renderedValue = *encoded;
        line.suffix = RemoveEscapedMultilineMarker(std::move(suffix));
        line.ending = std::move(ending);
        modificationIterator->value = std::move(value);
        modificationIterator->valueWasOpaque = false;
        return Status::Ok();
    }

    std::size_t insertion = lines_.size();
    for (const ObjectRecord& candidate : objects_) {
        if (candidate.sectionLine > record.sectionLine) {
            insertion = std::min(insertion, candidate.sectionLine);
        }
    }
    if (insertion == lines_.size() && !lines_.empty() && lines_.back().ending.empty()) {
        lines_.back().ending = preferredLineEnding_;
    }
    for (ObjectRecord& candidate : objects_) {
        if (candidate.sectionLine >= insertion) {
            ++candidate.sectionLine;
        }
        for (ObjectModification& candidateModification : candidate.modifications) {
            if (candidateModification.sourceLine >= insertion) {
                ++candidateModification.sourceLine;
            }
        }
    }

    const std::string sourceKey = RenderModificationKey(modification);
    SourceLine newLine;
    newLine.kind = LineKind::Assignment;
    newLine.ending = preferredLineEnding_;
    newLine.prefix = sourceKey + " = ";
    newLine.renderedValue = *encoded;
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(insertion), std::move(newLine));
    record.modifications.push_back(ObjectModification{
        modification,
        std::move(value),
        sourceKey,
        {},
        false,
        insertion,
        std::nullopt,
    });
    return Status::Ok();
}

Status LniObjectDocument::RemoveModification(const ObjectKey& object, const ModificationKey& modification) {
    Status editable = RequireEditable("重置字段");
    if (!editable.Succeeded()) return editable;
    auto record = std::find_if(objects_.begin(), objects_.end(), [&object](const ObjectRecord& item) { return item.key == object; });
    if (record == objects_.end()) return Status::Failure(std::string(kSubsystem), "lni-object-not-found", "目标对象不存在。");
    auto field = std::find_if(record->modifications.begin(), record->modifications.end(), [&modification](const ObjectModification& item) { return item.key == modification; });
    if (field == record->modifications.end()) return Status::Failure(std::string(kSubsystem), "lni-field-not-found", "目标覆盖不存在。");
    if (field->valueWasOpaque) return Status::Failure(std::string(kSubsystem), "lni-opaque-field-read-only", "未知结构字段不能重置。");
    const std::size_t removedLine = field->sourceLine;
    const auto block = DecodeFramedLongBracket(lines_, removedLine);
    const std::size_t removedEnd = block ? block->endLine : removedLine;
    const std::size_t removedCount = removedEnd - removedLine + 1;
    record->modifications.erase(field);
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(removedLine),
        lines_.begin() + static_cast<std::ptrdiff_t>(removedEnd + 1));
    for (ObjectRecord& candidate : objects_) {
        if (candidate.sectionLine > removedEnd) candidate.sectionLine -= removedCount;
        for (ObjectModification& candidateField : candidate.modifications) {
            if (candidateField.sourceLine > removedEnd) candidateField.sourceLine -= removedCount;
        }
    }
    return Status::Ok();
}

Status LniObjectDocument::AddObject(
    const std::string& rawcode,
    const std::string& parentRawcode) {
    if (!ValidRawcode(parentRawcode) || rawcode == parentRawcode) {
        return Status::Failure(std::string(kSubsystem), "lni-parent-rawcode-invalid",
            "自定义对象必须指定不同于自身的有效基础对象。");
    }
    return AppendObject(rawcode, parentRawcode);
}

Status LniObjectDocument::AddOriginalObject(const std::string& rawcode) {
    return AppendObject(rawcode, std::nullopt);
}

Status LniObjectDocument::CloneCustomObject(
    const ObjectKey& source, const std::string& newRawcode) {
    const Status editable = RequireEditable("复制自定义对象");
    if (!editable.Succeeded()) return editable;
    if (cloneSources_.size() >= kMaximumCloneSources) {
        return Status::Failure(std::string(kSubsystem), "lni-clone-source-budget",
            "本次事务的克隆来源记录超过预算，未复制部分对象。");
    }
    if (source.type != type_ || !ValidRawcode(source.rawcode)) {
        return Status::Failure(std::string(kSubsystem), "lni-clone-source-invalid",
            "复制来源的类型或 rawcode 无效。");
    }
    if (!ValidRawcode(newRawcode)) {
        return Status::Failure(std::string(kSubsystem), "lni-clone-id-invalid",
            "新对象的 rawcode 必须是四字节可打印 ASCII。");
    }
    const ObjectRecord* original = FindObject(source);
    if (original == nullptr) {
        return Status::Failure(std::string(kSubsystem), "lni-clone-source-missing",
            "当前文档中不存在复制来源。");
    }
    if (!original->parentRawcode.has_value()) {
        return Status::Failure(std::string(kSubsystem), "lni-clone-original-override",
            "原版对象覆盖不能复制为自定义对象；请选择 Custom 对象。");
    }
    if (newRawcode == *original->parentRawcode || FindObject({type_, newRawcode}) != nullptr) {
        return Status::Failure(std::string(kSubsystem), "lni-clone-id-conflict",
            "新 rawcode 与父对象相同或已被当前文档占用。");
    }

    const std::size_t start = original->sectionLine;
    std::size_t end = lines_.size();
    for (const ObjectRecord& record : objects_) {
        if (record.sectionLine > start) end = std::min(end, record.sectionLine);
    }
    if (start >= end || end > lines_.size() || lines_[start].kind != LineKind::Section) {
        return Status::Failure(std::string(kSubsystem), "lni-clone-section-invalid",
            "复制来源的对象节边界无法确认，未修改文档。");
    }
    const std::size_t open = lines_[start].text.find('[');
    const std::size_t close = lines_[start].text.find(']', open == std::string::npos ? 0 : open + 1);
    if (open == std::string::npos || close == std::string::npos
        || lines_[start].text.substr(open + 1, close - open - 1) != source.rawcode) {
        return Status::Failure(std::string(kSubsystem), "lni-clone-header-invalid",
            "复制来源的节头与对象身份不符，未修改文档。");
    }
    for (const ObjectModification& field : original->modifications) {
        if (field.sourceLine <= start || field.sourceLine >= end) {
            return Status::Failure(std::string(kSubsystem), "lni-clone-field-boundary-invalid",
                "复制来源有字段落在节外，未修改文档。");
        }
    }

    LniObjectDocument candidate = *this;
    const std::size_t newSection = candidate.lines_.size() + (candidate.lines_.empty() ? 0 : 1);
    if (!candidate.lines_.empty()) {
        if (candidate.lines_.back().ending.empty()) candidate.lines_.back().ending = preferredLineEnding_;
        SourceLine separator;
        separator.kind = LineKind::Blank;
        separator.ending = preferredLineEnding_;
        candidate.lines_.push_back(std::move(separator));
    }
    for (std::size_t index = start; index < end; ++index) {
        SourceLine line = lines_[index];
        line.offset = 0; // 复制行不再指向来源原文的字节位置。
        if (index == start) line.text.replace(open + 1, 4, newRawcode);
        candidate.lines_.push_back(std::move(line));
    }
    ObjectRecord cloned = *original;
    cloned.key.rawcode = newRawcode;
    cloned.stableId = MakeObjectStableId(cloned.key);
    cloned.sectionLine = newSection;
    for (ObjectModification& field : cloned.modifications) {
        field.sourceLine = newSection + (field.sourceLine - start);
        field.origin.reset();
    }
    candidate.objects_.push_back(std::move(cloned));
    candidate.cloneSources_.push_back({newRawcode, source.rawcode});

    const std::string rendered = candidate.Render();
    const auto* bytes = reinterpret_cast<const std::byte*>(rendered.data());
    const CodecParseResult verified = LniObjectCodec(type_).Parse(
        std::span<const std::byte>(bytes, rendered.size()));
    const auto* parsed = dynamic_cast<const LniObjectDocument*>(verified.document.get());
    if (!verified.Succeeded() || parsed == nullptr || !parsed->IsStructurallySafe()
        || parsed->FindObject({type_, newRawcode}) == nullptr
        || parsed->Objects().size() != candidate.objects_.size()) {
        return Status::Failure(std::string(kSubsystem), "lni-clone-verification-failed",
            "复制候选无法完整重解析，已拒绝写入文档。");
    }
    *this = std::move(candidate);
    return Status::Ok();
}

std::span<const LniObjectDocument::CloneSource> LniObjectDocument::CloneSources() const noexcept {
    return cloneSources_;
}

void LniObjectDocument::ClearCloneSources() noexcept {
    cloneSources_.clear();
}

Status LniObjectDocument::PrepareNativeClones(NativeObjectDocument& nativeDocument) const {
    if (cloneSources_.empty()) return Status::Ok();
    const auto plan = PlanNativeCloneProjection(nativeDocument, *this);
    if (!plan.Succeeded() || !plan.value) {
        Status failed; failed.diagnostics = plan.diagnostics; return failed;
    }
    const auto reject = [](std::string code, std::string message) {
        return Status::Failure(std::string(kSubsystem), std::move(code), std::move(message));
    };
    NativeObjectDocument candidate = nativeDocument;
    for (const auto index : *plan.value) {
        const auto& source = cloneSources_[index];
        const auto* original = candidate.FindObject({type_, source.sourceRawcode});
        const auto* editableSource = FindObject({type_, source.sourceRawcode});
        if (!original || original->parentRawcode != editableSource->parentRawcode) {
            return reject("lni-native-clone-parent", "克隆来源的 native 父 ID 与 LNI 不同: " + source.sourceRawcode);
        }
        if (std::any_of(original->modifications.begin(), original->modifications.end(), [](const auto& field) {
            return field.valueWasOpaque || std::holds_alternative<UnknownBytesValue>(field.value);
        })) {
            return reject("lni-native-clone-opaque", "实际 native 来源含无法安全复制的原始字段。");
        }
        const auto cloned = candidate.CloneCustomObject({type_, source.sourceRawcode}, source.newRawcode);
        if (!cloned.Succeeded()) return cloned;
    }
    nativeDocument = std::move(candidate);
    return Status::Ok();
}

Status LniObjectDocument::RestoreCloneSources(std::span<const CloneSource> sources,
    const NativeObjectDocument& nativeBaseline, const ObjectFieldSchema* schema,
    const StandardObjectCatalog* standardCatalog) {
    if (sources.size() > kMaximumCloneSources) {
        return Status::Failure(std::string(kSubsystem), "lni-clone-source-budget", "工程克隆来源记录超过预算。");
    }
    LniObjectDocument candidate = *this;
    candidate.cloneSources_.assign(sources.begin(), sources.end());
    const auto plan = PlanNativeCloneProjection(nativeBaseline, candidate, schema);
    if (!plan.Succeeded()) { Status failed; failed.diagnostics = plan.diagnostics; return failed; }
    NativeObjectDocument probe = nativeBaseline;
    const auto staged = StagePendingNativeCloneSources(probe, candidate, schema, standardCatalog);
    if (!staged.Succeeded()) return staged;
    const auto valid = candidate.PrepareNativeClones(probe);
    if (!valid.Succeeded()) return valid;
    cloneSources_ = std::move(candidate.cloneSources_);
    return Status::Ok();
}

Status LniObjectDocument::AppendObject(
    const std::string& rawcode, const std::optional<std::string>& parentRawcode) {
    Status editable = RequireEditable("新增对象");
    if (!editable.Succeeded()) {
        return editable;
    }
    if (!ValidRawcode(rawcode)) {
        return Status::Failure(
            std::string(kSubsystem),
            "lni-rawcode-invalid",
            "新对象的 rawcode 必须是四字节可打印 ASCII。");
    }
    if (parentRawcode && !ValidRawcode(*parentRawcode)) {
        return Status::Failure(
            std::string(kSubsystem),
            "lni-parent-rawcode-invalid",
            "父对象的 rawcode 必须是四字节可打印 ASCII。");
    }
    const ObjectKey newKey{type_, rawcode};
    if (FindObject(newKey) != nullptr) {
        return Status::Failure(
            std::string(kSubsystem),
            "lni-object-already-exists",
            "对象 rawcode 已存在于文档中。");
    }

    // 确保前一行有换行符
    if (!lines_.empty() && lines_.back().ending.empty()) {
        lines_.back().ending = preferredLineEnding_;
    }

    // 插入空行分隔（如果已有内容）
    const std::size_t sectionLine = lines_.size();
    if (sectionLine > 0) {
        SourceLine blankLine;
        blankLine.kind = LineKind::Blank;
        blankLine.ending = preferredLineEnding_;
        lines_.push_back(std::move(blankLine));
    }

    // 插入节头 [rawcode]
    const std::size_t actualSectionLine = lines_.size();
    SourceLine sectionLineData;
    sectionLineData.kind = LineKind::Section;
    sectionLineData.ending = preferredLineEnding_;
    sectionLineData.text = "[" + rawcode + "]";
    sectionLineData.prefix = sectionLineData.text;
    lines_.push_back(std::move(sectionLineData));

    // 插入 _parent = "parentRawcode"
    if (parentRawcode) {
        SourceLine parentLineData;
        parentLineData.kind = LineKind::Assignment;
        parentLineData.ending = preferredLineEnding_;
        parentLineData.prefix = "_parent = ";
        parentLineData.renderedValue = "\"" + *parentRawcode + "\"";
        lines_.push_back(std::move(parentLineData));
    }

    // 构建 ObjectRecord
    ObjectRecord record;
    record.key = newKey;
    record.stableId = MakeObjectStableId(newKey);
    record.parentRawcode = parentRawcode;
    record.sectionLine = actualSectionLine;
    objects_.push_back(std::move(record));
    return Status::Ok();
}

Status LniObjectDocument::DeleteObject(
    const ObjectKey& object,
    const DeletionAuditToken& token) {
    Status editable = RequireEditable("删除");
    if (!editable.Succeeded()) {
        return editable;
    }
    if (token.nonce_ == 0 || token.subject_ != object
        || token.documentFingerprint_ != DocumentFingerprint()) {
        return Status::Failure(
            std::string(kSubsystem),
            "lni-delete-token-mismatch",
            "删除令牌未绑定当前对象和当前文档版本，必须重新完成引用审计。");
    }

    const auto objectIterator = std::find_if(
        objects_.begin(),
        objects_.end(),
        [&object](const ObjectRecord& candidate) {
            return candidate.key == object;
        });
    if (objectIterator == objects_.end()) {
        return Status::Failure(
            std::string(kSubsystem),
            "lni-object-not-found",
            "目标对象不存在。");
    }
    if (std::any_of(cloneSources_.begin(), cloneSources_.end(), [&](const auto& source) {
        return source.sourceRawcode == object.rawcode;
    })) {
        return Status::Failure(std::string(kSubsystem), "lni-clone-source-in-use",
            "对象仍被本次事务的克隆来源链使用，未丢弃 native 类型来源。");
    }

    const std::size_t start = objectIterator->sectionLine;
    std::size_t end = lines_.size();
    for (const ObjectRecord& candidate : objects_) {
        if (candidate.sectionLine > start) {
            end = std::min(end, candidate.sectionLine);
        }
    }
    const std::size_t removedLineCount = end - start;
    lines_.erase(
        lines_.begin() + static_cast<std::ptrdiff_t>(start),
        lines_.begin() + static_cast<std::ptrdiff_t>(end));
    objects_.erase(objectIterator);
    std::erase_if(cloneSources_, [&](const auto& source) { return source.newRawcode == object.rawcode; });

    for (ObjectRecord& candidate : objects_) {
        if (candidate.sectionLine >= end) {
            candidate.sectionLine -= removedLineCount;
        }
        for (ObjectModification& modification : candidate.modifications) {
            if (modification.sourceLine >= end) {
                modification.sourceLine -= removedLineCount;
            }
        }
    }
    return Status::Ok();
}

LniObjectCodec::LniObjectCodec(ObjectType type) noexcept
    : type_(type) {
}

ObjectType LniObjectCodec::Type() const noexcept {
    return type_;
}

std::string LniObjectCodec::Name() const {
    return "LNI " + std::string(ToString(type_)) + " object codec";
}

CodecParseResult LniObjectCodec::Parse(std::span<const std::byte> input) const {
    auto document = std::make_unique<LniObjectDocument>();
    document->type_ = type_;

    const auto sourceHash = War3Map::Sha256(input);
    if (!sourceHash.Succeeded()) {
        CodecParseResult result;
        result.diagnostics = sourceHash.diagnostics;
        return result;
    }
    document->sourceSha256_ = *sourceHash.value;

    std::string source;
    if (!input.empty()) {
        source.assign(
            reinterpret_cast<const char*>(input.data()),
            input.size());
    }
    if (source.find('\0') != std::string::npos) {
        document->structurallySafe_ = false;
        document->parseDiagnostics_.push_back(MakeDiagnostic(
            "lni-nul-byte",
            "LNI 文本包含 NUL 字节，可能是未知二进制或错误编码。"));
    }
    if (source.size() >= 2
        && ((static_cast<unsigned char>(source[0]) == 0xff
             && static_cast<unsigned char>(source[1]) == 0xfe)
            || (static_cast<unsigned char>(source[0]) == 0xfe
                && static_cast<unsigned char>(source[1]) == 0xff))) {
        document->structurallySafe_ = false;
        document->parseDiagnostics_.push_back(MakeDiagnostic(
            "lni-utf16-unsupported",
            "首版 LNI codec 不接受 UTF-16；必须先由显式编码层转换。"));
    }

    std::size_t cursor = 0;
    if (source.size() >= 3
        && static_cast<unsigned char>(source[0]) == 0xef
        && static_cast<unsigned char>(source[1]) == 0xbb
        && static_cast<unsigned char>(source[2]) == 0xbf) {
        document->hasUtf8Bom_ = true;
        cursor = 3;
    }

    std::uint64_t sourceOffset = cursor;
    while (cursor < source.size()) {
        const std::size_t lineStart = cursor;
        while (cursor < source.size() && source[cursor] != '\r' && source[cursor] != '\n') {
            ++cursor;
        }
        const std::string text = source.substr(lineStart, cursor - lineStart);
        std::string ending;
        if (cursor < source.size()) {
            if (source[cursor] == '\r' && cursor + 1 < source.size()
                && source[cursor + 1] == '\n') {
                ending = "\r\n";
                cursor += 2;
            } else {
                ending.assign(1, source[cursor]);
                ++cursor;
            }
            if (document->lines_.empty()) {
                document->preferredLineEnding_ = ending;
            }
        }
        LniObjectDocument::SourceLine line;
        line.text = text;
        line.ending = ending;
        line.offset = sourceOffset;
        document->lines_.push_back(std::move(line));
        sourceOffset = cursor;
    }
    if (cursor == 0 && source.empty()) {
        document->preferredLineEnding_ = "\n";
    }

    std::optional<std::size_t> currentObject;
    std::unordered_set<std::string> objectRawcodes;
    std::unordered_set<std::string> currentKeys;
    bool currentHasParent = false;
    std::optional<std::string> activeBlockTerminator;
    std::uint64_t activeBlockOffset = 0;

    for (std::size_t lineIndex = 0; lineIndex < document->lines_.size(); ++lineIndex) {
        LniObjectDocument::SourceLine& line = document->lines_[lineIndex];
        const std::string_view trimmed = War3Map::Text::TrimHorizontal(line.text);
        if (activeBlockTerminator.has_value()) {
            line.kind = LniObjectDocument::LineKind::OpaqueContinuation;
            const bool closed = *activeBlockTerminator == "}"
                ? trimmed == "}"
                : line.text.find(*activeBlockTerminator) != std::string::npos;
            if (closed) {
                activeBlockTerminator.reset();
                activeBlockOffset = 0;
            }
            continue;
        }
        if (trimmed.empty()) {
            line.kind = LniObjectDocument::LineKind::Blank;
            continue;
        }
        if (StartsComment(trimmed)) {
            line.kind = LniObjectDocument::LineKind::Comment;
            continue;
        }
        if (trimmed.front() == '[') {
            const std::size_t close = trimmed.find(']');
            const std::string_view trailing = close == std::string_view::npos
                ? std::string_view{}
                : War3Map::Text::TrimHorizontal(trimmed.substr(close + 1));
            const std::string_view rawcode = close == std::string_view::npos
                ? std::string_view{}
                : trimmed.substr(1, close - 1);
            if (close == std::string_view::npos || !ValidRawcode(rawcode)
                || (!trailing.empty() && !StartsComment(trailing))) {
                document->structurallySafe_ = false;
                document->parseDiagnostics_.push_back(MakeDiagnostic(
                    "lni-section-invalid",
                    "无法安全识别对象节；节名必须是四字节 rawcode。",
                    line.offset,
                    line.text.size()));
                continue;
            }
            if (!objectRawcodes.insert(std::string(rawcode)).second) {
                document->structurallySafe_ = false;
                document->parseDiagnostics_.push_back(MakeDiagnostic(
                    "lni-section-duplicate",
                    "同一文件出现重复对象节 [" + std::string(rawcode) + "]。",
                    line.offset,
                    line.text.size()));
                continue;
            }

            line.kind = LniObjectDocument::LineKind::Section;
            ObjectRecord record;
            record.key = ObjectKey{type_, std::string(rawcode)};
            record.stableId = MakeObjectStableId(record.key);
            record.sectionLine = lineIndex;
            document->objects_.push_back(std::move(record));
            currentObject = document->objects_.size() - 1;
            currentKeys.clear();
            currentHasParent = false;
            continue;
        }

        const std::optional<ParsedAssignment> assignment = ParseAssignmentLine(line.text);
        if (!assignment.has_value() || !currentObject.has_value()) {
            document->structurallySafe_ = false;
            document->parseDiagnostics_.push_back(MakeDiagnostic(
                "lni-unknown-structure",
                "无法安全框定该行，或字段出现在对象节之外。",
                line.offset,
                line.text.size()));
            continue;
        }
        if (!currentKeys.insert(assignment->key).second) {
            document->structurallySafe_ = false;
            document->parseDiagnostics_.push_back(MakeDiagnostic(
                "lni-field-duplicate",
                "对象节内出现重复字段 " + assignment->key + "。",
                line.offset,
                line.text.size()));
            continue;
        }

        line.kind = LniObjectDocument::LineKind::Assignment;
        line.prefix = assignment->prefix;
        line.renderedValue = assignment->valueToken;
        line.suffix = assignment->suffix;
        line.text.clear();

        auto [value, opaque] = ParseValueToken(assignment->valueToken);
        if (LooksLikeEscapedMultilineMarker(assignment->suffix) &&
            !HasEscapedMultilineMarker(assignment->suffix)) {
            document->structurallySafe_ = false;
            document->parseDiagnostics_.push_back(MakeDiagnostic(
                "lni-escaped-multiline-marker-invalid",
                "无法确认 FORGE 多行字段标记版本，禁止编辑或写回。",
                line.offset,
                line.prefix.size() + line.renderedValue.size()));
            opaque = true;
        } else if (HasEscapedMultilineMarker(assignment->suffix)) {
            const auto decoded = DecodeEscapedMultiline(assignment->valueToken);
            if (!decoded) {
                document->structurallySafe_ = false;
                document->parseDiagnostics_.push_back(MakeDiagnostic(
                    "lni-escaped-multiline-invalid",
                    "带 FORGE 多行标记的字段没有可验证的转义字符串，禁止编辑或写回。",
                    line.offset,
                    line.prefix.size() + line.renderedValue.size()));
                opaque = true;
            } else {
                value = *decoded;
                opaque = false;
            }
        }
        activeBlockTerminator = BlockTerminator(assignment->valueToken);
        if (activeBlockTerminator.has_value()) {
            activeBlockOffset = line.offset;
            if (*activeBlockTerminator != "}") {
                const auto framed = DecodeFramedLongBracket(document->lines_, lineIndex);
                if (framed) {
                    value = std::move(framed->value);
                    opaque = false;
                }
            }
        }
        ObjectRecord& record = document->objects_[*currentObject];
        if (assignment->key == "_parent") {
            if (currentHasParent) {
                document->structurallySafe_ = false;
                document->parseDiagnostics_.push_back(MakeDiagnostic(
                    "lni-parent-duplicate",
                    "对象节包含多个 _parent 字段。",
                    line.offset,
                    assignment->valueToken.size()));
                continue;
            }
            const std::string* parent = std::get_if<std::string>(&value);
            if (parent == nullptr || !ValidRawcode(*parent)) {
                document->structurallySafe_ = false;
                document->parseDiagnostics_.push_back(MakeDiagnostic(
                    "lni-parent-invalid",
                    "_parent 必须是四字节 rawcode 字符串。",
                    line.offset,
                    assignment->valueToken.size()));
                continue;
            }
            record.parentRawcode = *parent;
            currentHasParent = true;
            continue;
        }

        record.modifications.push_back(ObjectModification{
            ParseModificationKey(assignment->key, type_),
            value,
            assignment->key,
            assignment->valueToken,
            opaque,
            lineIndex,
            OriginSpan{{}, line.offset, line.prefix.size() + line.renderedValue.size(), {}},
        });
    }

    if (activeBlockTerminator.has_value()) {
        document->structurallySafe_ = false;
        document->parseDiagnostics_.push_back(MakeDiagnostic(
            "lni-block-unterminated",
            "LNI 表或长字符串在文件结束前没有匹配的结束标记。",
            activeBlockOffset,
            1));
    }

    CodecParseResult result;
    result.diagnostics = document->parseDiagnostics_;
    result.document = std::move(document);
    return result;
}

Status LniObjectCodec::Validate(const CodecDocument& document) const {
    const auto* lni = dynamic_cast<const LniObjectDocument*>(&document);
    if (lni == nullptr || lni->type_ != type_) {
        return Status::Failure(
            std::string(kSubsystem),
            "lni-document-type-mismatch",
            "codec 与文档的物编类型不匹配。");
    }
    if (!lni->structurallySafe_) {
        Status status;
        status.diagnostics = lni->parseDiagnostics_;
        if (status.diagnostics.empty()) {
            status = Status::Failure(
                std::string(kSubsystem),
                "lni-unsafe-document",
                "LNI 文档包含未知结构。");
        }
        return status;
    }
    return Status::Ok();
}

Result<std::vector<std::byte>> LniObjectCodec::Serialize(
    const CodecDocument& document) const {
    const Status validation = Validate(document);
    if (!validation.Succeeded()) {
        Result<std::vector<std::byte>> result;
        result.diagnostics = validation.diagnostics;
        return result;
    }
    const auto& lni = static_cast<const LniObjectDocument&>(document);
    std::string rendered;
    bool hasLegacyMarker = false;
    for (const auto& line : lni.lines_) {
        if (line.kind == LniObjectDocument::LineKind::Assignment
            && HasEscapedMultilineMarker(line.suffix)) {
            hasLegacyMarker = true;
            break;
        }
    }
    if (hasLegacyMarker) {
        // 只在重新写出时迁移历史私有格式；原文和调用者持有的文档不变。
        LniObjectDocument migrated = lni;
        for (const ObjectRecord& record : lni.objects_) {
            for (const ObjectModification& field : record.modifications) {
                if (field.sourceLine < lni.lines_.size()
                    && HasEscapedMultilineMarker(lni.lines_[field.sourceLine].suffix)) {
                    const Status status = migrated.SetModification(record.key, field.key, field.value);
                    if (!status.Succeeded()) {
                        Result<std::vector<std::byte>> result;
                        result.diagnostics = status.diagnostics;
                        return result;
                    }
                }
            }
        }
        rendered = migrated.Render();
    } else {
        rendered = lni.Render();
    }
    std::vector<std::byte> bytes(rendered.size());
    std::transform(
        rendered.begin(),
        rendered.end(),
        bytes.begin(),
        [](unsigned char value) {
            return static_cast<std::byte>(value);
        });
    return Result<std::vector<std::byte>>::Success(std::move(bytes));
}

SemanticDiff LniObjectCodec::Diff(
    const CodecDocument& before,
    const CodecDocument& after) const {
    SemanticDiff result;
    const auto* oldLni = dynamic_cast<const LniObjectDocument*>(&before);
    const auto* newLni = dynamic_cast<const LniObjectDocument*>(&after);
    if (oldLni == nullptr || newLni == nullptr
        || oldLni->type_ != type_ || newLni->type_ != type_) {
        return result;
    }

    const ObjectSemanticDiff objectDiff = DiffObjects(oldLni->objects_, newLni->objects_);
    for (const ObjectChange& change : objectDiff.changes) {
        result.changes.push_back(SemanticChange{
            change.kind == ObjectChangeKind::Added
                ? "object-data-add"
                : change.kind == ObjectChangeKind::Removed
                    ? "object-data-remove"
                    : "object-data-modify",
            std::filesystem::path(LniFileName(type_)),
            ChangeSummary(change),
            oldLni->DocumentFingerprint(),
            newLni->DocumentFingerprint(),
        });
    }
    return result;
}

CodecInspection LniObjectCodec::Inspect(const CodecDocument& document) const {
    CodecInspection inspection;
    inspection.formatName = Name();
    const auto* lni = dynamic_cast<const LniObjectDocument*>(&document);
    if (lni == nullptr || lni->type_ != type_) {
        inspection.diagnostics.push_back(MakeDiagnostic(
            "lni-document-type-mismatch",
            "codec 与文档的物编类型不匹配。"));
        return inspection;
    }

    std::size_t modificationCount = 0;
    std::size_t opaqueValueCount = 0;
    for (const ObjectRecord& object : lni->objects_) {
        modificationCount += object.modifications.size();
        opaqueValueCount += static_cast<std::size_t>(std::count_if(
            object.modifications.begin(),
            object.modifications.end(),
            [](const ObjectModification& modification) {
                return modification.valueWasOpaque;
            }));
    }
    inspection.properties.emplace_back("objectType", std::string(ToString(type_)));
    inspection.properties.emplace_back("fileName", std::string(LniFileName(type_)));
    inspection.properties.emplace_back("objectCount", std::to_string(lni->objects_.size()));
    inspection.properties.emplace_back("modificationCount", std::to_string(modificationCount));
    inspection.properties.emplace_back("opaqueValueCount", std::to_string(opaqueValueCount));
    inspection.properties.emplace_back(
        "structurallySafe",
        lni->structurallySafe_ ? "true" : "false");
    inspection.diagnostics = lni->parseDiagnostics_;
    return inspection;
}

Result<War3Map::ObjectPatch> LniObjectCodec::BuildPatch(
    const LniObjectDocument& document,
    std::filesystem::path relativePath,
    std::string summary) const {
    auto encoded = Serialize(document);
    if (!encoded.Succeeded()) {
        Result<War3Map::ObjectPatch> result;
        result.diagnostics = std::move(encoded.diagnostics);
        return result;
    }
    if (document.sourceSha256_.size() != 64) {
        return Result<War3Map::ObjectPatch>::Failure(
            std::string(kSubsystem),
            "lni-source-identity-missing",
            "LNI patch requires a document parsed from a source member with a SHA-256 identity.");
    }

    std::vector<War3Map::BinaryFilePatch> files;
    files.push_back(War3Map::BinaryFilePatch{
        std::move(relativePath),
        std::move(*encoded.value),
        std::move(summary),
        document.sourceSha256_,
    });
    return Result<War3Map::ObjectPatch>::Success(
        War3Map::ObjectPatch(std::move(files), document.Type(), false));
}

Result<std::vector<std::byte>> RenderObjectRecordsToLni(
    ObjectType type, std::span<const ObjectRecord> records) {
    std::string text;
    std::size_t skipped = 0;
    for (const ObjectRecord& record : records) {
        if (record.key.type != type || !ValidRawcode(record.key.rawcode)) {
            continue; // 跳过类型不符或非法 rawcode，避免产出无法重新解析的节头
        }
        text += '[';
        text += record.key.rawcode;
        text += "]\n";
        // 只有自定义对象（有继承源）才写 _parent；原始对象保持无 _parent。
        if (record.parentRawcode.has_value() &&
            ValidRawcode(*record.parentRawcode) &&
            *record.parentRawcode != record.key.rawcode) {
            text += "_parent = \"";
            text += *record.parentRawcode;
            text += "\"\n";
        }
        for (const ObjectModification& modification : record.modifications) {
            const std::optional<std::string> encoded = EncodeValue(modification.value);
            if (!encoded.has_value()) {
                ++skipped; // UnknownBytes 等无法文本化的值，首版跳过
                continue;
            }
            text += RenderModificationKey(modification.key);
            text += " = ";
            text += *encoded;
            text += '\n';
        }
        text += '\n';
    }

    std::vector<std::byte> bytes(text.size());
    std::transform(text.begin(), text.end(), bytes.begin(), [](unsigned char c) {
        return static_cast<std::byte>(c);
    });
    Result<std::vector<std::byte>> result =
        Result<std::vector<std::byte>>::Success(std::move(bytes));
    if (skipped != 0) {
        result.diagnostics.push_back(Diagnostic{
            DiagnosticSeverity::Warning,
            std::string(kSubsystem),
            "lni-render-skipped-opaque",
            std::to_string(skipped) +
                " 个字段因为不可文本化（如未知字节）在导入时被跳过。"});
    }
    return result;
}

} // namespace War3Map::ObjectData
