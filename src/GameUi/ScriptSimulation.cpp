#include "War3MapCore/GameUi/ScriptSimulation.h"

#include "War3MapCore/Text/TextUtil.h"
#include "ScriptUiApi.h"
#include "ScriptValue.h"
#include "../Jass/JassParser.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <set>
#include <unordered_map>

namespace War3Map::GameUi {
namespace {

using Jass::detail::Expr;
using Jass::detail::ExprKind;
using Jass::detail::Operator;
using Jass::detail::Stmt;
using Jass::detail::StmtKind;
using Jass::detail::SyntaxPool;
using detail::CallKind;
using detail::CallSpec;
using detail::ScriptValue;

constexpr const char* kSubsystem = "GameUi";
constexpr std::size_t kMaxLoopIterations = 64;
constexpr std::size_t kMaxSteps = 100000;
constexpr std::size_t kMaxInputs = 200;
constexpr std::size_t kMaxLabel = 160;

Diagnostic MakeDiagnostic(DiagnosticSeverity severity, const char* code, std::string message) {
    return Diagnostic{severity, kSubsystem, code, std::move(message), std::nullopt};
}

// 脚本里声明的类型；句柄类型（timer、unit…）不能作为输入。
enum class Type : std::uint8_t { Unknown, Integer, Real, String, Boolean };

Type TypeOf(std::string_view jassType) {
    if (jassType == "integer") return Type::Integer;
    if (jassType == "real") return Type::Real;
    if (jassType == "string") return Type::String;
    if (jassType == "boolean") return Type::Boolean;
    return Type::Unknown;
}

Type TypeOfValue(const ScriptValue& value) {
    switch (value.kind) {
    case ScriptValue::Kind::Number: return value.integer ? Type::Integer : Type::Real;
    case ScriptValue::Kind::String: return Type::String;
    case ScriptValue::Kind::Boolean: return Type::Boolean;
    default: return Type::Unknown;
    }
}

bool IsNumeric(Type type) { return type == Type::Integer || type == Type::Real; }

// 常用的取值原生函数：返回类型与求值。
struct NativeSpec {
    std::string_view name;
    Type returns;
};
constexpr NativeSpec kNatives[] = {
    {"I2S", Type::String}, {"R2S", Type::String}, {"R2SW", Type::String}, {"SubString", Type::String},
    {"I2R", Type::Real}, {"S2R", Type::Real}, {"RMinBJ", Type::Real}, {"RMaxBJ", Type::Real}, {"RAbsBJ", Type::Real},
    {"ModuloReal", Type::Real}, {"R2I", Type::Integer}, {"S2I", Type::Integer}, {"StringLength", Type::Integer},
    {"IMinBJ", Type::Integer}, {"IMaxBJ", Type::Integer}, {"IAbsBJ", Type::Integer}, {"ModuloInteger", Type::Integer},
};

std::optional<Type> NativeReturn(std::string_view name) {
    for (const auto& native : kNatives) {
        if (native.name == name) return native.returns;
    }
    return std::nullopt;
}

std::string FormatReal(double value, int precision, int width = 0) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%*.*f", std::clamp(width, 0, 32), std::clamp(precision, 0, 16), value);
    return buffer;
}

std::string Label(std::string_view text) {
    std::string label = Text::TrimCWhitespaceCopy(text);
    if (label.size() > kMaxLabel) label = label.substr(0, kMaxLabel) + "…";
    return label;
}

std::string LowerName(std::string_view name) { return Text::LowerAscii(name); }

// 已解析的脚本与要模拟的函数。
struct PreparedScript {
    std::vector<MapScriptText> scripts;
    ScriptFramePlan plan;
    std::string function;
    std::size_t script = 0;
    Jass::detail::ProgramFile program;
    Jass::detail::FunctionBody body;
    const Jass::detail::FunctionDecl* decl = nullptr;
    std::unordered_map<std::string, Type> globalTypes;
    std::unordered_map<std::string, ScriptValue> constants;
    std::unordered_map<std::string, Type> localTypes;
    std::unordered_map<std::string, std::uint32_t> frameByVariable;
    std::unordered_map<std::string, std::vector<std::uint32_t>> framesByBase;
    std::unordered_map<std::string, std::vector<std::uint32_t>> framesByName;
};

// 一次模拟执行。
class Execution {
public:
    Execution(const PreparedScript& state, const std::map<std::string, std::string>& values)
        : state_(state), values_(values) {
        result_.function = state.function;
        result_.frames = state.plan.frames;
    }

