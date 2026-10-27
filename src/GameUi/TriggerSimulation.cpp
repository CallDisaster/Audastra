#include "War3MapCore/GameUi/TriggerSimulation.h"

#include "ScriptValue.h"
#include "UiDescriptionText.h"
#include "../Jass/JassParser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::GameUi {
namespace {

using Jass::detail::Expr;
using Jass::detail::ExprKind;
using Jass::detail::FunctionBody;
using Jass::detail::FunctionDecl;
using Jass::detail::Operator;
using Jass::detail::ProgramFile;
using Jass::detail::Stmt;
using Jass::detail::StmtKind;
using Jass::detail::SyntaxPool;
using Jass::detail::VarDecl;

constexpr const char* kSubsystem = "GameUi";
constexpr std::size_t kMaxSteps = 1000000;  // 一次执行（初始化时为一条语句）的表达式与语句总数
constexpr std::size_t kMaxDepth = 64;       // 调用深度
constexpr std::int32_t kMaxArrayIndex = 32767;

// JASS 实数是 32 位浮点：每个实数值都舍入到单精度，累加误差与比较结果和游戏一致。
double Single(double value) {
    if (std::isfinite(value) && std::fabs(value) > std::numeric_limits<float>::max())
        return std::copysign(std::numeric_limits<double>::infinity(), value);
    return static_cast<float>(value);
}

struct Value {
    enum class Kind : std::uint8_t { Null, Integer, Real, Boolean, String, Handle, Code } kind = Kind::Null;
    std::int32_t integer = 0;
    double real = 0.0;
    bool boolean = false;
    std::string text;          // String 内容、Code 的函数名
    std::uint32_t handle = 0;  // Handle 编号

    static Value Int(std::int32_t value) { Value v; v.kind = Kind::Integer; v.integer = value; return v; }
    static Value Float(double value) { Value v; v.kind = Kind::Real; v.real = Single(value); return v; }
    static Value Bool(bool value) { Value v; v.kind = Kind::Boolean; v.boolean = value; return v; }
    static Value Str(std::string value) { Value v; v.kind = Kind::String; v.text = std::move(value); return v; }
    static Value Object(std::uint32_t id) { Value v; v.kind = Kind::Handle; v.handle = id; return v; }
    static Value Function(std::string name) { Value v; v.kind = Kind::Code; v.text = std::move(name); return v; }
    [[nodiscard]] bool Numeric() const { return kind == Kind::Integer || kind == Kind::Real; }
    [[nodiscard]] double Number() const { return kind == Kind::Integer ? integer : real; }
};

// 与界面脚本分析共用的取值（数值、文字、布尔），用它的运算规则。
std::optional<GameUi::detail::ScriptValue> Shared(const Value& value) {
    using GameUi::detail::ScriptValue;
    switch (value.kind) {
    case Value::Kind::Integer: return ScriptValue::Number(value.integer, true);
    case Value::Kind::Real: return ScriptValue::Number(value.real);
    case Value::Kind::Boolean: return ScriptValue::Boolean(value.boolean);
    case Value::Kind::String: return ScriptValue::String(value.text);
    default: return std::nullopt;
    }
}

Value FromShared(const GameUi::detail::ScriptValue& value) {
    using Kind = GameUi::detail::ScriptValue::Kind;
    if (value.kind == Kind::Boolean) return Value::Bool(value.boolean);
    if (value.kind == Kind::String) return Value::Str(value.text);
    return value.integer ? Value::Int(static_cast<std::int32_t>(value.number)) : Value::Float(value.number);
}

struct Location {
    std::string file;
    std::uint32_t line = 0;
};

// 游戏自带的脚本（common.j、Blizzard.j），作者看不到。
bool LibraryScript(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
    std::string name(path);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name == "common.j" || name == "blizzard.j";
}

// 停下一次执行：带上调用位置的诊断。
struct Stop {
    Diagnostic diagnostic;
};

enum class BaseType : std::uint8_t { Integer, Real, Boolean, String, Code, Handle };

BaseType TypeOf(std::string_view type) {
    if (type == "integer") return BaseType::Integer;
    if (type == "real") return BaseType::Real;
    if (type == "boolean") return BaseType::Boolean;
    if (type == "string") return BaseType::String;
    if (type == "code") return BaseType::Code;
    return BaseType::Handle;
}

// 实数参数、变量收整数时按实数存。
Value Coerce(Value value, BaseType type) {
    if (type == BaseType::Real && value.kind == Value::Kind::Integer) return Value::Float(value.integer);
    return value;
}

// 数组元素没赋过值时的取值。
Value ArrayDefault(BaseType type) {
    switch (type) {
    case BaseType::Integer: return Value::Int(0);
    case BaseType::Real: return Value::Float(0.0);
    case BaseType::Boolean: return Value::Bool(false);
    default: return {};
    }
}

std::int32_t Wrap(std::int64_t value) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(value)); }

// 'A' 或 'A000'：按字节拼成整数。
std::optional<std::int32_t> RawcodeValue(std::string_view text) {
    if (text.size() < 3 || text.front() != '\'' || text.back() != '\'') return std::nullopt;
    text = text.substr(1, text.size() - 2);
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) ++i;
        value = value * 256u + static_cast<unsigned char>(text[i]);
    }
    return static_cast<std::int32_t>(value);
}

struct Variable {
    BaseType type = BaseType::Integer;
    bool array = false;
    std::optional<Value> value;
    std::map<std::int32_t, Value> elements;
};

