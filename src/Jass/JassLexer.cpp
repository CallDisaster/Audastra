#include "JassLexer.h"
#include "JassCheckBudget.h"

#include <algorithm>
#include <array>
#include <utility>

namespace War3Map::Jass::detail {

LexerSource::LexerSource(std::string_view text, std::optional<Text::SourceEncoding> encoding)
    : text_(text), encoding_(encoding) {
    if (encoding && *encoding != Text::SourceEncoding::Utf8 && *encoding != Text::SourceEncoding::Gbk) {
        Error("SOURCE_ENCODING_INVALID", "来源编码枚举无效；不猜测编码。", 0, 0);
    } else if (encoding && text.size() > 16 * 1024 * 1024) {
        Error("SOURCE_ENCODING_BUDGET", "来源文本超过16MiB词法读取预算。", 0, 0);
    }
}

void LexerSource::Error(std::string code, std::string message, std::size_t at, std::size_t length) {
    if (encoding_ && Good()) {
        diagnostics_.push_back({DiagnosticSeverity::Error, "SourceLexer", std::move(code), std::move(message),
            OriginSpan{{}, at, length, {}}});
    }
}

Text::SourceScalar LexerSource::Scalar(std::size_t at) {
    if (!Good() || at >= text_.size()) return {};
    const auto first = static_cast<unsigned char>(text_[at]);
    if (!encoding_ || first < 0x80) {
        if (encoding_ && (first == 0 || first == 0x1A)) {
            Error("SOURCE_LEX_CONTROL", "来源包含NUL或Ctrl-Z控制字符；不截断后续原字节。", at, 1);
            return {};
        }
        return {first, static_cast<std::uint32_t>(at), 1};
    }
    if (cached_ && cached_->offset == at) return *cached_;
    auto scalar = Text::ReadSourceScalar(text_, static_cast<std::uint32_t>(at), *encoding_);
    if (!scalar.Succeeded()) {
        diagnostics_ = std::move(scalar.diagnostics);
        return {};
    }
    cached_ = *scalar.value;
    return *cached_;
}

std::size_t LexerSource::Width(std::size_t at) { return Scalar(at).bytes; }

std::size_t LexerSource::IdentifierWidth(std::size_t at, bool first, bool dollar) {
    const auto scalar = Scalar(at);
    const auto c = scalar.codepoint;
    const bool ascii = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
        || (!first && c >= '0' && c <= '9') || (dollar && c == '$');
    const bool han = encoding_ && ((c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF));
    return ascii || han ? scalar.bytes : 0;
}

namespace {

constexpr std::array<std::pair<std::string_view, Keyword>, 31> kKeywords = {{
    {"globals", Keyword::Globals}, {"endglobals", Keyword::EndGlobals}, {"type", Keyword::Type},
    {"extends", Keyword::Extends}, {"constant", Keyword::Constant}, {"native", Keyword::Native},
    {"takes", Keyword::Takes}, {"returns", Keyword::Returns}, {"nothing", Keyword::Nothing},
    {"function", Keyword::Function}, {"endfunction", Keyword::EndFunction}, {"local", Keyword::Local},
    {"array", Keyword::Array}, {"set", Keyword::Set}, {"call", Keyword::Call}, {"if", Keyword::If},
    {"then", Keyword::Then}, {"elseif", Keyword::ElseIf}, {"else", Keyword::Else}, {"endif", Keyword::EndIf},
    {"loop", Keyword::Loop}, {"exitwhen", Keyword::ExitWhen}, {"endloop", Keyword::EndLoop},
    {"return", Keyword::Return}, {"not", Keyword::Not}, {"and", Keyword::And}, {"or", Keyword::Or},
    {"true", Keyword::True}, {"false", Keyword::False}, {"null", Keyword::Null}, {"debug", Keyword::Debug},
}};

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHexDigit(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

class Lexer {
public:
    Lexer(std::string_view text, const LexOptions& options, std::optional<Text::SourceEncoding> encoding = {},
        std::optional<std::size_t> maxTokens = {}, CheckBudget* budget = nullptr)
        : text_(text), options_(options), source_(text, encoding), maxTokens_(maxTokens), budget_(budget) {}

    Result<std::vector<Token>> Run() {
        std::vector<Token> tokens;
        if (budget_) budget_->Poll();
        if (!source_.Good()) return {std::nullopt, source_.TakeDiagnostics()};
        if (maxTokens_) {
            if (*maxTokens_ == 0) return TokenBudgetFailure(0, 0);
            // 长注释不应按源码字节预留大量记号；后续只在实际产生记号时增长。
            tokens.reserve((std::min)({text_.size() / 4 + 16, std::size_t{4096}, *maxTokens_}));
        } else {
            tokens.reserve(text_.size() / 4 + 16);
        }
        while (at_ < text_.size() && source_.Good()) {
            Tick();
            const char c = text_[at_];
            if (c == '\n' || c == '\r') {
                const std::size_t length = c == '\r' && Next(1) == '\n' ? 2 : 1;
                const auto token = Make(TokenKind::Newline, at_, length);
                if (!AppendToken(tokens, token)) return TokenBudgetFailure(token.offset, token.text.size());
                at_ += length;
                ++line_;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
                ++at_;
                continue;
            }
            if (c == '/' && Next(1) == '/') {
                const std::size_t start = at_;
                while (at_ < text_.size() && text_[at_] != '\n' && text_[at_] != '\r') {
                    Tick();
                    const auto width = source_.Width(at_);
                    if (!width) break;
                    at_ += width;
                }
                if (text_.substr(start).starts_with("//#")
                    || (options_.directiveComments && text_.substr(start).starts_with("//!"))) {
                    const auto token = Make(TokenKind::Pragma, start, at_ - start);
                    if (!AppendToken(tokens, token)) return TokenBudgetFailure(token.offset, token.text.size());
                }
                continue;
            }
            if (options_.blockComments && c == '/' && Next(1) == '*') {
                SkipBlockComment();
                continue;
            }
            // 预算已满时先拒绝，连下一记号的扫描也不继续。
            if (maxTokens_ && tokens.size() >= *maxTokens_) return TokenBudgetFailure(at_, 1);
            const auto token = Scan();
            if (!AppendToken(tokens, token)) return TokenBudgetFailure(token.offset, token.text.size());
        }
        if (!source_.Good()) return {std::nullopt, source_.TakeDiagnostics()};
        if (!AppendToken(tokens, Make(TokenKind::End, text_.size(), 0))) return TokenBudgetFailure(text_.size(), 0);
        return Result<std::vector<Token>>::Success(std::move(tokens));
    }

private:
    void Tick() { if (budget_ && (++ticks_ & 255u) == 0) budget_->Poll(); }

    Result<std::vector<Token>> TokenBudgetFailure(std::size_t offset, std::size_t length) const {
        auto result = Result<std::vector<Token>>::Failure(
            "SourceLexer", "SOURCE_LEX_TOKEN_BUDGET", "来源词法记号超过预算（包含末尾 End）；未返回部分记号。");
        result.diagnostics.back().origin = OriginSpan{{}, offset, length, {}};
        return result;
    }

    bool AppendToken(std::vector<Token>& tokens, Token token) const {
        if (maxTokens_) {
            if (tokens.size() >= *maxTokens_) return false;
            if (tokens.size() == tokens.capacity()) {
                // 在 push_back 前显式扩容，避免 vector 自行倍增越过调用方上限。
                const auto capacity = tokens.capacity();
                const auto growth = (std::min)((std::max)(capacity, std::size_t{1}), *maxTokens_ - capacity);
                tokens.reserve(capacity + growth);
            }
        }
        tokens.push_back(token);
        return true;
    }

    char Next(std::size_t delta) const { return at_ + delta < text_.size() ? text_[at_ + delta] : '\0'; }

    Token Make(TokenKind kind, std::size_t start, std::size_t length) const {
        Token token;
        token.text = text_.substr(start, length);
        token.kind = kind;
        token.line = line_;
        token.offset = static_cast<std::uint32_t>(start);
        return token;
    }

    void SkipBlockComment() {
        const auto start = at_;
        at_ += 2;
        while (at_ < text_.size() && !(text_[at_] == '*' && Next(1) == '/')) {
            Tick();
            if (text_[at_] == '\n' || (text_[at_] == '\r' && Next(1) != '\n')) ++line_;
            const auto width = source_.Width(at_);
            if (!width) return;
            at_ += width;
        }
        if (at_ == text_.size()) source_.Error("SOURCE_LEX_UNTERMINATED", "块注释没有结束。", start, at_ - start);
        at_ = at_ < text_.size() ? at_ + 2 : at_;
    }

    Token Scan() {
        const std::size_t start = at_;
        const std::uint32_t line = line_;
        const char c = text_[at_];
        TokenKind kind = TokenKind::Operator;
        if (const auto width = source_.IdentifierWidth(at_, true); width) {
            at_ += width;
            while (const auto nextWidth = source_.IdentifierWidth(at_, false)) { Tick(); at_ += nextWidth; }
            kind = TokenKind::Identifier;
        } else if (c == '$' && IsHexDigit(Next(1))) {
            ++at_;
            while (at_ < text_.size() && IsHexDigit(text_[at_])) { Tick(); ++at_; }
            kind = TokenKind::Integer;
        } else if (c == '0' && (Next(1) == 'x' || Next(1) == 'X') && IsHexDigit(Next(2))) {
            at_ += 2;
            while (at_ < text_.size() && IsHexDigit(text_[at_])) { Tick(); ++at_; }
            kind = TokenKind::Integer;
        } else if (IsDigit(c) || (c == '.' && IsDigit(Next(1)))) {
            kind = TokenKind::Integer;
            while (at_ < text_.size() && IsDigit(text_[at_])) { Tick(); ++at_; }
            if (at_ < text_.size() && text_[at_] == '.') {
                kind = TokenKind::Real;
                ++at_;
                while (at_ < text_.size() && IsDigit(text_[at_])) { Tick(); ++at_; }
            }
        } else if (c == '"') {
            kind = ScanString() ? TokenKind::String : TokenKind::Invalid;
        } else if (c == '\'') {
            kind = ScanRawcode() ? TokenKind::Rawcode : TokenKind::Invalid;
        } else {
            kind = ScanOperator() ? TokenKind::Operator : TokenKind::Invalid;
        }
        if (kind == TokenKind::Invalid) {
            source_.Error("SOURCE_LEX_TOKEN_INVALID", "无法识别的标量或未结束的字面量；未返回部分词法结果。", start, at_ - start);
        }
        Token token;
        token.text = text_.substr(start, at_ - start);
        token.kind = kind;
        token.line = line;
        token.offset = static_cast<std::uint32_t>(start);
        if (kind == TokenKind::Identifier) {
            token.keyword = KeywordOf(token.text);
            if (token.keyword != Keyword::None) token.kind = TokenKind::Keyword;
        }
        return token;
    }

    // 字符串可以跨行；未结束时整段到文件尾作为 Invalid。
    bool ScanString() {
        ++at_;
        while (at_ < text_.size()) {
            Tick();
            const char c = text_[at_];
            if (c == '"') {
                ++at_;
                return true;
            }
            if (c == '\\' && at_ + 1 < text_.size()) {
                if (text_[at_ + 1] == '\n' || (text_[at_ + 1] == '\r' && Next(2) != '\n')) ++line_;
                const auto width = source_.Width(at_ + 1);
                if (!width) return false;
                at_ += 1 + width;
                continue;
            }
            if (c == '\n' || (c == '\r' && Next(1) != '\n')) ++line_;
            const auto width = source_.Width(at_);
            if (!width) return false;
            at_ += width;
        }
        return false;
    }

    // 'x' 或 'wxyz'（反斜杠转义下一字符）；只在同一行内查找结束引号。
    bool ScanRawcode() {
        std::size_t at = at_ + 1;
        while (at < text_.size() && text_[at] != '\'' && text_[at] != '\n' && text_[at] != '\r') {
            Tick();
            if (text_[at] == '\\') {
                ++at;
                if (at >= text_.size()) break;
            }
            const auto width = source_.Width(at);
            if (!width) return false;
            at += width;
        }
        if (at >= text_.size() || text_[at] != '\'') {
            ++at_;
            return false;
        }
        at_ = at + 1;
        return true;
    }

    bool ScanOperator() {
        const char c = text_[at_];
        const char n = Next(1);
        if ((c == '=' || c == '!' || c == '<' || c == '>') && n == '=') {
            at_ += 2;
            return true;
        }
        at_ += std::max<std::size_t>(source_.Width(at_), 1);
        switch (c) {
        case '(': case ')': case '[': case ']': case ',': case '=': case '<': case '>':
        case '+': case '-': case '*': case '/': case '.':
            return true;
        default:
            return false;
        }
    }

    std::string_view text_;
    LexOptions options_;
    LexerSource source_;
    std::optional<std::size_t> maxTokens_;
    CheckBudget* budget_ = nullptr;
    std::size_t ticks_ = 0;
    std::size_t at_ = 0;
    std::uint32_t line_ = 1;
};

} // namespace

std::vector<Token> LexJass(std::string_view text, const LexOptions& options) {
    return std::move(*Lexer(text, options).Run().value);
}

Result<std::vector<Token>> LexJassBounded(std::string_view text, const LexOptions& options, std::size_t maxTokens, CheckBudget* budget) {
    return Lexer(text, options, {}, maxTokens, budget).Run();
}

Result<std::vector<Token>> LexJassBounded(std::string_view text, Text::SourceEncoding encoding, const LexOptions& options,
    std::size_t maxTokens, CheckBudget* budget) {
    return Lexer(text, options, encoding, maxTokens, budget).Run();
}

Result<std::vector<Token>> LexJass(std::string_view text, Text::SourceEncoding encoding, const LexOptions& options) {
    return Lexer(text, options, encoding).Run();
}

Keyword KeywordOf(std::string_view text) noexcept {
    if (text.size() < 2 || text.size() > 11) return Keyword::None;
    for (const auto& [word, keyword] : kKeywords) {
        if (word == text) return keyword;
    }
    return Keyword::None;
}

std::string_view KeywordText(Keyword keyword) noexcept {
    for (const auto& [word, value] : kKeywords) {
        if (value == keyword) return word;
    }
    return {};
}

bool ValidStringEscapes(std::string_view literal) noexcept {
    for (std::size_t i = 1; i + 1 < literal.size(); ++i) {
        if (literal[i] != '\\') continue;
        switch (literal[++i]) {
        case 'b': case 't': case 'n': case 'f': case 'r': case '"': case '\\':
            break;
        default:
            return false;
        }
    }
    return true;
}

bool ValidStringEscapes(std::string_view literal, Text::SourceEncoding encoding) {
    LexerSource source(literal, encoding);
    if (literal.size() < 2 || literal.front() != '"' || literal.back() != '"') return false;
    for (std::size_t at = 1; at + 1 < literal.size();) {
        if (literal[at] == '\\') {
            if (++at + 1 >= literal.size()) return false;
            const auto c = literal[at];
            if (c != 'b' && c != 't' && c != 'n' && c != 'f' && c != 'r' && c != '"' && c != '\\') return false;
        }
        const auto width = source.Width(at);
        if (!width || at + width >= literal.size()) return false;
        at += width;
    }
    return source.Good();
}

} // namespace War3Map::Jass::detail
