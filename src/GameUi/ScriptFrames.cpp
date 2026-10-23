#include "War3MapCore/GameUi/ScriptFrames.h"

#include "War3MapCore/GameUi/GameCodeLayout.h"
#include "War3MapCore/Text/TextUtil.h"
#include "ScriptUiApi.h"
#include "ScriptValue.h"
#include "../Jass/JassLexer.h"
#include "../Jass/JassParser.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::GameUi {
namespace {

using Jass::detail::Expr;
using Jass::detail::ExprKind;
using Jass::detail::Operator;
using Jass::detail::Stmt;
using Jass::detail::StmtKind;
using Jass::detail::SyntaxPool;
using Jass::detail::VarDecl;
using detail::CallKind;
using detail::CallSpec;
using detail::IsSetter;
using detail::LookupCall;
using detail::NumberOf;
using detail::ScriptValue;
using detail::StringOf;

constexpr const char* kSubsystem = "GameUi";
constexpr std::size_t kMaxFrames = 2000;
constexpr std::size_t kMaxSamples = 5;
constexpr std::size_t kMaxRuntimeFunctions = 50;

Diagnostic MakeDiagnostic(DiagnosticSeverity severity, const char* code, std::string message) {
    return Diagnostic{severity, kSubsystem, code, std::move(message), std::nullopt};
}

// 一份脚本函数的分析上下文。
struct Scope {
    const SyntaxPool& pool;
    std::string_view source;
    const std::string& scriptFile;
    std::string_view function;
    std::unordered_set<std::string_view> locals;
};

// 语句块内已保存框架的变量 → 框架下标。
using BlockFrames = std::unordered_map<std::string, std::uint32_t>;

class Analyzer {
public:
    explicit Analyzer(ScriptFramePlan& plan) : plan_(plan) {}

    void Script(const MapScriptText& script);
    void Finish();

private:
    struct Pending {
        std::string key;  // 变量基名（局部变量带函数名）或 name:框架名；为空表示无法追踪
        ScriptCallSite site;
        UiApiFamily family = UiApiFamily::Blizzard;
    };

    const Expr& Unwrap(const Scope& scope, std::int32_t index) const;
    std::optional<ScriptValue> Eval(const Scope& scope, std::int32_t index) const;
    std::string VarKey(const Scope& scope, std::int32_t index) const;
    std::string BaseKey(const Scope& scope, std::int32_t index) const;
    ScriptFrameRef Ref(const Scope& scope, std::int32_t index, const BlockFrames& block) const;
    ScriptCallSite Site(const Scope& scope, const Expr& call) const;

    void Block(const Scope& scope, const std::vector<std::int32_t>& statements, const std::vector<VarDecl>* locals);
    void Statement(const Scope& scope, std::int32_t index, BlockFrames& block);
    void Call(const Scope& scope, std::int32_t index, BlockFrames& block);
    void Nested(const Scope& scope, std::int32_t index, BlockFrames& block);
    std::optional<std::uint32_t> Create(const Scope& scope, const CallSpec& spec, std::int32_t index,
        const std::string& key, BlockFrames& block);
    void Apply(const Scope& scope, const CallSpec& spec, std::int32_t index, BlockFrames& block);
    bool ApplyTo(const Scope& scope, ScriptFrame& frame, const CallSpec& spec, const Expr& call, const BlockFrames& block);
    void Group(const std::string& function, UiApiFamily family, ScriptUiCallReason reason, ScriptCallSite site);

