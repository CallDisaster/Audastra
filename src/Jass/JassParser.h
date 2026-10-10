#pragma once

#include "JassLexer.h"
#include "JassSyntax.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// JASS 语法分析。Parser 负责表达式、语句、变量声明与参数表，纯 JASS 顶层与 vJass 翻译共用；
// vjass 模式额外接受成员访问（a.b、.b、a.b(...)、任意后缀下标）与 function A.b。
namespace War3Map::Jass::detail {

struct SyntaxError {
    std::uint32_t line = 0;
    std::string message;
};

// 语句块的结束标记：JASS 关键字或（vJass 的）标识符，均须位于行首。调用方用静态数组提供。
struct BlockEnd {
    std::span<const Keyword> keywords;
    std::span<const std::string_view> words = {};
};

class Parser {
public:
    Parser(std::span<const Token> tokens, SyntaxPool& pool, std::vector<SyntaxError>& errors, bool vjass = false,
        CheckBudget* budget = nullptr);

    [[nodiscard]] std::size_t Position() const noexcept { return at_; }
    void Seek(std::size_t position) noexcept { at_ = position; }
    [[nodiscard]] const Token& Peek(std::size_t ahead = 0) const noexcept;
    const Token& Take();
    [[nodiscard]] bool AtEnd() const noexcept { return Peek().kind == TokenKind::End; }
    [[nodiscard]] bool AtLineEnd() const noexcept { return Peek().EndsLine(); }
    bool Accept(Keyword keyword);
    bool AcceptOperator(std::string_view op);
    bool AcceptWord(std::string_view word);
    // 跳过空行与 //# 注释；pragmas 非空时收集 //# 注释。
    void SkipBlankLines(std::vector<std::string_view>* pragmas = nullptr);
    // 丢弃本行剩余记号（含换行）。
    void SkipLine();
    // 要求到行尾；否则报错并跳过本行。
    void ExpectLineEnd(std::string_view context);
    void Error(std::uint32_t line, std::string message);
    template<class T>
    void AddMetadata(std::vector<T>& values, T value) {
        if (budget_) {
            budget_->ConsumeAst();
            ReserveBounded(values, values.size() + 1, budget_->AstLimit(), *budget_);
        }
        values.push_back(std::move(value));
    }

    [[nodiscard]] std::int32_t Expression();
    // 解析一条语句（含嵌套块）；当前行不是语句时报错、跳过本行并返回 -1。
    std::int32_t Statement();
    // 解析语句直到行首出现 end 之一（不消耗结束标记）；遇到 local 时报错（位于语句之后）。
    std::vector<std::int32_t> Block(const BlockEnd& end);
    // 函数体：开头的 local 声明与之后的语句，直到 end。
    void Body(const BlockEnd& end, std::vector<VarDecl>& locals, std::vector<std::int32_t>& statements);
    // [constant] 类型 [array] 名称 [= 表达式]；local 关键字由调用方处理。
    bool Variable(VarDecl& decl);
    // takes 之后：nothing 或 类型 名称 (, 类型 名称)*；returns 之后：nothing 或类型（nothing 返回空视图）。
    bool Params(std::vector<Param>& params);
    bool ReturnType(std::string_view& type);
    // 标识符（关键字不算）；失败时报错。
    bool Name(std::string_view& name, std::string_view what);

    [[nodiscard]] bool AtBlockEnd(const BlockEnd& end) const noexcept;

private:
    std::int32_t AddBinary(Expr expr);
    // 已消耗的最后一个非换行记号的结束偏移。
    [[nodiscard]] std::uint32_t LastEnd() const;
    std::int32_t Logical();
    std::int32_t Comparison();
    std::int32_t Negation();
    std::int32_t Additive();
    std::int32_t Multiplicative();
    std::int32_t Unary();
    std::int32_t Postfix(std::int32_t expr);
    std::int32_t Primary();
    std::int32_t Arguments(Expr call);
    std::int32_t ErrorExpr(std::uint32_t line);
    std::int32_t IfStatement(bool debug, std::uint32_t line, std::uint32_t begin);

    std::span<const Token> tokens_;
    Token end_; // 范围之后的结束记号（行号取范围末尾的下一行，便于报告缺少的结束关键字）
    SyntaxPool& pool_;
    std::vector<SyntaxError>& errors_;
    bool vjass_ = false;
    CheckBudget* budget_ = nullptr;
    std::size_t nesting_ = 0;
    std::size_t ticks_ = 0;
    void Tick() { if (budget_ && (++ticks_ & 255u) == 0) budget_->Poll(); }
    std::size_t at_ = 0;
    std::vector<VarDecl> lateLocals_; // 语句之后出现的 local（已报错，仍登记以免连锁报“未声明”）
};

// ---- 纯 JASS 文件的顶层结构 ----
struct TypeDecl {
    std::string_view name;
    std::string_view parent;
    std::uint32_t line = 0;
};

struct NativeDecl {
    std::string_view name;
    bool constant = false;
    std::vector<Param> params;
    std::string_view returns; // 空为 nothing
    std::uint32_t line = 0;
};

struct FunctionDecl {
    std::string_view name;
    bool constant = false;
    std::vector<Param> params;
    std::string_view returns;
    std::uint32_t line = 0;
    std::uint32_t endLine = 0;
    std::size_t bodyBegin = 0; // 函数体记号范围 [bodyBegin, bodyEnd)，不含 endfunction
    std::size_t bodyEnd = 0;
    std::vector<std::string_view> pragmas; // 紧邻函数之前的 //# 注释
};

// 顶层声明按出现顺序编号：函数只能看到在它之前声明的函数 / 原生函数（以及自身）。
struct Declaration {
    enum class Kind : std::uint8_t { Type, Global, Native, Function } kind;
    std::uint32_t index = 0; // 在对应数组中的下标
};

struct ProgramFile {
    std::vector<Token> tokens;
    std::vector<TypeDecl> types;
    std::vector<VarDecl> globals;
    SyntaxPool globalPool; // 全局变量初始值
    std::vector<NativeDecl> natives;
    std::vector<FunctionDecl> functions;
    std::vector<Declaration> order;
    std::vector<SyntaxError> errors;
    std::uint32_t lines = 0;
};

// 解析顶层声明；函数体只定位记号范围（由 ParseFunctionBody 单独解析，可并行）。
[[nodiscard]] ProgramFile ParseProgram(std::string_view text, CheckBudget* budget = nullptr);
// 已完成严格编码词法的内部入口；token 的 text 生命周期仍由调用者持有。
[[nodiscard]] ProgramFile ParseProgram(std::vector<Token> tokens, CheckBudget* budget = nullptr);

struct FunctionBody {
    std::vector<VarDecl> locals;
    std::vector<std::int32_t> statements;
    SyntaxPool pool;
    std::vector<SyntaxError> errors;
};

[[nodiscard]] FunctionBody ParseFunctionBody(std::span<const Token> tokens, const FunctionDecl& function, bool vjass = false,
    CheckBudget* budget = nullptr);

// 记号 i 是否位于行首（前一个记号为换行或 //# 注释）。
[[nodiscard]] bool AtLineStart(std::span<const Token> tokens, std::size_t index) noexcept;

} // namespace War3Map::Jass::detail
