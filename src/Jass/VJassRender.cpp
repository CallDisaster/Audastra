#include "VJassTranslate.h"

#include <algorithm>

// vJass 翻译：表达式与语句改写。只替换需要改写的部分（成员访问、方法调用、私有名称、类型转换、
// boolexpr 参数等），其余源文本原样保留；每个表达式同时推导 vJass 类型，用于解析 a.b 的结构类型。
namespace War3Map::Jass::detail {

std::uint32_t Translation::Offset(std::string_view view) const noexcept {
    return static_cast<std::uint32_t>(view.data() - text_.data());
}

std::string Translation::TextOf(const Rendered& rendered, const Expr& expr) const {
    textBudget_.Consume(rendered.changed ? rendered.text.size() : expr.end - expr.begin);
    return rendered.changed ? rendered.text : StripBlockComments(text_.substr(expr.begin, expr.end - expr.begin), &budget_);
}

Piece Translation::PieceOf(const Expr& expr, const Rendered& rendered) {
    return {expr.begin, expr.end, rendered.changed ? &rendered.text : nullptr, rendered.changed ? &rendered.byteOrigins : nullptr};
}

bool Translation::Splice(std::uint32_t begin, std::uint32_t end, std::span<const Piece> pieces, std::string& out,
    std::vector<VJassByteOrigin>* origins) const {
    bool changed = false;
    for (const auto& piece : pieces) changed = changed || piece.text != nullptr;
    if (!changed) return false;
    MappedOutput mapped(trace_);
    std::uint32_t at = begin;
    static const std::vector<VJassByteOrigin> generated;
    for (const auto& piece : pieces) {
        budget_.Poll();
        if (piece.text == nullptr) continue;
        if (piece.begin < at || piece.end < piece.begin || piece.end > end) budget_.Fail(CheckFailure::InvalidTree);
        mapped.Original(at, piece.begin);
        mapped.Tracked(*piece.text, piece.byteOrigins ? *piece.byteOrigins : generated, piece.begin);
        at = piece.end;
    }
    mapped.Original(at, end);
    auto result = mapped.Take();
    out = std::move(result.text);
    if (origins) *origins = std::move(result.byteOrigins);
    return true;
}

std::string Translation::OutputType(const VType& type) const {
    switch (type.kind) {
    case VType::Kind::Struct:
    case VType::Kind::StructType: return "integer";
    case VType::Kind::Jass: return std::string(type.jass);
    default: return {};
    }
}

std::string Translation::LocalName(std::string_view name, std::int32_t library) const {
    const auto final = ScopedName(name, library);
    const auto found = globals_.find(final);
    if (found != globals_.end() && found->second.fromMap) return "l__" + std::string(final);
    return std::string(name);
}

bool Translation::CheckOrder(std::int32_t method, const CodeScope& scope, std::uint32_t line) {
    const auto& function = program_.functions[static_cast<std::size_t>(method)];
    if (function.order < scope.order) return true;
    Fail(line, "方法 " + std::string(function.decl.name) + " 在定义之前使用（jasshelper 以触发器调用实现）");
    return false;
}

std::string Translation::MethodCall(std::int32_t method, const std::string* self, const std::vector<std::string>& arguments) const {
    std::string text = program_.functions[static_cast<std::size_t>(method)].finalName;
    text += '(';
    bool first = true;
    if (self != nullptr) {
        text += *self;
        first = false;
    }
    for (const auto& argument : arguments) {
        budget_.Poll();
        if (!first) text += ',';
        textBudget_.Append(text, argument);
        first = false;
    }
    text += ')';
    return text;
}

std::vector<std::string> Translation::ArgumentTexts(const Expr& call, const std::vector<Rendered>& arguments, const CodeScope& scope) const {
    std::vector<std::string> texts;
    texts.reserve(arguments.size());
    for (std::uint32_t i = 0; i < arguments.size(); ++i) {
        budget_.Poll();
        texts.push_back(TextOf(arguments[i], scope.pool->At(scope.pool->Argument(call, i))));
    }
    return texts;
}

Rendered Translation::Render(std::int32_t index, CodeScope& scope) {
    budget_.Poll();
    auto result = RenderImpl(index, scope);
    textBudget_.Consume(result.text.size());
    textBudget_.Consume(result.content.size());
    return result;
}

Rendered Translation::RenderImpl(std::int32_t index, CodeScope& scope) {
    const Expr& expr = scope.pool->At(index);
    Rendered result;
    switch (expr.kind) {
    case ExprKind::Integer:
    case ExprKind::Rawcode: result.type = VType::Jass("integer"); return result;
    case ExprKind::Real: result.type = VType::Jass("real"); return result;
    case ExprKind::String: {
        // 字符串中的换行改写为 \n（与 jasshelper 相同）
        result.type = VType::Jass("string");
        result.literal = true;
        const auto raw = text_.substr(expr.begin + 1, expr.end - expr.begin - 2);
        if (raw.find_first_of("\r\n") == std::string_view::npos) {
            result.content = std::string(raw);
            return result;
        }
        for (std::size_t i = 0; i < raw.size(); ++i) {
            budget_.Poll();
            if (raw[i] == '\r' || raw[i] == '\n') {
                if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
                result.content += "\\n";
            } else {
                result.content += raw[i];
            }
        }
        result.changed = true;
        result.text = "\"" + result.content + "\"";
        return result;
    }
    case ExprKind::Boolean: result.type = VType::Jass("boolean"); return result;
    case ExprKind::Null: result.type = VType::Jass("null"); return result;
    case ExprKind::Name: return RenderName(expr, scope);
    case ExprKind::Index: return RenderIndex(expr, scope);
    case ExprKind::Call: return expr.left >= 0 ? RenderMethodCall(expr, scope) : RenderCall(expr, scope);
    case ExprKind::Member: return RenderMember(expr, scope);
    case ExprKind::FunctionRef: return RenderFunctionRef(expr, scope);
    case ExprKind::Paren:
    case ExprKind::Unary: {
        const Rendered operand = Render(expr.left, scope);
        result.type = expr.op == Operator::Not ? VType::Jass("boolean") : operand.type;
        result.changed = Splice(expr.begin, expr.end, {PieceOf(scope.pool->At(expr.left), operand)}, result.text, &result.byteOrigins);
        return result;
    }
    case ExprKind::Binary: {
        if (expr.op == Operator::Plus) return RenderConcat(expr, scope);
        const Rendered left = Render(expr.left, scope);
        const Rendered right = Render(expr.right, scope);
        switch (expr.op) {
        case Operator::Minus:
        case Operator::Multiply:
        case Operator::Divide:
            if (left.type.Is("real") || right.type.Is("real")) {
                result.type = VType::Jass("real");
            } else {
                result.type = VType::Jass("integer");
            }
            break;
        default: result.type = VType::Jass("boolean"); break;
        }
        result.changed = Splice(expr.begin, expr.end,
            {PieceOf(scope.pool->At(expr.left), left), PieceOf(scope.pool->At(expr.right), right)}, result.text, &result.byteOrigins);
        return result;
    }
    case ExprKind::Error: return result;
    }
    return result;
}

// a + b + c …：沿左侧展开整条加法链。与 jasshelper 相同，链的最后一项是字符串字面量时，
// 合并相邻的字符串字面量（"k" + "l" + x + "m" → "kl" + x + "m"）；括号内是独立的链。
Rendered Translation::RenderConcat(const Expr& expr, CodeScope& scope) {
    std::vector<std::int32_t> operands;
    auto current = &expr;
    while (current->kind == ExprKind::Binary && current->op == Operator::Plus) {
        budget_.Poll();
        operands.push_back(current->right);
        current = &scope.pool->At(current->left);
    }
    operands.push_back(static_cast<std::int32_t>(current - scope.pool->expressions.data()));
    std::reverse(operands.begin(), operands.end());
    std::vector<Rendered> parts;
    parts.reserve(operands.size());
    bool isString = false;
    bool isReal = false;
    bool adjacent = false;
    for (const auto operand : operands) {
        budget_.Poll();
        parts.push_back(Render(operand, scope));
        const auto& part = parts.back();
        isString = isString || part.type.Is("string");
        isReal = isReal || part.type.Is("real");
        adjacent = adjacent || (parts.size() > 1 && part.literal && parts[parts.size() - 2].literal);
    }
    Rendered result;
    result.type = VType::Jass(isString ? "string" : isReal ? "real" : "integer");
    if (adjacent && parts.back().literal) {
        result.changed = true;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            budget_.Poll();
            if (parts[i].literal) {
                std::string content = parts[i].content;
                while (i + 1 < parts.size() && parts[i + 1].literal) textBudget_.Append(content, parts[++i].content);
                if (!result.text.empty()) result.text += " + ";
                result.text += "\"" + content + "\"";
                continue;
            }
            if (!result.text.empty()) result.text += " + ";
            result.text += TextOf(parts[i], scope.pool->At(operands[i]));
        }
        result.literal = std::all_of(parts.begin(), parts.end(), [](const Rendered& part) { return part.literal; });
        if (result.literal) result.content = result.text.substr(1, result.text.size() - 2);
        return result;
    }
    std::vector<Piece> pieces;
    pieces.reserve(parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i) pieces.push_back(PieceOf(scope.pool->At(operands[i]), parts[i]));
    result.changed = Splice(expr.begin, expr.end, pieces, result.text, &result.byteOrigins);
    return result;
}