struct FunctionEntry {
    std::size_t file = 0;
    const FunctionDecl* decl = nullptr;
};

struct SimulatedTrigger {
    bool enabled = true;
    std::vector<std::string> actions;     // 函数名
    std::vector<std::string> conditions;  // 函数名
    std::string variable;                 // 第一次赋给的全局量名
};

std::optional<std::string> TextOf(const Value& value) {
    switch (value.kind) {
    case Value::Kind::Integer: return std::to_string(value.integer);
    case Value::Kind::Real: {
        // 单精度的最短写法（0.6999999），不写它的双精度展开。
        const auto single = static_cast<float>(value.real);
        if (single == 0.0f || !std::isfinite(single)) return std::string("0");
        std::array<char, 32> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), single);
        if (error != std::errc{}) return detail::FormatUiNumber(value.real);
        return std::string(buffer.data(), end);
    }
    case Value::Kind::Boolean: return value.boolean ? "true" : "false";
    case Value::Kind::String: return value.text;
    default: return std::nullopt;
    }
}

std::string FormatReal(double value, int precision) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", std::clamp(precision, 0, 16), value);
    return buffer;
}

} // namespace

namespace detail {

struct TriggerSimulatorState {
    std::vector<MapScriptText> scripts;
    std::vector<ProgramFile> programs;
    std::unordered_map<std::string, FunctionEntry> functions;
    std::unordered_set<std::string> natives;
    std::unordered_map<std::string, FunctionBody> bodies;  // 首次调用时解析
    std::unordered_map<std::string, Variable> globals;
    std::unordered_set<std::string> intercepted;

    std::uint32_t nextHandle = 1;
    std::unordered_map<std::uint32_t, SimulatedTrigger> triggers;
    std::unordered_map<std::uint32_t, std::string> boolexprs;  // Condition/Filter 包的函数
    std::unordered_map<std::uint32_t, std::map<std::pair<std::int32_t, std::int32_t>, Value>> hashtables;
    std::map<std::pair<std::string, std::int32_t>, std::uint32_t> constants;  // Convert*(n) 的对象
    std::vector<std::pair<std::uint32_t, std::pair<std::string, std::string>>> subscriptions;
    std::mt19937 random{20261027u};

    const FunctionBody& Body(const std::string& name) {
        const auto found = bodies.find(name);
        if (found != bodies.end()) return found->second;
        const FunctionEntry& entry = functions.at(name);
        return bodies.emplace(name, Jass::detail::ParseFunctionBody(programs[entry.file].tokens, *entry.decl)).first->second;
    }
};

} // namespace detail

namespace {

using State = detail::TriggerSimulatorState;

// 一次执行（一条初始化语句、一个全局量初始值或一次到期）。
class Run {
public:
    Run(State& state, std::vector<Diagnostic>& diagnostics, TriggerRunResult& result)
        : state_(state), diagnostics_(diagnostics), result_(result) {}

    // 执行函数；返回值为空表示无返回值。
    std::optional<Value> Call(const std::string& name, std::vector<Value> arguments, const Location& where);

    // 逐条执行无参函数的语句；某条停下时记一条提示并接着执行下一条。
    void CallEachStatement(const std::string& name) {
        const auto function = state_.functions.find(name);
        if (function == state_.functions.end() || !function->second.decl->params.empty()) return;
        const FunctionBody& body = state_.Body(name);
        if (!body.errors.empty()) return;
        Enter(name, function->second.file, body, {}, {});
        const std::size_t depth = frames_.size();
        for (const std::int32_t statement : body.statements) {
            steps_ = 0;
            try {
                if (Execute(statement) == Flow::Return) break;
            } catch (const Stop& stop) {
                Diagnostic skipped = stop.diagnostic;
                skipped.severity = DiagnosticSeverity::Info;
                skipped.code = "AUI_SIM_INIT_SKIPPED";
                skipped.message = "初始化跳过一条语句：" + skipped.message;
                diagnostics_.push_back(std::move(skipped));
                frames_.resize(depth);
                current_ = 0;
            }
        }
        frames_.pop_back();
    }

    // 全局量初始值（在全局量的语法池里求值）。
    Value Constant(const SyntaxPool& pool, std::size_t file, std::int32_t index) {
        Frame frame;
        frame.file = file;
        frame.pool = &pool;
        frames_.push_back(std::move(frame));
        Value value = Eval(index);
        frames_.pop_back();
        return value;
    }

    void ExecuteTrigger(std::uint32_t handle, const Location& where) {
        const auto found = state_.triggers.find(handle);
        if (found == state_.triggers.end()) throw Stop{Error(where, "TRIGGER_UNKNOWN", "执行的不是触发器")};
        if (Intercepted(found->second)) {
            result_.intercepted.push_back(found->second.variable);
            return;
        }
        const std::uint32_t saved = std::exchange(current_, handle);
        const std::vector<std::string> actions = found->second.actions;
        for (const auto& action : actions) (void)Call(action, {}, where);
        current_ = saved;
        ++result_.triggers;
    }

