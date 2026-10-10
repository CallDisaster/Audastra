#include "War3MapCore/Feedback/Privacy.h"
#include "War3MapCore/Hash.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <exception>
#include <limits>

namespace War3Map::Feedback {
namespace {

template<class T> Result<T> Failure(std::string code, std::string message) {
    return Result<T>::Failure("Feedback.Privacy", std::move(code), std::move(message));
}

bool Alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool Digit(char c) { return c >= '0' && c <= '9'; }
bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = Lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool ValidUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto a = static_cast<unsigned char>(text[i]);
        if (a < 0x80) {
            if ((a < 0x20 && a != '\t' && a != '\r' && a != '\n') || a == 0x7f) return false;
            ++i;
            continue;
        }
        const auto n = a >= 0xc2 && a <= 0xdf ? 2U : a >= 0xe0 && a <= 0xef ? 3U : a >= 0xf0 && a <= 0xf4 ? 4U : 0U;
        if (n == 0 || n > text.size() - i) return false;
        std::uint32_t cp = a & (n == 2 ? 0x1fU : n == 3 ? 0x0fU : 0x07U);
        for (std::size_t j = 1; j < n; ++j) {
            const auto b = static_cast<unsigned char>(text[i + j]);
            if ((b & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3f);
        }
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000)
            || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp == 0xfeff) return false;
        i += n;
    }
    return true;
}

void AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

bool ReadUnicode(std::string_view text, std::size_t offset, std::uint32_t& cp) {
    if (offset + 6 > text.size() || text[offset] != '\\' || text[offset + 1] != 'u') return false;
    cp = 0;
    for (std::size_t j = 0; j < 4; ++j) {
        const auto digit = Hex(text[offset + 2 + j]);
        if (digit < 0) return false;
        cp = (cp << 4) | static_cast<std::uint32_t>(digit);
    }
    return true;
}

// 清理前展开百分号/JSON Unicode 编码，避免把同一敏感项换个写法绕过规则。
Result<std::string> NormalizeEncoded(std::string_view text) {
    if (!ValidUtf8(text)) return Failure<std::string>("FEEDBACK_TEXT_ENCODING", "反馈只接受有效 UTF-8 文本，不包含控制字符。");
    std::string current(text);
    for (unsigned round = 0; round < 4; ++round) {
        std::string out;
        out.reserve(current.size());
        bool changed = false;
        for (std::size_t i = 0; i < current.size();) {
            if (current[i] == '%' && i + 2 < current.size() && Hex(current[i + 1]) >= 0 && Hex(current[i + 2]) >= 0) {
                out.push_back(static_cast<char>((Hex(current[i + 1]) << 4) | Hex(current[i + 2])));
                i += 3;
                changed = true;
                continue;
            }
            std::uint32_t cp = 0;
            if (ReadUnicode(current, i, cp)) {
                i += 6;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    std::uint32_t low = 0;
                    if (!ReadUnicode(current, i, low) || low < 0xdc00 || low > 0xdfff)
                        return Failure<std::string>("FEEDBACK_TEXT_ENCODING", "反馈包含不完整的 Unicode 转义。");
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                    i += 6;
                } else if (cp >= 0xdc00 && cp <= 0xdfff)
                    return Failure<std::string>("FEEDBACK_TEXT_ENCODING", "反馈包含不完整的 Unicode 转义。");
                AppendUtf8(out, cp);
                changed = true;
                continue;
            }
            // JSON 的 escaped slash 也不能避开绝对路径/URL 检查。
            if (current[i] == '\\' && i + 1 < current.size() && current[i + 1] == '/') {
                out.push_back('/');
                i += 2;
                changed = true;
                continue;
            }
            out.push_back(current[i++]);
        }
        if (!ValidUtf8(out)) return Failure<std::string>("FEEDBACK_TEXT_ENCODING", "反馈编码展开后不是有效 UTF-8 文本。");
        current = std::move(out);
        if (!changed) return Result<std::string>::Success(std::move(current));
    }
    for (std::size_t i = 0; i < current.size(); ++i) {
        std::uint32_t cp = 0;
        if ((current[i] == '%' && i + 2 < current.size() && Hex(current[i + 1]) >= 0 && Hex(current[i + 2]) >= 0)
            || ReadUnicode(current, i, cp))
            return Failure<std::string>("FEEDBACK_ENCODING_DEPTH", "反馈包含过深的编码，未保存不确定的清理结果。");
    }
    return Result<std::string>::Success(std::move(current));
}

