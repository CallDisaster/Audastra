#pragma once

#include "War3MapCore/GameUi/FrameDefinition.h"
#include "../Jass/JassSyntax.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

// 地图脚本界面分析与预览模拟共用的 JASS 取值：字面量、运算与界面参数转换。
namespace War3Map::GameUi::detail {

struct ScriptValue {
    enum class Kind : std::uint8_t { Number, String, Boolean, Point, Origin } kind = Kind::Number;
    double number = 0.0;
    bool integer = false;  // JASS integer（除法取整）
    std::string text;      // String 内容、Origin 常量名
    bool boolean = false;
    FramePoint point = FramePoint::TopLeft;

    [[nodiscard]] static ScriptValue Number(double value, bool integer = false);
    [[nodiscard]] static ScriptValue String(std::string value);
    [[nodiscard]] static ScriptValue Boolean(bool value);
};

// 字面量表达式（整数、实数、字符串、布尔）的值；其他表达式为空。
[[nodiscard]] std::optional<ScriptValue> LiteralValue(const Jass::detail::Expr& expr);
// FRAMEPOINT_* 与 ORIGIN_FRAME_* 常量名。
[[nodiscard]] std::optional<ScriptValue> FrameConstant(std::string_view name);
[[nodiscard]] std::optional<ScriptValue> ApplyUnary(Jass::detail::Operator op, ScriptValue operand);
// 数值运算两边都是整数时按整数算（除法向零取整）；字符串只支持 +；比较与逻辑运算得布尔值。
[[nodiscard]] std::optional<ScriptValue> ApplyBinary(Jass::detail::Operator op, const ScriptValue& left,
    const ScriptValue& right);

// 去掉括号后的表达式。
[[nodiscard]] const Jass::detail::Expr& Unparenthesized(const Jass::detail::SyntaxPool& pool, std::int32_t index);
// 表达式原文去掉空白（作为变量键）。
[[nodiscard]] std::string CompactSource(std::string_view text);

using NameLookup = std::function<std::optional<ScriptValue>(std::string_view name)>;
// 只含字面量、名字（交给 lookup）、括号、一元/二元运算和 ConvertFramePointType、I2R 的表达式的值；
// 其他表达式或 lookup 查不到的名字为空。
[[nodiscard]] std::optional<ScriptValue> FoldExpression(const Jass::detail::SyntaxPool& pool, std::int32_t index,
    const NameLookup& lookup);

[[nodiscard]] std::optional<FramePoint> PointOf(const std::optional<ScriptValue>& value);
[[nodiscard]] std::optional<float> NumberOf(const std::optional<ScriptValue>& value);
[[nodiscard]] std::optional<std::string> StringOf(const std::optional<ScriptValue>& value);
[[nodiscard]] std::optional<bool> BooleanOf(const std::optional<ScriptValue>& value);

} // namespace War3Map::GameUi::detail
