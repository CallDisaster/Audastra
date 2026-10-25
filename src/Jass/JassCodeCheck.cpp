#include "JassCheckContext.h"

// 表达式与语句的类型检查（单个函数体或全局初始值；只读访问共享符号表，可并行）。
namespace War3Map::Jass::detail {
namespace {

// rawcode 的字符数（反斜杠转义算一个字符）。
std::size_t RawcodeLength(std::string_view literal, CheckBudget* budget) {
    std::size_t count = 0;
    for (std::size_t i = 1; i + 1 < literal.size(); ++i) {
        if (budget) budget->PollOccasionally(count);
        if (literal[i] == '\\') ++i;
        ++count;
    }
    return count;
}

std::string Quote(std::string_view text) {
    return "“" + DiagnosticText(text) + "”";
}

} // namespace

CheckFlags ApplyPragmas(CheckFlags flags, const std::vector<std::string_view>& pragmas, CheckBudget* budget) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    for (const auto pragma : pragmas) {
        if (budget) budget->Poll();
        for (std::size_t at = (std::min)(pragma.size(), std::size_t{3}); at < pragma.size();) {
            while (at < pragma.size() && space(pragma[at])) { if (budget) budget->PollOccasionally(at); ++at; }
            const auto begin = at;
            while (at < pragma.size() && !space(pragma[at])) { if (budget) budget->PollOccasionally(at); ++at; }
            const auto word = pragma.substr(begin, at - begin);
            if (word.size() < 2 || (word[0] != '+' && word[0] != '-')) continue;
            const bool on = word[0] == '+';
            const auto name = word.substr(1);
            if (name == "rb") flags.returnBug = on;
            else if (name == "shadow") flags.shadow = on;
            else if (name == "filter") flags.filter = on;
            else if (name == "nosemanticerror") flags.noSemanticErrors = on;
            else if (name == "nosyntaxerror") flags.noSyntaxErrors = on;
        }
    }
    return flags;
}

void CodeChecker::Report(std::uint32_t line, std::string code, std::string message) {
    if (budget_) {
        message = budget_->Message(message);
        budget_->ConsumeDiagnostic(code.size() + message.size());
        ReserveBounded(issues_, issues_.size() + 1, budget_->DiagnosticLimit(), *budget_);
    }
    issues_.push_back({line, std::move(code), std::move(message), true});
}

void CodeChecker::Report(const Expr& origin, std::string code, std::string message) {
    Report(origin.line, std::move(code), std::move(message));
    if (origin.end >= origin.begin) issues_.back().byteSpan = std::pair{origin.begin, origin.end - origin.begin};
}

void CodeChecker::Require(TypeId from, TypeId to, std::uint32_t line, const std::string& context, const Expr* origin) {
    if (context_.types.CanConvert(from, to)) return;
    auto message = "类型不匹配：" + context + "需要 " + TypeName(to) + "，实际为 " + TypeName(from);
    if (origin) Report(*origin, "JASS_TYPE", std::move(message));
    else Report(line, "JASS_TYPE", std::move(message));
}

const GlobalSymbol* CodeChecker::LookupGlobal(std::string_view name) const {
    const auto found = context_.globals.find(name);
    if (found == context_.globals.end() || found->second.order >= visibleBefore_) return nullptr;
    return &found->second;
}

const FunctionSymbol* CodeChecker::LookupFunction(std::string_view name, std::uint32_t line, bool reportMissing, const Expr* origin) {
    const auto record = [&](std::optional<SymbolKey> target) {
        if (symbols_ && origin) symbols_->Reference(name, target,
            origin->kind == ExprKind::FunctionRef ? SymbolUse::FunctionReference : SymbolUse::Call);
    };
    const auto found = context_.functions.find(name);
    if (found == context_.functions.end()) {
        record({});
        if (reportMissing) {
            if (origin) Report(*origin, "JASS_UNDECLARED", "未声明的函数 " + Quote(name));
            else Report(line, "JASS_UNDECLARED", "未声明的函数 " + Quote(name));
        }
        return nullptr;
    }
    const auto& symbol = found->second;
    if (symbol.order >= visibleBefore_ && name != self_) {
        record({});
        if (reportMissing) {
            auto message = "函数 " + Quote(name) + " 在此处尚未声明（定义于 " +
                context_.DisplayLocation(symbol.file, symbol.line) + "，须先定义后使用）";
            if (origin) Report(*origin, "JASS_DECLARATION_ORDER", std::move(message));
            else Report(line, "JASS_DECLARATION_ORDER", std::move(message));
        }
        return nullptr;
    }
    record(symbol.definition);
    return &symbol;
}

