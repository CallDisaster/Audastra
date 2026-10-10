#pragma once

#include "PpLexer.h"

#include <cstdint>
#include <span>
#include <string>

// #if / #elif 常量表达式求值（C99：intmax / uintmax 语义，带 U 后缀或与无符号数运算时按无符号）。
// 输入已完成宏展开且 defined 已替换为 0 / 1；剩余标识符按 0 处理（true / false 与 Wave 的 C99 模式一样视为错误）。
namespace War3Map::Jass::detail {

struct PpValue {
    std::int64_t value = 0;
    bool isUnsigned = false;
};

struct PpEvaluation {
    PpValue value;
    std::string error; // 非空表示表达式无效
};

class CheckBudget;
[[nodiscard]] PpEvaluation EvaluatePpExpression(std::span<const PpToken> tokens, CheckBudget* budget = nullptr);

} // namespace War3Map::Jass::detail
