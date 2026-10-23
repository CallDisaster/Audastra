#pragma once

#include "War3MapCore/Text/SourceEncoding.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// JASS / vJass 词法分析：自研检查器与 vJass 翻译共用。
// 注释不产生记号，只有 "//#" 开头的注释作为 Pragma 记号保留（pjass 的逐函数开关）。
namespace War3Map::Jass::detail {

class CheckBudget;

enum class TokenKind : std::uint8_t {
    Identifier,
    Keyword,
    Integer, // 十进制 / 八进制 / 十六进制（$ 或 0x）
    Rawcode, // 'a' 或 'abcd'
    Real,
    String,
    Operator,
    Newline,
    Pragma,  // //# +flag
    Invalid, // 无法识别的字符、未结束的字符串等
    End,
};

enum class Keyword : std::uint8_t {
    None,
    Globals, EndGlobals, Type, Extends, Constant, Native, Takes, Returns, Nothing,
    Function, EndFunction, Local, Array, Set, Call, If, Then, ElseIf, Else, EndIf,
    Loop, ExitWhen, EndLoop, Return, Not, And, Or, True, False, Null, Debug,
};

struct Token {
    std::string_view text;
    TokenKind kind = TokenKind::End;
    Keyword keyword = Keyword::None;
    std::uint32_t line = 0;   // 从 1 开始
    std::uint32_t offset = 0; // 在源文本中的字节偏移

    [[nodiscard]] bool Is(Keyword word) const noexcept { return kind == TokenKind::Keyword && keyword == word; }
    [[nodiscard]] bool IsOperator(std::string_view op) const noexcept { return kind == TokenKind::Operator && text == op; }
    [[nodiscard]] bool EndsLine() const noexcept { return kind == TokenKind::Newline || kind == TokenKind::End; }
};

struct LexOptions {
    bool blockComments = false; // vJass 的 /* */（纯 JASS 不支持）
    bool directiveComments = false; // 改名阶段保留 //! 指令，避免把未展开模板当普通注释漏过。
    // vJass/Zinc 的 : { } ; % ! & | && || += -= *= /= ++ -- ->：只做改名的阶段须原样跨过，语法交给后续编译器判定。
    bool vjassOperators = false;
};

// 对整段文本分词；结尾总有一个 End 记号。字符串可以跨行（WE 与 jasshelper 都会写出）。
[[nodiscard]] std::vector<Token> LexJass(std::string_view text, const LexOptions& options = {});

// 已编译脚本的有界字节词法；保留旧词法语义，不猜来源编码。
// maxTokens 包含末尾 End；超限不返回部分记号，分配和扩容均受此上限约束。
[[nodiscard]] Result<std::vector<Token>> LexJassBounded(
    std::string_view text, const LexOptions& options, std::size_t maxTokens, CheckBudget* budget = nullptr);
[[nodiscard]] Result<std::vector<Token>> LexJassBounded(
    std::string_view text, Text::SourceEncoding encoding, const LexOptions& options, std::size_t maxTokens,
    CheckBudget* budget = nullptr);

// 显式来源编码：严格验证全部标量，任何错误都不返回部分tokens；offset仍指向原字节。
// 首批标识符支持ASCII及CJK统一表意文字基本区/扩展A（U+4E00–9FFF/U+3400–4DBF）。
// 其他有效标量仅允许留在字符串、注释和rawcode中；不猜编码、不转码或归一化。
[[nodiscard]] Result<std::vector<Token>> LexJass(
    std::string_view text, Text::SourceEncoding encoding, const LexOptions& options = {});

[[nodiscard]] Keyword KeywordOf(std::string_view text) noexcept;
[[nodiscard]] std::string_view KeywordText(Keyword keyword) noexcept;

// 字符串字面量（含引号）中的转义是否都有效（\b \t \n \f \r \" \\）。
[[nodiscard]] bool ValidStringEscapes(std::string_view literal) noexcept;
[[nodiscard]] bool ValidStringEscapes(std::string_view literal, Text::SourceEncoding encoding);
// 字符串字面量（含引号）的值：按上述转义解码，其余字节原样；转义无效时为空。
[[nodiscard]] std::optional<std::string> StringLiteralValue(std::string_view literal);

// 两个内部lexer共用的按原字节读取器。空编码只保留旧逐字节行为；新模式有16MiB输入预算。
// 仅缓存最近一个标量，不保留整篇解码表，也不成为新的预处理/保存通道。
class LexerSource {
public:
    LexerSource(std::string_view text, std::optional<Text::SourceEncoding> encoding);
    [[nodiscard]] bool Good() const noexcept { return diagnostics_.empty(); }
    [[nodiscard]] std::size_t Width(std::size_t at);
    [[nodiscard]] std::size_t IdentifierWidth(std::size_t at, bool first, bool dollar = false);
    void Error(std::string code, std::string message, std::size_t at, std::size_t length);
    [[nodiscard]] std::vector<Diagnostic> TakeDiagnostics() { return std::move(diagnostics_); }

private:
    [[nodiscard]] Text::SourceScalar Scalar(std::size_t at);
    std::string_view text_;
    std::optional<Text::SourceEncoding> encoding_;
    std::optional<Text::SourceScalar> cached_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace War3Map::Jass::detail
