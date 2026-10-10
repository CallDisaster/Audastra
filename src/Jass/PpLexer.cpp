#include "PpLexer.h"
#include "JassLexer.h"
#include "JassCheckBudget.h"

#include <array>
#include <algorithm>
#include <limits>

namespace War3Map::Jass::detail {
namespace {

// 最长匹配的多字符标点（C99 全集中本工具会遇到的部分；其余按单字符）。
constexpr std::array<std::string_view, 22> kPunctuators = {
    "...", "<<=", ">>=", "##", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>",
    "++", "--", "->", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
};

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

class Lexer {
public:
    explicit Lexer(std::string_view text, std::optional<Text::SourceEncoding> encoding = {}, CheckBudget* budget = nullptr)
        : text_(text), source_(text, encoding), budget_(budget), tokenLimit_(budget ? budget->RemainingTokens() : (std::numeric_limits<std::size_t>::max)()) {}

    Result<std::vector<PpToken>> Run() {
        std::vector<PpToken> tokens;
        if (!source_.Good()) return {std::nullopt, source_.TakeDiagnostics()};
        if (budget_) budget_->Poll();
        else tokens.reserve(text_.size() / 5 + 16);
        bool space = false;
        bool lineStart = true;
        while (at_ < text_.size() && source_.Good()) {
            Poll();
            const char c = text_[at_];
            // 换行：\r\n、\n，以及旧式 Mac 的单独 \r（与 Wave 一致）。
            if (c == '\n' || c == '\r') {
                const std::size_t length = c == '\r' && Next(1) == '\n' ? 2 : 1;
                Push(tokens, Make(PpKind::Newline, length, space, false));
                at_ += length;
                ++line_;
                space = false;
                lineStart = true;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
                ++at_;
                space = true;
                continue;
            }
            if (c == '\\' && (Next(1) == '\n' || Next(1) == '\r')) {
                at_ += Next(1) == '\r' && Next(2) == '\n' ? 3 : 2; // 续行
                ++line_;
                space = true;
                continue;
            }
            if (c == '/' && Next(1) == '/') {
                SkipLineComment();
                space = true;
                continue;
            }
            if (c == '/' && Next(1) == '*') {
                SkipBlockComment();
                space = true;
                continue;
            }
            Push(tokens, Token(space, lineStart));
            space = false;
            lineStart = false;
        }
        if (!source_.Good()) return {std::nullopt, source_.TakeDiagnostics()};
        PpToken end;
        end.kind = PpKind::End;
        end.offset = static_cast<std::uint32_t>(text_.size());
        end.line = line_;
        end.space = space;
        end.lineStart = lineStart;
        end.source = true;
        Push(tokens, end);
        return Result<std::vector<PpToken>>::Success(std::move(tokens));
    }

private:
    void Push(std::vector<PpToken>& tokens, PpToken token) {
        if (budget_) {
            budget_->ConsumeTokens(1);
            ReserveBounded(tokens, tokens.size() + 1, tokenLimit_, *budget_);
        }
        tokens.push_back(token);
    }
    void Poll() { if (budget_) budget_->PollOccasionally(ticks_++); }
    char Next(std::size_t delta) const { return at_ + delta < text_.size() ? text_[at_ + delta] : '\0'; }

    PpToken Make(PpKind kind, std::size_t length, bool space, bool lineStart) const {
        PpToken token;
        token.text = text_.substr(at_, length);
        token.kind = kind;
        token.space = space;
        token.lineStart = lineStart;
        token.source = true;
        token.line = line_;
        token.offset = static_cast<std::uint32_t>(at_);
        return token;
    }

    // // 注释到行尾；行尾的反斜杠使注释延续到下一行（C 语义，与 Wave 一致）。
    void SkipLineComment() {
        at_ += 2;
        while (at_ < text_.size()) {
            Poll();
            const char c = text_[at_];
            if (c == '\\' && (Next(1) == '\n' || Next(1) == '\r')) {
                at_ += Next(1) == '\r' && Next(2) == '\n' ? 3 : 2;
                ++line_;
                continue;
            }
            if (c == '\n' || c == '\r') return;
            const auto width = source_.Width(at_);
            if (!width) return;
            at_ += width;
        }
    }

    void SkipBlockComment() {
        const auto start = at_;
        at_ += 2;
        while (at_ < text_.size()) {
            Poll();
            if (text_[at_] == '*' && Next(1) == '/') {
                at_ += 2;
                return;
            }
            if (text_[at_] == '\n' || (text_[at_] == '\r' && Next(1) != '\n')) ++line_;
            const auto width = source_.Width(at_);
            if (!width) return;
            at_ += width;
        }
        source_.Error("SOURCE_LEX_UNTERMINATED", "块注释没有结束。", start, at_ - start);
    }