Rendered Translation::RenderName(const Expr& expr, CodeScope& scope) {
    Rendered result;
    if (expr.begin == expr.end) {
        // .成员 中隐含的 this
        result.changed = true;
        const auto found = scope.locals.find("this");
        if (found == scope.locals.end()) {
            Error(expr.line, "VJASS_THIS", "此处没有 this（.成员 只能用在方法中）");
            result.text = "this";
            return result;
        }
        result.text = found->second.name;
        result.type = found->second.type;
        return result;
    }
    if (const auto found = scope.locals.find(expr.text); found != scope.locals.end()) {
        result.type = found->second.type;
        result.array = found->second.array;
        if (found->second.name != expr.text) {
            result.changed = true;
            result.text = found->second.name;
        }
        return result;
    }
    if (expr.text == "thistype") {
        if (scope.structIndex < 0) Error(expr.line, "VJASS_THISTYPE", "thistype 只能用在结构之内");
        result.type = VType::TypeOf(scope.structIndex);
        return result;
    }
    if (const auto found = FindStruct(expr.text, scope.library); found >= 0) {
        result.type = VType::TypeOf(found);
        return result;
    }
    std::string scopedFinal;
    std::string_view final = ScopedName(expr.text, scope.library);
    if (scope.scope >= 0) {
        scopedFinal = program_.scopes[static_cast<std::size_t>(scope.scope)].path + "__" + std::string(expr.text);
        if (globals_.contains(scopedFinal)) final = scopedFinal;
    }
    if (const auto found = globals_.find(final); found != globals_.end()) {
        result.type = found->second.type;
        result.array = found->second.array;
    }
    if (final != expr.text) {
        result.changed = true;
        result.text = std::string(final);
    }
    return result;
}

