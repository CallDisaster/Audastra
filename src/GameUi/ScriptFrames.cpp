#include "War3MapCore/GameUi/ScriptFrames.h"

#include "War3MapCore/GameUi/GameCodeLayout.h"
#include "War3MapCore/Text/TextUtil.h"
#include "ScriptUiApi.h"
#include "ScriptValue.h"
#include "../Jass/JassLexer.h"
#include "../Jass/JassParser.h"

#include <algorithm>
#include <charconv>
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
    // 多面板格子：所在多面板（本语句块里确定时）、多面板变量基名与行列（常量时）。
    struct ItemRef {
        std::optional<std::uint32_t> frame;
        std::string boardKey;
        std::optional<detail::BoardCell> cell;
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
    void CreateObject(const Scope& scope, const CallSpec& spec, const Expr& call, ScriptFrame& frame);
    void Bind(const Scope& scope, std::int32_t target, const std::string& key, std::uint32_t id, BlockFrames& block);
    std::optional<std::uint32_t> LastCreated(const Scope& scope, std::int32_t index, const BlockFrames& block) const;
    void BindItem(const Scope& scope, const std::string& key, std::int32_t index, BlockFrames& block);
    ItemRef ResolveItem(const Scope& scope, std::int32_t index, const BlockFrames& block) const;
    void ApplyItem(const Scope& scope, const CallSpec& spec, std::int32_t index, BlockFrames& block);
    void Apply(const Scope& scope, const CallSpec& spec, std::int32_t index, BlockFrames& block);
    bool ApplyTo(const Scope& scope, ScriptFrame& frame, const CallSpec& spec, const Expr& call, const BlockFrames& block);
    void Group(const std::string& function, UiApiFamily family, ScriptUiCallReason reason, ScriptCallSite site);

    ScriptFramePlan& plan_;
    std::unordered_map<std::string, ScriptValue> constants_;
    std::unordered_map<std::string, std::uint32_t> keyFrames_;               // 变量原文 → 最近一次保存的框架
    std::unordered_map<std::string, std::vector<std::uint32_t>> baseFrames_;  // 变量基名或 name:名字 → 框架
    std::unordered_map<std::string, std::size_t> groups_;
    std::vector<Pending> pending_;
    std::vector<ItemRef> items_;  // 语句块里 “#item:变量” 指向的格子
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
            const std::string key = std::string(scope.function) + "::" + std::string(local.name);
            if (spec && spec->kind == CallKind::Create) {
                (void)Create(scope, *spec, local.init, key, block);
            } else if (spec && spec->kind == CallKind::BoardGetItem) {
                BindItem(scope, key, local.init, block);
            } else if (const auto last = LastCreated(scope, local.init, block)) {
                Bind(scope, -1, key, *last, block);
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
        if (spec && spec->kind == CallKind::BoardGetItem) {
            BindItem(scope, key, statement.value, block);
            Nested(scope, statement.value, block);
            return;
        }
        if (const auto last = key.empty() ? std::nullopt : LastCreated(scope, statement.value, block)) {
            Bind(scope, statement.target, key, *last, block);
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
            block.erase("#item:" + key);
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
    if (spec.object != ScriptObjectKind::Frame) {
        CreateObject(scope, spec, call, frame);
        const auto id = static_cast<std::uint32_t>(plan_.frames.size());
        plan_.frames.push_back(std::move(frame));
        // BJ 版本记为“最近创建”，供同一语句块里的 GetLastCreated… 取用。
        if (spec.shown) block["#last:" + std::to_string(static_cast<int>(spec.object))] = id;
        if (!key.empty()) Bind(scope, -1, key, id, block);
        return id;
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

void Analyzer::CreateObject(const Scope& scope, const CallSpec& spec, const Expr& call, ScriptFrame& frame) {
    frame.object = spec.object;
    frame.templateName = std::string(ScriptObjectTemplate(spec.object));
    frame.name = frame.variable.empty() ? frame.templateName : frame.variable;
    frame.visible = spec.shown;
    const auto argument = [&](std::int8_t position) -> std::optional<ScriptValue> {
        if (position < 0 || static_cast<std::uint32_t>(position) >= call.argumentCount) return std::nullopt;
        return Eval(scope, scope.pool.Argument(call, static_cast<std::uint32_t>(position)));
    };
    if (spec.titleArg >= 0) {
        if (auto title = StringOf(argument(spec.titleArg))) frame.text = std::move(*title);
        else frame.dynamic.push_back("标题 第 " + std::to_string(call.line) + " 行");
    }
    if (spec.object == ScriptObjectKind::Leaderboard) {
        // 新建的排行榜标签为空、不显示标题。
        frame.leaderboard.emplace();
        if (spec.titleArg < 0) frame.text = std::string();
        frame.leaderboard->showTitle = frame.text && !ResolveTriggerString(plan_.strings, *frame.text).empty();
    }
    if (spec.object == ScriptObjectKind::Multiboard) {
        frame.board.emplace();
        if (spec.rowsArg >= 0 || spec.columnsArg >= 0) {
            // CreateMultiboardBJ 依次设置行数、列数。
            const auto rows = NumberOf(argument(spec.rowsArg));
            const auto columns = NumberOf(argument(spec.columnsArg));
            if (rows && columns) {
                frame.board->Resize(static_cast<std::uint32_t>(std::max(0.0f, *rows)), 0);
                frame.board->Resize(frame.board->rows, static_cast<std::uint32_t>(std::max(0.0f, *columns)));
            } else {
                frame.dynamic.push_back("行列数 第 " + std::to_string(call.line) + " 行");
            }
        }
    }
}

// 保存 MultiboardGetItem 结果的变量：格子所在的多面板与行列。
void Analyzer::BindItem(const Scope& scope, const std::string& key, std::int32_t index, BlockFrames& block) {
    if (key.empty()) return;
    block["#item:" + key] = static_cast<std::uint32_t>(items_.size());
    items_.push_back(ResolveItem(scope, index, block));
}

// MultiboardGetItem(多面板, 行, 列) 调用对应的格子；多面板不在本语句块里时只记它的变量基名。
Analyzer::ItemRef Analyzer::ResolveItem(const Scope& scope, std::int32_t index, const BlockFrames& block) const {
    ItemRef item;
    const Expr& call = Unwrap(scope, index);
    if (call.argumentCount < 3) return item;
    const std::int32_t board = scope.pool.Argument(call, 0);
    item.frame = LastCreated(scope, board, block);
    if (!item.frame) {
        const std::string key = VarKey(scope, board);
        if (const auto found = key.empty() ? block.end() : block.find(key); found != block.end()) item.frame = found->second;
    }
    item.boardKey = BaseKey(scope, board);
    item.cell = detail::BoardCellOf(Eval(scope, scope.pool.Argument(call, 1)), Eval(scope, scope.pool.Argument(call, 2)));
    return item;
}

// 作用在单个格子上的设置：格子来自保存 MultiboardGetItem 结果的变量，或直接写在参数里的 MultiboardGetItem。
void Analyzer::ApplyItem(const Scope& scope, const CallSpec& spec, std::int32_t index, BlockFrames& block) {
    const Expr& call = Unwrap(scope, index);
    if (call.argumentCount == 0) return;
    const std::int32_t target = scope.pool.Argument(call, 0);
    const Expr& expr = Unwrap(scope, target);
    std::optional<ItemRef> item;
    if (expr.kind == ExprKind::Call && expr.left < 0) {
        if (const auto inner = LookupCall(expr.text); inner && inner->kind == CallKind::BoardGetItem) {
            item = ResolveItem(scope, target, block);
        }
    } else if (const std::string key = VarKey(scope, target); !key.empty()) {
        if (const auto found = block.find("#item:" + key); found != block.end()) item = items_[found->second];
    }
    if (!item || !item->frame) {
        pending_.push_back(Pending{item ? item->boardKey : BaseKey(scope, target), Site(scope, call), spec.family});
        return;
    }
    ScriptFrame& board = plan_.frames[*item->frame];
    std::vector<std::optional<ScriptValue>> args;
    for (std::uint32_t i = 0; i < call.argumentCount; ++i) args.push_back(i == 0 ? std::nullopt : Eval(scope, scope.pool.Argument(call, i)));
    if (!item->cell || !detail::ApplyBoardCall(board, spec, args, item->cell)) {
        board.dynamic.push_back(std::string(call.text) + " 第 " + std::to_string(call.line) + " 行");
    }
}

// 变量指向已有的脚本对象（target 为赋值目标表达式，局部变量声明时为 -1）。
void Analyzer::Bind(const Scope& scope, std::int32_t target, const std::string& key, std::uint32_t id, BlockFrames& block) {
    block[key] = id;
    keyFrames_[key] = id;
    baseFrames_[target >= 0 ? BaseKey(scope, target) : key.substr(0, key.find('['))].push_back(id);
    ScriptFrame& frame = plan_.frames[id];
    if (frame.variable.empty()) {
        const auto separator = key.find("::");
        frame.variable = separator == std::string::npos ? key : key.substr(separator + 2);
        if (frame.object != ScriptObjectKind::Frame) frame.name = frame.variable;
    }
}

// GetLastCreated… 调用：同一语句块里最近一次 BJ 创建的同类对象。
std::optional<std::uint32_t> Analyzer::LastCreated(const Scope& scope, std::int32_t index, const BlockFrames& block) const {
    if (index < 0) return std::nullopt;
    const Expr& expr = Unwrap(scope, index);
    if (expr.kind != ExprKind::Call || expr.left >= 0) return std::nullopt;
    const auto spec = LookupCall(expr.text);
    if (!spec || spec->kind != CallKind::LastCreated) return std::nullopt;
    const auto found = block.find("#last:" + std::to_string(static_cast<int>(spec->object)));
    return found == block.end() ? std::nullopt : std::optional<std::uint32_t>(found->second);
}

void Analyzer::Apply(const Scope& scope, const CallSpec& spec, std::int32_t index, BlockFrames& block) {
    if (spec.kind == CallKind::BoardItems && spec.board == detail::BoardScope::Item) {
        ApplyItem(scope, spec, index, block);
        return;
    }
    const Expr& call = Unwrap(scope, index);
    if (spec.targetArg < 0 || static_cast<std::uint32_t>(spec.targetArg) >= call.argumentCount) return;
    const std::int32_t target = scope.pool.Argument(call, static_cast<std::uint32_t>(spec.targetArg));
    std::optional<std::uint32_t> frame = LastCreated(scope, target, block);
    const std::string key = VarKey(scope, target);
    if (const auto found = frame || key.empty() ? block.end() : block.find(key); found != block.end()) {
        frame = found->second;
    } else if (!frame) {
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
        args.push_back(static_cast<std::int32_t>(i) == spec.targetArg ? std::nullopt : Eval(scope, scope.pool.Argument(call, i)));
    }
    ScriptFrameRef relative;
    if (spec.kind == CallKind::SetPoint && call.argumentCount > 2) relative = Ref(scope, scope.pool.Argument(call, 2), block);
    if (spec.kind == CallKind::SetAllPoints && call.argumentCount > 1) relative = Ref(scope, scope.pool.Argument(call, 1), block);
    return detail::ApplyFrameCall(frame, spec, args, relative, plan_.strings);
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

ScriptBoardItem* ScriptBoard::At(std::uint32_t row, std::uint32_t column) {
    if (row >= rows || column >= columns) return nullptr;
    return &items[static_cast<std::size_t>(row) * columns + column];
}

const ScriptBoardItem* ScriptBoard::At(std::uint32_t row, std::uint32_t column) const {
    return const_cast<ScriptBoard*>(this)->At(row, column);
}

bool ScriptBoard::Resize(std::uint32_t newRows, std::uint32_t newColumns) {
    const bool fits = newRows <= kMaxBoardRows && newColumns <= kMaxBoardColumns;
    newRows = std::min(newRows, kMaxBoardRows);
    newColumns = std::min(newColumns, kMaxBoardColumns);
    // 游戏先按行数增删整行（新行复制默认值），再按列数增删每行的格子（新格子不带文字）。
    ScriptBoardItem newColumn = defaults;
    newColumn.value.clear();
    std::vector<ScriptBoardItem> resized;
    resized.reserve(static_cast<std::size_t>(newRows) * newColumns);
    for (std::uint32_t r = 0; r < newRows; ++r) {
        for (std::uint32_t c = 0; c < newColumns; ++c) {
            if (r < rows && c < columns) resized.push_back(items[static_cast<std::size_t>(r) * columns + c]);
            else if (c < columns) resized.push_back(defaults);
            else resized.push_back(newColumn);
        }
    }
    items = std::move(resized);
    rows = newRows;
    columns = newColumns;
    if (!fits) clipped = true;
    return fits;
}

std::string_view ScriptObjectTemplate(ScriptObjectKind object) noexcept {
    switch (object) {
    case ScriptObjectKind::TimerDialog: return "TimerDialog";
    case ScriptObjectKind::Dialog: return "ScriptDialog";
    case ScriptObjectKind::Leaderboard: return "Leaderboard";
    case ScriptObjectKind::Multiboard: return "Multiboard";
    default: return {};
    }
}

std::string_view ScriptObjectKindName(ScriptObjectKind object) noexcept {
    switch (object) {
    case ScriptObjectKind::TimerDialog: return "timer-dialog";
    case ScriptObjectKind::Dialog: return "dialog";
    case ScriptObjectKind::Leaderboard: return "leaderboard";
    case ScriptObjectKind::Multiboard: return "multiboard";
    default: return "frame";
    }
}

std::string_view UiApiFamilyName(UiApiFamily family) noexcept {
    switch (family) {
    case UiApiFamily::Dz: return "DzAPI";
    case UiApiFamily::MemHack: return "MemHack";
    default: return "Blizzard";
    }
}

std::string ResolveTriggerString(const TriggerStrings& strings, const std::string& text) {
    constexpr std::string_view prefix = "TRIGSTR_";
    if (text.size() <= prefix.size() || !text.starts_with(prefix)) return text;
    std::uint32_t id = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data() + prefix.size(), end, id);
    if (error != std::errc() || stop != end) return text;
    const auto found = strings.find(id);
    return found == strings.end() ? text : found->second;
}

ScriptFramePlan FindScriptFrames(const std::vector<MapScriptText>& scripts, TriggerStrings strings) {
    ScriptFramePlan plan;
    plan.strings = std::move(strings);
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
