#include "ScriptValue.h"

#include "../Jass/JassLexer.h"

#include <charconv>
#include <cmath>
#include <cstdint>

namespace War3Map::GameUi::detail {
namespace {

using Jass::detail::Expr;
using Jass::detail::ExprKind;
using Jass::detail::Operator;

std::optional<double> ParseInteger(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        base = 16;
        text.remove_prefix(2);
    } else if (text.starts_with("$")) {
        base = 16;
        text.remove_prefix(1);
    } else if (text.size() > 1 && text.front() == '0') {
        base = 8;
        text.remove_prefix(1);
    }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc() || end != text.data() + text.size()) return std::nullopt;
    // JASS 整数为 32 位，十六进制写法可表示负数。
    return static_cast<double>(static_cast<std::int32_t>(static_cast<std::uint32_t>(value)));
}

std::optional<double> ParseReal(std::string_view text) {
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) return std::nullopt;
    return value;
}

double Truncate32(double value) {
    if (!std::isfinite(value)) return 0.0;
    return static_cast<double>(static_cast<std::int32_t>(static_cast<std::int64_t>(value)));
}

} // namespace

ScriptValue ScriptValue::Number(double value, bool integer) {
    ScriptValue result;
    result.kind = Kind::Number;
    result.number = integer ? Truncate32(value) : value;
    result.integer = integer;
    return result;
}

ScriptValue ScriptValue::String(std::string value) {
    ScriptValue result;
    result.kind = Kind::String;
    result.text = std::move(value);
    return result;
}

ScriptValue ScriptValue::Boolean(bool value) {
    ScriptValue result;
    result.kind = Kind::Boolean;
    result.boolean = value;
    return result;
}

ScriptValue ScriptValue::Player(std::int32_t index) {
    ScriptValue result;
    result.kind = Kind::Player;
    result.number = index;
    result.integer = true;
    return result;
}

std::optional<ScriptValue> LiteralValue(const Expr& expr) {
    switch (expr.kind) {
    case ExprKind::Integer:
        if (const auto number = ParseInteger(expr.text)) return ScriptValue::Number(*number, true);
        return std::nullopt;
    case ExprKind::Real:
        if (const auto number = ParseReal(expr.text)) return ScriptValue::Number(*number);
        return std::nullopt;
    case ExprKind::String:
        if (auto text = Jass::detail::StringLiteralValue(expr.text)) return ScriptValue::String(std::move(*text));
        return std::nullopt;
    case ExprKind::Boolean:
        return ScriptValue::Boolean(expr.text == "true");
    default:
        return std::nullopt;
    }
}

std::optional<ScriptValue> FrameConstant(std::string_view name) {
    if (name.starts_with("FRAMEPOINT_")) {
        const auto point = ParseFramePoint(name.substr(11));
        if (!point) return std::nullopt;
        ScriptValue value;
        value.kind = ScriptValue::Kind::Point;
        value.point = *point;
        return value;
    }
    if (name.starts_with("ORIGIN_FRAME_")) {
        ScriptValue value;
        value.kind = ScriptValue::Kind::Origin;
        value.text = std::string(name);
        return value;
    }
    // common.j 与 Blizzard.j 里的整数常量（地图脚本不含它们的声明）。
    struct IntegerConstant {
        std::string_view name;
        int value;
    };
    static constexpr IntegerConstant kIntegers[] = {
        {"PLAYER_NEUTRAL_AGGRESSIVE", 12}, {"PLAYER_NEUTRAL_PASSIVE", 15}, {"bj_PLAYER_NEUTRAL_VICTIM", 13},
        {"bj_PLAYER_NEUTRAL_EXTRA", 14}, {"bj_MAX_PLAYERS", 12}, {"bj_MAX_PLAYER_SLOTS", 16},
        {"bj_SORTTYPE_SORTBYVALUE", 0}, {"bj_SORTTYPE_SORTBYPLAYER", 1}, {"bj_SORTTYPE_SORTBYLABEL", 2},
    };
    for (const auto& constant : kIntegers) {
        if (constant.name == name) return ScriptValue::Number(constant.value, true);
    }
    return std::nullopt;
}

std::optional<ScriptValue> ApplyUnary(Operator op, ScriptValue operand) {
    if (op == Operator::Minus && operand.kind == ScriptValue::Kind::Number) {
        operand.number = operand.integer ? Truncate32(-operand.number) : -operand.number;
        return operand;
    }
    if (op == Operator::Plus && operand.kind == ScriptValue::Kind::Number) return operand;
    if (op == Operator::Not && operand.kind == ScriptValue::Kind::Boolean) {
        operand.boolean = !operand.boolean;
        return operand;
    }
    return std::nullopt;
}