    bool EvaluateTrigger(std::uint32_t handle, const Location& where) {
        const auto found = state_.triggers.find(handle);
        if (found == state_.triggers.end()) throw Stop{Error(where, "TRIGGER_UNKNOWN", "判断的不是触发器")};
        if (Intercepted(found->second)) return true;
        const std::uint32_t saved = std::exchange(current_, handle);
        const std::vector<std::string> conditions = found->second.conditions;
        bool passed = true;
        for (const auto& condition : conditions) {
            const auto value = Call(condition, {}, where);
            if (!value || value->kind != Value::Kind::Boolean || !value->boolean) {
                passed = false;
                break;
            }
        }
        current_ = saved;
        return passed;
    }

private:
    struct Frame {
        std::size_t file = 0;
        Location caller;  // 调用这个函数的位置
        const SyntaxPool* pool = nullptr;
        std::unordered_map<std::string, Variable> locals;
        std::optional<Value> returned;
    };
    enum class Flow : std::uint8_t { Next, Return, Exit };

    bool Intercepted(const SimulatedTrigger& trigger) const {
        return !trigger.variable.empty() && state_.intercepted.contains(trigger.variable);
    }

    // 停在游戏自带脚本里时，位置报作者脚本里最近的那次调用，库里的位置写在后面；执行触发器时写出是哪个触发器。
    Diagnostic Error(const Location& where, const char* code, const std::string& message) const {
        Location at = where;
        std::string inside;
        if (LibraryScript(where.file)) {
            for (auto frame = frames_.rbegin(); frame != frames_.rend(); ++frame) {
                if (frame->caller.file.empty() || LibraryScript(frame->caller.file)) continue;
                inside = "（在 " + where.file + ":" + std::to_string(where.line) + "）";
                at = frame->caller;
                break;
            }
        }
        std::string text = at.file.empty() ? std::string() : at.file + ":" + std::to_string(at.line) + "：";
        const auto trigger = current_ == 0 ? state_.triggers.end() : state_.triggers.find(current_);
        if (trigger != state_.triggers.end() && !trigger->second.variable.empty()) text += "触发器 " + trigger->second.variable + "：";
        return Diagnostic{DiagnosticSeverity::Error, kSubsystem, std::string("AUI_SIM_") + code, text + message + inside, std::nullopt};
    }

    Location Here(std::uint32_t line) const {
        return frames_.empty() ? Location{} : Location{state_.scripts[frames_.back().file].path, line};
    }

    const SyntaxPool& Pool() const { return *frames_.back().pool; }

    void Tick(std::uint32_t line) {
        if (++steps_ > kMaxSteps) {
            throw Stop{Error(Here(line), "STEPS", "一次执行超过 " + std::to_string(kMaxSteps) + " 步，可能是死循环")};
        }
    }

    void Enter(const std::string& name, std::size_t file, const FunctionBody& body, std::vector<Value> arguments, Location caller) {
        const FunctionDecl& decl = *state_.functions.at(name).decl;
        Frame frame;
        frame.file = file;
        frame.caller = std::move(caller);
        frame.pool = &body.pool;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            Variable parameter;
            parameter.type = TypeOf(decl.params[i].type);
            parameter.value = Coerce(std::move(arguments[i]), parameter.type);
            frame.locals[std::string(decl.params[i].name)] = std::move(parameter);
        }
        frames_.push_back(std::move(frame));
        for (const VarDecl& local : body.locals) {
            Variable variable;
            variable.type = TypeOf(local.type);
            variable.array = local.array;
            if (local.init >= 0) variable.value = Coerce(Eval(local.init), variable.type);
            frames_.back().locals[std::string(local.name)] = std::move(variable);
        }
    }

    Variable* Lookup(const std::string& name) {
        if (!frames_.empty()) {
            const auto local = frames_.back().locals.find(name);
            if (local != frames_.back().locals.end()) return &local->second;
        }
        const auto global = state_.globals.find(name);
        return global == state_.globals.end() ? nullptr : &global->second;
    }

    std::int32_t IndexOf(const Value& value, std::uint32_t line) {
        if (value.kind != Value::Kind::Integer) throw Stop{Error(Here(line), "INDEX", "数组下标不是整数")};
        if (value.integer < 0 || value.integer > kMaxArrayIndex) {
            throw Stop{Error(Here(line), "INDEX", "数组下标 " + std::to_string(value.integer) + " 越界")};
        }
        return value.integer;
    }

    std::string VariableName(const Expr& expr) const {
        return std::string(expr.kind == ExprKind::Index ? Pool().At(expr.left).text : expr.text);
    }

    Value Read(const Expr& expr) {
        const std::string name = VariableName(expr);
        Variable* variable = Lookup(name);
        if (!variable) throw Stop{Error(Here(expr.line), "NAME", "不认识的名字 " + name)};
        if (expr.kind == ExprKind::Index) {
            if (!variable->array) throw Stop{Error(Here(expr.line), "INDEX", name + " 不是数组")};
            const std::int32_t at = IndexOf(Eval(expr.right), expr.line);
            const auto found = variable->elements.find(at);
            return found == variable->elements.end() ? ArrayDefault(variable->type) : found->second;
        }
        if (variable->array) throw Stop{Error(Here(expr.line), "INDEX", name + " 是数组，要写下标")};
        if (!variable->value) throw Stop{Error(Here(expr.line), "UNSET", "变量 " + name + " 还没有值（或初始值在模拟里算不出）")};
        return *variable->value;
    }

