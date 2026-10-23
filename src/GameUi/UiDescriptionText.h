#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 界面描述读取、检查与展开共用的上限、取值引用与文字模板。
namespace War3Map::GameUi::detail {

inline constexpr std::size_t kMaxTextBytes = 1024;
inline constexpr std::size_t kMaxPathBytes = 260;
inline constexpr std::size_t kMaxValues = 128;
inline constexpr std::size_t kMaxFrames = 512;
inline constexpr std::size_t kMaxStates = 64;
inline constexpr std::size_t kMaxEvents = 128;
inline constexpr std::size_t kMaxDepth = 16;
inline constexpr std::uint32_t kMaxArraySize = 64;
inline constexpr std::uint32_t kMaxRepeat = 64;

[[nodiscard]] inline Diagnostic MakeUiError(const char* code, std::string message) {
    return Diagnostic{DiagnosticSeverity::Error, "GameUi", code, std::move(message), std::nullopt};
}

// "编号"、"编号[]"、"编号[下标]"。
struct UiValueReference {
    std::string id;
    bool perInstance = false;
    std::optional<std::uint32_t> index;
};
[[nodiscard]] std::optional<UiValueReference> ParseUiValueReference(std::string_view text);

// 文字模板的一段：原样文字，或一个取值（可带小数位数 0..6）。
struct UiTextPiece {
    std::string literal;
    std::optional<UiValueReference> reference;
    std::optional<int> decimals;
};
// {编号}、{编号:小数位}、{编号[]}；{{ 与 }} 为花括号本身。不合法时返回空并写明原因。
[[nodiscard]] std::optional<std::vector<UiTextPiece>> ParseUiTextTemplate(std::string_view text, std::string* error);

// 实数最短往返写法（整值不带小数点，-0 写作 0）。
[[nodiscard]] std::string FormatUiNumber(double value);

} // namespace War3Map::GameUi::detail