bool Translation::ResolveBase(const Expr& member, CodeScope& scope, MemberBase& base) {
    const Expr& left = scope.pool->At(member.left);
    if (left.kind == ExprKind::Name && left.begin == left.end && scope.structIndex >= 0) {
        // 静态方法中的 .静态成员 / .静态方法()：按结构类型访问，不需要 this。
        bool isStatic = false;
        std::int32_t owner = -1;
        if (const auto* found = FindMember(scope.structIndex, member.text, owner)) {
            isStatic = found->isStatic;
        } else {
            for (const auto op : {OperatorKind::None, OperatorKind::Get, OperatorKind::Set}) {
                budget_.Poll();
                const auto method = FindMethod(scope.structIndex, member.text, op);
                if (method >= 0) {
                    isStatic = program_.functions[static_cast<std::size_t>(method)].isStatic;
                    break;
                }
            }
        }
        if (isStatic) {
            base.structIndex = scope.structIndex;
            base.viaType = true;
            base.rendered.type = VType::TypeOf(scope.structIndex);
            return true;
        }
    }
    base.rendered = Render(member.left, scope);
    if (base.rendered.type.IsStructType() || base.rendered.type.IsInstance()) {
        base.structIndex = base.rendered.type.structIndex;
        base.viaType = base.rendered.type.IsStructType();
        if (base.structIndex >= 0) return true;
    }
    Error(member.line, "VJASS_MEMBER",
        "无法确定“" + StripBlockComments(text_.substr(left.begin, left.end - left.begin), &budget_) + "”的结构类型（访问 ." + std::string(member.text) + "）");
    return false;
}

