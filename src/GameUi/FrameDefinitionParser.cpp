#include "War3MapCore/GameUi/FrameDefinition.h"

#include "War3MapCore/Text/TextUtil.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace War3Map::GameUi {
namespace {

constexpr const char* kSubsystem = "GameUi";

constexpr std::array<std::string_view, 9> kPointNames{
    "TOPLEFT", "TOP", "TOPRIGHT", "LEFT", "CENTER", "RIGHT", "BOTTOMLEFT", "BOTTOM", "BOTTOMRIGHT"};

enum class TokenKind : std::uint8_t { Word, Quoted, Comma, LBrace, RBrace, Newline, End };

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    std::uint32_t line = 0;
    std::size_t offset = 0;
};

bool IsWordChar(char c) {
    return c != '"' && c != ',' && c != '{' && c != '}' && c != ' ' && c != '\t' && c != '\r' && c != '\n';
}

bool IsKeyword(std::string_view word, std::string_view lowerKeyword) {
    return word.size() == lowerKeyword.size() && Text::LowerAscii(word) == lowerKeyword;
}

// fdf 的逗号既分隔参数也结束属性；不带引号的参数只有数字、锚点名与 JUSTIFY 常量，
// 所以逗号之后出现的其他单词是同一行的下一个属性名。
bool StartsNextProperty(std::string_view word) {
    if (word.empty() || !std::isalpha(static_cast<unsigned char>(word.front()))) return false;
    if (ParseFramePoint(word)) return false;
    return !Text::LowerAscii(word).starts_with("justify");
}

class Parser {
public:
    Parser(std::string_view text, std::string path) : text_(text), path_(std::move(path)) {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB && static_cast<unsigned char>(text_[2]) == 0xBF) {
            text_.remove_prefix(3);
            base_ = 3;
        }
        Tokenize();
    }

    Result<FdfFile> Run() {
        FdfFile file;
        file.path = path_;
        while (Peek().kind != TokenKind::End) {
            const Token& token = Peek();
            switch (token.kind) {
            case TokenKind::Newline:
            case TokenKind::Comma:
                Next();
                break;
            case TokenKind::RBrace:
                Report(DiagnosticSeverity::Error, "GAMEUI_FDF_UNBALANCED_BRACE", token, "多余的 }");
                Next();
                break;
            case TokenKind::LBrace:
                Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_ANONYMOUS_BLOCK", token, "没有名字的块已跳过");
                SkipBlock();
                break;
            case TokenKind::Quoted:
                Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_STRAY_VALUE", token, "行首是字符串，已跳过该行");
                SkipLine();
                break;
            case TokenKind::Word:
                ParseTopLevelStatement(file);
                break;
            case TokenKind::End:
                break;
            }
        }
        Result<FdfFile> result;
        result.value.emplace(std::move(file));
        result.diagnostics = std::move(diagnostics_);
        return result;
    }