    ScriptFramePlan& plan_;
    std::unordered_map<std::string, ScriptValue> constants_;
    std::unordered_map<std::string, std::uint32_t> keyFrames_;               // 变量原文 → 最近一次保存的框架
    std::unordered_map<std::string, std::vector<std::uint32_t>> baseFrames_;  // 变量基名或 name:名字 → 框架
    std::unordered_map<std::string, std::size_t> groups_;
    std::vector<Pending> pending_;
    bool limitReported_ = false;
};

const Expr& Analyzer::Unwrap(const Scope& scope, std::int32_t index) const {
    return detail::Unparenthesized(scope.pool, index);
}

std::optional<ScriptValue> Analyzer::Eval(const Scope& scope, std::int32_t index) const {
    // 局部变量在静态分析里没有确定的值；全局只取常量。
    return detail::FoldExpression(scope.pool, index, [&](std::string_view name) -> std::optional<ScriptValue> {
        if (scope.locals.contains(name)) return std::nullopt;
        if (auto constant = detail::FrameConstant(name)) return constant;
        const auto found = constants_.find(std::string(name));
        return found == constants_.end() ? std::nullopt : std::optional<ScriptValue>(found->second);
    });
}

std::string Analyzer::VarKey(const Scope& scope, std::int32_t index) const {
    const Expr& expr = Unwrap(scope, index);
    std::string_view base;
    if (expr.kind == ExprKind::Name) {
        base = expr.text;
    } else if (expr.kind == ExprKind::Index && expr.left >= 0 && scope.pool.At(expr.left).kind == ExprKind::Name) {
        base = scope.pool.At(expr.left).text;
    } else {
        return {};
    }
    std::string key = detail::CompactSource(scope.source.substr(expr.begin, expr.end - expr.begin));
    if (scope.locals.contains(base)) key = std::string(scope.function) + "::" + key;
    return key;
}

std::string Analyzer::BaseKey(const Scope& scope, std::int32_t index) const {
    const Expr& expr = Unwrap(scope, index);
    if (expr.kind == ExprKind::Call && expr.left < 0) {
        // 按名字取到的框架：与同名脚本框架对应。
        if (const auto spec = LookupCall(expr.text); spec && spec->kind == CallKind::ByName && expr.argumentCount >= 1) {
            if (const auto name = StringOf(Eval(scope, scope.pool.Argument(expr, 0)))) return "name:" + Text::LowerAscii(*name);
        }
        return {};
    }
    std::string_view base;
    if (expr.kind == ExprKind::Name) {
        base = expr.text;
    } else if (expr.kind == ExprKind::Index && expr.left >= 0 && scope.pool.At(expr.left).kind == ExprKind::Name) {
        base = scope.pool.At(expr.left).text;
    } else {
        return {};
    }
    return scope.locals.contains(base) ? std::string(scope.function) + "::" + std::string(base) : std::string(base);
}

ScriptFrameRef Analyzer::Ref(const Scope& scope, std::int32_t index, const BlockFrames& block) const {
    ScriptFrameRef ref;
    if (index < 0) return ref;
    const Expr& expr = Unwrap(scope, index);
    if (expr.kind == ExprKind::Call && expr.left < 0) {
        const auto spec = LookupCall(expr.text);
        if (!spec) return ref;
        std::vector<std::optional<ScriptValue>> args;
        for (std::uint32_t i = 0; i < expr.argumentCount; ++i) args.push_back(Eval(scope, scope.pool.Argument(expr, i)));
        auto found = detail::FrameRefOfCall(*spec, args);
        if (!found) return ref;
        ref = std::move(*found);
        // 按名字取到的若是脚本创建的同名框架，指向它。
        if (ref.target == ScriptFrameTarget::Named) {
            const auto named = baseFrames_.find("name:" + Text::LowerAscii(ref.name));
            if (named != baseFrames_.end() && !named->second.empty()) {
                ref.target = ScriptFrameTarget::Script;
                ref.frame = named->second.back();
            }
        }
        return ref;
    }
    const std::string key = VarKey(scope, index);
    if (key.empty()) return ref;
    std::optional<std::uint32_t> frame;
    if (const auto found = block.find(key); found != block.end()) frame = found->second;
    else if (const auto global = keyFrames_.find(key); global != keyFrames_.end()) frame = global->second;
    if (frame) {
        ref.target = ScriptFrameTarget::Script;
        ref.frame = *frame;
        ref.name = plan_.frames[*frame].name;
    }
    return ref;
}

ScriptCallSite Analyzer::Site(const Scope& scope, const Expr& call) const {
    return ScriptCallSite{scope.scriptFile, call.line, std::string(scope.function), std::string(call.text)};
}

void Analyzer::Group(const std::string& function, UiApiFamily family, ScriptUiCallReason reason, ScriptCallSite site) {
    const std::string key = function + '#' + std::to_string(static_cast<int>(reason));
    auto [found, inserted] = groups_.emplace(key, plan_.calls.size());
    if (inserted) plan_.calls.push_back(ScriptUiCallGroup{function, family, reason, 0, {}});
    auto& group = plan_.calls[found->second];
    ++group.count;
    if (group.samples.size() < kMaxSamples) group.samples.push_back(std::move(site));
}

void Analyzer::Script(const MapScriptText& script) {
    const auto program = Jass::detail::ParseProgram(script.text);
    std::size_t errors = program.errors.size();

    // 全局常量（只取常量，普通全局变量可能在运行时被改写）。
    const Scope globalScope{program.globalPool, script.text, script.path, {}, {}};
    for (const auto& global : program.globals) {
        if (!global.constant || global.array || global.init < 0) continue;
        if (auto value = Eval(globalScope, global.init)) constants_.insert_or_assign(std::string(global.name), std::move(*value));
    }

    for (const auto& function : program.functions) {
        // 界面接口自身的封装函数（如地图里声明的同名包装）不是使用方，跳过。
        if (LookupCall(function.name)) continue;
        bool relevant = false;
        for (std::size_t i = function.bodyBegin; i < function.bodyEnd && !relevant; ++i) {
            const auto& token = program.tokens[i];
            relevant = token.kind == Jass::detail::TokenKind::Identifier && LookupCall(token.text).has_value();
        }
        if (!relevant) continue;
        const auto body = Jass::detail::ParseFunctionBody(program.tokens, function);
        errors += body.errors.size();
        Scope scope{body.pool, script.text, script.path, function.name, {}};
        for (const auto& param : function.params) scope.locals.insert(param.name);
        for (const auto& local : body.locals) scope.locals.insert(local.name);
        Block(scope, body.statements, &body.locals);
    }
    if (errors > 0) {
        plan_.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Info, "GAMEUI_SCRIPT_PARSE",
            script.path + " 有 " + std::to_string(errors) + " 处语法错误，所在函数里的界面调用可能没有识别"));
    }
}