bool SameInsensitive(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
}
bool StartsInsensitive(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && SameInsensitive(value.substr(0, prefix.size()), prefix);
}
std::string NormalPath(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) { return c == '\\' ? '/' : Lower(c); });
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}
std::string_view PathPlaceholder(std::string_view path, const PrivacyContext& context) {
    const auto normalized = NormalPath(path);
    const PrivatePath* matched = nullptr;
    std::size_t longest = 0;
    for (const auto& root : context.paths) {
        const auto prefix = NormalPath(root.pathUtf8);
        if (normalized.size() >= prefix.size() && normalized.substr(0, prefix.size()) == prefix
            && (normalized.size() == prefix.size() || normalized[prefix.size()] == '/') && prefix.size() > longest) {
            matched = &root;
            longest = prefix.size();
        }
    }
    if (matched) {
        switch (matched->role) {
        case PathRole::Game: return "<游戏目录>";
        case PathRole::Ydwe: return "<YDWE目录>";
        case PathRole::User: return "<用户目录>";
        }
    }
    return StartsInsensitive(path, "\\\\") || path.starts_with("//") ? "<网络路径>" : "<本机路径>";
}

bool HardStop(char c) {
    return c == '\r' || c == '\n' || c == ',' || c == ';' || c == '|' || c == ')' || c == ']' || c == '}'
        || c == '"' || c == '\'' || c == '<' || c == '>';
}
std::string KeyName(std::string_view token);
std::string_view KeyReplacement(std::string_view key);
bool KeyChar(char c);

bool SensitiveFieldStart(std::string_view text, std::size_t start) {
    // 未加引号的路径允许空格，但不能把后续密钥字段名吞进路径、留下短密钥原文。
    // 这里只检查边界；编码展开只作用于字段候选，不会误改真实的 C:\u1234 目录。
    auto keyStart = start;
    const char quote = keyStart < text.size() && (text[keyStart] == '"' || text[keyStart] == '\'') ? text[keyStart++] : '\0';
    auto end = keyStart;
    while (end < text.size() && end - keyStart < 128 && (KeyChar(text[end]) || text[end] == '%' || text[end] == '\\')) ++end;
    if (end == keyStart) return false;
    const auto decoded = NormalizeEncoded(text.substr(keyStart, end - keyStart));
    if (!decoded.Succeeded() || KeyReplacement(KeyName(*decoded.value)).empty()) return false;
    auto separator = end;
    if (quote && separator < text.size() && text[separator] == quote) ++separator;
    while (separator < text.size() && Space(text[separator])) ++separator;
    return separator < text.size() && (text[separator] == '=' || text[separator] == ':'
        || (separator > end && !HardStop(text[separator])));
}
std::size_t PathEnd(std::string_view text, std::size_t start) {
    const char enclosingQuote = start > 0 && (text[start - 1] == '\'' || text[start - 1] == '"') ? text[start - 1] : '\0';
    auto end = start;
    // 逗号、分号、括号和单引号可以是合法文件名，不能提前截断后泄露余下路径。
    while (end < text.size() && text[end] != '\r' && text[end] != '\n' && text[end] != '"'
        && text[end] != '<' && text[end] != '>' && text[end] != '|' && (!enclosingQuote || text[end] != enclosingQuote)) {
        if (!enclosingQuote && Space(text[end])) {
            auto field = end;
            while (field < text.size() && Space(text[field])) ++field;
            if (SensitiveFieldStart(text, field)) break;
        }
        ++end;
    }
    while (end > start && Space(text[end - 1])) --end;
    return end;
}
std::size_t UrlEnd(std::string_view text, std::size_t start) {
    auto end = start;
    while (end < text.size() && !Space(text[end]) && text[end] != '"' && text[end] != '\'' && text[end] != '<' && text[end] != '>') ++end;
    return end;
}
bool IsAbsolute(std::string_view text, std::size_t i) {
    if (i + 2 < text.size() && Alpha(text[i]) && text[i + 1] == ':' && (text[i + 2] == '/' || text[i + 2] == '\\')) return true;
    if (i + 1 < text.size() && ((text[i] == '\\' && text[i + 1] == '\\') || (text[i] == '/' && text[i + 1] == '/'))) return true;
    return text[i] == '/' && i + 1 < text.size() && !Space(text[i + 1])
        && (i == 0 || (!Alpha(text[i - 1]) && !Digit(text[i - 1]) && text[i - 1] != '/' && text[i - 1] != ':'));
}
bool IsUrl(std::string_view text, std::size_t i) {
    if (!Alpha(text[i])) return false;
    auto j = i;
    while (j < text.size() && (Alpha(text[j]) || Digit(text[j]) || text[j] == '+' || text[j] == '-' || text[j] == '.')) ++j;
    return j + 2 < text.size() && text[j] == ':' && text[j + 1] == '/' && text[j + 2] == '/';
}
bool IsMapExtension(std::string_view value, std::size_t offset) {
    return offset + 4 <= value.size() && (SameInsensitive(value.substr(offset, 4), ".w3x") || SameInsensitive(value.substr(offset, 4), ".w3m"));
}
Result<std::string> PathReplacement(std::string_view path, const PrivacyContext& context) {
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (!IsMapExtension(path, i)) continue;
        const auto start = path.find_last_of("/\\", i);
        const auto name = path.substr(start == std::string_view::npos ? 0 : start + 1, i + 4 - (start == std::string_view::npos ? 0 : start + 1));
        auto hash = MapNameHash8(name);
        if (!hash.Succeeded()) return hash;
        return Result<std::string>::Success("<地图:" + *hash.value + ">");
    }
    return Result<std::string>::Success(std::string(PathPlaceholder(path, context)));
}