    void Assign(std::int32_t target, Value value) {
        const Expr& expr = Pool().At(target);
        const std::string name = VariableName(expr);
        Variable* variable = Lookup(name);
        if (!variable) throw Stop{Error(Here(expr.line), "NAME", "不认识的变量 " + name)};
        value = Coerce(std::move(value), variable->type);
        // 记下触发器第一次赋给的全局量，用来认出要拦截的触发器。
        if (value.kind == Value::Kind::Handle && state_.globals.contains(name)) {
            const auto trigger = state_.triggers.find(value.handle);
            if (trigger != state_.triggers.end() && trigger->second.variable.empty()) trigger->second.variable = name;
        }
        if (expr.kind == ExprKind::Index) {
            if (!variable->array) throw Stop{Error(Here(expr.line), "INDEX", name + " 不是数组")};
            variable->elements[IndexOf(Eval(expr.right), expr.line)] = std::move(value);
        } else {
            if (variable->array) throw Stop{Error(Here(expr.line), "INDEX", name + " 是数组，要写下标")};
            variable->value = std::move(value);
        }
    }

    Value Literal(const Expr& expr) {
        if (expr.kind == ExprKind::Null) return {};
        if (expr.kind == ExprKind::Rawcode) {
            if (const auto value = RawcodeValue(expr.text)) return Value::Int(*value);
            throw Stop{Error(Here(expr.line), "LITERAL", "读不出四字码 " + std::string(expr.text))};
        }
        const auto literal = GameUi::detail::LiteralValue(expr);
        if (!literal) throw Stop{Error(Here(expr.line), "LITERAL", "读不出字面量 " + std::string(expr.text))};
        return FromShared(*literal);
    }

    bool Truth(const Value& value, std::uint32_t line) {
        if (value.kind != Value::Kind::Boolean) throw Stop{Error(Here(line), "TYPE", "条件不是布尔值")};
        return value.boolean;
    }

    static bool Equal(const Value& left, const Value& right) {
        if (left.Numeric() && right.Numeric()) return left.Number() == right.Number();
        const auto none = [](const Value& v) { return v.kind == Value::Kind::Null || (v.kind == Value::Kind::Handle && v.handle == 0); };
        if (none(left) || none(right)) return none(left) && none(right);
        if (left.kind != right.kind) return false;
        switch (left.kind) {
        case Value::Kind::Boolean: return left.boolean == right.boolean;
        case Value::Kind::String:
        case Value::Kind::Code: return left.text == right.text;
        case Value::Kind::Handle: return left.handle == right.handle;
        default: return false;
        }
    }

    Value Binary(const Expr& expr) {
        if (expr.op == Operator::And || expr.op == Operator::Or) {
            const bool left = Truth(Eval(expr.left), expr.line);
            if (expr.op == Operator::And ? !left : left) return Value::Bool(left);
            return Value::Bool(Truth(Eval(expr.right), expr.line));
        }
        const Value left = Eval(expr.left);
        const Value right = Eval(expr.right);
        if (expr.op == Operator::Equal) return Value::Bool(Equal(left, right));
        if (expr.op == Operator::NotEqual) return Value::Bool(!Equal(left, right));
        const auto textual = [](const Value& v) { return v.kind == Value::Kind::String || v.kind == Value::Kind::Null; };
        if (expr.op == Operator::Plus && textual(left) && textual(right) &&
            (left.kind == Value::Kind::String || right.kind == Value::Kind::String)) {
            return Value::Str(left.text + right.text);  // null 字符串按空串相加
        }
        if (expr.op == Operator::Divide && right.Numeric() && right.Number() == 0.0) {
            throw Stop{Error(Here(expr.line), "DIVIDE", "除以 0：" + std::string(expr.text))};
        }
        const auto a = Shared(left);
        const auto b = Shared(right);
        if (a && b) {
            if (auto value = GameUi::detail::ApplyBinary(expr.op, *a, *b)) return FromShared(*value);
        }
        throw Stop{Error(Here(expr.line), "TYPE", "运算两边的类型不能这样算：" + std::string(expr.text))};
    }

    // 调用表达式；事件注册不计算参数（注册本身不做任何事，参数多半是模拟里算不出的游戏对象）。
    std::optional<Value> CallExpression(const Expr& call) {
        const std::string name(call.text);
        if (name.starts_with("TriggerRegister") && !state_.functions.contains(name) && state_.natives.contains(name)) {
            return Value::Object(state_.nextHandle++);
        }
        std::vector<Value> arguments;
        arguments.reserve(call.argumentCount);
        for (std::uint32_t i = 0; i < call.argumentCount; ++i) arguments.push_back(Eval(Pool().Argument(call, i)));
        return Call(name, std::move(arguments), Here(call.line));
    }

    Value Eval(std::int32_t index) {
        const Expr& expr = Pool().At(index);
        Tick(expr.line);
        switch (expr.kind) {
        case ExprKind::Integer:
        case ExprKind::Rawcode:
        case ExprKind::Real:
        case ExprKind::String:
        case ExprKind::Boolean:
        case ExprKind::Null: return Literal(expr);
        case ExprKind::Name:
        case ExprKind::Index: return Read(expr);
        case ExprKind::Paren: return Eval(expr.left);
        case ExprKind::FunctionRef: {
            const std::string name(expr.text);
            if (!state_.functions.contains(name)) throw Stop{Error(Here(expr.line), "NAME", "没有函数 " + name)};
            return Value::Function(name);
        }
        case ExprKind::Unary: {
            const auto operand = Shared(Eval(expr.left));
            if (operand) {
                if (auto value = GameUi::detail::ApplyUnary(expr.op, *operand)) return FromShared(*value);
            }
            throw Stop{Error(Here(expr.line), "TYPE", "运算对象的类型不对：" + std::string(expr.text))};
        }
        case ExprKind::Binary: return Binary(expr);
        case ExprKind::Call: {
            auto value = CallExpression(expr);
            if (!value) throw Stop{Error(Here(expr.line), "VOID", std::string(expr.text) + " 没有返回值")};
            return std::move(*value);
        }
        default: throw Stop{Error(Here(expr.line), "SYNTAX", "不支持的写法 " + std::string(expr.text))};
        }
    }

