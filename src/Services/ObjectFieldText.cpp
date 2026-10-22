#include "War3MapCore/Services/ObjectFieldText.h"

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <type_traits>
#include <variant>

namespace War3Map::Services {
namespace {
constexpr const char* kDomain = "Object.Text";
constexpr std::size_t kTextBytes = 1024 * 1024;
constexpr std::size_t kChanges = 128;

std::string Quoted(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    return out + "\"";
}
std::string Number(float value) {
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    std::string text(buffer.data(), error == std::errc{} ? end : buffer.data());
    // 小数字段始终带小数点，写回时不会被读成整数。
    if (text.find_first_of(".eEn") == std::string::npos) text += ".0";
    return text;
}
bool RawcodeText(std::string_view text) {
    return !text.empty() && text.size() <= 8 && text.find_first_of("'\"\\,[] ") == std::string_view::npos;
}
std::string ListItem(const std::string& item) { return RawcodeText(item) ? "'" + item + "'" : Quoted(item); }
// 与 objects_get 相同的读法：布尔字段存成整数时写 true/false，逗号列表字段写成 [列表]。
std::optional<std::string> Literal(const ObjectData::Value& value, const ObjectData::ObjectFieldDefinition* definition) {
    if (definition && (definition->valueType == "bool" || definition->valueType == "boolean"))
        if (const auto* integer = std::get_if<std::int32_t>(&value)) return std::string(*integer ? "true" : "false");
    if (definition && definition->isList) if (const auto* text = std::get_if<std::string>(&value)) {
        std::string out = "[";
        if (!text->empty() && *text != "_")
            for (std::size_t start = 0, index = 0;; ++index) {
                const auto comma = text->find(',', start);
                out += (index ? ", " : "") + ListItem(text->substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        return out + "]";
    }
    return std::visit([](const auto& item) -> std::optional<std::string> {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::int32_t>) return std::to_string(item);
        else if constexpr (std::is_same_v<T, float>) return Number(item);
        else if constexpr (std::is_same_v<T, bool>) return std::string(item ? "true" : "false");
        else if constexpr (std::is_same_v<T, std::string>) return Quoted(item);
        else if constexpr (std::is_same_v<T, ObjectData::RawcodeValue>) return "'" + item.value + "'";
        else if constexpr (std::is_same_v<T, ObjectData::RawcodeListValue>) {
            std::string out = "[";
            for (std::size_t i = 0; i < item.values.size(); ++i) out += (i ? ", " : "") + ListItem(item.values[i]);
            return out + "]";
        } else if constexpr (std::is_same_v<T, ObjectData::UnknownBytesValue>) return std::nullopt;
        else return Quoted(item.value); // 枚举、资源路径、本地化文本
    }, value);
}

class LineParser {
public:
    explicit LineParser(std::string_view text) : text_(text) {}
    std::string error;
    std::optional<ObjectData::Value> Value() {
        Skip();
        if (Peek() == '[') {
            ++at_; ObjectData::RawcodeListValue list;
            Skip();
            if (Peek() == ']') { ++at_; return End(list); }
            for (;;) {
                Skip();
                auto item = Peek() == '"' ? String() : Peek() == '\'' ? Rawcode() : Bare();
                if (!item) return std::nullopt;
                list.values.push_back(std::move(*item));
                if (list.values.size() > 256) return Fail("列表最多 256 项。");
                Skip();
                if (Peek() == ',') { ++at_; continue; }
                if (Peek() == ']') { ++at_; return End(list); }
                return Fail("列表项之间用逗号分隔，以 ] 结束。");
            }
        }
        if (Peek() == '"') { auto text = String(); return text ? End(std::move(*text)) : std::nullopt; }
        if (Peek() == '\'') { auto code = Rawcode(); return code ? End(std::move(*code)) : std::nullopt; }
        auto word = Bare();
        if (!word) return std::nullopt;
        if (*word == "true" || *word == "false") return End(*word == "true");
        return NumberValue(*word);
    }
private:
    char Peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }
    void Skip() { while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t')) ++at_; }
    std::nullopt_t Fail(std::string message) { if (error.empty()) error = std::move(message); return std::nullopt; }
    std::optional<ObjectData::Value> End(ObjectData::Value value) {
        Skip();
        if (at_ < text_.size() && text_.substr(at_).starts_with("//")) at_ = text_.size();
        if (at_ != text_.size()) return Fail("值后面有多余内容：" + std::string(text_.substr(at_)));
        return value;
    }
    std::optional<std::string> String() {
        std::string out; ++at_;
        while (at_ < text_.size() && text_[at_] != '"') {
            char c = text_[at_++];
            if (c == '\\') {
                if (at_ >= text_.size()) break;
                const char next = text_[at_++];
                c = next == 'n' ? '\n' : next == 'r' ? '\r' : next == 't' ? '\t' : next;
                if (next != 'n' && next != 'r' && next != 't' && next != '"' && next != '\\')
                    return Fail(std::string("不支持的转义 \\") + next + "。");
            }
            out += c;
        }
        if (Peek() != '"') return Fail("文本缺少结尾的双引号。");
        ++at_;
        return out;
    }
    std::optional<std::string> Rawcode() {
        const auto close = text_.find('\'', at_ + 1);
        if (close == std::string_view::npos) return Fail("对象编号缺少结尾的单引号。");
        const auto code = std::string(text_.substr(at_ + 1, close - at_ - 1));
        if (!RawcodeText(code)) return Fail("对象编号写法无效：'" + code + "'。");
        at_ = close + 1;
        return code;
    }
    std::optional<std::string> Bare() {
        const auto start = at_;
        while (at_ < text_.size() && text_[at_] != ',' && text_[at_] != ']' && text_[at_] != ' ' && text_[at_] != '\t'
            && !text_.substr(at_).starts_with("//")) ++at_;
        if (start == at_) return Fail("缺少值。");
        return std::string(text_.substr(start, at_ - start));
    }
    std::optional<ObjectData::Value> NumberValue(const std::string& word) {
        const bool decimal = word.find_first_of(".eE") != std::string::npos;
        if (!decimal) {
            std::int64_t value = 0;
            const auto [end, status] = std::from_chars(word.data(), word.data() + word.size(), value);
            if (status == std::errc{} && end == word.data() + word.size()
                && value >= (std::numeric_limits<std::int32_t>::min)() && value <= (std::numeric_limits<std::int32_t>::max)())
                return End(static_cast<std::int32_t>(value));
        } else {
            double value = 0;
            const auto [end, status] = std::from_chars(word.data(), word.data() + word.size(), value);
            if (status == std::errc{} && end == word.data() + word.size() && std::isfinite(value)
                && std::abs(value) <= static_cast<double>((std::numeric_limits<float>::max)()))
                return End(static_cast<float>(value));
        }
        return Fail("值“" + word + "”不是数字、\"文本\"、'编号'、true/false 或 [列表]。");
    }
    std::string_view text_;
    std::size_t at_ = 0;
};

// 字段身份：“(字段[:等级[:变体]])”或整个左侧就是字段键。
bool Address(std::string_view left, ObjectFieldChange& change, std::string& error) {
    while (!left.empty() && (left.back() == ' ' || left.back() == '\t')) left.remove_suffix(1);
    std::string_view key = left;
    if (left.ends_with(")")) {
        const auto open = left.rfind('(');
        if (open == std::string_view::npos) { error = "字段键的括号不配对。"; return false; }
        key = left.substr(open + 1, left.size() - open - 2);
    } else {
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.remove_prefix(1);
    }
    std::vector<std::string_view> parts;
    for (std::size_t start = 0;;) {
        const auto colon = key.find(':', start);
        parts.push_back(key.substr(start, colon == std::string_view::npos ? std::string_view::npos : colon - start));
        if (colon == std::string_view::npos) break;
        start = colon + 1;
    }
    if (parts.size() > 3 || parts[0].empty() || parts[0].size() > 128 || parts[0].find_first_of(" \t()") != std::string_view::npos) {
        error = "字段键应写成 (字段) 、(字段:等级) 或 (字段:等级:变体)。"; return false;
    }
    change.field = std::string(parts[0]);
    for (std::size_t i = 1; i < parts.size(); ++i) {
        std::uint32_t value = 0;
        const auto [end, failure] = std::from_chars(parts[i].data(), parts[i].data() + parts[i].size(), value);
        if (failure != std::errc{} || end != parts[i].data() + parts[i].size() || value > 1024 || (i == 1 && value == 0)) {
            error = "等级须是 1..1024，变体须是 0..1024 的整数。"; return false;
        }
        if (i == 1) change.level = value; else if (value) change.variation = value;
    }
    return true;
}
}

Result<std::string> RenderObjectFieldText(std::span<const ObjectFieldRow> rows, const ObjectFieldTextOptions& options,
    const ObjectFieldValueNamer& namer) {
    std::string out;
    std::size_t lines = 0;
    for (const auto& row : rows) {
        if ((!options.all && !row.overridden) || !row.value) continue;
        if (++lines > options.maxLines)
            return Result<std::string>::Failure(kDomain, "OBJECT_TEXT_TOO_LARGE", "字段超过 " + std::to_string(options.maxLines) + " 行；用 fields 筛选或分页读取。");
        const auto literal = Literal(*row.value, row.definition);
        const auto name = row.definition && !row.definition->displayName.empty() ? row.definition->displayName : row.sourceKey;
        std::string key = row.key.fieldId;
        if (row.key.level && *row.key.level > 0) key += ":" + std::to_string(*row.key.level);
        if (row.key.variation && *row.key.variation > 0) {
            if (!row.key.level || *row.key.level == 0) key += ":1";
            key += ":" + std::to_string(*row.key.variation);
        }
        out += name + " (" + key + ") = " + (literal ? *literal : std::string("?"));
        std::string note;
        if (!literal) note = "不透明的原始字节，不能用文本写回";
        else if (!row.editable) note = "只读" + (row.reason.empty() ? std::string{} : "：" + row.reason);
        else if (namer) note = namer(row, ObjectData::ValueToDisplayString(*row.value));
        if (!note.empty() && note.find('\n') == std::string::npos) out += "  // " + note;
        out += '\n';
        if (out.size() > kTextBytes)
            return Result<std::string>::Failure(kDomain, "OBJECT_TEXT_TOO_LARGE", "导出文本超过 1 MiB；用 fields 筛选。");
    }
    return Result<std::string>::Success(std::move(out));
}

Result<std::vector<ObjectFieldTextChange>> ParseObjectFieldText(std::string_view text) {
    if (text.size() > kTextBytes || text.find('\0') != std::string_view::npos)
        return Result<std::vector<ObjectFieldTextChange>>::Failure(kDomain, "OBJECT_TEXT_TOO_LARGE", "文本最多 1 MiB，不能含 NUL。");
    std::vector<ObjectFieldTextChange> changes;
    std::vector<Diagnostic> errors;
    const auto fail = [&](std::size_t line, const std::string& message) {
        if (errors.size() < 64) errors.push_back({DiagnosticSeverity::Error, kDomain, "OBJECT_TEXT_INVALID",
            "第 " + std::to_string(line) + " 行：" + message, {}});
    };
    std::size_t number = 0;
    for (std::size_t start = 0; start <= text.size();) {
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        start = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++number;
        if (line.ends_with('\r')) line.remove_suffix(1);
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos || line.substr(first).starts_with("#") || line.substr(first).starts_with("//")) continue;
        // 字段身份在第一个紧跟“)”的“=”之前（显示名里的 = 不算）；没有括号键时取第一个“=”。值里的 = 不影响。
        auto equals = line.find('=');
        if (equals == std::string_view::npos) { fail(number, "缺少“= 值”。"); continue; }
        if (line.substr(0, equals).find('(') != std::string_view::npos) for (auto at = equals; at != std::string_view::npos; at = line.find('=', at + 1)) {
            const auto left = line.substr(0, at);
            if (const auto last = left.find_last_not_of(" \t"); last != std::string_view::npos && left[last] == ')') { equals = at; break; }
        }
        ObjectFieldTextChange row; row.line = number;
        std::string error;
        if (!Address(line.substr(0, equals), row.change, error)) { fail(number, error); continue; }
        LineParser parser(line.substr(equals + 1));
        auto value = parser.Value();
        if (!value) { fail(number, parser.error); continue; }
        row.change.value = std::move(*value);
        if (changes.size() >= kChanges) { fail(number, "一次最多写 128 个字段。"); continue; }
        changes.push_back(std::move(row));
    }
    if (!errors.empty()) return {std::nullopt, std::move(errors)};
    if (changes.empty())
        return Result<std::vector<ObjectFieldTextChange>>::Failure(kDomain, "OBJECT_TEXT_EMPTY", "文本里没有任何字段行。");
    return Result<std::vector<ObjectFieldTextChange>>::Success(std::move(changes));
}

} // namespace War3Map::Services