std::string Translation::MemberText(const MemberBase& base, const Expr& left, const VMember& member, std::int32_t owner) const {
    std::string text = "s__" + program_.structs[static_cast<std::size_t>(owner)].fullName + "_" + std::string(member.decl.name);
    if (member.isStatic) return text;
    return text + "[" + TextOf(base.rendered, left) + "]";
}

Rendered Translation::RenderMember(const Expr& expr, CodeScope& scope) {
    Rendered result;
    MemberBase base;
    if (!ResolveBase(expr, scope, base)) return result;
    const Expr& left = scope.pool->At(expr.left);
    const auto& data = program_.structs[static_cast<std::size_t>(base.structIndex)];
    result.changed = true;
    std::int32_t owner = -1;
    if (const auto* member = FindMember(base.structIndex, expr.text, owner)) {
        if (!member->isStatic && base.viaType) {
            Error(expr.line, "VJASS_MEMBER", "通过结构类型访问非静态成员 " + data.fullName + "." + std::string(expr.text));
        }
        result.text = MemberText(base, left, *member, owner);
        result.type = ResolveType(member->decl.type, program_.structs[static_cast<std::size_t>(owner)].library, owner);
        result.array = member->decl.array;
        return result;
    }
    const auto getter = FindMethod(base.structIndex, expr.text, OperatorKind::Get, base.viaType ? 1 : 0);
    if (getter < 0) {
        Error(expr.line, "VJASS_MEMBER", "结构 " + std::string(data.name) + " 没有成员 " + std::string(expr.text));
        result.text = std::string(expr.text);
        return result;
    }
    CheckOrder(getter, scope, expr.line);
    scope.callsMap = true;
    const std::string self = base.viaType ? std::string{} : TextOf(base.rendered, left);
    result.text = MethodCall(getter, base.viaType ? nullptr : &self, {});
    result.type = signatures_[static_cast<std::size_t>(getter)].returns;
    return result;
}

Rendered Translation::RenderIndex(const Expr& expr, CodeScope& scope) {
    const Expr& left = scope.pool->At(expr.left);
    const Expr& right = scope.pool->At(expr.right);
    const Rendered base = Render(expr.left, scope);
    const Rendered index = Render(expr.right, scope);
    Rendered result;
    if (base.type.IsStructType() || (base.type.IsInstance() && !base.array)) {
        // S[i] / 实例[i]：method operator []
        const bool isStatic = base.type.IsStructType();
        const auto method = base.type.structIndex < 0 ? -1 : FindMethod(base.type.structIndex, {}, OperatorKind::Index, isStatic ? 1 : 0);
        result.changed = true;
        if (method < 0) {
            Error(expr.line, "VJASS_OPERATOR", std::string("结构没有") + (isStatic ? "静态 " : "") + "method operator []");
            return result;
        }
        CheckOrder(method, scope, expr.line);
        scope.callsMap = true;
        const std::string self = isStatic ? std::string{} : TextOf(base, left);
        result.text = MethodCall(method, isStatic ? nullptr : &self, {TextOf(index, right)});
        result.type = signatures_[static_cast<std::size_t>(method)].returns;
        return result;
    }
    result.type = base.type;
    result.changed = Splice(expr.begin, expr.end, {PieceOf(left, base), PieceOf(right, index)}, result.text, &result.byteOrigins);
    return result;
}

std::vector<Rendered> Translation::RenderArguments(const Expr& call, CodeScope& scope, const VFunctionSymbol* signature) {
    std::vector<Rendered> arguments;
    arguments.reserve(call.argumentCount);
    for (std::uint32_t i = 0; i < call.argumentCount; ++i) {
        budget_.Poll();
        const auto index = scope.pool->Argument(call, i);
        const Expr& argument = scope.pool->At(index);
        Rendered rendered = Render(index, scope);
        // 把 function 传给原生 / 游戏脚本函数的 boolexpr 参数：补 Condition(...)（地图函数不补，与 jasshelper 相同）
        if (signature != nullptr && signature->function < 0 && i < signature->params.size() && signature->params[i].Is("boolexpr") &&
            argument.kind == ExprKind::FunctionRef) {
            rendered.text = "Condition(" + TextOf(rendered, argument) + ")";
            rendered.byteOrigins.clear(); // 整体生成的包装不得继承旧相对偏移并冒称精确。
            rendered.changed = true;
        }
        arguments.push_back(std::move(rendered));
    }
    return arguments;
}