Result<CleanText> ReplacePaths(std::string_view text, const PrivacyContext& context) {
    CleanText result;
    for (std::size_t i = 0; i < text.size();) {
        if (IsUrl(text, i) || StartsInsensitive(text.substr(i), "data:")) {
            auto end = UrlEnd(text, i);
            if (StartsInsensitive(text.substr(i), "data:")) {
                end = i;
                while (end < text.size() && text[end] != '\r' && text[end] != '\n' && text[end] != '"' && text[end] != '\'') ++end;
            }
            result.text += "<地址>";
            i = end;
            ++result.replacements;
        } else if (IsAbsolute(text, i)) {
            const auto end = PathEnd(text, i);
            auto replacement = PathReplacement(text.substr(i, end - i), context);
            if (!replacement.Succeeded()) { Result<CleanText> failure; failure.diagnostics = replacement.diagnostics; return failure; }
            result.text += *replacement.value;
            i = end;
            ++result.replacements;
        } else result.text.push_back(text[i++]);
    }
    return Result<CleanText>::Success(std::move(result));
}

std::string KeyName(std::string_view token) {
    std::string out;
    for (char c : token) if (c != '_' && c != '-' && c != '.') out.push_back(Lower(c));
    return out;
}
std::string_view KeyReplacement(std::string_view key) {
    if (key == "credentialref" || key == "credentials" || key == "credential" || key == "凭据引用" || key == "凭据") return "<凭据>";
    if (key == "mapcontent" || key == "triggercontent" || key == "conversation" || key == "conversationtext"
        || key == "dialogue" || key == "地图内容" || key == "触发器内容" || key == "对话原文") return "<内容已排除>";
    constexpr std::array names{"apikey", "key", "secret", "clientsecret", "token", "accesstoken", "refreshtoken", "bearertoken",
        "password", "passwd", "pwd", "authorization", "auth", "bearer", "accesskey", "secretkey", "密钥", "密码", "令牌"};
    return std::find(names.begin(), names.end(), key) != names.end() ? "<密钥>" : "";
}
bool KeyChar(char c) { return Alpha(c) || Digit(c) || c == '_' || c == '-' || c == '.' || static_cast<unsigned char>(c) >= 0x80; }
std::size_t ValueEnd(std::string_view text, std::size_t start) {
    if (start >= text.size()) return start;
    if (text[start] == '"' || text[start] == '\'') {
        const auto quote = text[start];
        for (auto j = start + 1; j < text.size(); ++j) {
            if (text[j] == '\\' && j + 1 < text.size()) { ++j; continue; }
            if (text[j] == quote) return j + 1;
        }
        return text.size();
    }
    if (text[start] == '{' || text[start] == '[') {
        std::size_t depth = 0;
        for (auto j = start; j < text.size(); ++j) {
            if (text[j] == '"' || text[j] == '\'') { j = ValueEnd(text, j) - 1; continue; }
            if (text[j] == '{' || text[j] == '[') ++depth;
            if ((text[j] == '}' || text[j] == ']') && --depth == 0) return j + 1;
        }
        return text.size();
    }
    auto end = start;
    while (end < text.size() && text[end] != '\r' && text[end] != '\n' && text[end] != ',' && text[end] != ';'
        && text[end] != '}' && text[end] != ']' && text[end] != '|' && text[end] != ')') ++end;
    while (end > start && Space(text[end - 1])) --end;
    return end;
}

