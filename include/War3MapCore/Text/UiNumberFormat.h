#pragma once

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

namespace War3Map::Text {
namespace UiNumberFormatDetail {

static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559
    && std::numeric_limits<float>::digits == 24 && std::numeric_limits<float>::max_exponent == 128,
    "UI number formatting requires IEEE binary32");

// 精确求出二进制实数乘 10^precision 后是否落在偶数整数上方的半值。
// 不用浮点乘法判断，避免乘法的舍入先把半值附近的数压到半值上。
inline bool NeedsHalfAwayCorrection(std::uint32_t magnitude, int precision) noexcept {
    constexpr std::array<std::uint64_t, 7> powersOfFive{1, 5, 25, 125, 625, 3125, 15625};
    const auto exponent = static_cast<int>((magnitude >> 23) & 0xffu);
    const auto fraction = magnitude & 0x7fffffu;
    const std::uint64_t mantissa = exponent ? fraction | 0x800000u : fraction;
    const int shift = (exponent ? exponent - 150 : -149) + precision;
    if (shift >= 0 || shift < -38) return false;
    const auto product = mantissa * powersOfFive[static_cast<std::size_t>(precision)];
    const auto count = static_cast<unsigned>(-shift);
    const auto mask = (std::uint64_t{1} << count) - 1;
    const bool half = (product & mask) == (std::uint64_t{1} << (count - 1));
    return half && ((product >> count) & 1u) == 0;
}

inline void IncrementLastDecimal(std::string& text) {
    for (auto index = text.size(); index != 0;) {
        const auto current = --index;
        if (text[current] == '.') continue;
        if (text[current] != '9') { ++text[current]; return; }
        text[current] = '0';
    }
    text.insert(text.begin(), '1');
}

} // namespace UiNumberFormatDetail

// 显示用 binary32 规则：默认最多三位；固定精度 0..6 位；半值远离零，负零统一为零。
// 返回空值表示精度无效、非有限数或转换失败，不替调用方填默认数值。
[[nodiscard]] inline std::optional<std::string> FormatUiReal(float value, int decimals) {
    if (decimals < -1 || decimals > 6 || !std::isfinite(value)) return std::nullopt;
    const auto raw = std::bit_cast<std::uint32_t>(value);
    const auto magnitude = raw & 0x7fffffffu;
    const bool negative = (raw & 0x80000000u) != 0;
    const int precision = decimals < 0 ? 3 : decimals;
    // 最大有限 binary32 的六位定点表示（含负号）最多 47 字节。
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
        std::bit_cast<float>(magnitude), std::chars_format::fixed, precision);
    if (converted.ec != std::errc{}) return std::nullopt;
    std::string text(buffer.data(), converted.ptr);
    if (UiNumberFormatDetail::NeedsHalfAwayCorrection(magnitude, precision)) {
        UiNumberFormatDetail::IncrementLastDecimal(text);
    }
    if (decimals < 0) {
        while (text.ends_with('0')) text.pop_back();
        if (text.ends_with('.')) text.pop_back();
    }
    if (negative && text.find_first_not_of("0.") != std::string::npos) text.insert(text.begin(), '-');
    return text;
}

// 整数不经 real 转换，保留 int32 全范围；显式精度只补小数零。
[[nodiscard]] inline std::optional<std::string> FormatUiInteger(std::int32_t value, int decimals = -1) {
    if (decimals < -1 || decimals > 6) return std::nullopt;
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{}) return std::nullopt;
    std::string text(buffer.data(), converted.ptr);
    if (decimals > 0) {
        text.push_back('.');
        text.append(static_cast<std::size_t>(decimals), '0');
    }
    return text;
}

} // namespace War3Map::Text
