#include "LuaLexer.h"
#include "War3MapCore/Lua/LuaApiCatalog.h"

#include <algorithm>
#include <string>

namespace War3Map::Lua::Detail {
namespace {
bool NameStart(unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
bool NameChar(unsigned char ch) { return NameStart(ch) || (ch >= '0' && ch <= '9'); }
bool Digit(unsigned char ch) { return ch >= '0' && ch <= '9'; }
bool HexDigit(unsigned char ch) { return Digit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); }
// '[' 起始的长括号层数 + 1；不是长括号返回 0。
std::size_t LongBracket(std::string_view text, std::size_t at) {
    std::size_t level = 0, cursor = at + 1;
    while (cursor < text.size() && text[cursor] == '=') { ++level; ++cursor; }
    return cursor < text.size() && text[cursor] == '[' ? level + 1 : 0;
}
} // namespace

bool IsKeyword(std::string_view text) {
    const auto& keywords = LuaKeywords();
    return std::find(keywords.begin(), keywords.end(), text) != keywords.end();
}

std::vector<Token> Tokenize(std::string_view source, bool comments) {
    std::vector<Token> tokens;
    tokens.reserve(source.size() / 4 + 1);
    std::size_t at = 0;
    std::uint32_t line = 1;
    const auto countLines = [&](std::size_t from, std::size_t to) {
        for (auto index = from; index < to; ++index) {
            if (source[index] == '\n') ++line;
            else if (source[index] == '\r' && (index + 1 >= source.size() || source[index + 1] != '\n')) ++line;
        }
    };
    const auto push = [&](TokenKind kind, std::size_t begin, std::size_t end, bool unterminated = false) {
        Token token;
        token.kind = kind; token.begin = static_cast<std::uint32_t>(begin); token.end = static_cast<std::uint32_t>(end);
        token.line = line; token.unterminated = unterminated;
        if (kind != TokenKind::Comment || comments) tokens.push_back(token);
        countLines(begin, end);
    };
    const auto longEnd = [&](std::size_t open, std::size_t level, bool& unterminated) {
        const std::string close = "]" + std::string(level - 1, '=') + "]";
        const auto found = source.find(close, open + level + 1);
        unterminated = found == std::string_view::npos;
        return unterminated ? source.size() : found + close.size();
    };
    while (at < source.size()) {
        const auto ch = static_cast<unsigned char>(source[at]);
        if (ch == '\n' || ch == '\r') { countLines(at, at + 1); ++at; continue; }
        if (ch == ' ' || ch == '\t' || ch == '\f' || ch == '\v') { ++at; continue; }
        const auto begin = at;
        if (ch == '-' && at + 1 < source.size() && source[at + 1] == '-') {
            if (at + 2 < source.size() && source[at + 2] == '[') {
                if (const auto level = LongBracket(source, at + 2)) {
                    bool unterminated = false;
                    at = longEnd(at + 2, level, unterminated);
                    push(TokenKind::Comment, begin, at, unterminated);
                    continue;
                }
            }
            while (at < source.size() && source[at] != '\n' && source[at] != '\r') ++at;
            push(TokenKind::Comment, begin, at);
            continue;
        }
        if (NameStart(ch)) {
            while (at < source.size() && NameChar(static_cast<unsigned char>(source[at]))) ++at;
            push(IsKeyword(source.substr(begin, at - begin)) ? TokenKind::Keyword : TokenKind::Name, begin, at);
            continue;
        }
        if (ch >= 0x80) {
            // 非 ASCII 字节在 Lua 5.3 不是合法名称；整段跳过，不产生记号。
            while (at < source.size() && static_cast<unsigned char>(source[at]) >= 0x80) ++at;
            continue;
        }
        if (Digit(ch) || (ch == '.' && at + 1 < source.size() && Digit(static_cast<unsigned char>(source[at + 1])))) {
            if (ch == '0' && at + 1 < source.size() && (source[at + 1] == 'x' || source[at + 1] == 'X')) {
                at += 2;
                while (at < source.size() && (HexDigit(static_cast<unsigned char>(source[at])) || source[at] == '.'
                    || ((source[at] == 'p' || source[at] == 'P')) || ((source[at] == '+' || source[at] == '-')
                        && (source[at - 1] == 'p' || source[at - 1] == 'P')))) ++at;
            } else {
                while (at < source.size() && (Digit(static_cast<unsigned char>(source[at])) || source[at] == '.'
                    || source[at] == 'e' || source[at] == 'E' || ((source[at] == '+' || source[at] == '-')
                        && (source[at - 1] == 'e' || source[at - 1] == 'E')))) ++at;
            }
            push(TokenKind::Number, begin, at);
            continue;
        }
        if (ch == '"' || ch == '\'') {
            ++at;
            bool closed = false;
            while (at < source.size() && source[at] != '\n' && source[at] != '\r') {
                if (source[at] == '\\' && at + 1 < source.size()) {
                    if (source[at + 1] == 'z') {
                        at += 2;
                        while (at < source.size() && (source[at] == ' ' || source[at] == '\t' || source[at] == '\r' || source[at] == '\n')) ++at;
                        continue;
                    }
                    at += 2; continue;
                }
                if (static_cast<unsigned char>(source[at]) == ch) { ++at; closed = true; break; }
                ++at;
            }
            push(TokenKind::String, begin, at, !closed);
            continue;
        }
        if (ch == '[') {
            if (const auto level = LongBracket(source, at)) {
                bool unterminated = false;
                at = longEnd(at, level, unterminated);
                push(TokenKind::String, begin, at, unterminated);
                continue;
            }
        }
        if (ch == '|' && at + 5 < source.size() && source[at + 5] == '|') {
            bool valid = true;
            for (std::size_t index = 1; index <= 4; ++index) {
                const auto byte = source[at + index];
                if (byte == '|' || byte == '\r' || byte == '\n') { valid = false; break; }
            }
            if (valid) {
                at += 6;
                push(TokenKind::Number, begin, at);
                tokens.back().rawcode = true;
                continue;
            }
        }
        static constexpr std::string_view three[] = {"..."};
        static constexpr std::string_view two[] = {"..", "==", "~=", "<=", ">=", "//", "::", "<<", ">>"};
        std::size_t length = 1;
        for (const auto symbol : three) if (source.substr(at, 3) == symbol) length = 3;
        if (length == 1) for (const auto symbol : two) if (source.substr(at, 2) == symbol) length = 2;
        at += length;
        push(TokenKind::Symbol, begin, at);
    }
    Token end;
    end.kind = TokenKind::End; end.begin = end.end = static_cast<std::uint32_t>(source.size()); end.line = line;
    tokens.push_back(end);
    return tokens;
}

std::string StringValue(std::string_view token) {
    if (token.empty()) return {};
    if (token.front() == '[') {
        const auto open = token.find('[', 1);
        if (open == std::string_view::npos) return {};
        auto body = token.substr(open + 1);
        const auto level = open - 1;
        const std::string close = "]" + std::string(level, '=') + "]";
        if (body.size() >= close.size() && body.substr(body.size() - close.size()) == close) body.remove_suffix(close.size());
        if (!body.empty() && body.front() == '\n') body.remove_prefix(1);
        return std::string(body);
    }
    const char quote = token.front();
    std::string result;
    for (std::size_t at = 1; at < token.size(); ++at) {
        const char ch = token[at];
        if (ch == quote) break;
        if (ch == '\\' && at + 1 < token.size()) {
            const char next = token[++at];
            switch (next) {
            case 'n': result += '\n'; break;
            case 't': result += '\t'; break;
            case '\\': result += '\\'; break;
            case '"': result += '"'; break;
            case '\'': result += '\''; break;
            default: result += next; break;
            }
            continue;
        }
        result += ch;
    }
    return result;
}

} // namespace War3Map::Lua::Detail