Rendered Translation::RenderCall(const Expr& expr, CodeScope& scope) {
    Rendered result;
    if (expr.argumentCount == 1) {
        // 类型转换：integer(x)、结构名(x)、thistype(x) 都输出为 (x)
        std::int32_t target = -1;
        if (expr.text == "thistype") {
            target = scope.structIndex;
            if (target < 0) Error(expr.line, "VJASS_THISTYPE", "thistype 只能用在结构之内");
        } else {
            target = FindStruct(expr.text, scope.library);
        }
        if (target >= 0 || expr.text == "integer" || expr.text == "thistype") {
            const auto index = scope.pool->Argument(expr, 0);
            const Rendered inner = Render(index, scope);
            result.changed = true;
            result.text = "(" + TextOf(inner, scope.pool->At(index)) + ")";
            result.type = target >= 0 ? VType::Instance(target) : VType::Jass("integer");
            return result;
        }
    }
    std::string scopedCall;
    std::string_view name = ScopedName(expr.text, scope.library);
    if (scope.scope >= 0) {
        scopedCall = program_.scopes[static_cast<std::size_t>(scope.scope)].path + "__" + std::string(expr.text);
        if (functions_.contains(scopedCall)) name = scopedCall;
    }
    const auto found = functions_.find(name);
    const VFunctionSymbol* signature = found == functions_.end() ? nullptr : &found->second;
    if (signature != nullptr) {
        result.type = signature->returns;
        if (signature->function >= 0) scope.callsMap = true;
    }
    const auto arguments = RenderArguments(expr, scope, signature);
    const std::string nameText(name);
    std::vector<Piece> pieces;
    pieces.reserve(arguments.size() + 1);
    pieces.push_back({expr.begin, expr.begin + static_cast<std::uint32_t>(expr.text.size()), name != expr.text ? &nameText : nullptr});
    for (std::uint32_t i = 0; i < arguments.size(); ++i) {
        budget_.Poll();
        pieces.push_back(PieceOf(scope.pool->At(scope.pool->Argument(expr, i)), arguments[i]));
    }
    result.changed = Splice(expr.begin, expr.end, pieces, result.text, &result.byteOrigins);
    return result;
}

Rendered Translation::RenderMethodCall(const Expr& expr, CodeScope& scope) {
    Rendered result;
    MemberBase base;
    if (!ResolveBase(expr, scope, base)) return result;
    const Expr& left = scope.pool->At(expr.left);
    const auto structIndex = base.structIndex;
    const auto& data = program_.structs[static_cast<std::size_t>(structIndex)];
    result.changed = true;
    scope.callsMap = true;
    std::int32_t method = -1;
    if (base.viaType) {
        if (expr.text == "create" || expr.text == "allocate") {
            if (expr.text == "create") {
                for (const auto candidate : data.methods) {
                    budget_.Poll();
                    const auto& function = program_.functions[static_cast<std::size_t>(candidate)];
                    if (function.isStatic && function.op == OperatorKind::None && function.decl.name == "create") method = candidate;
                }
            }
            if (method < 0) {
                if (expr.argumentCount != 0) Error(expr.line, "VJASS_ARGUMENTS", "结构 " + std::string(data.name) + " 没有自定义 create，不能带参数");
                result.text = "s__" + data.fullName + "__allocate()";
                result.type = VType::Instance(structIndex);
                return result;
            }
        } else {
            method = FindMethod(structIndex, expr.text, OperatorKind::None, 1);
        }
        if (method < 0) {
            Error(expr.line, "VJASS_METHOD", "结构 " + std::string(data.name) + " 没有静态方法 " + std::string(expr.text));
            return result;
        }
        CheckOrder(method, scope, expr.line);
        const auto arguments = RenderArguments(expr, scope, &signatures_[static_cast<std::size_t>(method)]);
        result.text = MethodCall(method, nullptr, ArgumentTexts(expr, arguments, scope));
        result.type = signatures_[static_cast<std::size_t>(method)].returns;
        return result;
    }
    const std::string self = TextOf(base.rendered, left);
    method = FindMethod(structIndex, expr.text, OperatorKind::None);
    if (method < 0 && expr.text == "destroy") {
        if (expr.argumentCount != 0) Error(expr.line, "VJASS_ARGUMENTS", "destroy 不带参数");
        result.text = DestroyName(structIndex, scope) + "(" + self + ")";
        result.type = VType::Jass("nothing");
        return result;
    }
    if (method < 0) {
        Error(expr.line, "VJASS_METHOD", "结构 " + std::string(data.name) + " 没有方法 " + std::string(expr.text));
        return result;
    }
    CheckOrder(method, scope, expr.line);
    const auto& function = program_.functions[static_cast<std::size_t>(method)];
    const auto arguments = RenderArguments(expr, scope, &signatures_[static_cast<std::size_t>(method)]);
    result.text = MethodCall(method, function.isStatic ? nullptr : &self, ArgumentTexts(expr, arguments, scope));
    result.type = signatures_[static_cast<std::size_t>(method)].returns;
    return result;
}

