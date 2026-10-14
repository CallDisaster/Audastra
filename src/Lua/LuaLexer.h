#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Lua::Detail {

enum class TokenKind : std::uint8_t { Name, Keyword, Number, String, Symbol, Comment, End };

// 字节偏移 [begin, end)；行号 1 基。未闭合的字符串、长字符串和注释延伸到文本末尾，
// 供编辑中途的补全使用，不报错。
struct Token {
    TokenKind kind = TokenKind::End;
    std::uint32_t begin = 0, end = 0, line = 1;
    bool unterminated = false;
    bool rawcode = false; // YDWE |xxxx| 四字码
    [[nodiscard]] std::string_view Text(std::string_view source) const { return source.substr(begin, end - begin); }
    [[nodiscard]] bool Is(std::string_view source, std::string_view text) const {
        return (kind == TokenKind::Symbol || kind == TokenKind::Keyword) && Text(source) == text;
    }
};

// 末尾总有一个 End 记号；comments 为 false 时不输出注释记号。
[[nodiscard]] std::vector<Token> Tokenize(std::string_view source, bool comments = false);

// 字符串记号的内容（去掉引号，只处理常见转义）；长字符串去掉括号。
[[nodiscard]] std::string StringValue(std::string_view token);

[[nodiscard]] bool IsKeyword(std::string_view text);

} // namespace War3Map::Lua::Detail