// ---- 表达式 ----

TypeId CodeChecker::Check(std::int32_t index) {
    if (budget_) budget_->Poll();
    if (index < 0) return kErrorType;
    const Expr& expr = pool_.At(index);
    switch (expr.kind) {
    case ExprKind::Integer: return kInteger;
    case ExprKind::Rawcode:
    case ExprKind::String:
        CheckLiteral(expr);
        return expr.kind == ExprKind::Rawcode ? kInteger : kString;
    case ExprKind::Real: return kReal;
    case ExprKind::Boolean: return kBoolean;
    case ExprKind::Null: return kNull;
    case ExprKind::Name: return CheckName(expr);
    case ExprKind::Index: return CheckIndex(expr);
    case ExprKind::Call: return CheckCall(expr);
    case ExprKind::FunctionRef: return CheckFunctionRef(expr);
    case ExprKind::Unary: return CheckUnary(expr);
    case ExprKind::Binary: return CheckBinary(expr);
    case ExprKind::Paren: return Check(expr.left);
    case ExprKind::Member:
        Report(expr, "JASS_SYNTAX", "JASS 不支持成员访问（." + DiagnosticText(expr.text) + "）");
        return kErrorType;
    case ExprKind::Error: return kErrorType;
    }
    return kErrorType;
}

void CodeChecker::CheckLiteral(const Expr& expr) {
    if (expr.kind == ExprKind::Rawcode) {
        const auto length = RawcodeLength(expr.text, budget_);
        if (length != 1 && length != 4) Report(expr, "JASS_RAWCODE", "rawcode " + DiagnosticText(expr.text) + " 必须是 1 或 4 个字符");
    } else {
        // 与字节版 ValidStringEscapes 相同的语义，并在单个长字面量内轮询取消。
        std::size_t ticks = 0;
        for (std::size_t i = 1; i + 1 < expr.text.size(); ++i) {
            if (budget_) budget_->PollOccasionally(ticks++);
            if (expr.text[i] != '\\') continue;
            const char c = expr.text[++i];
            if (c == 'b' || c == 't' || c == 'n' || c == 'f' || c == 'r' || c == '"' || c == '\\') continue;
            Report(expr, "JASS_ESCAPE", "字符串含无效的转义字符（只允许 \\b \\t \\n \\f \\r \\\" \\\\）");
            break;
        }
    }
}

TypeId CodeChecker::CheckName(const Expr& expr) {
    if (const auto local = locals_.find(expr.text); local != locals_.end()) {
        auto& info = local->second;
        if (symbols_) symbols_->Reference(expr.text, info.definition, SymbolUse::Read);
        if (info.declaring) {
            Report(expr, "JASS_UNINITIALIZED", "局部变量 " + Quote(expr.text) + " 在自身的初始值中被使用");
        } else if (info.array) {
            Report(expr, "JASS_ARRAY", "数组 " + Quote(expr.text) + " 缺少下标");
        } else if (!info.initialized && !info.reported) {
            info.reported = true;
            Report(expr, "JASS_UNINITIALIZED", "局部变量 " + Quote(expr.text) + " 在赋值前被读取");
        }
        return info.type;
    }
    if (const auto* global = LookupGlobal(expr.text)) {
        if (symbols_) symbols_->Reference(expr.text, global->definition, SymbolUse::Read);
        if (global->array) Report(expr, "JASS_ARRAY", "数组 " + Quote(expr.text) + " 缺少下标");
        return global->type;
    }
    if (symbols_) symbols_->Reference(expr.text, {}, SymbolUse::Read);
    if (const auto later = context_.globals.find(expr.text); later != context_.globals.end()) {
        Report(expr, "JASS_DECLARATION_ORDER", "全局变量 " + Quote(expr.text) + " 在此处尚未声明（定义于 " +
            context_.DisplayLocation(later->second.file, later->second.line) + "，须先定义后使用）");
        return later->second.type;
    }
    Report(expr, "JASS_UNDECLARED", "未声明的变量 " + Quote(expr.text));
    return kErrorType;
}