private:
    void Tokenize() {
        std::uint32_t line = 1;
        std::size_t i = 0;
        const std::size_t n = text_.size();
        while (i < n) {
            const char c = text_[i];
            if (c == '\n') {
                tokens_.push_back({TokenKind::Newline, {}, line, base_ + i});
                ++line;
                ++i;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r') {
                ++i;
                continue;
            }
            if (c == '/' && i + 1 < n && text_[i + 1] == '/') {
                while (i < n && text_[i] != '\n') ++i;
                continue;
            }
            if (c == '/' && i + 1 < n && text_[i + 1] == '*') {
                const std::size_t start = i;
                const std::uint32_t startLine = line;
                i += 2;
                while (i + 1 < n && !(text_[i] == '*' && text_[i + 1] == '/')) {
                    if (text_[i] == '\n') {
                        // 块注释里的换行仍然结束当前语句，保持行号与语句边界一致。
                        tokens_.push_back({TokenKind::Newline, {}, line, base_ + i});
                        ++line;
                    }
                    ++i;
                }
                if (i + 1 >= n) {
                    diagnostics_.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_FDF_UNTERMINATED_COMMENT",
                        startLine, start, "块注释没有结束"));
                    i = n;
                } else {
                    i += 2;
                }
                continue;
            }
            if (c == ',') {
                tokens_.push_back({TokenKind::Comma, {}, line, base_ + i});
                ++i;
                continue;
            }
            if (c == '{') {
                tokens_.push_back({TokenKind::LBrace, {}, line, base_ + i});
                ++i;
                continue;
            }
            if (c == '}') {
                tokens_.push_back({TokenKind::RBrace, {}, line, base_ + i});
                ++i;
                continue;
            }
            if (c == '"') {
                const std::size_t start = i;
                ++i;
                std::string value;
                while (i < n && text_[i] != '"' && text_[i] != '\n') {
                    value.push_back(text_[i]);
                    ++i;
                }
                if (i < n && text_[i] == '"') {
                    ++i;
                } else {
                    diagnostics_.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_FDF_UNTERMINATED_STRING",
                        line, start, "字符串在行尾前没有结束"));
                }
                if (!value.empty() && value.back() == '\r') value.pop_back();
                tokens_.push_back({TokenKind::Quoted, std::move(value), line, base_ + start});
                continue;
            }
            const std::size_t start = i;
            while (i < n && IsWordChar(text_[i])) {
                if (text_[i] == '/' && i + 1 < n && (text_[i + 1] == '/' || text_[i + 1] == '*')) break;
                ++i;
            }
            if (i == start) {
                ++i; // 理论上不会发生；防止死循环
                continue;
            }
            tokens_.push_back({TokenKind::Word, std::string(text_.substr(start, i - start)), line, base_ + start});
        }
        tokens_.push_back({TokenKind::End, {}, line, base_ + n});
    }

    const Token& Peek(std::size_t ahead = 0) const {
        const std::size_t index = position_ + ahead;
        return index < tokens_.size() ? tokens_[index] : tokens_.back();
    }

    const Token& Next() {
        const Token& token = Peek();
        if (position_ < tokens_.size() - 1) ++position_;
        return token;
    }

    Diagnostic MakeDiagnostic(DiagnosticSeverity severity, const char* code, std::uint32_t line, std::size_t offset,
        const std::string& message) const {
        return Diagnostic{severity, kSubsystem, code,
            path_ + " 第 " + std::to_string(line) + " 行：" + message,
            OriginSpan{path_, offset, 0, {}}};
    }

    void Report(DiagnosticSeverity severity, const char* code, const Token& token, const std::string& message) {
        diagnostics_.push_back(MakeDiagnostic(severity, code, token.line, token.offset, message));
    }

    void SkipLine() {
        while (Peek().kind != TokenKind::Newline && Peek().kind != TokenKind::End) {
            if (Peek().kind == TokenKind::LBrace) {
                SkipBlock();
                return;
            }
            if (Peek().kind == TokenKind::RBrace) return;
            Next();
        }
    }

    // 当前 token 为 {：跳过到配对的 }。
    void SkipBlock() {
        int depth = 0;
        while (Peek().kind != TokenKind::End) {
            const TokenKind kind = Next().kind;
            if (kind == TokenKind::LBrace) {
                ++depth;
            } else if (kind == TokenKind::RBrace) {
                if (--depth <= 0) return;
            }
        }
    }

    // 语句头之后（同一行或紧随的下一行）是否出现 {。
    std::optional<std::size_t> FindBlockOpen() const {
        std::size_t index = position_;
        while (index < tokens_.size()) {
            const TokenKind kind = tokens_[index].kind;
            if (kind == TokenKind::LBrace) return index;
            if (kind == TokenKind::RBrace || kind == TokenKind::End) return std::nullopt;
            if (kind == TokenKind::Newline) {
                std::size_t next = index;
                while (next < tokens_.size() && tokens_[next].kind == TokenKind::Newline) ++next;
                if (next < tokens_.size() && tokens_[next].kind == TokenKind::LBrace) return next;
                return std::nullopt;
            }
            ++index;
        }
        return std::nullopt;
    }

    static std::optional<DefinitionKind> BlockKind(std::string_view word) {
        if (IsKeyword(word, "frame")) return DefinitionKind::Frame;
        if (IsKeyword(word, "texture")) return DefinitionKind::Texture;
        if (IsKeyword(word, "string")) return DefinitionKind::String;
        if (IsKeyword(word, "layer")) return DefinitionKind::Layer;
        return std::nullopt;
    }

    void ParseTopLevelStatement(FdfFile& file) {
        const Token head = Next();
        if (IsKeyword(head.text, "includefile")) {
            if (Peek().kind == TokenKind::Quoted) {
                file.includes.push_back(Next().text);
            } else {
                Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_INCLUDE_INVALID", head, "IncludeFile 缺少文件路径");
            }
            SkipLine();
            return;
        }
        if (IsKeyword(head.text, "stringlist")) {
            const auto open = FindBlockOpen();
            if (!open) {
                Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_STRINGLIST_INVALID", head, "StringList 后缺少 {");
                SkipLine();
                return;
            }
            position_ = *open + 1;
            ParseStringList(file, head);
            return;
        }
        if (const auto kind = BlockKind(head.text)) {
            if (auto definition = ParseDefinition(*kind, head)) file.definitions.push_back(std::move(*definition));
            return;
        }
        Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_TOPLEVEL_UNKNOWN", head, "顶层不认识的语句“" + head.text + "”，已跳过");
        SkipLine();
    }

    void ParseStringList(FdfFile& file, const Token& head) {
        while (true) {
            const Token& token = Peek();
            if (token.kind == TokenKind::End) {
                Report(DiagnosticSeverity::Error, "GAMEUI_FDF_UNBALANCED_BRACE", head, "StringList 缺少结束的 }");
                return;
            }
            if (token.kind == TokenKind::RBrace) {
                Next();
                return;
            }
            if (token.kind == TokenKind::Newline || token.kind == TokenKind::Comma) {
                Next();
                continue;
            }
            if (token.kind == TokenKind::Word && Peek(1).kind == TokenKind::Quoted) {
                std::string key = Next().text;
                std::string value = Next().text;
                file.strings.emplace_back(std::move(key), std::move(value));
                // 逗号结束本条，同一行可继续写下一条。
                if (Peek().kind != TokenKind::Comma && Peek().kind != TokenKind::Newline &&
                    Peek().kind != TokenKind::RBrace && Peek().kind != TokenKind::End) {
                    Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_STRING_ENTRY_INVALID", Peek(), "StringList 条目后多余的内容已跳过");
                    while (Peek().kind != TokenKind::Comma && Peek().kind != TokenKind::Newline &&
                           Peek().kind != TokenKind::RBrace && Peek().kind != TokenKind::End) {
                        Next();
                    }
                }
                continue;
            }
            Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_STRING_ENTRY_INVALID", token, "StringList 中不是“键 \"值\"”的行已跳过");
            SkipLine();
        }
    }

    // head 为 Frame/Texture/String/Layer 关键字，当前位置在其后。
    std::optional<FdfDefinition> ParseDefinition(DefinitionKind kind, const Token& head) {
        const auto open = FindBlockOpen();
        if (!open) {
            Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_BLOCK_EXPECTED", head, head.text + " 后缺少 {，该行已跳过");
            SkipLine();
            return std::nullopt;
        }
        FdfDefinition definition;
        definition.kind = kind;
        definition.line = head.line;
        std::vector<std::string> positional;
        bool inheritsPending = false;
        while (position_ < *open) {
            const Token& token = Next();
            if (token.kind == TokenKind::Newline || token.kind == TokenKind::Comma) continue;
            if (token.kind == TokenKind::Word && IsKeyword(token.text, "inherits")) {
                inheritsPending = true;
                continue;
            }
            if (token.kind == TokenKind::Word && IsKeyword(token.text, "withchildren")) {
                if (!inheritsPending) {
                    Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_HEADER_INVALID", token, "WITHCHILDREN 必须跟在 INHERITS 后");
                }
                definition.inheritsWithChildren = true;
                continue;
            }
            if (token.kind == TokenKind::Quoted) {
                if (inheritsPending) {
                    definition.inherits = token.text;
                    inheritsPending = false;
                } else {
                    positional.push_back(token.text);
                }
                continue;
            }
            Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_HEADER_INVALID", token, "块头中不认识的内容“" + token.text + "”");
        }
        if (inheritsPending) {
            Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_HEADER_INVALID", head, "INHERITS 后缺少模板名");
        }
        switch (kind) {
        case DefinitionKind::Frame:
            if (positional.size() < 2) {
                Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_HEADER_INVALID", head, "Frame 需要类型和名字两个字符串");
            }
            if (!positional.empty()) definition.frameType = positional[0];
            if (positional.size() >= 2) definition.name = positional[1];
            break;
        case DefinitionKind::Texture:
        case DefinitionKind::String:
            if (!positional.empty()) definition.name = positional[0];
            break;
        case DefinitionKind::Layer:
            if (!positional.empty()) definition.frameType = positional[0];
            break;
        }
        if (positional.size() > (kind == DefinitionKind::Frame ? 2u : 1u)) {
            Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_HEADER_INVALID", head, "块头中多余的字符串已忽略");
        }
        Next(); // {
        ParseBody(definition, head);
        return definition;
    }

    void ParseBody(FdfDefinition& definition, const Token& head) {
        while (true) {
            const Token& token = Peek();
            switch (token.kind) {
            case TokenKind::End:
                Report(DiagnosticSeverity::Error, "GAMEUI_FDF_UNBALANCED_BRACE", head,
                    head.text + (definition.name.empty() ? std::string() : " \"" + definition.name + "\"") + " 缺少结束的 }");
                return;
            case TokenKind::RBrace:
                Next();
                return;
            case TokenKind::Newline:
            case TokenKind::Comma:
                Next();
                break;
            case TokenKind::LBrace:
                Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_ANONYMOUS_BLOCK", token, "没有名字的块已跳过");
                SkipBlock();
                break;
            case TokenKind::Quoted:
                Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_STRAY_VALUE", token, "行首是字符串，已跳过该行");
                SkipLine();
                break;
            case TokenKind::Word: {
                const Token keyword = Next();
                if (const auto kind = BlockKind(keyword.text); kind && FindBlockOpen()) {
                    if (auto child = ParseDefinition(*kind, keyword)) definition.children.push_back(std::move(*child));
                    break;
                }
                FdfProperty property;
                property.key = keyword.text;
                property.line = keyword.line;
                bool afterComma = false;
                while (true) {
                    const Token& value = Peek();
                    if (value.kind == TokenKind::Newline || value.kind == TokenKind::End || value.kind == TokenKind::RBrace) break;
                    if (value.kind == TokenKind::LBrace) {
                        Report(DiagnosticSeverity::Warning, "GAMEUI_FDF_UNEXPECTED_BLOCK", value,
                            "属性 " + property.key + " 后出现的块已跳过");
                        SkipBlock();
                        break;
                    }
                    if (value.kind == TokenKind::Word && afterComma && StartsNextProperty(value.text)) break;
                    Next();
                    afterComma = value.kind == TokenKind::Comma;
                    if (afterComma) continue;
                    property.values.push_back(FdfValue{value.text, value.kind == TokenKind::Quoted});
                }
                definition.properties.push_back(std::move(property));
                break;
            }
            }
        }
    }

    std::string_view text_;
    std::size_t base_ = 0;
    std::string path_;
    std::vector<Token> tokens_;
    std::size_t position_ = 0;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace

std::optional<FramePoint> ParseFramePoint(std::string_view text) {
    const std::string upper = Text::UpperAscii(text);
    for (std::size_t i = 0; i < kPointNames.size(); ++i) {
        if (upper == kPointNames[i]) return static_cast<FramePoint>(i);
    }
    return std::nullopt;
}

std::string_view FramePointName(FramePoint point) noexcept {
    return kPointNames[static_cast<std::size_t>(point)];
}

Result<FdfFile> ParseFdf(std::string_view text, std::string path) {
    Parser parser(text, std::move(path));
    return parser.Run();
}

std::vector<std::string> ParseToc(std::string_view text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    std::vector<std::string> files;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (!line.empty() && !line.starts_with("//")) files.emplace_back(line);
        if (end == text.size()) break;
        start = end + 1;
    }
    return files;
}

} // namespace War3Map::GameUi