    SimulationResult Run() {
        for (const auto& local : state_.body.locals) {
            if (local.init < 0) continue;
            if (auto value = Eval(local.init, TypeOfDeclared(local.name))) env_[std::string(local.name)] = std::move(*value);
        }
        Block(state_.body.statements);
        result_.touched.assign(touched_.begin(), touched_.end());
        return std::move(result_);
    }

private:
    const SyntaxPool& Pool() const { return state_.body.pool; }
    std::string_view Source() const { return state_.scripts[state_.script].text; }

    const Expr& Unwrap(std::int32_t index) const {
        const Expr* expr = &Pool().At(index);
        while (expr->kind == ExprKind::Paren && expr->left >= 0) expr = &Pool().At(expr->left);
        return *expr;
    }

    std::string_view SourceOf(const Expr& expr) const { return Source().substr(expr.begin, expr.end - expr.begin); }

    Type TypeOfDeclared(std::string_view name) const {
        if (const auto local = state_.localTypes.find(std::string(name)); local != state_.localTypes.end()) return local->second;
        if (const auto global = state_.globalTypes.find(std::string(name)); global != state_.globalTypes.end()) return global->second;
        return Type::Unknown;
    }

    bool IsLocal(std::string_view name) const { return state_.localTypes.contains(std::string(name)); }

    // 不求值、只按字面量与声明推断的类型；推不出时为 Unknown。
    Type StaticType(std::int32_t index) const {
        if (index < 0) return Type::Unknown;
        const Expr& expr = Unwrap(index);
        switch (expr.kind) {
        case ExprKind::Integer: return Type::Integer;
        case ExprKind::Real: return Type::Real;
        case ExprKind::String: return Type::String;
        case ExprKind::Boolean: return Type::Boolean;
        case ExprKind::Name: return TypeOfDeclared(expr.text);
        case ExprKind::Index:
            return expr.left >= 0 && Pool().At(expr.left).kind == ExprKind::Name ? TypeOfDeclared(Pool().At(expr.left).text)
                                                                                  : Type::Unknown;
        case ExprKind::Unary: return expr.op == Operator::Not ? Type::Boolean : StaticType(expr.left);
        case ExprKind::Binary: {
            switch (expr.op) {
            case Operator::Equal: case Operator::NotEqual: case Operator::Less: case Operator::LessEqual:
            case Operator::Greater: case Operator::GreaterEqual: case Operator::And: case Operator::Or:
                return Type::Boolean;
            default: break;
            }
            const Type left = StaticType(expr.left);
            const Type right = StaticType(expr.right);
            if (left == Type::String || right == Type::String) return Type::String;
            if (left == Type::Real || right == Type::Real) return Type::Real;
            if (left == Type::Integer && right == Type::Integer) return Type::Integer;
            return Type::Unknown;
        }
        case ExprKind::Call:
            if (expr.left < 0) {
                if (const auto native = NativeReturn(expr.text)) return *native;
            }
            return Type::Unknown;
        default: return Type::Unknown;
        }
    }

