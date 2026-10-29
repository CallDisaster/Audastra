#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace War3Map::Jass {
[[nodiscard]] inline bool IsSourceControlByte(char c) noexcept { return c == '\0' || c == '\x1a'; }
// JASS 不透明文本按字节处理。反斜杠只消耗紧随的一个字节，不按代码页吞掉尾字节。
[[nodiscard]] inline std::size_t QuotedByteNext(std::string_view text, std::size_t at) noexcept {
    return at + (text[at] == '\\' && at + 1 < text.size() ? 2 : 1);
}
struct ByteOpaqueEnd { std::size_t end; bool closed; };
[[nodiscard]] inline std::optional<ByteOpaqueEnd> ScanByteOpaque(std::string_view text, std::size_t begin) noexcept {
    if (text.substr(begin).starts_with("//")) {
        auto end = begin + 2;
        while (end < text.size() && text[end] != '\r' && text[end] != '\n') ++end;
        return ByteOpaqueEnd{end, true};
    }
    if (text.substr(begin).starts_with("/*")) {
        const auto end = text.find("*/", begin + 2);
        return ByteOpaqueEnd{end == std::string_view::npos ? text.size() : end + 2, end != std::string_view::npos};
    }
    if (begin >= text.size() || (text[begin] != '"' && text[begin] != '\'')) return std::nullopt;
    const auto quote = text[begin];
    for (auto at = begin + 1; at < text.size(); at = QuotedByteNext(text, at)) {
        if (text[at] == quote) return ByteOpaqueEnd{at + 1, true};
        if (quote == '\'' && (text[at] == '\r' || text[at] == '\n')) return ByteOpaqueEnd{at, false};
    }
    return ByteOpaqueEnd{text.size(), false};
}
[[nodiscard]] inline std::optional<std::string> DecodeStringBytes(std::string_view literal) {
    if (literal.size() < 2 || literal.front() != '"' || literal.back() != '"') return std::nullopt;
    std::string value;
    for (std::size_t at = 1; at + 1 < literal.size(); at = QuotedByteNext(literal, at)) {
        if (literal[at] != '\\') { value += literal[at]; continue; }
        if (at + 2 >= literal.size()) return std::nullopt;
        switch (literal[at + 1]) {
        case 'b': value += '\b'; break;
        case 't': value += '\t'; break;
        case 'n': value += '\n'; break;
        case 'f': value += '\f'; break;
        case 'r': value += '\r'; break;
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        default: return std::nullopt;
        }
    }
    return value;
}
}