Rendered Translation::RenderFunctionRef(const Expr& expr, CodeScope& scope) {
    Rendered result;
    result.type = VType::Jass("code");
    if (expr.left < 0) {
        const auto name = ScopedName(expr.text, scope.library);
        if (const auto found = functions_.find(name); found != functions_.end() && found->second.function >= 0) scope.callsMap = true;
        if (name != expr.text) {
            result.changed = true;
            result.text = "function " + std::string(name);
        }
        return result;
    }
    // function 结构.静态方法
    const Expr& member = scope.pool->At(expr.left);
    MemberBase base;
    result.changed = true;
    result.text = "function " + std::string(expr.text);
    if (member.kind != ExprKind::Member || !ResolveBase(member, scope, base)) return result;
    const auto method = FindMethod(base.structIndex, member.text, OperatorKind::None, 1);
    if (!base.viaType || method < 0) {
        Error(expr.line, "VJASS_METHOD", "function 引用的静态方法 " + std::string(member.text) + " 不存在");
        return result;
    }
    CheckOrder(method, scope, expr.line);
    scope.callsMap = true;
    result.text = "function " + program_.functions[static_cast<std::size_t>(method)].finalName;
    return result;
}

void Translation::Replace(std::int32_t index, CodeScope& scope, std::vector<TextEdit>& edits) {
    Rendered rendered = Render(index, scope);
    if (!rendered.changed) return;
    const Expr& expr = scope.pool->At(index);
    edits.push_back({expr.begin, expr.end, std::move(rendered.text), std::move(rendered.byteOrigins)});
}

bool Translation::AssignmentCall(const Stmt& stmt, CodeScope& scope, std::vector<TextEdit>& edits) {
    const Expr& target = scope.pool->At(stmt.target);
    const Expr& valueExpr = scope.pool->At(stmt.value);
    if (target.kind == ExprKind::Member) {
        MemberBase base;
        if (!ResolveBase(target, scope, base)) return true;
        const Expr& left = scope.pool->At(target.left);
        std::int32_t owner = -1;
        if (const auto* member = FindMember(base.structIndex, target.text, owner)) {
            if (!member->isStatic && base.viaType) Error(target.line, "VJASS_MEMBER", "通过结构类型访问非静态成员 " + std::string(target.text));
            edits.push_back({target.begin, target.end, MemberText(base, left, *member, owner)});
            Replace(stmt.value, scope, edits);
            return true;
        }
        // set a.x = v：method operator x=
        const auto setter = FindMethod(base.structIndex, target.text, OperatorKind::Set, base.viaType ? 1 : 0);
        if (setter < 0) {
            Error(target.line, "VJASS_MEMBER", "结构 " + std::string(program_.structs[static_cast<std::size_t>(base.structIndex)].name) + " 没有成员 " +
                std::string(target.text));
            return true;
        }
        CheckOrder(setter, scope, target.line);
        scope.callsMap = true;
        const Rendered value = Render(stmt.value, scope);
        const std::string self = base.viaType ? std::string{} : TextOf(base.rendered, left);
        edits.push_back({stmt.begin, stmt.end, "call " + MethodCall(setter, base.viaType ? nullptr : &self, {TextOf(value, valueExpr)})});
        return true;
    }
    if (target.kind != ExprKind::Index) return false;
    const Expr& left = scope.pool->At(target.left);
    const Expr& right = scope.pool->At(target.right);
    const Rendered base = Render(target.left, scope);
    const Rendered index = Render(target.right, scope);
    if (base.type.IsStructType() || (base.type.IsInstance() && !base.array)) {
        // set S[i] = v / set 实例[i] = v：method operator []=
        const bool isStatic = base.type.IsStructType();
        const auto method = base.type.structIndex < 0 ? -1 : FindMethod(base.type.structIndex, {}, OperatorKind::IndexSet, isStatic ? 1 : 0);
        if (method < 0) {
            Error(target.line, "VJASS_OPERATOR", std::string("结构没有") + (isStatic ? "静态 " : "") + "method operator []=");
            return true;
        }
        CheckOrder(method, scope, target.line);
        scope.callsMap = true;
        const Rendered value = Render(stmt.value, scope);
        const std::string self = isStatic ? std::string{} : TextOf(base, left);
        edits.push_back({stmt.begin, stmt.end,
            "call " + MethodCall(method, isStatic ? nullptr : &self, {TextOf(index, right), TextOf(value, valueExpr)})});
        return true;
    }
    std::string text;
    std::vector<VJassByteOrigin> origins;
    if (Splice(target.begin, target.end, {PieceOf(left, base), PieceOf(right, index)}, text, &origins))
        edits.push_back({target.begin, target.end, std::move(text), std::move(origins)});
    Replace(stmt.value, scope, edits);
    return true;
}

