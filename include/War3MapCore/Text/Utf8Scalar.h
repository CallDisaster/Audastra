#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace War3Map::Text::Detail {

enum class Utf8ScalarError { None, Offset, InvalidStart, Truncated, InvalidContinuation, InvalidScalar };
struct Utf8ScalarResult {
    std::uint32_t codepoint = 0;
    std::uint8_t bytes = 0;
    std::uint8_t examinedBytes = 0;
    Utf8ScalarError error = Utf8ScalarError::None;
    [[nodiscard]] bool Valid() const { return error == Utf8ScalarError::None; }
};

// SourceEncoding 与格式预览唯一共用的 UTF-8 标量读取；不分配、不猜编码。
[[nodiscard]] inline Utf8ScalarResult ReadUtf8Scalar(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) return {0, 0, 0, Utf8ScalarError::Offset};
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return {first, 1, 1, Utf8ScalarError::None};
    std::uint8_t length = 0;
    std::uint32_t value = 0, minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) { length = 2; value = first & 0x1Fu; minimum = 0x80; }
    else if (first >= 0xE0 && first <= 0xEF) { length = 3; value = first & 0x0Fu; minimum = 0x800; }
    else if (first >= 0xF0 && first <= 0xF4) { length = 4; value = first & 0x07u; minimum = 0x10000; }
    else return {0, 0, 1, Utf8ScalarError::InvalidStart};
    if (text.size() - offset < length)
        return {0, 0, static_cast<std::uint8_t>(text.size() - offset), Utf8ScalarError::Truncated};
    for (std::uint8_t index = 1; index < length; ++index) {
        const auto next = static_cast<unsigned char>(text[offset + index]);
        if ((next & 0xC0u) != 0x80u)
            return {0, 0, static_cast<std::uint8_t>(index + 1u), Utf8ScalarError::InvalidContinuation};
        value = (value << 6u) | (next & 0x3Fu);
    }
    if (value < minimum || (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF)
        return {0, 0, length, Utf8ScalarError::InvalidScalar};
    return {value, length, length, Utf8ScalarError::None};
}

} // namespace War3Map::Text::Detail