void Analyzer::Block(const Scope& scope, const std::vector<std::int32_t>& statements, const std::vector<VarDecl>* locals) {
    BlockFrames block;
    if (locals) {
        for (const auto& local : *locals) {
            if (local.init < 0) continue;
            const Expr& init = Unwrap(scope, local.init);
            const auto spec = init.kind == ExprKind::Call && init.left < 0 ? LookupCall(init.text) : std::nullopt;
            if (spec && spec->kind == CallKind::Create) {
                (void)Create(scope, *spec, local.init, std::string(scope.function) + "::" + std::string(local.name), block);
            } else {
                Nested(scope, local.init, block);
            }
        }
    }
    for (const std::int32_t index : statements) Statement(scope, index, block);
}

void Analyzer::Statement(const Scope& scope, std::int32_t index, BlockFrames& block) {
    if (index < 0) return;
    const Stmt& statement = scope.pool.StmtAt(index);
    switch (statement.kind) {
    case StmtKind::Set: {
        if (statement.target < 0 || statement.value < 0) return;
        const std::string key = VarKey(scope, statement.target);
        const Expr& value = Unwrap(scope, statement.value);
        const auto spec = value.kind == ExprKind::Call && value.left < 0 ? LookupCall(value.text) : std::nullopt;
        if (spec && spec->kind == CallKind::Create) {
            (void)Create(scope, *spec, statement.value, key, block);
            return;
        }
        if (!key.empty()) {
            // 把已保存的框架赋给另一个变量：两个变量指向同一个框架。
            const std::string source = VarKey(scope, statement.value);
            const auto found = source.empty() ? block.end() : block.find(source);
            if (found != block.end()) {
                block[key] = found->second;
                keyFrames_[key] = found->second;
                baseFrames_[BaseKey(scope, statement.target)].push_back(found->second);
            } else {
                block.erase(key);
            }
        }
        Nested(scope, statement.value, block);
        return;
    }
    case StmtKind::Call:
        Call(scope, statement.value, block);
        return;
    case StmtKind::If:
        // 条件分支里的设置不一定执行，分支各自成块。
        for (const auto& branch : statement.branches) {
            Nested(scope, branch.condition, block);
            Block(scope, branch.body, nullptr);
        }
        return;
    case StmtKind::Loop:
        Block(scope, statement.body, nullptr);
        return;
    case StmtKind::ExitWhen:
    case StmtKind::Return:
        Nested(scope, statement.value, block);
        return;
    default:
        return;
    }
}

