#pragma once

#include "JassParser.h"
#include "JassTypes.h"
#include "JassSymbolCollector.h"

#include "War3MapCore/Jass/JassChecker.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// 检查器内部：顶层符号表（只读，供各函数并行检查共享）与表达式 / 语句检查。
namespace War3Map::Jass::detail {

struct GlobalSymbol {
    TypeId type = kErrorType;
    bool array = false;
    bool constant = false;
    std::uint32_t order = 0; // 全部源文件中的声明序号
    std::uint32_t line = 0;
    std::optional<SymbolKey> definition;
};

struct FunctionSymbol {
    std::vector<TypeId> params;
    std::vector<std::string_view> paramNames;
    TypeId returns = kNothing;
    bool constant = false;
    bool native = false;
    std::uint32_t order = 0;
    std::uint32_t file = 0;
    std::uint32_t line = 0;
    std::optional<SymbolKey> definition;
};

struct CheckContext {
    TypeTable types;
    std::unordered_map<std::string_view, GlobalSymbol> globals;
    std::unordered_map<std::string_view, FunctionSymbol> functions;
    std::vector<std::string> files;
};

// pjass 的开关：命令行默认值，可被函数前的 //# +flag / -flag 覆盖（只作用于该函数）。
struct CheckFlags {
    bool returnBug = false;
    bool shadow = false;
    bool filter = false;
    bool noSemanticErrors = false;
    bool noSyntaxErrors = false;
};

[[nodiscard]] CheckFlags ApplyPragmas(CheckFlags flags, const std::vector<std::string_view>& pragmas, CheckBudget* budget = nullptr);

struct Issue {
    std::uint32_t line = 0;
    std::string code;
    std::string message;
    bool semantic = true; // false 为语法错误
    std::optional<std::pair<std::uint32_t, std::uint32_t>> byteSpan; // 实际AST原字节offset/length；空值仅知整行。
};

// 一段代码（函数体或全局初始值）的检查器。只读共享 CheckContext。
class CodeChecker {
public:
    CodeChecker(const CheckContext& context, const SyntaxPool& pool, CheckFlags flags, std::vector<Issue>& issues,
        CheckBudget* budget = nullptr, SymbolCollector* symbols = nullptr)
        : context_(context), pool_(pool), flags_(flags), issues_(issues), budget_(budget), symbols_(symbols) {}

    // 可见范围：声明序号 < visibleBefore 的函数与全局可见；self 为当前函数名（可递归调用自身）。
    void SetScope(std::uint32_t visibleBefore, std::string_view self = {}) {
        visibleBefore_ = visibleBefore;
        self_ = self;
    }

    // 全局变量初始值：类型须能转为 type。
    void CheckInitializer(std::int32_t expr, TypeId type, std::string_view name, std::uint32_t line);
    // 函数体：参数、局部变量、语句、返回路径。
    void CheckFunction(const FunctionDecl& decl, const FunctionSymbol& symbol, const FunctionBody& body);

private:
    struct Local {
        TypeId type = kErrorType;
        bool array = false;
        bool initialized = true;
        bool declaring = false; // 正在检查自身初始值
        bool reported = false;  // 已报过未初始化
        bool param = false;
        std::optional<SymbolKey> definition;
    };

    TypeId Check(std::int32_t expr);
    TypeId CheckName(const Expr& expr);
    TypeId CheckIndex(const Expr& expr);
    TypeId CheckCall(const Expr& expr);
    TypeId CheckFunctionRef(const Expr& expr);
    TypeId CheckUnary(const Expr& expr);
    TypeId CheckBinary(const Expr& expr);
    void CheckLiteral(const Expr& expr);
    void CheckStatements(const std::vector<std::int32_t>& statements);
    void CheckStatement(const Stmt& stmt);
    void CheckSet(const Stmt& stmt);
    void CheckCondition(std::int32_t expr, std::string_view what);
    void CheckReturn(const Stmt& stmt);
    void Require(TypeId from, TypeId to, std::uint32_t line, const std::string& context, const Expr* origin = nullptr);
    [[nodiscard]] bool Returns(const std::vector<std::int32_t>& statements) const;
    [[nodiscard]] const FunctionSymbol* LookupFunction(std::string_view name, std::uint32_t line, bool reportMissing, const Expr* origin = nullptr);
    [[nodiscard]] const GlobalSymbol* LookupGlobal(std::string_view name) const;
    void Report(std::uint32_t line, std::string code, std::string message);
    void Report(const Expr& origin, std::string code, std::string message);
    [[nodiscard]] std::string TypeName(TypeId type) const { return DiagnosticText(context_.types.Name(type)); }

    const CheckContext& context_;
    const SyntaxPool& pool_;
    CheckFlags flags_;
    std::vector<Issue>& issues_;
    CheckBudget* budget_ = nullptr;
    SymbolCollector* symbols_ = nullptr;
    std::uint32_t visibleBefore_ = 0;
    std::string_view self_;
    std::unordered_map<std::string_view, Local> locals_;
    const FunctionSymbol* function_ = nullptr;
    std::int32_t lastStatement_ = -1; // 函数体最后一条语句（returnbug 只检查它）
    int loopDepth_ = 0;
};

} // namespace War3Map::Jass::detail