    Flow Block(const std::vector<std::int32_t>& statements) {
        for (const std::int32_t statement : statements) {
            const Flow flow = Execute(statement);
            if (flow != Flow::Next) return flow;
        }
        return Flow::Next;
    }

    Flow Execute(std::int32_t index) {
        const Stmt& stmt = Pool().StmtAt(index);
        Tick(stmt.line);
        if (stmt.debug) return Flow::Next;  // debug 语句只在调试模式执行
        switch (stmt.kind) {
        case StmtKind::Set: Assign(stmt.target, Eval(stmt.value)); return Flow::Next;
        case StmtKind::Call: (void)CallExpression(Pool().At(stmt.value)); return Flow::Next;
        case StmtKind::If:
            for (const auto& branch : stmt.branches) {
                if (branch.condition >= 0 && !Truth(Eval(branch.condition), branch.line)) continue;
                return Block(branch.body);
            }
            return Flow::Next;
        case StmtKind::Loop:
            for (;;) {
                const Flow flow = Block(stmt.body);
                if (flow == Flow::Return) return flow;
                if (flow == Flow::Exit) return Flow::Next;
            }
        case StmtKind::ExitWhen: return Truth(Eval(stmt.value), stmt.line) ? Flow::Exit : Flow::Next;
        case StmtKind::Return:
            if (stmt.value >= 0) frames_.back().returned = Eval(stmt.value);
            return Flow::Return;
        default: throw Stop{Error(Here(stmt.line), "SYNTAX", "有语法错误的语句")};
        }
    }

    std::optional<Value> Native(const std::string& name, std::vector<Value>& a, const Location& where);

    State& state_;
    std::vector<Diagnostic>& diagnostics_;
    TriggerRunResult& result_;
    std::vector<Frame> frames_;
    std::size_t steps_ = 0;
    std::uint32_t current_ = 0;  // 当前触发器
};

std::optional<Value> Run::Call(const std::string& name, std::vector<Value> arguments, const Location& where) {
    if (name == kSimulationTimerEvent) {
        // 界面模拟计时器到期：记下订阅；函数本身在游戏里是空的。
        if (arguments.size() != 3 || arguments[0].kind != Value::Kind::Handle || arguments[1].kind != Value::Kind::String ||
            arguments[2].kind != Value::Kind::String) {
            throw Stop{Error(where, "ARGUMENT", "模拟计时器事件的参数应为 触发器、界面编号、计时器编号")};
        }
        state_.subscriptions.push_back({arguments[0].handle, {arguments[1].text, arguments[2].text}});
        return std::nullopt;
    }
    const auto function = state_.functions.find(name);
    if (function == state_.functions.end()) {
        if (!state_.natives.contains(name)) throw Stop{Error(where, "NAME", "没有函数 " + name)};
        return Native(name, arguments, where);
    }
    if (frames_.size() >= kMaxDepth) throw Stop{Error(where, "DEPTH", "调用超过 " + std::to_string(kMaxDepth) + " 层")};
    const FunctionDecl& decl = *function->second.decl;
    if (arguments.size() != decl.params.size()) throw Stop{Error(where, "ARGUMENT", name + " 参数个数不符")};
    const FunctionBody& body = state_.Body(name);
    if (!body.errors.empty()) throw Stop{Error(where, "SYNTAX", name + " 有语法错误")};
    Enter(name, function->second.file, body, std::move(arguments), where);
    (void)Block(body.statements);
    std::optional<Value> returned = std::move(frames_.back().returned);
    frames_.pop_back();
    if (returned) returned = Coerce(std::move(*returned), TypeOf(decl.returns));
    return returned;
}