void Analyzer::Call(const Scope& scope, std::int32_t index, BlockFrames& block) {
    if (index < 0) return;
    const Expr& call = Unwrap(scope, index);
    const auto spec = call.kind == ExprKind::Call && call.left < 0 ? LookupCall(call.text) : std::nullopt;
    if (!spec) {
        Nested(scope, index, block);
        return;
    }
    if (spec->kind == CallKind::Create) {
        (void)Create(scope, *spec, index, {}, block);
        return;
    }
    if (IsSetter(spec->kind)) {
        Apply(scope, *spec, index, block);
    } else if (spec->kind == CallKind::FrameOther || spec->kind == CallKind::Other) {
        Group(std::string(call.text), spec->family, ScriptUiCallReason::NotPreviewed, Site(scope, call));
    } else if (spec->kind == CallKind::Unknown) {
        Group(std::string(call.text), spec->family, ScriptUiCallReason::Unknown, Site(scope, call));
    }
    for (std::uint32_t i = 0; i < call.argumentCount; ++i) Nested(scope, scope.pool.Argument(call, i), block);
}

void Analyzer::Nested(const Scope& scope, std::int32_t index, BlockFrames& block) {
    if (index < 0) return;
    const Expr& expr = scope.pool.At(index);
    if (expr.kind == ExprKind::Call && expr.left < 0) {
        if (const auto spec = LookupCall(expr.text)) {
            if (spec->kind == CallKind::Create || IsSetter(spec->kind) || spec->kind == CallKind::FrameOther ||
                spec->kind == CallKind::Other || spec->kind == CallKind::Unknown) {
                Call(scope, index, block);
                return;
            }
        }
    }
    Nested(scope, expr.left, block);
    Nested(scope, expr.right, block);
    if (expr.kind == ExprKind::Call) {
        for (std::uint32_t i = 0; i < expr.argumentCount; ++i) Nested(scope, scope.pool.Argument(expr, i), block);
    }
}

std::optional<std::uint32_t> Analyzer::Create(const Scope& scope, const CallSpec& spec, std::int32_t index,
    const std::string& key, BlockFrames& block) {
    const Expr& call = Unwrap(scope, index);
    for (std::uint32_t i = 0; i < call.argumentCount; ++i) Nested(scope, scope.pool.Argument(call, i), block);
    if (plan_.frames.size() >= kMaxFrames) {
        if (!limitReported_) {
            plan_.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_SCRIPT_FRAME_LIMIT",
                "脚本创建的框架超过 " + std::to_string(kMaxFrames) + " 个，其余不再预览"));
            limitReported_ = true;
        }
        return std::nullopt;
    }
    const auto argument = [&](std::int8_t position) -> std::optional<ScriptValue> {
        if (position < 0 || static_cast<std::uint32_t>(position) >= call.argumentCount) return std::nullopt;
        return Eval(scope, scope.pool.Argument(call, static_cast<std::uint32_t>(position)));
    };
    ScriptFrame frame;
    frame.family = spec.family;
    frame.created = Site(scope, call);
    if (!key.empty()) {
        const auto separator = key.find("::");
        frame.variable = separator == std::string::npos ? key : key.substr(separator + 2);
    }
    if (spec.typeArg >= 0) {
        if (const auto type = StringOf(argument(spec.typeArg))) frame.frameType = *type;
        else frame.dynamic.push_back("框架类型 第 " + std::to_string(call.line) + " 行");
    }
    if (spec.templateArg >= 0) {
        if (const auto name = StringOf(argument(spec.templateArg))) frame.templateName = *name;
        else frame.dynamic.push_back("模板 第 " + std::to_string(call.line) + " 行");
    }
    std::optional<std::string> literalName;
    if (spec.nameArg >= 0) {
        literalName = StringOf(argument(spec.nameArg));
        frame.nameDynamic = !literalName.has_value();
    } else if (!frame.templateName.empty()) {
        literalName = frame.templateName;  // 按模板创建的框架以模板名为名
    }
    if (literalName && !literalName->empty()) frame.name = *literalName;
    else if (!frame.variable.empty()) frame.name = frame.variable;
    else frame.name = frame.templateName.empty() ? std::string(call.text) : frame.templateName;
    if (spec.parentArg >= 0 && static_cast<std::uint32_t>(spec.parentArg) < call.argumentCount) {
        frame.parent = Ref(scope, scope.pool.Argument(call, static_cast<std::uint32_t>(spec.parentArg)), block);
    }

    const auto id = static_cast<std::uint32_t>(plan_.frames.size());
    if (literalName && !literalName->empty()) baseFrames_["name:" + Text::LowerAscii(*literalName)].push_back(id);
    plan_.frames.push_back(std::move(frame));
    if (!key.empty()) {
        block[key] = id;
        keyFrames_[key] = id;
        baseFrames_[key.substr(0, key.find('['))].push_back(id);  // 与 BaseKey 一致：数组取基名
    }
    return id;
}