    PpToken Token(bool space, bool lineStart) {
        const std::size_t start = at_;
        const std::uint32_t line = line_;
        const char c = text_[at_];
        PpKind kind = PpKind::Punct;
        if (const auto width = source_.IdentifierWidth(at_, true, true); width) {
            kind = PpKind::Identifier;
            at_ += width;
            while (const auto nextWidth = source_.IdentifierWidth(at_, false, true)) { Poll(); at_ += nextWidth; }
        } else if (IsDigit(c) || (c == '.' && IsDigit(Next(1)))) {
            kind = PpKind::Number;
            ++at_;
            bool exponent = false;
            while (at_ < text_.size()) {
                Poll();
                const char d = text_[at_];
                if ((d == '+' || d == '-') && exponent) {
                    ++at_;
                    exponent = false;
                } else if (d == '.') {
                    ++at_;
                    exponent = false;
                } else if (const auto nextWidth = source_.IdentifierWidth(at_, false, true); nextWidth) {
                    at_ += nextWidth;
                    exponent = nextWidth == 1 && (d == 'e' || d == 'E' || d == 'p' || d == 'P');
                } else {
                    break;
                }
            }
        } else if (c == '"' && ScanQuoted('"', true)) {
            kind = PpKind::String;
        } else if (c == '\'' && ScanQuoted('\'', false)) {
            kind = PpKind::Char;
        } else {
            if (c == '\'' || c == '"') {
                source_.Error("SOURCE_LEX_UNTERMINATED", "字面量没有配对的结束引号。", start, 1);
            } else if (static_cast<unsigned char>(c) >= 0x80) {
                source_.Error("SOURCE_LEX_IDENTIFIER_UNSUPPORTED", "此标量不属于当前标识符支持范围。", start, source_.Width(start));
            }
            at_ += PunctuatorLength();
        }
        PpToken token;
        token.text = text_.substr(start, at_ - start);
        token.kind = kind;
        token.space = space;
        token.lineStart = lineStart;
        token.source = true;
        token.line = line;
        token.offset = static_cast<std::uint32_t>(start);
        return token;
    }

    // 扫描到配对的引号（反斜杠转义下一字符）；失败时位置不变。
    bool ScanQuoted(char quote, bool multiline) {
        std::size_t at = at_ + 1;
        std::uint32_t lines = 0;
        while (at < text_.size()) {
            Poll();
            const char c = text_[at];
            if (c == quote) {
                at_ = at + 1;
                line_ += lines;
                return true;
            }
            if (c == '\\' && at + 1 < text_.size()) {
                if (text_[at + 1] == '\n' || (text_[at + 1] == '\r' && (at + 2 >= text_.size() || text_[at + 2] != '\n'))) ++lines;
                const auto width = source_.Width(at + 1);
                if (!width) return false;
                at += 1 + width;
                continue;
            }
            if (c == '\n' || (c == '\r' && (at + 1 >= text_.size() || text_[at + 1] != '\n'))) {
                if (!multiline) return false;
                ++lines;
            }
            const auto width = source_.Width(at);
            if (!width) return false;
            at += width;
        }
        return false;
    }

    std::size_t PunctuatorLength() const {
        const auto rest = text_.substr(at_);
        for (const auto punct : kPunctuators) {
            if (rest.starts_with(punct)) return punct.size();
        }
        return 1;
    }