// 纯计算的原生函数、哈希表与触发器；Convert* 得到按参数区分的常量对象。其余一律停下。
std::optional<Value> Run::Native(const std::string& name, std::vector<Value>& a, const Location& where) {
    const auto need = [&](std::size_t count) {
        if (a.size() != count) throw Stop{Error(where, "ARGUMENT", name + " 参数个数不符")};
    };
    const auto argument = [&](std::size_t i) { return name + " 的第 " + std::to_string(i + 1) + " 个参数"; };
    const auto number = [&](std::size_t i) {
        if (!a[i].Numeric()) throw Stop{Error(where, "TYPE", argument(i) + "不是数值")};
        return a[i].Number();
    };
    const auto integer = [&](std::size_t i) {
        if (a[i].kind != Value::Kind::Integer) throw Stop{Error(where, "TYPE", argument(i) + "不是整数")};
        return a[i].integer;
    };
    const auto text = [&](std::size_t i) -> std::string {
        if (a[i].kind == Value::Kind::Null) return {};
        if (a[i].kind != Value::Kind::String) throw Stop{Error(where, "TYPE", argument(i) + "不是文字")};
        return a[i].text;
    };
    const auto boolean = [&](std::size_t i) {
        if (a[i].kind != Value::Kind::Boolean) throw Stop{Error(where, "TYPE", argument(i) + "不是布尔值")};
        return a[i].boolean;
    };
    const auto handle = [&](std::size_t i) {
        if (a[i].kind == Value::Kind::Null) return 0u;
        if (a[i].kind != Value::Kind::Handle) throw Stop{Error(where, "TYPE", argument(i) + "不是对象")};
        return a[i].handle;
    };
    const auto trigger = [&](std::size_t i) -> SimulatedTrigger& {
        const auto found = state_.triggers.find(handle(i));
        if (found == state_.triggers.end()) throw Stop{Error(where, "TRIGGER_UNKNOWN", argument(i) + "不是触发器")};
        return found->second;
    };
    const auto table = [&]() -> std::map<std::pair<std::int32_t, std::int32_t>, Value>& {
        const auto found = state_.hashtables.find(handle(0));
        if (found == state_.hashtables.end()) throw Stop{Error(where, "HASHTABLE", argument(0) + "不是哈希表")};
        return found->second;
    };
    const auto real = [](double value) { return Value::Float(value); };
    constexpr double kPi = 3.14159265358979323846;

    if (name.starts_with("TriggerRegister")) return Value::Object(state_.nextHandle++);
    if (name.starts_with("Convert") && a.size() == 1 && a[0].kind == Value::Kind::Integer) {
        const auto [entry, added] = state_.constants.try_emplace({name, a[0].integer}, state_.nextHandle);
        if (added) ++state_.nextHandle;
        return Value::Object(entry->second);
    }
    if (name == "I2R") { need(1); return real(integer(0)); }
    if (name == "R2I") {
        need(1);
        const double value = number(0);
        if (!std::isfinite(value)) return Value::Int(0);
        return Value::Int(static_cast<std::int32_t>(std::clamp(value, -2147483648.0, 2147483647.0)));
    }
    if (name == "I2S") { need(1); return Value::Str(std::to_string(integer(0))); }
    if (name == "R2S") { need(1); return Value::Str(FormatReal(number(0), 3)); }
    if (name == "R2SW") { need(3); return Value::Str(FormatReal(number(0), integer(2))); }
    if (name == "S2I") { need(1); return Value::Int(Wrap(std::strtoll(text(0).c_str(), nullptr, 10))); }
    if (name == "S2R") { need(1); return real(std::strtod(text(0).c_str(), nullptr)); }
    if (name == "StringLength") { need(1); return Value::Int(static_cast<std::int32_t>(text(0).size())); }
    if (name == "SubString") {
        need(3);
        const std::string s = text(0);
        const auto size = static_cast<std::int32_t>(s.size());
        const std::int32_t begin = std::clamp(integer(1), 0, size);
        const std::int32_t end = std::clamp(integer(2), begin, size);
        return Value::Str(s.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin)));
    }
    if (name == "StringCase") {
        need(2);
        std::string s = text(0);
        const bool upper = boolean(1);
        for (char& c : s) {
            const auto byte = static_cast<unsigned char>(c);
            c = static_cast<char>(upper ? std::toupper(byte) : std::tolower(byte));
        }
        return Value::Str(std::move(s));
    }
    if (name == "StringHash") {
        // 只保证模拟内部一致（作哈希表键用），不等于游戏里的值。
        need(1);
        std::uint32_t hash = 2166136261u;
        for (const char c : text(0)) hash = (hash ^ static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(c)))) * 16777619u;
        return Value::Int(static_cast<std::int32_t>(hash));
    }
    if (name == "SquareRoot") { need(1); return real(std::sqrt(std::max(0.0, number(0)))); }
    if (name == "Pow") { need(2); return real(std::pow(number(0), number(1))); }
    if (name == "Sin") { need(1); return real(std::sin(number(0))); }
    if (name == "Cos") { need(1); return real(std::cos(number(0))); }
    if (name == "Tan") { need(1); return real(std::tan(number(0))); }
    if (name == "Asin") { need(1); return real(std::asin(std::clamp(number(0), -1.0, 1.0))); }
    if (name == "Acos") { need(1); return real(std::acos(std::clamp(number(0), -1.0, 1.0))); }
    if (name == "Atan") { need(1); return real(std::atan(number(0))); }
    if (name == "Atan2") { need(2); return real(std::atan2(number(0), number(1))); }
    if (name == "Deg2Rad") { need(1); return real(number(0) * kPi / 180.0); }
    if (name == "Rad2Deg") { need(1); return real(number(0) * 180.0 / kPi); }
    if (name == "GetRandomInt") {
        need(2);
        const auto [low, high] = std::minmax(integer(0), integer(1));
        return Value::Int(std::uniform_int_distribution<std::int32_t>(low, high)(state_.random));
    }
    if (name == "GetRandomReal") {
        need(2);
        const auto [low, high] = std::minmax(number(0), number(1));
        return real(low == high ? low : std::uniform_real_distribution<double>(low, high)(state_.random));
    }

    if (name == "InitHashtable") {
        need(0);
        const std::uint32_t id = state_.nextHandle++;
        state_.hashtables[id];
        return Value::Object(id);
    }
    // 哈希表的整数、实数、布尔与文字项：Save/Load 用 Str，HaveSaved/RemoveSaved 用 String。
    const auto key = [&] { return std::pair{integer(1), integer(2)}; };
    const auto zero = [](Value::Kind kind) {
        return kind == Value::Kind::Integer ? Value::Int(0) : kind == Value::Kind::Real ? Value::Float(0.0)
            : kind == Value::Kind::Boolean ? Value::Bool(false) : Value{};
    };
    struct Entry {
        std::string_view suffix;
        Value::Kind kind;
    };
    static constexpr Entry kEntries[] = {{"Integer", Value::Kind::Integer}, {"Real", Value::Kind::Real},
        {"Boolean", Value::Kind::Boolean}, {"Str", Value::Kind::String}, {"String", Value::Kind::String}};
    for (const auto& [suffix, kind] : kEntries) {
        const bool shortName = suffix != "String";
        if (shortName && name == "Save" + std::string(suffix)) {
            need(4);
            const Value stored = kind == Value::Kind::Real ? Coerce(a[3], BaseType::Real) : a[3];
            if (stored.kind != kind && !(kind == Value::Kind::String && stored.kind == Value::Kind::Null)) {
                throw Stop{Error(where, "TYPE", argument(3) + "类型不符")};
            }
            table()[key()] = stored;
            return kind == Value::Kind::String ? std::optional<Value>(Value::Bool(true)) : std::nullopt;
        }
        if (shortName && name == "Load" + std::string(suffix)) {
            need(3);
            const auto& entries = table();
            const auto found = entries.find(key());
            return found != entries.end() && found->second.kind == kind ? found->second : zero(kind);
        }
        if (suffix != "Str" && name == "HaveSaved" + std::string(suffix)) {
            need(3);
            const auto& entries = table();
            const auto found = entries.find(key());
            return Value::Bool(found != entries.end() && found->second.kind == kind);
        }
        if (suffix != "Str" && name == "RemoveSaved" + std::string(suffix)) {
            need(3);
            auto& entries = table();
            if (const auto found = entries.find(key()); found != entries.end() && found->second.kind == kind) entries.erase(found);
            return std::nullopt;
        }
    }
    if (name == "FlushChildHashtable") {
        need(2);
        const std::int32_t parent = integer(1);
        std::erase_if(table(), [&](const auto& entry) { return entry.first.first == parent; });
        return std::nullopt;
    }
    if (name == "FlushParentHashtable") { need(1); table().clear(); return std::nullopt; }

    if (name == "CreateTrigger") {
        need(0);
        const std::uint32_t id = state_.nextHandle++;
        state_.triggers[id];
        return Value::Object(id);
    }
    if (name == "DestroyTrigger") { need(1); state_.triggers.erase(handle(0)); return std::nullopt; }
    if (name == "EnableTrigger" || name == "DisableTrigger") { need(1); trigger(0).enabled = name == "EnableTrigger"; return std::nullopt; }
    if (name == "IsTriggerEnabled") { need(1); return Value::Bool(trigger(0).enabled); }
    if (name == "TriggerAddAction") {
        need(2);
        if (a[1].kind != Value::Kind::Code) throw Stop{Error(where, "TYPE", argument(1) + "不是函数")};
        trigger(0).actions.push_back(a[1].text);
        return Value::Object(state_.nextHandle++);
    }
    if (name == "Condition" || name == "Filter") {
        need(1);
        if (a[0].kind != Value::Kind::Code) throw Stop{Error(where, "TYPE", argument(0) + "不是函数")};
        const std::uint32_t id = state_.nextHandle++;
        state_.boolexprs[id] = a[0].text;
        return Value::Object(id);
    }
    if (name == "TriggerAddCondition") {
        need(2);
        const auto found = state_.boolexprs.find(handle(1));
        if (found == state_.boolexprs.end()) throw Stop{Error(where, "TYPE", argument(1) + "不是 Condition(函数)")};
        trigger(0).conditions.push_back(found->second);
        return Value::Object(state_.nextHandle++);
    }
    if (name == "TriggerClearActions") { need(1); trigger(0).actions.clear(); return std::nullopt; }
    if (name == "TriggerClearConditions") { need(1); trigger(0).conditions.clear(); return std::nullopt; }
    if (name == "TriggerEvaluate") { need(1); (void)trigger(0); return Value::Bool(EvaluateTrigger(handle(0), where)); }
    if (name == "TriggerExecute") { need(1); (void)trigger(0); ExecuteTrigger(handle(0), where); return std::nullopt; }
    if (name == "GetTriggeringTrigger") { need(0); return current_ ? Value::Object(current_) : Value{}; }
    if (name == "GetHandleId") { need(1); return Value::Int(static_cast<std::int32_t>(handle(0))); }
    if (name == "ExecuteFunc") {
        need(1);
        const std::string target = text(0);
        if (!state_.functions.contains(target)) throw Stop{Error(where, "NAME", "ExecuteFunc 的函数 " + target + " 不存在")};
        (void)Call(target, {}, where);
        return std::nullopt;
    }
    throw Stop{Error(where, "UNSUPPORTED", "模拟里不能执行游戏函数 " + name)};
}

} // namespace