void Analyzer::Apply(const Scope& scope, const CallSpec& spec, std::int32_t index, BlockFrames& block) {
    const Expr& call = Unwrap(scope, index);
    if (call.argumentCount == 0) return;
    const std::int32_t target = scope.pool.Argument(call, 0);
    std::optional<std::uint32_t> frame;
    const std::string key = VarKey(scope, target);
    if (const auto found = key.empty() ? block.end() : block.find(key); found != block.end()) {
        frame = found->second;
    } else {
        // 按名字取到同一语句块里刚创建的框架。
        const ScriptFrameRef ref = Ref(scope, target, block);
        if (ref.target == ScriptFrameTarget::Script && Unwrap(scope, target).kind == ExprKind::Call) {
            for (const auto& [name, id] : block) {
                if (id == ref.frame) frame = id;
            }
        }
    }
    if (!frame) {
        pending_.push_back(Pending{BaseKey(scope, target), Site(scope, call), spec.family});
        return;
    }
    ScriptFrame& applied = plan_.frames[*frame];
    if (!ApplyTo(scope, applied, spec, call, block)) {
        applied.dynamic.push_back(std::string(call.text) + " 第 " + std::to_string(call.line) + " 行");
    }
}

bool Analyzer::ApplyTo(const Scope& scope, ScriptFrame& frame, const CallSpec& spec, const Expr& call,
    const BlockFrames& block) {
    std::vector<std::optional<ScriptValue>> args;
    for (std::uint32_t i = 0; i < call.argumentCount; ++i) {
        args.push_back(i == 0 ? std::nullopt : Eval(scope, scope.pool.Argument(call, i)));
    }
    ScriptFrameRef relative;
    if (spec.kind == CallKind::SetPoint && call.argumentCount > 2) relative = Ref(scope, scope.pool.Argument(call, 2), block);
    if (spec.kind == CallKind::SetAllPoints && call.argumentCount > 1) relative = Ref(scope, scope.pool.Argument(call, 1), block);
    return detail::ApplyFrameCall(frame, spec.kind, args, relative);
}

void Analyzer::Finish() {
    for (auto& pending : pending_) {
        const auto found = pending.key.empty() ? baseFrames_.end() : baseFrames_.find(pending.key);
        if (found == baseFrames_.end() || found->second.empty()) {
            const std::string function = pending.site.call;
            Group(function, pending.family, ScriptUiCallReason::UntracedTarget, std::move(pending.site));
            continue;
        }
        std::vector<std::uint32_t> frames = found->second;
        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        for (const std::uint32_t id : frames) {
            auto& frame = plan_.frames[id];
            ++frame.runtimeCalls;
            if (frame.runtimeSamples.size() < kMaxSamples) frame.runtimeSamples.push_back(pending.site);
            auto& functions = frame.runtimeFunctions;
            if (functions.size() < kMaxRuntimeFunctions &&
                std::find(functions.begin(), functions.end(), pending.site.function) == functions.end()) {
                functions.push_back(pending.site.function);
            }
        }
    }
    pending_.clear();
}

} // namespace

std::string_view UiApiFamilyName(UiApiFamily family) noexcept {
    switch (family) {
    case UiApiFamily::Dz: return "DzAPI";
    case UiApiFamily::MemHack: return "MemHack";
    default: return "Blizzard";
    }
}

ScriptFramePlan FindScriptFrames(const std::vector<MapScriptText>& scripts) {
    ScriptFramePlan plan;
    Analyzer analyzer(plan);
    for (const auto& script : scripts) {
        if (script.language == ScriptLanguage::Lua) {
            plan.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Info, "GAMEUI_SCRIPT_FRAMES_LUA",
                script.path + " 中创建的界面暂不识别（目前只分析 JASS）"));
            continue;
        }
        analyzer.Script(script);
    }
    analyzer.Finish();
    return plan;
}

} // namespace War3Map::GameUi