void ReplaceKnown(std::string& text, std::string_view value, std::string_view replacement, bool insensitive, std::size_t& count) {
    if (value.empty()) return;
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (i + value.size() <= text.size() && (insensitive ? SameInsensitive(std::string_view(text).substr(i, value.size()), value)
            : std::string_view(text).substr(i, value.size()) == value)) {
            out.append(replacement);
            i += value.size();
            ++count;
        } else out.push_back(text[i++]);
    }
    text = std::move(out);
}

std::uint32_t Big32(std::span<const std::byte> data, std::size_t offset) {
    return (std::to_integer<std::uint32_t>(data[offset]) << 24) | (std::to_integer<std::uint32_t>(data[offset + 1]) << 16)
        | (std::to_integer<std::uint32_t>(data[offset + 2]) << 8) | std::to_integer<std::uint32_t>(data[offset + 3]);
}

} // namespace

Status ValidatePrivacyContext(const PrivacyContext& context, PrivacyLimits limits) {
    if (limits.maxTextBytes == 0 || limits.maxTextBytes > 1024 * 1024 || limits.maxContextEntries == 0 || limits.maxContextEntries > 1024
        || limits.maxContextTextBytes == 0 || limits.maxContextTextBytes > 16384
        || context.paths.size() + context.mapNames.size() + context.secretValues.size() > limits.maxContextEntries)
        return Status::Failure("Feedback.Privacy", "FEEDBACK_PRIVACY_BUDGET", "反馈清理配置超过允许范围。");
    auto valid = [&](std::string_view value) { return !value.empty() && value.size() <= limits.maxContextTextBytes && ValidUtf8(value); };
    for (const auto& root : context.paths) {
        if (!valid(root.pathUtf8) || !IsAbsolute(root.pathUtf8, 0) || static_cast<unsigned>(root.role) > static_cast<unsigned>(PathRole::User))
            return Status::Failure("Feedback.Privacy", "FEEDBACK_PRIVACY_CONTEXT", "反馈目录映射必须使用有效的绝对路径和已知类别。");
    }
    for (const auto& value : context.mapNames) if (!valid(value))
        return Status::Failure("Feedback.Privacy", "FEEDBACK_PRIVACY_CONTEXT", "反馈地图名映射无效。");
    for (const auto& value : context.secretValues) if (!valid(value) || value.size() < 4)
        return Status::Failure("Feedback.Privacy", "FEEDBACK_PRIVACY_CONTEXT", "反馈敏感值映射无效。");
    return Status::Ok();
}