TriggerSimulator::TriggerSimulator(std::unique_ptr<detail::TriggerSimulatorState> state) : state_(std::move(state)) {}
TriggerSimulator::TriggerSimulator(TriggerSimulator&&) noexcept = default;
TriggerSimulator& TriggerSimulator::operator=(TriggerSimulator&&) noexcept = default;
TriggerSimulator::~TriggerSimulator() = default;

Result<TriggerSimulator> TriggerSimulator::Prepare(std::vector<MapScriptText> scripts) {
    auto state = std::make_unique<State>();
    state->scripts = std::move(scripts);
    std::erase_if(state->scripts, [](const MapScriptText& script) { return script.language != ScriptLanguage::Jass; });
    Result<TriggerSimulator> result;
    state->programs.reserve(state->scripts.size());
    for (std::size_t i = 0; i < state->scripts.size(); ++i) {
        state->programs.push_back(Jass::detail::ParseProgram(state->scripts[i].text));
        const ProgramFile& program = state->programs.back();
        for (const auto& error : program.errors) {
            result.diagnostics.push_back(Diagnostic{DiagnosticSeverity::Error, kSubsystem, "AUI_SIM_SYNTAX",
                state->scripts[i].path + ":" + std::to_string(error.line) + "：" + error.message, std::nullopt});
        }
        for (const auto& native : program.natives) state->natives.insert(std::string(native.name));
        for (const auto& function : program.functions) state->functions[std::string(function.name)] = FunctionEntry{i, &function};
    }
    if (!result.diagnostics.empty()) return result;
    result.value = TriggerSimulator(std::move(state));
    return result;
}