void Translation::Statements(const std::vector<std::int32_t>& statements, CodeScope& scope, std::vector<TextEdit>& edits) {
    for (const auto index : statements) { budget_.Poll(); Statement(scope.pool->StmtAt(index), scope, edits); }
}

void Translation::Statement(const Stmt& stmt, CodeScope& scope, std::vector<TextEdit>& edits) {
    if (stmt.debug) {
        // 非调试模式：debug 语句整行删除（含嵌套块）
        edits.push_back({LineBegin(text_, stmt.begin), LineEnd(text_, stmt.end), {}});
        return;
    }
    switch (stmt.kind) {
    case StmtKind::Set:
        if (!AssignmentCall(stmt, scope, edits)) {
            Replace(stmt.target, scope, edits);
            Replace(stmt.value, scope, edits);
        }
        break;
    case StmtKind::Call: {
        // 与 jasshelper 相同：调用不存在的 InitTrig_ 函数（已禁用的触发器）时注释掉该语句。
        const Expr& call = scope.pool->At(stmt.value);
        if (call.kind == ExprKind::Call && call.left < 0 && call.text.starts_with("InitTrig_") &&
            !functions_.contains(ScopedName(call.text, scope.library))) {
            // 无参数时 jasshelper 写成规整的 call X()；带参数时保留原文。
            const auto original = call.argumentCount == 0 ? "call " + std::string(call.text) + "()"
                                                           : StripBlockComments(text_.substr(stmt.begin, stmt.end - stmt.begin), &budget_);
            edits.push_back({stmt.begin, stmt.end, "//Function not found: " + original});
            break;
        }
        Replace(stmt.value, scope, edits);
        break;
    }
    case StmtKind::ExitWhen:
    case StmtKind::Return:
        if (stmt.value >= 0) Replace(stmt.value, scope, edits);
        break;
    case StmtKind::If:
        for (const auto& branch : stmt.branches) {
            budget_.Poll();
            if (branch.condition >= 0) Replace(branch.condition, scope, edits);
            Statements(branch.body, scope, edits);
        }
        break;
    case StmtKind::Loop: Statements(stmt.body, scope, edits); break;
    case StmtKind::Error: break;
    }
}

CodeScope Translation::ScopeFor(std::int32_t index) {
    const auto& function = program_.functions[static_cast<std::size_t>(index)];
    CodeScope scope;
    scope.pool = &bodies_[static_cast<std::size_t>(index)].pool;
    scope.library = function.library;
    scope.scope = function.scope;
    scope.structIndex = function.structIndex;
    scope.order = function.order;
    if (function.isMethod && !function.isStatic) scope.locals["this"] = {VType::Instance(function.structIndex), "this", false};
    const auto& signature = signatures_[static_cast<std::size_t>(index)];
    for (std::size_t i = 0; i < function.decl.params.size(); ++i) {
        budget_.Poll();
        const auto name = function.decl.params[i].name;
        scope.locals[name] = {signature.params[i], LocalName(name, function.library), false};
    }
    return scope;
}