    std::optional<ScriptValue> Input(const std::string& key, std::string_view label, Type type, std::uint32_t line,
        bool condition = false) {
        if (type == Type::Unknown) return std::nullopt;
        if (const auto found = inputIndex_.find(key); found != inputIndex_.end()) return Parse(result_.inputs[found->second]);
        if (result_.inputs.size() >= kMaxInputs) {
            if (!inputLimitReported_) {
                result_.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_SIMULATION_INPUT_LIMIT",
                    "模拟输入超过 " + std::to_string(kMaxInputs) + " 项，其余按无法确定处理"));
                inputLimitReported_ = true;
            }
            return std::nullopt;
        }
        SimulationInput input;
        input.key = key;
        input.label = Label(label);
        input.type = type == Type::String ? SimulationValueType::Text
            : type == Type::Boolean       ? SimulationValueType::Boolean
                                          : SimulationValueType::Number;
        input.integer = type == Type::Integer;
        input.line = line;
        input.condition = condition;
        input.value = input.type == SimulationValueType::Boolean ? "false" : input.type == SimulationValueType::Number ? "0" : "";
        if (const auto given = values_.find(key); given != values_.end()) input.value = given->second;
        inputIndex_.emplace(key, result_.inputs.size());
        result_.inputs.push_back(std::move(input));
        return Parse(result_.inputs.back());
    }

    static ScriptValue Parse(const SimulationInput& input) {
        switch (input.type) {
        case SimulationValueType::Text: return ScriptValue::String(input.value);
        case SimulationValueType::Boolean: return ScriptValue::Boolean(input.value == "true" || input.value == "1");
        default: {
            double number = 0.0;
            const std::string text = Text::TrimCWhitespaceCopy(input.value);
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
            if (error != std::errc() || end != text.data() + text.size() || !std::isfinite(number)) number = 0.0;
            return ScriptValue::Number(number, input.integer);
        }
        }
    }

    std::optional<ScriptValue> Native(const Expr& call, Type hint) {
        const auto argument = [&](std::uint32_t i, Type type) -> std::optional<ScriptValue> {
            return i < call.argumentCount ? Eval(Pool().Argument(call, i), type) : std::nullopt;
        };
        const std::string_view name = call.text;
        if (name == "I2S") {
            if (const auto value = detail::NumberOf(argument(0, Type::Integer)))
                return ScriptValue::String(std::to_string(static_cast<std::int32_t>(*value)));
        } else if (name == "R2S") {
            if (const auto value = detail::NumberOf(argument(0, Type::Real))) return ScriptValue::String(FormatReal(*value, 3));
        } else if (name == "R2SW") {
            const auto value = detail::NumberOf(argument(0, Type::Real));
            const auto width = detail::NumberOf(argument(1, Type::Integer));
            const auto precision = detail::NumberOf(argument(2, Type::Integer));
            if (value && width && precision)
                return ScriptValue::String(FormatReal(*value, static_cast<int>(*precision), static_cast<int>(*width)));
        } else if (name == "I2R") {
            if (const auto value = detail::NumberOf(argument(0, Type::Integer))) return ScriptValue::Number(*value);
        } else if (name == "R2I") {
            if (const auto value = detail::NumberOf(argument(0, Type::Real))) return ScriptValue::Number(*value, true);
        } else if (name == "S2I" || name == "S2R") {
            if (const auto text = detail::StringOf(argument(0, Type::String))) {
                double number = 0.0;
                const std::string trimmed = Text::TrimCWhitespaceCopy(*text);
                (void)std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), number);
                return ScriptValue::Number(number, name == "S2I");
            }
        } else if (name == "StringLength") {
            if (const auto text = detail::StringOf(argument(0, Type::String)))
                return ScriptValue::Number(static_cast<double>(text->size()), true);
        } else if (name == "SubString") {
            const auto text = detail::StringOf(argument(0, Type::String));
            const auto start = detail::NumberOf(argument(1, Type::Integer));
            const auto end = detail::NumberOf(argument(2, Type::Integer));
            if (text && start && end) {
                const auto size = static_cast<long long>(text->size());
                const long long from = std::clamp(static_cast<long long>(*start), 0LL, size);
                const long long to = std::clamp(static_cast<long long>(*end), from, size);
                return ScriptValue::String(text->substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)));
            }
        } else if (name == "RMinBJ" || name == "RMaxBJ" || name == "IMinBJ" || name == "IMaxBJ") {
            const bool integer = name.front() == 'I';
            const auto a = detail::NumberOf(argument(0, integer ? Type::Integer : Type::Real));
            const auto b = detail::NumberOf(argument(1, integer ? Type::Integer : Type::Real));
            if (a && b) return ScriptValue::Number(name.substr(1, 3) == "Min" ? std::min(*a, *b) : std::max(*a, *b), integer);
        } else if (name == "RAbsBJ" || name == "IAbsBJ") {
            const bool integer = name.front() == 'I';
            if (const auto value = detail::NumberOf(argument(0, integer ? Type::Integer : Type::Real)))
                return ScriptValue::Number(std::fabs(*value), integer);
        } else if (name == "ModuloInteger" || name == "ModuloReal") {
            const bool integer = name == "ModuloInteger";
            const auto a = detail::NumberOf(argument(0, integer ? Type::Integer : Type::Real));
            const auto b = detail::NumberOf(argument(1, integer ? Type::Integer : Type::Real));
            if (a && b && *b != 0.0f) {
                double modulo = std::fmod(static_cast<double>(*a), static_cast<double>(*b));
                if (modulo < 0.0) modulo += std::fabs(static_cast<double>(*b));
                return ScriptValue::Number(modulo, integer);
            }
        }
        (void)hint;
        return std::nullopt;
    }

    std::optional<ScriptValue> Eval(std::int32_t index, Type hint) {
        if (index < 0) return std::nullopt;
        const Expr& expr = Unwrap(index);
        switch (expr.kind) {
        case ExprKind::Integer:
        case ExprKind::Real:
        case ExprKind::String:
        case ExprKind::Boolean:
            return detail::LiteralValue(expr);
        case ExprKind::Name: {
            if (auto constant = detail::FrameConstant(expr.text)) return constant;
            const std::string name(expr.text);
            if (const auto found = env_.find(name); found != env_.end()) return found->second;
            if (!IsLocal(name)) {
                if (const auto constant = state_.constants.find(name); constant != state_.constants.end()) return constant->second;
            }
            const Type declared = TypeOfDeclared(name);
            return Input(name, expr.text, declared != Type::Unknown ? declared : hint, expr.line);
        }
        case ExprKind::Index: {
            const std::string key = detail::CompactSource(SourceOf(expr));
            if (const auto found = env_.find(key); found != env_.end()) return found->second;
            const Type declared = StaticType(index);
            return Input(key, SourceOf(expr), declared != Type::Unknown ? declared : hint, expr.line);
        }
        case ExprKind::Unary: {
            const Type inner = expr.op == Operator::Not ? Type::Boolean : (IsNumeric(hint) ? hint : Type::Real);
            auto operand = Eval(expr.left, inner);
            return operand ? detail::ApplyUnary(expr.op, std::move(*operand)) : std::nullopt;
        }
        case ExprKind::Binary: {
            Type leftHint = StaticType(expr.left);
            Type rightHint = StaticType(expr.right);
            switch (expr.op) {
            case Operator::And:
            case Operator::Or:
                leftHint = rightHint = Type::Boolean;
                break;
            case Operator::Equal:
            case Operator::NotEqual:
                // 两边都推不出类型（多为句柄比较）时不造输入，由外层条件整体作为输入。
                if (leftHint == Type::Unknown && rightHint == Type::Unknown) return std::nullopt;
                if (leftHint == Type::Unknown) leftHint = rightHint;
                if (rightHint == Type::Unknown) rightHint = leftHint;
                break;
            case Operator::Plus:
                if (leftHint == Type::String || rightHint == Type::String || hint == Type::String) {
                    leftHint = rightHint = Type::String;
                    break;
                }
                [[fallthrough]];
            default: {
                const Type numeric = IsNumeric(hint) ? hint : Type::Real;
                if (leftHint == Type::Unknown) leftHint = IsNumeric(rightHint) ? rightHint : numeric;
                if (rightHint == Type::Unknown) rightHint = IsNumeric(leftHint) ? leftHint : numeric;
                break;
            }
            }
            const auto left = Eval(expr.left, leftHint);
            const auto right = Eval(expr.right, rightHint);
            if (!left || !right) return std::nullopt;
            // JASS 把整数自动转成实数参与实数运算与比较。
            ScriptValue l = *left;
            ScriptValue r = *right;
            if (l.kind == ScriptValue::Kind::Number && r.kind == ScriptValue::Kind::Number && l.integer != r.integer) {
                l.integer = r.integer = false;
            }
            return detail::ApplyBinary(expr.op, l, r);
        }
        case ExprKind::Call: {
            if (expr.left >= 0) return std::nullopt;
            if (expr.text == "ConvertFramePointType" && expr.argumentCount == 1) {
                if (const auto point = detail::PointOf(Eval(Pool().Argument(expr, 0), Type::Integer))) {
                    ScriptValue value;
                    value.kind = ScriptValue::Kind::Point;
                    value.point = *point;
                    return value;
                }
                return std::nullopt;
            }
            if (NativeReturn(expr.text)) return Native(expr, hint);
            // 其他函数的返回值：作为输入。
            return Input(detail::CompactSource(SourceOf(expr)), SourceOf(expr), hint, expr.line);
        }
        default:
            return std::nullopt;
        }
    }

    std::vector<std::uint32_t> FramesOf(std::int32_t index) {
        const Expr& expr = Unwrap(index);
        if (expr.kind == ExprKind::Call && expr.left < 0) {
            const auto spec = detail::LookupCall(expr.text);
            if (!spec || spec->kind != CallKind::ByName || expr.argumentCount == 0) return {};
            const auto name = detail::StringOf(Eval(Pool().Argument(expr, 0), Type::String));
            if (!name) return {};
            const auto found = state_.framesByName.find(LowerName(*name));
            return found == state_.framesByName.end() ? std::vector<std::uint32_t>{} : found->second;
        }
        if (expr.kind != ExprKind::Name && expr.kind != ExprKind::Index) return {};
        if (const auto found = state_.frameByVariable.find(detail::CompactSource(SourceOf(expr)));
            found != state_.frameByVariable.end()) {
            return {found->second};
        }
        const std::string_view base = expr.kind == ExprKind::Name ? expr.text
            : expr.left >= 0 && Pool().At(expr.left).kind == ExprKind::Name ? Pool().At(expr.left).text
                                                                             : std::string_view{};
        if (base.empty() || IsLocal(base)) return {};
        const auto found = state_.framesByBase.find(std::string(base));
        return found == state_.framesByBase.end() ? std::vector<std::uint32_t>{} : found->second;
    }

    ScriptFrameRef RefOf(std::int32_t index) {
        const Expr& expr = Unwrap(index);
        if (expr.kind == ExprKind::Call && expr.left < 0) {
            if (const auto spec = detail::LookupCall(expr.text)) {
                std::vector<std::optional<ScriptValue>> args;
                for (std::uint32_t i = 0; i < expr.argumentCount; ++i) {
                    args.push_back(Eval(Pool().Argument(expr, i), i == 0 && spec->kind == CallKind::ByName ? Type::String
                                                                                                              : Type::Integer));
                }
                if (auto ref = detail::FrameRefOfCall(*spec, args)) {
                    if (ref->target == ScriptFrameTarget::Named) {
                        if (const auto found = state_.framesByName.find(LowerName(ref->name)); found != state_.framesByName.end()) {
                            ref->target = ScriptFrameTarget::Script;
                            ref->frame = found->second.back();
                        }
                    }
                    return *ref;
                }
            }
            return {};
        }
        const auto frames = FramesOf(index);
        if (frames.size() != 1) return {};
        return ScriptFrameRef{ScriptFrameTarget::Script, state_.plan.frames[frames.front()].name, frames.front()};
    }

    void Call(std::int32_t index) {
        if (index < 0) return;
        const Expr& call = Unwrap(index);
        if (call.kind != ExprKind::Call || call.left >= 0) return;
        const auto spec = detail::LookupCall(call.text);
        if (!spec) return;
        if (spec->kind == CallKind::Create) {
            NoteOnce("GAMEUI_SIMULATION_CREATE", "模拟不创建新框架：" + std::string(call.text) + " 第 " +
                std::to_string(call.line) + " 行");
            return;
        }
        if (!detail::IsSetter(spec->kind) || spec->targetArg < 0 ||
            static_cast<std::uint32_t>(spec->targetArg) >= call.argumentCount) {
            return;
        }
        const auto frames = FramesOf(Pool().Argument(call, static_cast<std::uint32_t>(spec->targetArg)));
        if (frames.empty()) {
            NoteOnce("GAMEUI_SIMULATION_UNTRACED", std::string(call.text) + " 第 " + std::to_string(call.line) +
                " 行作用的框架不是脚本创建后保存的变量，模拟跳过");
            return;
        }
        std::vector<std::optional<ScriptValue>> args(call.argumentCount);
        ScriptFrameRef relative;
        const auto value = [&](std::uint32_t i, Type type) {
            if (i < call.argumentCount) args[i] = Eval(Pool().Argument(call, i), type);
        };
        switch (spec->kind) {
        case CallKind::SetAbsPoint: value(1, Type::Integer); value(2, Type::Real); value(3, Type::Real); break;
        case CallKind::SetPoint:
            value(1, Type::Integer); value(3, Type::Integer); value(4, Type::Real); value(5, Type::Real);
            if (call.argumentCount > 2) relative = RefOf(Pool().Argument(call, 2));
            break;
        case CallKind::SetAllPoints:
            if (call.argumentCount > 1) relative = RefOf(Pool().Argument(call, 1));
            break;
        case CallKind::SetSize: value(1, Type::Real); value(2, Type::Real); break;
        case CallKind::SetWidth: case CallKind::SetHeight: value(1, Type::Real); break;
        case CallKind::SetTexture: value(1, Type::String); break;
        case CallKind::SetText: case CallKind::AddButton:
            if (spec->valueArg >= 0) value(static_cast<std::uint32_t>(spec->valueArg), Type::String);
            break;
        case CallKind::SetVisible: case CallKind::SetHidden:
            if (spec->valueArg >= 0) value(static_cast<std::uint32_t>(spec->valueArg), Type::Boolean);
            break;
        default: break;
        }
        for (const std::uint32_t frame : frames) {
            if (detail::ApplyFrameCall(result_.frames[frame], *spec, args, relative)) {
                touched_.insert(frame);
                ++result_.appliedCalls;
            } else {
                result_.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Info, "GAMEUI_SIMULATION_ARGUMENT",
                    std::string(call.text) + " 第 " + std::to_string(call.line) + " 行的参数无法确定，未应用"));
            }
        }
    }

    void NoteOnce(const char* code, std::string message) {
        if (!noted_.insert(message).second) return;
        result_.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Info, code, std::move(message)));
    }

    bool Condition(std::int32_t condition, std::uint32_t line) {
        if (const auto value = detail::BooleanOf(Eval(condition, Type::Boolean))) return *value;
        const Expr& expr = Unwrap(condition);
        const auto chosen = Input("if:" + std::to_string(line), SourceOf(expr), Type::Boolean, line, true);
        return chosen && chosen->boolean;
    }

    void Block(const std::vector<std::int32_t>& statements) {
        for (const std::int32_t index : statements) {
            if (returned_ || exitLoop_ || steps_ > kMaxSteps) return;
            Statement(index);
        }
    }

    void Statement(std::int32_t index) {
        if (++steps_ > kMaxSteps) {
            NoteOnce("GAMEUI_SIMULATION_STEPS", "模拟执行超过 " + std::to_string(kMaxSteps) + " 条语句，已停止");
            return;
        }
        const Stmt& statement = Pool().StmtAt(index);
        switch (statement.kind) {
        case StmtKind::Set: {
            if (statement.target < 0) return;
            const Expr& target = Unwrap(statement.target);
            std::string key;
            if (target.kind == ExprKind::Name) key = std::string(target.text);
            else if (target.kind == ExprKind::Index) key = detail::CompactSource(SourceOf(target));
            // 返回句柄的界面设置（如 DialogAddButton）：执行界面效果，返回值不模拟。
            const Expr* assigned = statement.value >= 0 ? &Unwrap(statement.value) : nullptr;
            if (assigned && assigned->kind == ExprKind::Call && assigned->left < 0) {
                if (const auto spec = detail::LookupCall(assigned->text); spec && detail::IsSetter(spec->kind)) {
                    Call(statement.value);
                    if (!key.empty()) env_.erase(key);
                    return;
                }
            }
            if (key.empty()) return;
            if (auto value = Eval(statement.value, StaticType(statement.target))) env_[key] = std::move(*value);
            else env_.erase(key);
            return;
        }
        case StmtKind::Call:
            Call(statement.value);
            return;
        case StmtKind::If:
            for (const auto& branch : statement.branches) {
                if (branch.condition < 0 || Condition(branch.condition, branch.line)) {
                    Block(branch.body);
                    return;
                }
            }
            return;
        case StmtKind::Loop: {
            std::size_t iterations = 0;
            while (!returned_ && steps_ <= kMaxSteps) {
                if (++iterations > kMaxLoopIterations) {
                    NoteOnce("GAMEUI_SIMULATION_LOOP", "第 " + std::to_string(statement.line) + " 行的循环超过 " +
                        std::to_string(kMaxLoopIterations) + " 次，已停止");
                    break;
                }
                Block(statement.body);
                if (exitLoop_) break;
            }
            exitLoop_ = false;
            return;
        }
        case StmtKind::ExitWhen:
            if (const auto value = detail::BooleanOf(Eval(statement.value, Type::Boolean))) {
                exitLoop_ = *value;
            } else {
                NoteOnce("GAMEUI_SIMULATION_LOOP", "第 " + std::to_string(statement.line) + " 行的退出条件无法确定，循环只执行一次");
                exitLoop_ = true;
            }
            return;
        case StmtKind::Return:
            returned_ = true;
            return;
        default:
            return;
        }
    }

    const PreparedScript& state_;
    const std::map<std::string, std::string>& values_;
    SimulationResult result_;
    std::unordered_map<std::string, ScriptValue> env_;
    std::unordered_map<std::string, std::size_t> inputIndex_;
    std::set<std::uint32_t> touched_;
    std::set<std::string> noted_;
    std::size_t steps_ = 0;
    bool returned_ = false;
    bool exitLoop_ = false;
    bool inputLimitReported_ = false;
};

} // namespace