TypeId CodeChecker::CheckIndex(const Expr& expr) {
    const auto indexType = Check(expr.right);
    Require(indexType, kInteger, expr.line, "数组下标");
    const std::string_view name = pool_.At(expr.left).text;
    TypeId type = kErrorType;
    bool array = false;
    if (const auto local = locals_.find(name); local != locals_.end()) {
        if (symbols_) symbols_->Reference(name, local->second.definition, SymbolUse::Read);
        type = local->second.type;
        array = local->second.array;
    } else if (const auto* global = LookupGlobal(name)) {
        if (symbols_) symbols_->Reference(name, global->definition, SymbolUse::Read);
        type = global->type;
        array = global->array;
    } else {
        if (symbols_) symbols_->Reference(name, {}, SymbolUse::Read);
        Report(expr, "JASS_UNDECLARED", "未声明的变量 " + Quote(name));
        return kErrorType;
    }
    if (!array) Report(expr, "JASS_ARRAY", Quote(name) + " 不是数组，不能使用下标");
    return type;
}

TypeId CodeChecker::CheckCall(const Expr& expr) {
    std::vector<TypeId> arguments;
    arguments.reserve(expr.argumentCount);
    for (std::uint32_t i = 0; i < expr.argumentCount; ++i) arguments.push_back(Check(pool_.Argument(expr, i)));
    const auto* symbol = LookupFunction(expr.text, expr.line, true, &expr);
    if (symbol == nullptr) return kErrorType;
    if (arguments.size() != symbol->params.size()) {
        Report(expr, "JASS_ARGUMENTS", "函数 " + Quote(expr.text) + " 需要 " + std::to_string(symbol->params.size()) +
            " 个参数，实际传入 " + std::to_string(arguments.size()) + " 个");
    }
    for (std::size_t i = 0; i < arguments.size() && i < symbol->params.size(); ++i) {
        Require(arguments[i], symbol->params[i], expr.line,
            "调用 " + DiagnosticText(expr.text) + " 的参数 " + DiagnosticText(symbol->paramNames[i]) + " ",
            &pool_.At(pool_.Argument(expr, static_cast<std::uint32_t>(i))));
    }
    if (function_ != nullptr && function_->constant && !symbol->constant) {
        Report(expr, "JASS_CONSTANT", "常量函数中不能调用非常量函数 " + Quote(expr.text));
    }
    if (flags_.filter && (expr.text == "Filter" || expr.text == "Condition") && expr.argumentCount == 1) {
        const Expr& argument = pool_.At(pool_.Argument(expr, 0));
        if (argument.kind == ExprKind::FunctionRef) {
            const auto found = context_.functions.find(argument.text);
            if (found != context_.functions.end() && found->second.returns != kBoolean) {
                Report(expr, "JASS_FILTER", DiagnosticText(expr.text) + " 使用的函数 " + Quote(argument.text) + " 必须返回 boolean");
            }
        }
    }
    return symbol->returns;
}

TypeId CodeChecker::CheckFunctionRef(const Expr& expr) {
    if (const auto* symbol = LookupFunction(expr.text, expr.line, true, &expr); symbol != nullptr && !symbol->params.empty()) {
        Report(expr, "JASS_CODE", "作为 code 使用的函数 " + Quote(expr.text) + " 不能有参数");
    }
    return kCode;
}

TypeId CodeChecker::CheckUnary(const Expr& expr) {
    const auto type = Check(expr.left);
    if (expr.op == Operator::Not) {
        Require(type, kBoolean, expr.line, "not 的操作数");
        return kBoolean;
    }
    if (type == kErrorType || TypeTable::IsNumeric(type)) return type;
    Report(expr, "JASS_TYPE", "正负号只能用于 integer 或 real，实际为 " + TypeName(type));
    return kErrorType;
}

TypeId CodeChecker::CheckBinary(const Expr& expr) {
    const auto left = Check(expr.left);
    const auto right = Check(expr.right);
    switch (expr.op) {
    case Operator::And:
    case Operator::Or:
        Require(left, kBoolean, expr.line, std::string(expr.op == Operator::And ? "and" : "or") + " 的左侧");
        Require(right, kBoolean, expr.line, std::string(expr.op == Operator::And ? "and" : "or") + " 的右侧");
        return kBoolean;
    case Operator::Equal:
    case Operator::NotEqual:
        if (!context_.types.CanCompare(left, right)) {
            Report(expr, "JASS_TYPE", "无法比较 " + TypeName(left) + " 与 " + TypeName(right) + "（只有同类型、数字之间、句柄之间或与 null 可以比较）");
        }
        return kBoolean;
    case Operator::Less:
    case Operator::LessEqual:
    case Operator::Greater:
    case Operator::GreaterEqual:
        if ((left != kErrorType && !TypeTable::IsNumeric(left)) || (right != kErrorType && !TypeTable::IsNumeric(right))) {
            Report(expr, "JASS_TYPE", "大小比较只能用于 integer 或 real，实际为 " + TypeName(left) + " 与 " + TypeName(right));
        }
        return kBoolean;
    default:
        break;
    }
    if (left == kErrorType || right == kErrorType) return kErrorType;
    if (TypeTable::IsNumeric(left) && TypeTable::IsNumeric(right)) return left == kReal || right == kReal ? kReal : kInteger;
    if (expr.op == Operator::Plus && left == kString && right == kString) return kString;
    Report(expr, "JASS_TYPE", "运算符两侧的类型不支持该运算：" + TypeName(left) + " 与 " + TypeName(right));
    return kErrorType;
}

