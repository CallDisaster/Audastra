#pragma once

#include "JassCheckBudget.h"

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

// JASS 语法树：表达式与语句存放在函数自己的池中，以下标互相引用（-1 表示无）。
// 文本均为指向源文本的视图，源文本须在语法树使用期间保持有效。
namespace War3Map::Jass::detail {

enum class ExprKind : std::uint8_t {
    Integer,     // text 为字面量原文
    Rawcode,
    Real,
    String,
    Boolean,
    Null,
    Name,        // 变量名（vJass 中也可能是类型名等，由翻译器解释）
    Index,       // left[right]；left 为 Name 或 Member
    Call,        // text(args)；left >= 0 时为 left.text(args)（vJass 方法调用）
    Member,      // left.text（vJass）
    FunctionRef, // function text
    Unary,       // op left
    Binary,      // left op right
    Paren,       // (left)
    Error,
};

enum class Operator : std::uint8_t {
    None, Plus, Minus, Multiply, Divide, Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual, And, Or, Not,
};

struct Expr {
    ExprKind kind = ExprKind::Error;
    Operator op = Operator::None;
    std::uint32_t line = 0;
    std::uint32_t begin = 0; // 源文本范围 [begin, end)
    std::uint32_t end = 0;
    std::string_view text;
    std::int32_t left = -1;
    std::int32_t right = -1;
    std::uint32_t firstArgument = 0; // Call：arguments 中的起始位置
    std::uint32_t argumentCount = 0;
};

enum class StmtKind : std::uint8_t { Set, Call, If, Loop, ExitWhen, Return, Error };

struct Stmt;

struct Branch {
    std::int32_t condition = -1; // else 分支为 -1
    std::uint32_t line = 0;
    std::vector<std::int32_t> body;
};

struct Stmt {
    StmtKind kind = StmtKind::Error;
    bool debug = false;       // debug 前缀
    std::uint32_t line = 0;
    std::uint32_t begin = 0;  // 源文本范围：首个记号到最后一个记号（复合语句含 endif / endloop）
    std::uint32_t end = 0;
    std::int32_t target = -1; // Set：左值（Name / Index / Member）
    std::int32_t value = -1;  // Set 右值、Call 的调用表达式、Return 值、ExitWhen 条件
    std::vector<Branch> branches; // If
    std::vector<std::int32_t> body; // Loop
};

struct VarDecl {
    std::string_view type; // 均指向源文本，可由指针求得偏移
    std::string_view name;
    bool constant = false;
    bool array = false;
    std::int32_t init = -1;
    std::uint32_t line = 0;
    std::uint32_t begin = 0; // 源文本范围：constant / 类型到最后一个记号
    std::uint32_t end = 0;
};

struct Param {
    std::string_view type;
    std::string_view name;
};

// 一个函数（或全局区）的表达式 / 语句池。
struct SyntaxPool {
    std::vector<Expr> expressions;
    std::vector<std::int32_t> arguments;
    std::vector<Stmt> statements;
    CheckBudget* budget = nullptr;
    std::vector<std::size_t> expressionDepths;
    std::vector<std::size_t> statementDepths;

    [[nodiscard]] std::size_t Size() const noexcept { return expressions.size() + statements.size() + arguments.size(); }

    // 临时参数表和最终参数边均先核预算再增长；旧翻译器入口无预算时保持原行为。
    void AddArgument(std::vector<std::int32_t>& temporary, std::int32_t index) {
        if (budget) {
            budget->CheckPool(Size(), temporary.size() + 1);
            ReserveBounded(temporary, temporary.size() + 1, budget->PoolLimit() - Size(), *budget);
        }
        temporary.push_back(index);
    }
    void AddArguments(Expr& call, const std::vector<std::int32_t>& values) {
        if (budget) {
            budget->CheckPool(Size(), values.size());
            budget->ConsumeAst(values.size());
            ReserveBounded(arguments, arguments.size() + values.size(), budget->PoolLimit(), *budget);
        }
        call.firstArgument = static_cast<std::uint32_t>(arguments.size());
        call.argumentCount = static_cast<std::uint32_t>(values.size());
        arguments.insert(arguments.end(), values.begin(), values.end());
    }

    [[nodiscard]] std::size_t ExpressionDepth(std::int32_t index) const {
        if (index < 0) return 0;
        if (static_cast<std::size_t>(index) >= expressionDepths.size()) budget->Fail(CheckFailure::InvalidTree);
        return expressionDepths[static_cast<std::size_t>(index)];
    }
    [[nodiscard]] std::size_t StatementDepth(std::int32_t index) const {
        if (index < 0) return 0;
        if (static_cast<std::size_t>(index) >= statementDepths.size()) budget->Fail(CheckFailure::InvalidTree);
        return statementDepths[static_cast<std::size_t>(index)];
    }

    std::int32_t Add(Expr expr) {
        if (budget) {
            budget->CheckPool(Size());
            auto depth = (std::max)(ExpressionDepth(expr.left), ExpressionDepth(expr.right));
            if (expr.firstArgument > arguments.size() || expr.argumentCount > arguments.size() - expr.firstArgument) {
                budget->Fail(CheckFailure::InvalidTree);
            }
            for (std::uint32_t i = 0; i < expr.argumentCount; ++i) {
                budget->PollOccasionally(i);
                depth = (std::max)(depth, ExpressionDepth(Argument(expr, i)));
            }
            budget->CheckDepth(++depth);
            budget->ConsumeAst();
            ReserveBounded(expressions, expressions.size() + 1, budget->PoolLimit(), *budget);
            ReserveBounded(expressionDepths, expressionDepths.size() + 1, budget->PoolLimit(), *budget);
            expressionDepths.push_back(depth);
        }
        expressions.push_back(expr);
        return static_cast<std::int32_t>(expressions.size() - 1);
    }
    std::int32_t Add(Stmt stmt) {
        if (budget) {
            budget->CheckPool(Size());
            auto depth = (std::max)(ExpressionDepth(stmt.target), ExpressionDepth(stmt.value));
            const auto bodyDepth = [&](const auto& body) {
                std::size_t value = 0;
                for (std::size_t i = 0; i < body.size(); ++i) {
                    budget->PollOccasionally(i);
                    value = (std::max)(value, StatementDepth(body[i]));
                }
                return value;
            };
            depth = (std::max)(depth, bodyDepth(stmt.body));
            for (const auto& branch : stmt.branches) {
                budget->Poll();
                depth = (std::max)({depth, ExpressionDepth(branch.condition), bodyDepth(branch.body)});
            }
            budget->CheckDepth(++depth);
            budget->ConsumeAst();
            ReserveBounded(statements, statements.size() + 1, budget->PoolLimit(), *budget);
            ReserveBounded(statementDepths, statementDepths.size() + 1, budget->PoolLimit(), *budget);
            statementDepths.push_back(depth);
        }
        statements.push_back(std::move(stmt));
        return static_cast<std::int32_t>(statements.size() - 1);
    }
    [[nodiscard]] const Expr& At(std::int32_t index) const { return expressions[static_cast<std::size_t>(index)]; }
    [[nodiscard]] const Stmt& StmtAt(std::int32_t index) const { return statements[static_cast<std::size_t>(index)]; }
    [[nodiscard]] std::int32_t Argument(const Expr& call, std::uint32_t i) const { return arguments[call.firstArgument + i]; }
};

} // namespace War3Map::Jass::detail
