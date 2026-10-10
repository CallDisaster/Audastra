#include "JassParser.h"

#include <array>

namespace War3Map::Jass::detail {
namespace {

constexpr std::array<Keyword, 1> kLoopEnd = {Keyword::EndLoop};
constexpr std::array<Keyword, 3> kIfBranchEnd = {Keyword::ElseIf, Keyword::Else, Keyword::EndIf};
constexpr std::array<Keyword, 1> kIfEnd = {Keyword::EndIf};

// 语句块内出现即说明外层结构未闭合的行首标记（不消耗，交给外层报告缺少的结束关键字）。
constexpr std::array<Keyword, 5> kHardStopKeywords = {
    Keyword::EndFunction, Keyword::Function, Keyword::Globals, Keyword::EndGlobals, Keyword::Native};
constexpr std::array<std::string_view, 16> kHardStopWords = {
    "method", "endmethod", "struct", "endstruct", "library", "library_once", "endlibrary", "scope", "endscope",
    "module", "endmodule", "interface", "endinterface", "private", "public", "static"};

std::uint32_t EndOf(const Token& token) {
    return token.offset + static_cast<std::uint32_t>(token.text.size());
}

std::string Quote(std::string_view text) {
    return "“" + DiagnosticText(text) + "”";
}

std::string Describe(const Token& token) {
    switch (token.kind) {
    case TokenKind::Newline: return "行尾";
    case TokenKind::End: return "文件结尾";
    default: return Quote(token.text);
    }
}

} // namespace

Parser::Parser(std::span<const Token> tokens, SyntaxPool& pool, std::vector<SyntaxError>& errors, bool vjass, CheckBudget* budget)
    : tokens_(tokens), pool_(pool), errors_(errors), vjass_(vjass), budget_(budget) {
    pool_.budget = budget;
    end_.kind = TokenKind::End;
    if (!tokens_.empty()) {
        const Token& last = tokens_.back();
        end_.line = last.line + (last.kind == TokenKind::Newline ? 1 : 0);
        end_.offset = last.offset + static_cast<std::uint32_t>(last.text.size());
    }
}

const Token& Parser::Peek(std::size_t ahead) const noexcept {
    return at_ + ahead < tokens_.size() ? tokens_[at_ + ahead] : end_;
}

const Token& Parser::Take() {
    Tick();
    const Token& token = Peek();
    if (at_ < tokens_.size()) ++at_;
    return token;
}

bool Parser::Accept(Keyword keyword) {
    Tick();
    if (!Peek().Is(keyword)) return false;
    ++at_;
    return true;
}

bool Parser::AcceptOperator(std::string_view op) {
    Tick();
    if (!Peek().IsOperator(op)) return false;
    ++at_;
    return true;
}

bool Parser::AcceptWord(std::string_view word) {
    Tick();
    if (Peek().kind != TokenKind::Identifier || Peek().text != word) return false;
    ++at_;
    return true;
}

void Parser::SkipBlankLines(std::vector<std::string_view>* pragmas) {
    while (Peek().kind == TokenKind::Newline || Peek().kind == TokenKind::Pragma) {
        if (Peek().kind == TokenKind::Pragma && pragmas != nullptr) AddMetadata(*pragmas, Peek().text);
        Take();
    }
}

void Parser::SkipLine() {
    while (!AtLineEnd()) Take();
    if (Peek().kind == TokenKind::Newline) Take();
}

void Parser::ExpectLineEnd(std::string_view context) {
    if (Peek().kind == TokenKind::Pragma) Take();
    if (AtLineEnd()) {
        if (Peek().kind == TokenKind::Newline) Take();
        return;
    }
    Error(Peek().line, std::string(context) + "之后出现多余的" + Describe(Peek()));
    SkipLine();
}

void Parser::Error(std::uint32_t line, std::string message) {
    // 同一行只报第一个语法错误，避免连锁报错。
    if (!errors_.empty() && errors_.back().line == line) return;
    if (budget_) {
        message = budget_->Message(message);
        budget_->ConsumeDiagnostic(message.size());
        ReserveBounded(errors_, errors_.size() + 1, budget_->DiagnosticLimit(), *budget_);
    }
    errors_.push_back({line, std::move(message)});
}

bool Parser::Name(std::string_view& name, std::string_view what) {
    if (Peek().kind != TokenKind::Identifier) {
        Error(Peek().line, "此处需要" + std::string(what) + "，实际为" + Describe(Peek()));
        return false;
    }
    name = Take().text;
    return true;
}

bool Parser::AtBlockEnd(const BlockEnd& end) const noexcept {
    const Token& token = Peek();
    if (token.kind == TokenKind::Keyword) {
        for (const auto keyword : end.keywords) {
            if (token.keyword == keyword) return true;
        }
    } else if (token.kind == TokenKind::Identifier) {
        for (const auto word : end.words) {
            if (token.text == word) return true;
        }
    }
    return false;
}

// ---- 表达式 ----

std::int32_t Parser::ErrorExpr(std::uint32_t line) {
    Expr expr;
    expr.kind = ExprKind::Error;
    expr.line = line;
    return pool_.Add(expr);
}

std::int32_t Parser::AddBinary(Expr expr) {
    expr.begin = pool_.At(expr.left).begin;
    expr.end = pool_.At(expr.right).end;
    return pool_.Add(expr);
}

std::uint32_t Parser::LastEnd() const {
    for (std::size_t i = at_; i > 0; --i) {
        if (budget_) budget_->PollOccasionally(i);
        const Token& token = tokens_[i - 1];
        if (token.kind != TokenKind::Newline && token.kind != TokenKind::Pragma) return EndOf(token);
    }
    return 0;
}

std::int32_t Parser::Expression() {
    NestingGuard guard(budget_, nesting_);
    return Logical();
}

// and / or 同级、左结合（JASS 语义）。
std::int32_t Parser::Logical() {
    std::int32_t left = Comparison();
    while (Peek().Is(Keyword::And) || Peek().Is(Keyword::Or)) {
        const Token& op = Take();
        Expr expr;
        expr.kind = ExprKind::Binary;
        expr.op = op.keyword == Keyword::And ? Operator::And : Operator::Or;
        expr.line = op.line;
        expr.left = left;
        expr.right = Comparison();
        left = AddBinary(expr);
    }
    return left;
}

// 相等与大小比较同级、左结合；操作数可以是 not 表达式（not 比比较运算结合得更紧）。
std::int32_t Parser::Comparison() {
    std::int32_t left = Negation();
    for (;;) {
        const Token& token = Peek();
        if (token.kind != TokenKind::Operator) break;
        Operator op = Operator::None;
        if (token.text == "==") op = Operator::Equal;
        else if (token.text == "!=") op = Operator::NotEqual;
        else if (token.text == "<") op = Operator::Less;
        else if (token.text == "<=") op = Operator::LessEqual;
        else if (token.text == ">") op = Operator::Greater;
        else if (token.text == ">=") op = Operator::GreaterEqual;
        else break;
        Take();
        Expr expr;
        expr.kind = ExprKind::Binary;
        expr.op = op;
        expr.line = token.line;
        expr.left = left;
        expr.right = Negation();
        left = AddBinary(expr);
    }
    return left;
}

// not 的操作数是完整的加减表达式：not a + b 即 not (a + b)。
std::int32_t Parser::Negation() {
    if (!Peek().Is(Keyword::Not)) return Additive();
    NestingGuard guard(budget_, nesting_);
    const Token& op = Take();
    Expr expr;
    expr.kind = ExprKind::Unary;
    expr.op = Operator::Not;
    expr.line = op.line;
    expr.begin = op.offset;
    expr.left = Negation();
    expr.end = pool_.At(expr.left).end;
    return pool_.Add(expr);
}

std::int32_t Parser::Additive() {
    std::int32_t left = Multiplicative();
    while (Peek().IsOperator("+") || Peek().IsOperator("-")) {
        const Token& op = Take();
        Expr expr;
        expr.kind = ExprKind::Binary;
        expr.op = op.text == "+" ? Operator::Plus : Operator::Minus;
        expr.line = op.line;
        expr.left = left;
        expr.right = Multiplicative();
        left = AddBinary(expr);
    }
    return left;
}

std::int32_t Parser::Multiplicative() {
    std::int32_t left = Unary();
    while (Peek().IsOperator("*") || Peek().IsOperator("/")) {
        const Token& op = Take();
        Expr expr;
        expr.kind = ExprKind::Binary;
        expr.op = op.text == "*" ? Operator::Multiply : Operator::Divide;
        expr.line = op.line;
        expr.left = left;
        expr.right = Unary();
        left = AddBinary(expr);
    }
    return left;
}

std::int32_t Parser::Unary() {
    const Token& token = Peek();
    if (token.IsOperator("-") || token.IsOperator("+") || token.Is(Keyword::Not)) {
        NestingGuard guard(budget_, nesting_);
        Take();
        Expr expr;
        expr.kind = ExprKind::Unary;
        expr.op = token.IsOperator("-") ? Operator::Minus : token.IsOperator("+") ? Operator::Plus : Operator::Not;
        expr.line = token.line;
        expr.begin = token.offset;
        expr.left = expr.op == Operator::Not ? Additive() : Unary();
        expr.end = pool_.At(expr.left).end;
        return pool_.Add(expr);
    }
    return Postfix(Primary());
}

std::int32_t Parser::Primary() {
    const Token& token = Peek();
    Expr expr;
    expr.line = token.line;
    expr.text = token.text;
    expr.begin = token.offset;
    expr.end = EndOf(token);
    switch (token.kind) {
    case TokenKind::Integer: expr.kind = ExprKind::Integer; break;
    case TokenKind::Rawcode: expr.kind = ExprKind::Rawcode; break;
    case TokenKind::Real: expr.kind = ExprKind::Real; break;
    case TokenKind::String: expr.kind = ExprKind::String; break;
    case TokenKind::Identifier: expr.kind = ExprKind::Name; break;
    case TokenKind::Keyword:
        if (token.keyword == Keyword::True || token.keyword == Keyword::False) {
            expr.kind = ExprKind::Boolean;
        } else if (token.keyword == Keyword::Null) {
            expr.kind = ExprKind::Null;
        } else if (token.keyword == Keyword::Function) {
            Take();
            expr.kind = ExprKind::FunctionRef;
            expr.end = EndOf(Peek());
            if (!Name(expr.text, "函数名")) return ErrorExpr(token.line);
            if (vjass_ && Peek().IsOperator(".")) {
                // function 结构.方法：left 为名称链
                Expr name;
                name.kind = ExprKind::Name;
                name.line = token.line;
                name.text = expr.text;
                name.begin = expr.end - static_cast<std::uint32_t>(expr.text.size());
                name.end = expr.end;
                std::int32_t chain = pool_.Add(name);
                while (AcceptOperator(".")) {
                    Expr member;
                    member.kind = ExprKind::Member;
                    member.line = token.line;
                    member.left = chain;
                    member.begin = name.begin;
                    member.end = EndOf(Peek());
                    if (!Name(member.text, "成员名")) return ErrorExpr(token.line);
                    chain = pool_.Add(member);
                    expr.end = member.end;
                }
                expr.left = chain;
                expr.text = pool_.At(chain).text;
            }
            return pool_.Add(expr);
        } else {
            Error(token.line, "此处需要表达式，实际为" + Describe(token));
            return ErrorExpr(token.line);
        }
        break;
    case TokenKind::Operator:
        if (token.text == "(") {
            Take();
            expr.kind = ExprKind::Paren;
            expr.left = Expression();
            expr.end = EndOf(Peek());
            if (!AcceptOperator(")")) {
                Error(Peek().line, "缺少“)”，实际为" + Describe(Peek()));
                return ErrorExpr(token.line);
            }
            return pool_.Add(expr);
        }
        if (vjass_ && token.text == ".") {
            // .成员：隐含 this
            Expr self;
            self.kind = ExprKind::Name;
            self.line = token.line;
            self.text = "this";
            self.begin = token.offset;
            self.end = token.offset; // 隐含的 this 不占源文本
            return pool_.Add(self);
        }
        [[fallthrough]];
    default:
        Error(token.line, "此处需要表达式，实际为" + Describe(token));
        return ErrorExpr(token.line);
    }
    Take();
    return pool_.Add(expr);
}

std::int32_t Parser::Postfix(std::int32_t index) {
    for (;;) {
        const Expr current = pool_.At(index);
        if (current.kind == ExprKind::Error) return index;
        const Token& token = Peek();
        if (token.IsOperator("(") && (current.kind == ExprKind::Name || (vjass_ && current.kind == ExprKind::Member))) {
            Take();
            Expr call;
            call.kind = ExprKind::Call;
            call.line = current.line;
            call.text = current.text;
            call.left = current.kind == ExprKind::Member ? current.left : -1;
            call.begin = current.begin;
            index = Arguments(call);
            continue;
        }
        if (token.IsOperator("[") && (vjass_ || current.kind == ExprKind::Name)) {
            Take();
            Expr expr;
            expr.kind = ExprKind::Index;
            expr.line = current.line;
            expr.text = current.text;
            expr.left = index;
            expr.begin = current.begin;
            expr.right = Expression();
            expr.end = EndOf(Peek());
            if (!AcceptOperator("]")) {
                Error(Peek().line, "缺少“]”，实际为" + Describe(Peek()));
                return ErrorExpr(current.line);
            }
            index = pool_.Add(expr);
            continue;
        }
        if (vjass_ && token.IsOperator(".")) {
            Take();
            Expr member;
            member.kind = ExprKind::Member;
            member.line = token.line;
            member.left = index;
            member.begin = current.begin;
            member.end = EndOf(Peek());
            if (!Name(member.text, "成员名")) return ErrorExpr(token.line);
            index = pool_.Add(member);
            continue;
        }
        return index;
    }
}

std::int32_t Parser::Arguments(Expr call) {
    std::vector<std::int32_t> arguments;
    call.end = EndOf(Peek());
    if (!AcceptOperator(")")) {
        for (;;) {
            const auto argument = Expression();
            pool_.AddArgument(arguments, argument);
            call.end = EndOf(Peek());
            if (AcceptOperator(",")) continue;
            if (AcceptOperator(")")) break;
            Error(Peek().line, "参数列表缺少“)”，实际为" + Describe(Peek()));
            return ErrorExpr(call.line);
        }
    }
    pool_.AddArguments(call, arguments);
    return pool_.Add(call);
}

// ---- 语句 ----

std::int32_t Parser::Statement() {
    NestingGuard guard(budget_, nesting_);
    const Token& first = Peek();
    const std::uint32_t line = first.line;
    const bool debug = Accept(Keyword::Debug);
    Stmt stmt;
    stmt.line = line;
    stmt.debug = debug;
    stmt.begin = first.offset;
    if (Accept(Keyword::Set)) {
        stmt.kind = StmtKind::Set;
        stmt.target = Postfix(Primary());
        const auto kind = pool_.At(stmt.target).kind;
        if (kind != ExprKind::Name && kind != ExprKind::Index && kind != ExprKind::Member && kind != ExprKind::Error) {
            Error(line, "set 的目标必须是变量");
        }
        if (!AcceptOperator("=")) {
            Error(Peek().line, "set 语句缺少“=”，实际为" + Describe(Peek()));
            SkipLine();
            stmt.kind = StmtKind::Error;
            stmt.end = LastEnd();
            return pool_.Add(std::move(stmt));
        }
        stmt.value = Expression();
        ExpectLineEnd("set 语句");
    } else if (Accept(Keyword::Call)) {
        stmt.kind = StmtKind::Call;
        stmt.value = Postfix(Primary());
        const auto kind = pool_.At(stmt.value).kind;
        if (kind != ExprKind::Call && kind != ExprKind::Error) Error(line, "call 之后必须是函数调用");
        ExpectLineEnd("call 语句");
    } else if (Accept(Keyword::If)) {
        return IfStatement(debug, line, first.offset);
    } else if (Accept(Keyword::Loop)) {
        stmt.kind = StmtKind::Loop;
        ExpectLineEnd("loop");
        stmt.body = Block({kLoopEnd});
        if (!Accept(Keyword::EndLoop)) {
            Error(Peek().line, "loop 缺少 endloop（第 " + std::to_string(line) + " 行开始）");
        } else {
            ExpectLineEnd("endloop");
        }
    } else if (Accept(Keyword::ExitWhen)) {
        stmt.kind = StmtKind::ExitWhen;
        stmt.value = Expression();
        ExpectLineEnd("exitwhen 语句");
    } else if (Accept(Keyword::Return)) {
        stmt.kind = StmtKind::Return;
        if (!AtLineEnd() && Peek().kind != TokenKind::Pragma) stmt.value = Expression();
        ExpectLineEnd("return 语句");
    } else {
        Error(line, "此处需要语句（set / call / if / loop / exitwhen / return），实际为" + Describe(Peek()));
        SkipLine();
        return -1;
    }
    stmt.end = LastEnd();
    return pool_.Add(std::move(stmt));
}

std::int32_t Parser::IfStatement(bool debug, std::uint32_t line, std::uint32_t begin) {
    Stmt stmt;
    stmt.kind = StmtKind::If;
    stmt.line = line;
    stmt.debug = debug;
    stmt.begin = begin;
    Branch first;
    first.line = line;
    first.condition = Expression();
    if (!Accept(Keyword::Then)) {
        Error(Peek().line, "if 条件之后缺少 then，实际为" + Describe(Peek()));
        SkipLine();
    } else {
        ExpectLineEnd("then");
    }
    first.body = Block({kIfBranchEnd});
    AddMetadata(stmt.branches, std::move(first));
    for (;;) {
        const std::uint32_t branchLine = Peek().line;
        if (Accept(Keyword::ElseIf)) {
            Branch branch;
            branch.line = branchLine;
            branch.condition = Expression();
            if (!Accept(Keyword::Then)) {
                Error(Peek().line, "elseif 条件之后缺少 then，实际为" + Describe(Peek()));
                SkipLine();
            } else {
                ExpectLineEnd("then");
            }
            branch.body = Block({kIfBranchEnd});
            AddMetadata(stmt.branches, std::move(branch));
            continue;
        }
        if (Accept(Keyword::Else)) {
            Branch branch;
            branch.line = branchLine;
            ExpectLineEnd("else");
            branch.body = Block({kIfEnd});
            AddMetadata(stmt.branches, std::move(branch));
            if (!Accept(Keyword::EndIf)) {
                Error(Peek().line, "if 缺少 endif（第 " + std::to_string(line) + " 行开始）");
                stmt.end = LastEnd();
                return pool_.Add(std::move(stmt));
            }
            ExpectLineEnd("endif");
            stmt.end = LastEnd();
            return pool_.Add(std::move(stmt));
        }
        if (Accept(Keyword::EndIf)) {
            ExpectLineEnd("endif");
            stmt.end = LastEnd();
            return pool_.Add(std::move(stmt));
        }
        Error(Peek().line, "if 缺少 endif（第 " + std::to_string(line) + " 行开始）");
        stmt.end = LastEnd();
        return pool_.Add(std::move(stmt));
    }
}

std::vector<std::int32_t> Parser::Block(const BlockEnd& end) {
    std::vector<std::int32_t> statements;
    for (;;) {
        SkipBlankLines();
        if (AtEnd() || AtBlockEnd(end)) return statements;
        const Token& token = Peek();
        if (token.kind == TokenKind::Keyword) {
            bool stop = token.keyword == Keyword::Constant;
            for (const auto keyword : kHardStopKeywords) stop = stop || token.keyword == keyword;
            if (stop) return statements;
            if (token.keyword == Keyword::Local) {
                Error(token.line, "local 声明必须位于函数开头、其他语句之前");
                Take();
                VarDecl decl;
                if (Variable(decl)) {
                    decl.line = token.line;
                    AddMetadata(lateLocals_, decl);
                }
                ExpectLineEnd("local 声明");
                continue;
            }
        } else if (vjass_ && token.kind == TokenKind::Identifier) {
            bool stop = false;
            for (const auto word : kHardStopWords) stop = stop || token.text == word;
            if (stop) return statements;
        }
        if (const auto statement = Statement(); statement >= 0) AddMetadata(statements, statement);
    }
}

void Parser::Body(const BlockEnd& end, std::vector<VarDecl>& locals, std::vector<std::int32_t>& statements) {
    for (;;) {
        SkipBlankLines();
        const Token& token = Peek();
        if (!token.Is(Keyword::Local)) break;
        Take();
        VarDecl decl;
        decl.line = token.line;
        if (Variable(decl)) AddMetadata(locals, decl);
        ExpectLineEnd("local 声明");
    }
    lateLocals_.clear();
    statements = Block(end);
    if (budget_) ReserveBounded(locals, locals.size() + lateLocals_.size(), budget_->AstLimit(), *budget_);
    locals.insert(locals.end(), lateLocals_.begin(), lateLocals_.end());
    lateLocals_.clear();
}

bool Parser::Variable(VarDecl& decl) {
    decl.line = Peek().line;
    decl.begin = Peek().offset;
    decl.constant = Accept(Keyword::Constant);
    if (!Name(decl.type, "类型名")) {
        SkipLine();
        return false;
    }
    decl.array = Accept(Keyword::Array);
    if (!Name(decl.name, "变量名")) {
        SkipLine();
        return false;
    }
    if (AcceptOperator("=")) {
        if (decl.array) Error(decl.line, "数组变量不能在声明时初始化");
        decl.init = Expression();
    } else if (decl.constant) {
        Error(decl.line, "常量 " + DiagnosticText(decl.name) + " 必须在声明时赋值");
    }
    decl.end = LastEnd();
    return true;
}

bool Parser::Params(std::vector<Param>& params) {
    if (Accept(Keyword::Nothing)) return true;
    for (;;) {
        Param param;
        if (!Name(param.type, "参数类型") || !Name(param.name, "参数名")) return false;
        AddMetadata(params, param);
        if (!AcceptOperator(",")) return true;
    }
}

bool Parser::ReturnType(std::string_view& type) {
    if (Accept(Keyword::Nothing)) {
        type = {};
        return true;
    }
    return Name(type, "返回类型");
}

bool AtLineStart(std::span<const Token> tokens, std::size_t index) noexcept {
    return index == 0 || tokens[index - 1].kind == TokenKind::Newline || tokens[index - 1].kind == TokenKind::Pragma;
}

// ---- 纯 JASS 顶层 ----
namespace {

// 从 begin 开始找行首的 endfunction；先遇到下一个函数头或文件结尾时返回其位置并置 found = false。
std::size_t FindFunctionEnd(const std::vector<Token>& tokens, std::size_t begin, bool& found, CheckBudget* budget) {
    for (std::size_t i = begin; i < tokens.size(); ++i) {
        if (budget) budget->PollOccasionally(i);
        const Token& token = tokens[i];
        if (token.kind == TokenKind::End) break;
        if (token.kind != TokenKind::Keyword || !AtLineStart(tokens, i)) continue;
        if (token.keyword == Keyword::EndFunction) {
            found = true;
            return i;
        }
        const bool header = token.keyword == Keyword::Function ||
            (token.keyword == Keyword::Constant && i + 1 < tokens.size() && tokens[i + 1].Is(Keyword::Function)) ||
            token.keyword == Keyword::Globals || token.keyword == Keyword::Native;
        if (header) {
            found = false;
            return i;
        }
    }
    found = false;
    return tokens.size() - 1;
}

} // namespace

ProgramFile ParseProgram(std::string_view text, CheckBudget* budget) {
    if (budget) {
        budget->RegisterSource(text.size());
        auto tokens = LexJassBounded(text, {}, budget->RemainingTokens(), budget);
        if (!tokens.Succeeded()) budget->RejectLexical(std::move(tokens.diagnostics));
        budget->ConsumeTokens(tokens.value->size());
        return ParseProgram(std::move(*tokens.value), budget);
    }
    return ParseProgram(LexJass(text));
}

ProgramFile ParseProgram(std::vector<Token> tokens, CheckBudget* budget) {
    ProgramFile file;
    file.tokens = std::move(tokens);
    file.lines = file.tokens.empty() ? 0 : file.tokens.back().line;
    Parser parser(file.tokens, file.globalPool, file.errors, false, budget);
    std::vector<std::string_view> pragmas;
    bool functionSeen = false;
    const auto lateDeclaration = [&](std::uint32_t line, std::string_view what) {
        if (functionSeen) parser.Error(line, std::string(what) + "必须位于所有函数之前");
    };
    for (;;) {
        if (budget) budget->Poll();
        parser.SkipBlankLines(&pragmas);
        if (parser.AtEnd()) break;
        const Token& token = parser.Peek();
        const std::uint32_t line = token.line;
        if (parser.Accept(Keyword::Type)) {
            lateDeclaration(line, "类型声明");
            TypeDecl decl;
            decl.line = line;
            if (!parser.Name(decl.name, "类型名") || !parser.Accept(Keyword::Extends) || !parser.Name(decl.parent, "父类型名")) {
                if (decl.name.empty() || !decl.parent.empty()) parser.Error(line, "类型声明格式应为 type 名称 extends 父类型");
                parser.SkipLine();
                continue;
            }
            parser.AddMetadata(file.order, Declaration{Declaration::Kind::Type, static_cast<std::uint32_t>(file.types.size())});
            parser.AddMetadata(file.types, decl);
            parser.ExpectLineEnd("类型声明");
        } else if (parser.Accept(Keyword::Globals)) {
            lateDeclaration(line, "globals 块");
            parser.ExpectLineEnd("globals");
            for (;;) {
                parser.SkipBlankLines();
                if (parser.Accept(Keyword::EndGlobals)) {
                    parser.ExpectLineEnd("endglobals");
                    break;
                }
                const Token& next = parser.Peek();
                if (next.kind == TokenKind::End || next.Is(Keyword::Function) || next.Is(Keyword::Native) || next.Is(Keyword::Globals)) {
                    parser.Error(next.line, "globals 缺少 endglobals（第 " + std::to_string(line) + " 行开始）");
                    break;
                }
                VarDecl decl;
                if (parser.Variable(decl)) {
                    parser.AddMetadata(file.order, Declaration{Declaration::Kind::Global, static_cast<std::uint32_t>(file.globals.size())});
                    parser.AddMetadata(file.globals, decl);
                    parser.ExpectLineEnd("全局变量声明");
                }
            }
        } else if (token.Is(Keyword::Native) || (token.Is(Keyword::Constant) && parser.Peek(1).Is(Keyword::Native))) {
            lateDeclaration(line, "原生函数声明");
            NativeDecl decl;
            decl.line = line;
            decl.constant = parser.Accept(Keyword::Constant);
            parser.Accept(Keyword::Native);
            const bool ok = parser.Name(decl.name, "函数名") && parser.Accept(Keyword::Takes) && parser.Params(decl.params) &&
                parser.Accept(Keyword::Returns) && parser.ReturnType(decl.returns);
            if (!ok) {
                parser.Error(line, "原生函数声明格式应为 native 名称 takes 参数 returns 类型");
                parser.SkipLine();
                continue;
            }
            parser.AddMetadata(file.order, Declaration{Declaration::Kind::Native, static_cast<std::uint32_t>(file.natives.size())});
            parser.AddMetadata(file.natives, std::move(decl));
            parser.ExpectLineEnd("原生函数声明");
        } else if (token.Is(Keyword::Function) || (token.Is(Keyword::Constant) && parser.Peek(1).Is(Keyword::Function))) {
            functionSeen = true;
            FunctionDecl decl;
            decl.line = line;
            decl.constant = parser.Accept(Keyword::Constant);
            parser.Accept(Keyword::Function);
            decl.pragmas = std::move(pragmas);
            pragmas.clear();
            const bool ok = parser.Name(decl.name, "函数名") && parser.Accept(Keyword::Takes) && parser.Params(decl.params) &&
                parser.Accept(Keyword::Returns) && parser.ReturnType(decl.returns);
            if (ok) {
                parser.ExpectLineEnd("函数声明");
            } else {
                parser.Error(line, "函数声明格式应为 function 名称 takes 参数 returns 类型");
                parser.SkipLine();
            }
            decl.bodyBegin = parser.Position();
            bool found = false;
            decl.bodyEnd = FindFunctionEnd(file.tokens, decl.bodyBegin, found, budget);
            decl.endLine = file.tokens[decl.bodyEnd].line;
            parser.Seek(decl.bodyEnd);
            if (found) {
                parser.Take();
                parser.ExpectLineEnd("endfunction");
            } else {
                parser.Error(decl.endLine, "函数 " + DiagnosticText(decl.name) + " 缺少 endfunction（第 " + std::to_string(line) + " 行开始）");
            }
            if (ok) {
                parser.AddMetadata(file.order, Declaration{Declaration::Kind::Function, static_cast<std::uint32_t>(file.functions.size())});
                parser.AddMetadata(file.functions, std::move(decl));
            }
            continue;
        } else {
            parser.Error(line, "此处需要声明（type / globals / native / function），实际为" + DiagnosticText(token.text));
            parser.SkipLine();
            continue;
        }
        pragmas.clear();
    }
    return file;
}

FunctionBody ParseFunctionBody(std::span<const Token> tokens, const FunctionDecl& function, bool vjass, CheckBudget* budget) {
    FunctionBody body;
    Parser parser(tokens.subspan(function.bodyBegin, function.bodyEnd - function.bodyBegin), body.pool, body.errors, vjass, budget);
    parser.Body({}, body.locals, body.statements);
    return body;
}

} // namespace War3Map::Jass::detail