    std::string_view text_;
    LexerSource source_;
    std::size_t at_ = 0;
    std::uint32_t line_ = 1;
    CheckBudget* budget_ = nullptr;
    std::size_t tokenLimit_, ticks_ = 0;
};

} // namespace

Result<PpSplicedSource> SplicePpLines(std::string_view text, std::size_t maxStorageBytes,
    std::optional<Text::SourceEncoding> encoding, CheckBudget* budget) {
    if (budget) budget->Poll();
    constexpr std::size_t hard=64u*1024u*1024u;
    const auto failure=[](std::size_t at) {
        auto result=Result<PpSplicedSource>::Failure("Jass.Preprocess", "PP_SOURCE_STORAGE_LIMIT",
            "行续接逻辑原文或稀疏位置表超过有界存储预算。");
        result.diagnostics.back().origin=OriginSpan{{},at,0,{}};
        return result;
    };
    if (maxStorageBytes>hard || text.size()>(std::numeric_limits<std::uint32_t>::max)()) return failure(0);
    LexerSource source(text,encoding);
    if (!source.Good()) return {std::nullopt,source.TakeDiagnostics()};
    PpSplicedSource result;
    std::size_t at=0,copied=0;
    std::size_t ticks=0;
    while (at<text.size()) {
        if (budget) budget->PollOccasionally(ticks++);
        if (text[at]=='\\' && at+1<text.size() && (text[at+1]=='\n' || text[at+1]=='\r')) {
            if (result.splices.empty()) {
                if (text.size()>=maxStorageBytes) return failure(at);
                result.text.reserve(text.size());
                if (result.text.capacity()>=maxStorageBytes) return failure(at);
            }
            const auto maximum=(maxStorageBytes-result.text.capacity()-1)/sizeof(PpLineSplice);
            if (result.splices.size()==maximum) return failure(at);
            if (result.splices.size()==result.splices.capacity()) {
                result.splices.reserve((std::min)(maximum,(std::max)(std::size_t{1},result.splices.capacity()*2)));
                if (result.splices.capacity()>maximum) return failure(at);
            }
            result.text.append(text.substr(copied,at-copied));
            const std::size_t length=text[at+1]=='\r' && at+2<text.size() && text[at+2]=='\n' ? 3 : 2;
            result.splices.push_back({static_cast<std::uint32_t>(result.text.size()),
                static_cast<std::uint32_t>(at),static_cast<std::uint32_t>(length)});
            at+=length; copied=at;
        } else {
            const auto width=source.Width(at);
            if (!width) return {std::nullopt,source.TakeDiagnostics()};
            at+=width;
        }
    }
    if (!result.splices.empty()) result.text.append(text.substr(copied));
    return Result<PpSplicedSource>::Success(std::move(result));
}

std::string_view PpSplicedSource::Slice(std::string_view physical, std::uint32_t from, std::uint32_t to) const {
    if (splices.empty()) return physical.substr(from,to-from);
    const auto logical=[&](std::uint32_t offset) {
        auto found=std::upper_bound(splices.begin(),splices.end(),offset,
            [](std::uint32_t value,const PpLineSplice& splice) {return value<splice.physicalOffset;});
        if (found==splices.begin()) return offset;
        --found;
        return offset<found->physicalOffset+found->length ? found->logicalOffset :
            offset-(found->physicalOffset+found->length-found->logicalOffset);
    };
    const auto begin=logical(from);
    return std::string_view(text).substr(begin,logical(to)-begin);
}

void PpSplicedSource::MapOrigins(std::vector<PpToken>& tokens, CheckBudget* budget) const {
    std::size_t first=0,last=0;
    std::uint32_t before=0,through=0;
    for (auto& token:tokens) {
        if (budget) budget->Poll();
        const auto start=token.offset;
        const auto finish=start+static_cast<std::uint32_t>(token.text.size());
        while (first<splices.size() && splices[first].logicalOffset<=start) before+=splices[first++].length;
        if (last<first) {last=first;through=before;}
        while (last<splices.size() && splices[last].logicalOffset<finish) through+=splices[last++].length;
        token.offset=start+before;
        token.physicalLength=finish+through-token.offset;
        token.line+=static_cast<std::uint32_t>(first);
    }
}

std::vector<PpToken> LexPp(std::string_view text) {
    return std::move(*Lexer(text).Run().value);
}

Result<std::vector<PpToken>> LexPp(std::string_view text, Text::SourceEncoding encoding) {
    return Lexer(text, encoding).Run();
}

Result<std::vector<PpToken>> LexPpBounded(std::string_view text, CheckBudget& budget, std::optional<Text::SourceEncoding> encoding) {
    return Lexer(text, encoding, &budget).Run();
}

bool LexSingle(std::string_view spelling, PpKind& kind, CheckBudget* budget) {
    if (spelling.empty()) return false;
    auto bounded = budget ? LexPpBounded(spelling, *budget) : Result<std::vector<PpToken>>::Success(LexPp(spelling));
    if (!bounded.Succeeded()) { if (budget) budget->RejectLexical(std::move(bounded.diagnostics)); return false; }
    const auto& tokens = *bounded.value;
    // 恰好一个记号且无前导空白 / 注释（结尾 End 记号之外）。
    if (tokens.size() != 2 || tokens[0].space || tokens[0].text.size() != spelling.size()) return false;
    if (tokens[0].kind == PpKind::Newline) return false;
    kind = tokens[0].kind;
    return true;
}

Result<PpKind> LexSingle(std::string_view spelling, Text::SourceEncoding encoding) {
    const auto tokens = LexPp(spelling, encoding);
    if (!tokens.Succeeded()) return {std::nullopt, tokens.diagnostics};
    if (tokens.value->size() == 2 && !tokens.value->front().space
        && tokens.value->front().text.size() == spelling.size() && tokens.value->front().kind != PpKind::Newline) {
        return Result<PpKind>::Success(tokens.value->front().kind);
    }
    auto result = Result<PpKind>::Failure("SourceLexer", "SOURCE_LEX_SINGLE_TOKEN", "粘贴结果并非恰好一个完整记号。");
    result.diagnostics.back().origin = OriginSpan{{}, 0, spelling.size(), {}};
    return result;
}

bool WouldMerge(char a, char b) noexcept {
    if (IsIdentifierChar(a) && (IsIdentifierChar(b) || b == '.')) return true;
    if (a == '.' && IsDigit(b)) return true;
    switch (a) {
    case '/': return b == '/' || b == '*' || b == '=';
    case '*': return b == '/' || b == '=';
    case '=': case '!': case '%': case '^': return b == '=';
    case '<': return b == '<' || b == '=';
    case '>': return b == '>' || b == '=';
    case '-': return b == '-' || b == '>' || b == '=';
    case '+': return b == '+' || b == '=';
    case '&': return b == '&' || b == '=';
    case '|': return b == '|' || b == '=';
    case '#': return b == '#';
    default: return false;
    }
}

} // namespace War3Map::Jass::detail