// ---- 语句 ----

void CodeChecker::CheckCondition(std::int32_t expr, std::string_view what) {
    Require(Check(expr), kBoolean, pool_.At(expr).line, std::string(what));
}

void CodeChecker::CheckStatements(const std::vector<std::int32_t>& statements) {
    for (const auto index : statements) CheckStatement(pool_.StmtAt(index));
}

void CodeChecker::CheckStatement(const Stmt& stmt) {
    if (budget_) budget_->Poll();
    switch (stmt.kind) {
    case StmtKind::Set: CheckSet(stmt); break;
    case StmtKind::Call: Check(stmt.value); break;
    case StmtKind::If:
        for (const auto& branch : stmt.branches) {
            if (budget_) budget_->Poll();
            if (branch.condition >= 0) CheckCondition(branch.condition, branch.line == stmt.line ? "if 条件" : "elseif 条件");
            CheckStatements(branch.body);
        }
        break;
    case StmtKind::Loop:
        ++loopDepth_;
        CheckStatements(stmt.body);
        --loopDepth_;
        break;
    case StmtKind::ExitWhen:
        if (loopDepth_ == 0) Report(stmt.line, "JASS_EXITWHEN", "exitwhen 只能用在 loop 之内");
        CheckCondition(stmt.value, "exitwhen 条件");
        break;
    case StmtKind::Return: CheckReturn(stmt); break;
    case StmtKind::Error: break;
    }
}

void CodeChecker::CheckSet(const Stmt& stmt) {
    const Expr& target = pool_.At(stmt.target);
    if (target.kind != ExprKind::Name && target.kind != ExprKind::Index) {
        Check(stmt.value);
        return;
    }
    const bool indexed = target.kind == ExprKind::Index;
    const std::string_view name = indexed ? pool_.At(target.left).text : target.text;
    if (indexed) Require(Check(target.right), kInteger, stmt.line, "数组下标");
    Local* local = nullptr;
    TypeId type = kErrorType;
    bool array = false;
    if (const auto found = locals_.find(name); found != locals_.end()) {
        local = &found->second;
        if (symbols_) symbols_->Reference(name, local->definition, SymbolUse::Write);
        type = local->type;
        array = local->array;
    } else if (const auto* global = LookupGlobal(name)) {
        if (symbols_) symbols_->Reference(name, global->definition, SymbolUse::Write);
        type = global->type;
        array = global->array;
        if (global->constant) Report(stmt.line, "JASS_CONSTANT", "不能给常量 " + Quote(name) + " 赋值");
        if (function_ != nullptr && function_->constant) Report(stmt.line, "JASS_CONSTANT", "常量函数中不能修改全局变量 " + Quote(name));
    } else {
        if (symbols_) symbols_->Reference(name, {}, SymbolUse::Write);
        Report(indexed ? pool_.At(target.left) : target, "JASS_UNDECLARED", "未声明的变量 " + Quote(name));
        Check(stmt.value);
        return;
    }
    if (array && !indexed) Report(stmt.line, "JASS_ARRAY", "数组 " + Quote(name) + " 缺少下标");
    if (!array && indexed) Report(stmt.line, "JASS_ARRAY", Quote(name) + " 不是数组，不能使用下标");
    Require(Check(stmt.value), type, stmt.line, "给 " + DiagnosticText(name) + " 赋值时");
    if (local != nullptr) local->initialized = true;
}