struct ScriptSimulator::State : PreparedScript {};

std::vector<std::string> SimulationFunctions(const ScriptFramePlan& plan) {
    std::vector<std::string> functions;
    for (const auto& frame : plan.frames) {
        for (const auto& function : frame.runtimeFunctions) {
            if (std::find(functions.begin(), functions.end(), function) == functions.end()) functions.push_back(function);
        }
    }
    return functions;
}

ScriptSimulator::ScriptSimulator(std::unique_ptr<State> state) : state_(std::move(state)) {}
ScriptSimulator::ScriptSimulator(ScriptSimulator&&) noexcept = default;
ScriptSimulator& ScriptSimulator::operator=(ScriptSimulator&&) noexcept = default;
ScriptSimulator::~ScriptSimulator() = default;

const std::string& ScriptSimulator::Function() const noexcept { return state_->function; }

Result<ScriptSimulator> ScriptSimulator::Prepare(std::vector<MapScriptText> scripts, ScriptFramePlan plan,
    const std::string& function) {
    auto state = std::make_unique<State>();
    state->scripts = std::move(scripts);
    state->plan = std::move(plan);
    state->function = function;
    bool found = false;
    for (std::size_t i = 0; i < state->scripts.size() && !found; ++i) {
        if (state->scripts[i].language != ScriptLanguage::Jass) continue;
        auto program = Jass::detail::ParseProgram(state->scripts[i].text);
        const auto match = std::find_if(program.functions.begin(), program.functions.end(),
            [&](const auto& decl) { return decl.name == function; });
        if (match == program.functions.end()) continue;
        state->script = i;
        state->program = std::move(program);
        found = true;
    }
    if (!found) {
        return Result<ScriptSimulator>::Failure(kSubsystem, "GAMEUI_SIMULATION_FUNCTION_MISSING",
            "地图脚本里没有函数 " + function);
    }
    auto& program = state->program;
    for (const auto& decl : program.functions) {
        if (decl.name == function) state->decl = &decl;
    }
    state->body = Jass::detail::ParseFunctionBody(program.tokens, *state->decl);
    for (const auto& global : program.globals) {
        state->globalTypes[std::string(global.name)] = TypeOf(global.type);
        if (!global.constant || global.array || global.init < 0) continue;
        const auto lookup = [&](std::string_view name) -> std::optional<ScriptValue> {
            if (auto constant = detail::FrameConstant(name)) return constant;
            const auto known = state->constants.find(std::string(name));
            return known == state->constants.end() ? std::nullopt : std::optional<ScriptValue>(known->second);
        };
        if (auto value = detail::FoldExpression(program.globalPool, global.init, lookup))
            state->constants[std::string(global.name)] = std::move(*value);
    }
    for (const auto& param : state->decl->params) state->localTypes[std::string(param.name)] = TypeOf(param.type);
    for (const auto& local : state->body.locals) state->localTypes[std::string(local.name)] = TypeOf(local.type);
    for (std::uint32_t i = 0; i < state->plan.frames.size(); ++i) {
        const auto& frame = state->plan.frames[i];
        if (!frame.variable.empty()) {
            state->frameByVariable[frame.variable] = i;
            state->framesByBase[frame.variable.substr(0, frame.variable.find('['))].push_back(i);
        }
        if (!frame.nameDynamic && !frame.name.empty()) state->framesByName[LowerName(frame.name)].push_back(i);
    }
    Result<ScriptSimulator> result;
    if (!state->body.errors.empty()) {
        result.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_SIMULATION_PARSE",
            function + " 有 " + std::to_string(state->body.errors.size()) + " 处语法错误，模拟可能不完整"));
    }
    result.value = ScriptSimulator(std::move(state));
    return result;
}

SimulationResult ScriptSimulator::Run(const std::map<std::string, std::string>& values) const {
    return Execution(*state_, values).Run();
}

} // namespace War3Map::GameUi