std::optional<ScriptValue> ApplyBinary(Operator op, const ScriptValue& left, const ScriptValue& right) {
    using Kind = ScriptValue::Kind;
    if (op == Operator::And || op == Operator::Or) {
        if (left.kind != Kind::Boolean || right.kind != Kind::Boolean) return std::nullopt;
        return ScriptValue::Boolean(op == Operator::And ? left.boolean && right.boolean : left.boolean || right.boolean);
    }
    if (op == Operator::Equal || op == Operator::NotEqual) {
        if (left.kind != right.kind) return std::nullopt;
        bool equal = false;
        switch (left.kind) {
        case Kind::Number: equal = left.number == right.number; break;
        case Kind::String: equal = left.text == right.text; break;
        case Kind::Boolean: equal = left.boolean == right.boolean; break;
        case Kind::Point: equal = left.point == right.point; break;
        case Kind::Origin: equal = left.text == right.text; break;
        case Kind::Player: equal = left.number == right.number; break;
        }
        return ScriptValue::Boolean(op == Operator::Equal ? equal : !equal);
    }
    if (left.kind == Kind::String && right.kind == Kind::String) {
        if (op != Operator::Plus) return std::nullopt;
        return ScriptValue::String(left.text + right.text);
    }
    if (left.kind != Kind::Number || right.kind != Kind::Number) return std::nullopt;
    const bool integer = left.integer && right.integer;
    switch (op) {
    case Operator::Plus: return ScriptValue::Number(left.number + right.number, integer);
    case Operator::Minus: return ScriptValue::Number(left.number - right.number, integer);
    case Operator::Multiply: return ScriptValue::Number(left.number * right.number, integer);
    case Operator::Divide:
        if (right.number == 0.0) return std::nullopt;
        return ScriptValue::Number(left.number / right.number, integer);
    case Operator::Less: return ScriptValue::Boolean(left.number < right.number);
    case Operator::LessEqual: return ScriptValue::Boolean(left.number <= right.number);
    case Operator::Greater: return ScriptValue::Boolean(left.number > right.number);
    case Operator::GreaterEqual: return ScriptValue::Boolean(left.number >= right.number);
    default: return std::nullopt;
    }
}

const Expr& Unparenthesized(const Jass::detail::SyntaxPool& pool, std::int32_t index) {
    const Expr* expr = &pool.At(index);
    while (expr->kind == ExprKind::Paren && expr->left >= 0) expr = &pool.At(expr->left);
    return *expr;
}

std::string CompactSource(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') result.push_back(c);
    }
    return result;
}

std::optional<ScriptValue> FoldExpression(const Jass::detail::SyntaxPool& pool, std::int32_t index, const NameLookup& lookup) {
    if (index < 0) return std::nullopt;
    const Expr& expr = Unparenthesized(pool, index);
    switch (expr.kind) {
    case ExprKind::Integer:
    case ExprKind::Real:
    case ExprKind::String:
    case ExprKind::Boolean:
        return LiteralValue(expr);
    case ExprKind::Name:
        return lookup(expr.text);
    case ExprKind::Unary: {
        auto operand = FoldExpression(pool, expr.left, lookup);
        return operand ? ApplyUnary(expr.op, std::move(*operand)) : std::nullopt;
    }
    case ExprKind::Binary: {
        const auto left = FoldExpression(pool, expr.left, lookup);
        const auto right = FoldExpression(pool, expr.right, lookup);
        if (!left || !right) return std::nullopt;
        ScriptValue l = *left;
        ScriptValue r = *right;
        if (l.kind == ScriptValue::Kind::Number && r.kind == ScriptValue::Kind::Number && l.integer != r.integer) {
            l.integer = r.integer = false;  // 整数参与实数运算时转成实数
        }
        return ApplyBinary(expr.op, l, r);
    }
    case ExprKind::Call: {
        if (expr.left >= 0 || expr.argumentCount != 1) return std::nullopt;
        const auto argument = FoldExpression(pool, pool.Argument(expr, 0), lookup);
        if (expr.text == "ConvertFramePointType") {
            const auto point = PointOf(argument);
            if (!point) return std::nullopt;
            ScriptValue value;
            value.kind = ScriptValue::Kind::Point;
            value.point = *point;
            return value;
        }
        if (expr.text == "I2R" && argument && argument->kind == ScriptValue::Kind::Number) {
            return ScriptValue::Number(argument->number);
        }
        if (expr.text == "Player" && argument && argument->kind == ScriptValue::Kind::Number && argument->integer) {
            return ScriptValue::Player(static_cast<std::int32_t>(argument->number));
        }
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

std::optional<FramePoint> PointOf(const std::optional<ScriptValue>& value) {
    if (!value) return std::nullopt;
    if (value->kind == ScriptValue::Kind::Point) return value->point;
    if (value->kind == ScriptValue::Kind::Number && value->number >= 0.0 && value->number <= 8.0 &&
        value->number == std::floor(value->number)) {
        return static_cast<FramePoint>(static_cast<int>(value->number));
    }
    return std::nullopt;
}

std::optional<float> NumberOf(const std::optional<ScriptValue>& value) {
    if (!value || value->kind != ScriptValue::Kind::Number) return std::nullopt;
    return static_cast<float>(value->number);
}

std::optional<std::string> StringOf(const std::optional<ScriptValue>& value) {
    if (!value || value->kind != ScriptValue::Kind::String) return std::nullopt;
    return value->text;
}

std::optional<bool> BooleanOf(const std::optional<ScriptValue>& value) {
    if (!value || value->kind != ScriptValue::Kind::Boolean) return std::nullopt;
    return value->boolean;
}

std::optional<std::uint32_t> PlayerOf(const std::optional<ScriptValue>& value) {
    if (!value || value->kind != ScriptValue::Kind::Player || value->number < 0.0) return std::nullopt;
    return static_cast<std::uint32_t>(value->number);
}

} // namespace War3Map::GameUi::detail