void TriggerSimulator::Intercept(std::vector<std::string> triggers) {
    state_->intercepted = std::unordered_set<std::string>(triggers.begin(), triggers.end());
}

TriggerRunResult TriggerSimulator::Initialize() {
    TriggerRunResult result;
    State& state = *state_;
    // 全局量按文件与声明顺序建立；初始值在模拟里算不出时留空，用到时再报。
    for (const ProgramFile& program : state.programs) {
        for (const VarDecl& global : program.globals) {
            Variable variable;
            variable.type = TypeOf(global.type);
            variable.array = global.array;
            state.globals[std::string(global.name)] = std::move(variable);
        }
    }
    for (std::size_t i = 0; i < state.programs.size(); ++i) {
        const ProgramFile& program = state.programs[i];
        for (const VarDecl& global : program.globals) {
            if (global.init < 0 || global.array) continue;
            std::vector<Diagnostic> ignored;
            TriggerRunResult unused;
            Run run(state, ignored, unused);
            try {
                state.globals[std::string(global.name)].value =
                    Coerce(run.Constant(program.globalPool, i, global.init), TypeOf(global.type));
            } catch (const Stop&) {
            }
        }
    }
    for (const char* entry : {"InitGlobals", "InitCustomTriggers"}) {
        Run run(state, result.diagnostics, result);
        run.CallEachStatement(entry);
    }
    return result;
}

TriggerRunResult TriggerSimulator::Fire(std::string_view description, std::string_view timer) {
    TriggerRunResult result;
    State& state = *state_;
    const auto subscriptions = state.subscriptions;
    // 每个触发器单独执行：一个停下不影响其他（与游戏里各自的线程一致）。
    for (const auto& [handle, key] : subscriptions) {
        if (key.first != description || key.second != timer) continue;
        const auto trigger = state.triggers.find(handle);
        if (trigger == state.triggers.end() || !trigger->second.enabled) continue;
        Run run(state, result.diagnostics, result);
        try {
            if (run.EvaluateTrigger(handle, {})) run.ExecuteTrigger(handle, {});
        } catch (const Stop& stop) {
            result.diagnostics.push_back(stop.diagnostic);
            result.completed = false;
        }
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> TriggerSimulator::Subscriptions() const {
    std::vector<std::pair<std::string, std::string>> list;
    list.reserve(state_->subscriptions.size());
    for (const auto& subscription : state_->subscriptions) list.push_back(subscription.second);
    return list;
}

std::optional<std::string> TriggerSimulator::Global(std::string_view name, std::optional<std::uint32_t> index) const {
    const auto found = state_->globals.find(std::string(name));
    if (found == state_->globals.end()) return std::nullopt;
    const Variable& variable = found->second;
    if (variable.array != index.has_value()) return std::nullopt;
    if (!variable.array) return variable.value ? TextOf(*variable.value) : std::nullopt;
    const auto element = variable.elements.find(static_cast<std::int32_t>(*index));
    return TextOf(element == variable.elements.end() ? ArrayDefault(variable.type) : element->second);
}

bool TriggerSimulator::SetGlobal(std::string_view name, std::optional<std::uint32_t> index, std::string_view text) {
    const auto found = state_->globals.find(std::string(name));
    if (found == state_->globals.end()) return false;
    Variable& variable = found->second;
    if (variable.array != index.has_value() || (index && *index > static_cast<std::uint32_t>(kMaxArrayIndex))) return false;
    Value value;
    switch (variable.type) {
    case BaseType::Integer: {
        std::int32_t parsed = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (error != std::errc{} || end != text.data() + text.size()) return false;
        value = Value::Int(parsed);
        break;
    }
    case BaseType::Real: {
        double parsed = 0.0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (error != std::errc{} || end != text.data() + text.size()) return false;
        value = Value::Float(parsed);
        break;
    }
    case BaseType::Boolean:
        if (text != "true" && text != "false") return false;
        value = Value::Bool(text == "true");
        break;
    case BaseType::String: value = Value::Str(std::string(text)); break;
    default: return false;
    }
    if (index) variable.elements[static_cast<std::int32_t>(*index)] = std::move(value);
    else variable.value = std::move(value);
    return true;
}

} // namespace War3Map::GameUi