std::string Translation::HeaderText(const VFunction& function, const CodeScope& scope) const {
    std::uint32_t indent = function.headerBegin;
    while (indent < text_.size() && (text_[indent] == ' ' || text_[indent] == '\t')) ++indent;
    std::string text(text_.substr(function.headerBegin, indent - function.headerBegin));
    if (function.decl.constant) text += "constant ";
    text += "function " + function.finalName + " takes ";
    bool first = true;
    if (function.isMethod && !function.isStatic) {
        text += "integer this";
        first = false;
    }
    const auto& signature = signatures_[static_cast<std::size_t>(&function - program_.functions.data())];
    for (std::size_t i = 0; i < function.decl.params.size(); ++i) {
        budget_.Poll();
        if (!first) text += ',';
        first = false;
        textBudget_.Append(text, OutputType(signature.params[i]) + " " + scope.locals.at(function.decl.params[i].name).name);
    }
    if (first) text += "nothing";
    text += " returns ";
    text += function.decl.returns.empty() ? std::string("nothing") : OutputType(signature.returns);
    text += '\n';
    return text;
}

MappedText Translation::BodyText(std::int32_t index, CodeScope& scope) {
    const auto& function = program_.functions[static_cast<std::size_t>(index)];
    const auto& body = bodies_[static_cast<std::size_t>(index)];
    std::vector<TextEdit> edits;
    for (const auto& decl : body.locals) {
        budget_.Poll();
        const auto type = ResolveType(decl.type, function.library, function.structIndex);
        auto typeText = OutputType(type);
        if (typeText != decl.type) edits.push_back({Offset(decl.type), Offset(decl.type) + static_cast<std::uint32_t>(decl.type.size()), std::move(typeText)});
        auto name = LocalName(decl.name, function.library);
        if (name != decl.name) edits.push_back({Offset(decl.name), Offset(decl.name) + static_cast<std::uint32_t>(decl.name.size()), name});
        if (decl.init >= 0) Replace(decl.init, scope, edits);
        scope.locals[decl.name] = {type, std::move(name), decl.array};
    }
    Statements(body.statements, scope, edits);
    const auto bodyEnd = LineBegin(text_, function.endToken);
    const auto firstDebug = std::lower_bound(program_.debugLines.begin(), program_.debugLines.end(), function.headerEnd,
        [](const LineRange& range, std::uint32_t offset) { return range.begin < offset; });
    for (auto line = firstDebug; line != program_.debugLines.end() && line->end <= bodyEnd; ++line) edits.push_back({line->begin, line->end, {}});
    if (index == mainFunction_ && !mainInsertion_.empty()) {
        // 库初始化与结构初始化：插在 main 的 call InitBlizzard() 之后
        bool inserted = false;
        for (const auto statement : body.statements) {
            budget_.Poll();
            const Stmt& stmt = body.pool.StmtAt(statement);
            if (stmt.kind != StmtKind::Call || stmt.debug) continue;
            const Expr& call = body.pool.At(stmt.value);
            if (call.kind != ExprKind::Call || call.left >= 0 || call.text != "InitBlizzard") continue;
            const auto at = LineEnd(text_, stmt.end);
            edits.push_back({at, at, "\n" + mainInsertion_ + "\n"});
            inserted = true;
            break;
        }
        if (!inserted) Fail(function.decl.line, "main 中没有 call InitBlizzard()，无法插入库初始化");
    }
    MappedOutput out(trace_);
    AppendWithEdits(out, text_, function.headerEnd, bodyEnd, edits, budget_);
    return out.Take();
}

void Translation::EmitFunction(MappedOutput& out, std::int32_t index) {
    const auto& function = program_.functions[static_cast<std::size_t>(index)];
    CodeScope scope = ScopeFor(index);
    out.Anchor(function.headerBegin);
    out += HeaderText(function, scope);
    if (const auto cached = destroyBodies_.find(index); cached != destroyBodies_.end()) {
        out += cached->second;
    } else {
        out += BodyText(index, scope);
    }
    const auto endBegin = LineBegin(text_, function.endToken);
    out.Anchor(endBegin);
    out += text_.substr(endBegin, function.endToken - endBegin);
    out += "endfunction";
    const auto after = function.endToken + static_cast<std::uint32_t>(function.isMethod ? 9 : 11); // endmethod / endfunction
    AppendSource(out, after, function.endLineEnd);
    if (out.back() != '\n') out += '\n';
}

} // namespace War3Map::Jass::detail