Result<std::string> MapNameHash8(std::string_view fileNameUtf8) {
    if (fileNameUtf8.empty() || fileNameUtf8.size() > 4096 || !ValidUtf8(fileNameUtf8))
        return Failure<std::string>("FEEDBACK_MAP_NAME_INVALID", "反馈地图名无法形成有界摘要。");
    auto name = std::string(fileNameUtf8);
    std::transform(name.begin(), name.end(), name.begin(), Lower);
    auto hash = Sha256Text(name);
    if (hash.Succeeded()) *hash.value = hash.value->substr(0, 8);
    return hash;
}

Result<CleanText> CleanPrivateText(std::string_view text, const PrivacyContext& context, PrivacyLimits limits) {
    try {
        const auto contextStatus = ValidatePrivacyContext(context, limits);
        if (!contextStatus.Succeeded()) { Result<CleanText> result; result.diagnostics = contextStatus.diagnostics; return result; }
        if (text.size() > limits.maxTextBytes) return Failure<CleanText>("FEEDBACK_TEXT_BUDGET", "反馈文本超过清理容量，未保存部分内容。");
        if (!ValidUtf8(text)) return Failure<CleanText>("FEEDBACK_TEXT_ENCODING", "反馈只接受有效 UTF-8 文本。");
        // 在编码展开前识别整段 URL。%20 等编码属于 URL 内部，不能展开成空格后
        // 将 token 截成两段，只清第一段。同时不吞掉 URL 后的独立凭据字段。
        std::string withoutUrls;
        std::size_t urlReplacements = 0;
        for (std::size_t i = 0; i < text.size();) {
            if (Space(text[i]) || text[i] == '"' || text[i] == '\'' || text[i] == '<' || text[i] == '>') { withoutUrls.push_back(text[i++]); continue; }
            const auto end = UrlEnd(text, i);
            const auto token = text.substr(i, end - i);
            bool url = false;
            if (token.find("://") != std::string_view::npos || token.find('%') != std::string_view::npos || token.find("\\u") != std::string_view::npos) {
                const auto expanded = NormalizeEncoded(token);
                if (!expanded.Succeeded()) { Result<CleanText> failure; failure.diagnostics = expanded.diagnostics; return failure; }
                for (std::size_t j = 0; j < expanded.value->size(); ++j) if (IsUrl(*expanded.value, j)) { url = true; break; }
            }
            if (url) { withoutUrls += "<地址>"; ++urlReplacements; }
            else withoutUrls.append(token);
            i = end;
        }
        // 真实 Windows 目录可以叫 u1234；不能先把 C:\u1234 误当 JSON Unicode
        // 转义解码并丢掉盘符后的分隔符。原始绝对路径先清理，编码路径展开后再清理。
        const auto rawPaths = ReplacePaths(withoutUrls, context);
        if (!rawPaths.Succeeded()) return rawPaths;
        auto normalized = NormalizeEncoded(rawPaths.value->text);
        if (!normalized.Succeeded()) { Result<CleanText> result; result.diagnostics = normalized.diagnostics; return result; }
        CleanText result{std::move(*normalized.value), urlReplacements + rawPaths.value->replacements};
        auto secrets = context.secretValues;
        std::stable_sort(secrets.begin(), secrets.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
        for (const auto& secret : secrets) ReplaceKnown(result.text, secret, "<密钥>", false, result.replacements);

        auto expandedPaths = ReplacePaths(result.text, context);
        if (!expandedPaths.Succeeded()) return expandedPaths;
        result.text = std::move(expandedPaths.value->text);
        result.replacements += expandedPaths.value->replacements;

        auto maps = context.mapNames;
        std::stable_sort(maps.begin(), maps.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
        for (const auto& name : maps) {
            auto hash = MapNameHash8(name);
            if (!hash.Succeeded()) { Result<CleanText> failure; failure.diagnostics = hash.diagnostics; return failure; }
            ReplaceKnown(result.text, name, "<地图:" + *hash.value + ">", true, result.replacements);
        }
        // 未提供目录/地图上下文也必须清理地图文件名；未知的带空格名字保守取完整文本片段。
        for (std::size_t i = 0; i + 4 <= result.text.size();) {
            if (!IsMapExtension(result.text, i)) { ++i; continue; }
            auto start = i;
            while (start > 0 && !HardStop(result.text[start - 1]) && result.text[start - 1] != ':' && result.text[start - 1] != '=') --start;
            while (start < i && Space(result.text[start])) ++start;
            auto hash = MapNameHash8(std::string_view(result.text).substr(start, i + 4 - start));
            if (!hash.Succeeded()) { Result<CleanText> failure; failure.diagnostics = hash.diagnostics; return failure; }
            const auto replacement = "<地图:" + *hash.value + ">";
            result.text.replace(start, i + 4 - start, replacement);
            i = start + replacement.size();
            ++result.replacements;
        }

        std::string fields;
        for (std::size_t i = 0; i < result.text.size();) {
            if (!KeyChar(result.text[i]) || (i > 0 && KeyChar(result.text[i - 1]))) { fields.push_back(result.text[i++]); continue; }
            auto end = i;
            while (end < result.text.size() && KeyChar(result.text[end])) ++end;
            const auto key = KeyName(std::string_view(result.text).substr(i, end - i));
            const auto replacement = KeyReplacement(key);
            auto separator = end;
            if (separator < result.text.size() && (result.text[separator] == '"' || result.text[separator] == '\'')) ++separator;
            while (separator < result.text.size() && Space(result.text[separator])) ++separator;
            const auto assignment = separator < result.text.size() && (result.text[separator] == '=' || result.text[separator] == ':');
            const auto separatedValue = separator > end && separator < result.text.size() && !HardStop(result.text[separator]);
            if (!replacement.empty() && (assignment || separatedValue)) {
                auto value = separator + (assignment ? 1 : 0);
                while (value < result.text.size() && Space(result.text[value])) ++value;
                fields.append(result.text, i, value - i);
                const auto last = ValueEnd(result.text, value);
                const auto quote = value < result.text.size() && (result.text[value] == '"' || result.text[value] == '\'') ? result.text[value] : '\0';
                if (quote) fields.push_back(quote);
                fields.append(replacement);
                if (quote) fields.push_back(quote);
                i = last;
                ++result.replacements;
            } else {
                fields.append(result.text, i, end - i);
                i = end;
            }
        }
        result.text = std::move(fields);

        std::string tokens;
        for (std::size_t i = 0; i < result.text.size();) {
            if (!Alpha(result.text[i]) && !Digit(result.text[i])) { tokens.push_back(result.text[i++]); continue; }
            auto end = i;
            while (end < result.text.size() && (Alpha(result.text[end]) || Digit(result.text[end]) || result.text[end] == '_'
                || result.text[end] == '-' || result.text[end] == '+' || result.text[end] == '=' || result.text[end] == '.' || result.text[end] == '/')) ++end;
            const auto token = std::string_view(result.text).substr(i, end - i);
            // 提交号和 SHA-256 是诊断身份；已知密钥值及敏感字段仍先由上面的规则清除。
            auto hashValue = token;
            for (const auto prefix : {"commit=", "sha1=", "sha256="}) {
                if (StartsInsensitive(hashValue, prefix)) { hashValue.remove_prefix(std::string_view(prefix).size()); break; }
            }
            // 只允许明确的诊断标签和句末单个句点，不拆开 JWT 或放宽其他长令牌。
            if (hashValue.ends_with('.')) hashValue.remove_suffix(1);
            const bool hash = (hashValue.size() == 40 || hashValue.size() == 64)
                && std::all_of(hashValue.begin(), hashValue.end(), [](char c) { return Hex(c) >= 0; });
            const bool credential = !hash && (token.size() >= 32 || (token.size() >= 8 && (StartsInsensitive(token, "sk-")
                || StartsInsensitive(token, "sk_") || StartsInsensitive(token, "ghp_") || StartsInsensitive(token, "github_pat_")
                || StartsInsensitive(token, "glpat-") || StartsInsensitive(token, "akia") || StartsInsensitive(token, "bearer"))));
            if (credential) { tokens += "<密钥>"; ++result.replacements; }
            else tokens.append(token);
            i = end;
        }
        result.text = std::move(tokens);
        if (result.text.size() > limits.maxTextBytes || !ValidUtf8(result.text))
            return Failure<CleanText>("FEEDBACK_CLEAN_RESULT_BUDGET", "反馈清理结果超出容量或编码无效。");
        return Result<CleanText>::Success(std::move(result));
    } catch (const std::exception&) {
        return Failure<CleanText>("FEEDBACK_PRIVACY_FAILED", "反馈清理失败，原始内容未写入队列。");
    }
}

Result<PreparedScreenshot> PrepareScreenshot(std::span<const std::byte> png, std::size_t maxBytes) {
    constexpr std::array<std::byte, 8> signature{std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
        std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a}};
    auto bad = [] { return Failure<PreparedScreenshot>("FEEDBACK_SCREENSHOT_INVALID", "截图必须是有界、完整的静态 PNG；未接受不确定附件。"); };
    if (maxBytes == 0 || maxBytes > 16 * 1024 * 1024 || png.size() > maxBytes || png.size() < signature.size()
        || !std::equal(signature.begin(), signature.end(), png.begin())) return bad();
    PreparedScreenshot result;
    result.png.assign(signature.begin(), signature.end());
    bool header = false, data = false, ended = false;
    std::size_t chunks = 0;
    for (std::size_t i = 8; i < png.size();) {
        if (png.size() - i < 12 || ++chunks > 2048) return bad();
        const auto length = Big32(png, i);
        if (length > png.size() - i - 12) return bad();
        const auto type = std::string_view(reinterpret_cast<const char*>(png.data() + i + 4), 4);
        if (!std::all_of(type.begin(), type.end(), Alpha)) return bad();
        const auto crc = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT,
            reinterpret_cast<const unsigned char*>(png.data() + i + 4), static_cast<mz_ulong>(length) + 4));
        if (crc != Big32(png, i + 8 + length)) return bad();
        bool keep = false;
        if (type == "IHDR") {
            if (header || i != 8 || length != 13) return bad();
            const auto width = Big32(png, i + 8), height = Big32(png, i + 12);
            const auto bitDepth = std::to_integer<unsigned>(png[i + 16]);
            const auto color = std::to_integer<unsigned>(png[i + 17]);
            const bool depthOk = color == 0 ? (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8 || bitDepth == 16)
                : color == 3 ? (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8)
                : (color == 2 || color == 4 || color == 6) && (bitDepth == 8 || bitDepth == 16);
            if (width == 0 || height == 0 || width > 8192 || height > 8192 || static_cast<std::uint64_t>(width) * height > 16 * 1024 * 1024
                || !depthOk || png[i + 18] != std::byte{0} || png[i + 19] != std::byte{0}
                || std::to_integer<unsigned>(png[i + 20]) > 1) return bad();
            header = keep = true;
        } else if (type == "IDAT") {
            if (!header || ended || length == 0) return bad();
            data = keep = true;
        } else if (type == "PLTE" || type == "tRNS") {
            if (!header || data || ended || length == 0 || (type == "PLTE" && (length > 768 || length % 3 != 0))) return bad();
            keep = true;
        } else if (type == "IEND") {
            if (!header || !data || ended || length != 0 || i + 12 != png.size()) return bad();
            ended = keep = true;
        } else if (type == "acTL" || type == "fcTL" || type == "fdAT" || (static_cast<unsigned char>(type[0]) & 0x20) == 0) return bad();
        if (keep) result.png.insert(result.png.end(), png.begin() + static_cast<std::ptrdiff_t>(i),
            png.begin() + static_cast<std::ptrdiff_t>(i + length + 12));
        i += length + 12;
    }
    if (!ended) return bad();
    const auto hash = Sha256(result.png);
    if (!hash.Succeeded()) { Result<PreparedScreenshot> failure; failure.diagnostics = hash.diagnostics; return failure; }
    result.sha256 = *hash.value;
    return Result<PreparedScreenshot>::Success(std::move(result));
}

} // namespace War3Map::Feedback