void CodeChecker::CheckReturn(const Stmt& stmt) {
    const auto returns = function_ == nullptr ? kNothing : function_->returns;
    if (returns == kNothing) {
        if (stmt.value >= 0) {
            Check(stmt.value);
            Report(stmt.line, "JASS_RETURN", "函数没有返回值（returns nothing），return 不能带值");
        }
        return;
    }
    if (stmt.value < 0) {
        Report(stmt.line, "JASS_RETURN", "函数需要返回 " + TypeName(returns) + "，return 缺少返回值");
        return;
    }
    const auto type = Check(stmt.value);
    // returnbug：只有函数最后一条语句的 return 检查类型（1.20 的类型转换写法）。
    if (flags_.returnBug && pool_.statements.data() + lastStatement_ != &stmt) return;
    Require(type, returns, stmt.line, "返回值");
}

bool CodeChecker::Returns(const std::vector<std::int32_t>& statements) const {
    for (const auto index : statements) {
        if (budget_) budget_->Poll();
        const Stmt& stmt = pool_.StmtAt(index);
        switch (stmt.kind) {
        case StmtKind::Return: return true;
        case StmtKind::Loop:
            if (Returns(stmt.body)) return true;
            break;
        case StmtKind::If: {
            bool all = stmt.branches.back().condition < 0;
            for (const auto& branch : stmt.branches) {
                if (budget_) budget_->Poll();
                all = all && Returns(branch.body);
            }
            if (all) return true;
            break;
        }
        default:
            break;
        }
    }
    return false;
}

// ---- 入口 ----

void CodeChecker::CheckInitializer(std::int32_t expr, TypeId type, std::string_view name, std::uint32_t line) {
    Require(Check(expr), type, line, "全局变量 " + DiagnosticText(name) + " 的初始值");
}

void CodeChecker::CheckFunction(const FunctionDecl& decl, const FunctionSymbol& symbol, const FunctionBody& body) {
    function_ = &symbol;
    lastStatement_ = body.statements.empty() ? -1 : body.statements.back();
    for (std::size_t i = 0; i < symbol.params.size(); ++i) {
        if (budget_) budget_->PollOccasionally(i);
        const auto name = symbol.paramNames[i];
        const auto [parameter, inserted] = locals_.emplace(name, Local{symbol.params[i], false, true, false, false, true});
        if (!inserted) {
            Report(decl.line, "JASS_REDECLARED", "参数 " + Quote(name) + " 重复");
        } else if (symbols_) parameter->second.definition = symbols_->Define(name, decl.params[i].type, SymbolKind::Parameter);
    }
    for (const auto& local : body.locals) {
        if (budget_) budget_->Poll();
        TypeId type = context_.types.Find(local.type);
        if (type == kUnknownType || type == kNothing || type == kNull) {
            Report(local.line, "JASS_UNKNOWN_TYPE", "未知的类型 " + Quote(local.type));
            type = kErrorType;
        }
        if (type == kCode && local.array) Report(local.line, "JASS_CODE", "不能声明 code 数组");
        if (context_.functions.contains(local.name)) {
            Report(local.line, "JASS_REDECLARED", "局部变量名 " + Quote(local.name) + " 已被声明为函数");
        }
        if (context_.types.Find(local.name) != kUnknownType) {
            Report(local.line, "JASS_REDECLARED", "局部变量名 " + Quote(local.name) + " 已被声明为类型");
        }
        if (flags_.shadow && context_.globals.contains(local.name)) {
            Report(local.line, "JASS_SHADOW", "局部变量 " + Quote(local.name) + " 与全局变量同名");
        }
        // 与参数同名允许（遮蔽参数），与之前的局部变量同名报错。
        if (const auto found = locals_.find(local.name); found != locals_.end() && !found->second.param) {
            Report(local.line, "JASS_REDECLARED", "局部变量 " + Quote(local.name) + " 重复声明");
        }
        auto& info = locals_[local.name];
        info = Local{type, local.array, local.array || local.init >= 0, true, false, false};
        if (symbols_) info.definition = symbols_->Define(local.name, local.type, SymbolKind::Local, local.array);
        if (local.init >= 0) Require(Check(local.init), type, local.line, "局部变量 " + DiagnosticText(local.name) + " 的初始值");
        info.declaring = false;
    }
    CheckStatements(body.statements);
    if (symbol.returns != kNothing && !Returns(body.statements)) {
        Report(decl.endLine, "JASS_MISSING_RETURN", "函数 " + Quote(decl.name) + " 缺少 return（不是所有路径都返回 " + TypeName(symbol.returns) + "）");
    }
}

} // namespace War3Map::Jass::detail
