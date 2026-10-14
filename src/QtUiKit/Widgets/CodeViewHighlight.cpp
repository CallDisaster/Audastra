#include "Widgets/CodeViewHighlight.h"

#include <QSet>
#include <optional>
#include <QStringView>

namespace War3Map::UiKit {
namespace {
const QSet<QString>& Keywords(CodeLanguage language) {
    static const QSet<QString> jass = [] {
        QSet<QString> result;
        for (const auto& word : QStringLiteral("function endfunction globals endglobals if then elseif else endif loop endloop exitwhen set call return local takes returns nothing native type extends array and or not null true false constant debug").split(' ')) result.insert(word);
        return result;
    }();
    static const QSet<QString> vjass = [&] {
        auto result = jass;
        for (const auto& word : QStringLiteral("library endlibrary scope endscope struct endstruct interface endinterface module endmodule method endmethod private public static readonly stub operator implement initializer requires optional key delegate thistype").split(' ')) result.insert(word);
        return result;
    }();
    static const QSet<QString> lua = [] {
        QSet<QString> result;
        for (const auto& word : QStringLiteral("and break do else elseif end false for function goto if in local nil not or repeat return then true until while").split(' ')) result.insert(word);
        return result;
    }();
    return language == CodeLanguage::Lua ? lua : language == CodeLanguage::VJass ? vjass : jass;
}
int LongDelimiter(QStringView line, int start, std::stop_token stop) {
    if (start >= line.size() || line[start] != '[') return -1;
    int next = start + 1;
    while (next < line.size() && line[next] == '=') { if ((next & 1023) == 0 && stop.stop_requested()) return -2; ++next; }
    return next < line.size() && line[next] == '[' ? next - start - 1 : -1;
}
bool IdentifierStart(QChar value) { return value.isLetter() || value == '_'; }
bool IdentifierPart(QChar value) { return value.isLetterOrNumber() || value == '_'; }
struct Fold { QString kind; int line; };
}

QStringList CodeKeywords(CodeLanguage language) {
    auto words = Keywords(language).values();
    words.sort();
    return words;
}

CodeLexSnapshot ScanCode(const QString& text, CodeLanguage language, std::stop_token stop) {
    CodeLexSnapshot result;
    if (text.size() > 32 * 1024 * 1024) { result.budgetExceeded = true; result.reason = QStringLiteral("正文超过 32 Mi UTF-16 展示预算。"); return result; }
    const auto& keywords = Keywords(language);
    const bool lua = language == CodeLanguage::Lua;
    bool blockComment = false;
    int luaLong = -1;
    bool longComment = false, globals = false;
    QVector<Fold> folds;
    QVector<QPair<QChar, int>> bracketStack;
    int offset = 0, lineNumber = 0, references = 0, spans = 0;
    while (offset <= text.size()) {
        if (stop.stop_requested()) { result.cancelled = true; return result; }
        if (lineNumber >= 262144 || spans > 1048576 || result.brackets.size() > 262144 || bracketStack.size() > 262144 || folds.size() > 512) {
            result.budgetExceeded = true; result.reason = QStringLiteral("展示词法索引达到预算（行/片段/括号/折叠深度）；正文没有修改。"); return result;
        }
        const auto newline = text.indexOf('\n', offset);
        const int end = newline < 0 ? static_cast<int>(text.size()) : static_cast<int>(newline);
        const auto line = QStringView(text).mid(offset, end - offset);
        CodeLexLine parsed;
        QVector<QString> words;
        int nextPoll = 0;
        for (int i = 0; i < line.size();) {
            if (i >= nextPoll) {
                nextPoll = i + 4096;
                if (stop.stop_requested()) { result.cancelled = true; return result; }
                if (parsed.spans.size() + spans > 1048576 || bracketStack.size() > 262144 || result.brackets.size() > 262144) {
                    result.budgetExceeded = true; result.reason = QStringLiteral("展示词法片段或括号索引达到预算；正文没有修改。"); return result;
                }
            }
            const int begin = i;
            if (luaLong >= 0) {
                const QString closer = ']' + QString(luaLong, '=') + ']';
                const auto close = line.indexOf(closer, i);
                i = close < 0 ? static_cast<int>(line.size()) : static_cast<int>(close) + closer.size();
                parsed.spans.push_back({begin, i - begin, longComment ? CodeTokenKind::Comment : CodeTokenKind::String});
                if (close >= 0) luaLong = -1;
                continue;
            }
            if (blockComment) {
                const auto close = line.indexOf(QStringLiteral("*/"), i);
                i = close < 0 ? static_cast<int>(line.size()) : static_cast<int>(close) + 2;
                parsed.spans.push_back({begin, i - begin, CodeTokenKind::Comment});
                if (close >= 0) blockComment = false;
                continue;
            }
            if ((lua && line.mid(i, 2) == u"--") || (!lua && line.mid(i, 2) == u"//")) {
                const int level = lua ? LongDelimiter(line, i + 2, stop) : -1;
                if (level == -2) { result.cancelled = true; return result; }
                if (level >= 0) { luaLong = level; longComment = true; i += level + 4; parsed.spans.push_back({begin, i - begin, CodeTokenKind::Comment}); continue; }
                parsed.spans.push_back({i, static_cast<int>(line.size()) - i, CodeTokenKind::Comment});
                break;
            }
            if (!lua && line.mid(i, 2) == u"/*") { blockComment = true; i += 2; parsed.spans.push_back({begin, 2, CodeTokenKind::Comment}); continue; }
            const int longLevel = lua ? LongDelimiter(line, i, stop) : -1;
            if (longLevel == -2) { result.cancelled = true; return result; }
            if (longLevel >= 0) { luaLong = longLevel; longComment = false; i += longLevel + 2; parsed.spans.push_back({begin, i - begin, CodeTokenKind::String}); continue; }
            if (line[i] == '"' || (lua && line[i] == '\'') || (!lua && line[i] == '\'')) {
                const auto quote = line[i++];
                while (i < line.size()) {
                    if (i >= nextPoll) { nextPoll = i + 4096; if (stop.stop_requested()) { result.cancelled = true; return result; } }
                    if (line[i] == '\\') { i = qMin(i + 2, static_cast<int>(line.size())); continue; }
                    if (line[i++] == quote) break;
                }
                parsed.spans.push_back({begin, i - begin, CodeTokenKind::String});
                continue;
            }
            if (IdentifierStart(line[i])) {
                ++i;
                while (i < line.size() && (IdentifierPart(line[i]) || (lua && (line[i] == '.' || line[i] == ':') && i + 1 < line.size() && IdentifierStart(line[i + 1])))) {
                    if ((i & 1023) == 0 && stop.stop_requested()) { result.cancelled = true; return result; } ++i;
                }
                const auto word = line.mid(begin, i - begin).toString();
                words.push_back(word);
                if (keywords.contains(word)) parsed.spans.push_back({begin, i - begin, CodeTokenKind::Keyword});
                else if (references++ < 65536) result.references[word].push_back(offset + begin);
                else result.referenceIndexComplete = false;
                continue;
            }
            if (line[i].isDigit() || line[i] == '$') {
                ++i; while (i < line.size() && (line[i].isLetterOrNumber() || line[i] == '.')) { if ((i & 1023) == 0 && stop.stop_requested()) { result.cancelled = true; return result; } ++i; }
                parsed.spans.push_back({begin, i - begin, CodeTokenKind::Number});
                continue;
            }
            const QChar ch = line[i];
            if (QStringLiteral("([{ ").contains(ch) && ch != ' ') bracketStack.push_back({ch, offset + i});
            else if (QStringLiteral(")]}").contains(ch) && !bracketStack.isEmpty()) {
                const QString opens = QStringLiteral("([{");
                const QString closes = QStringLiteral(")]}");
                if (opens.indexOf(bracketStack.back().first) == closes.indexOf(ch)) {
                    const int other = bracketStack.takeLast().second;
                    result.brackets.insert(other, offset + i); result.brackets.insert(offset + i, other);
                }
            }
            ++i;
        }
        spans += parsed.spans.size();
        if (spans > 1048576 || result.brackets.size() > 262144 || bracketStack.size() > 262144 || folds.size() > 512) {
            result.budgetExceeded = true; result.reason = QStringLiteral("展示词法索引达到预算；正文没有修改。"); return result;
        }
        result.lines.push_back(std::move(parsed));
        // 声明索引与着色共享一次后台词法扫描，不在 UI 再扫描全文。
        if (!words.isEmpty()) {
            if (words.front() == "globals") globals = true;
            else if (words.front() == "endglobals") globals = false;
            else if (result.declarations.size() < 4096) {
                int marker = -1; QString kind = QStringLiteral("函数");
                const auto declarationWord = [&](const QString& word) {
                    const int index = words.indexOf(word);
                    return index >= 0 && (index == 0 || words[index - 1] == "private" || words[index - 1] == "public" || words[index - 1] == "static" || (lua && words[index - 1] == "local")) ? index : -1;
                };
                marker = declarationWord(QStringLiteral("function"));
                if (!lua && marker < 0) marker = declarationWord(QStringLiteral("method"));
                if (!lua && marker < 0) marker = declarationWord(QStringLiteral("native"));
                if (marker < 0 && words.front() == "local") { marker = lua ? 0 : 1; if (!lua && marker + 1 < words.size() && words[marker + 1] == "array") ++marker; kind = QStringLiteral("局部变量"); }
                if (!lua && marker < 0 && globals) { marker = words.front() == "constant" ? 1 : 0; if (marker + 1 < words.size() && words[marker + 1] == "array") ++marker; kind = QStringLiteral("全局变量"); }
                if (marker >= 0 && marker + 1 < words.size()) {
                    const auto name = words[marker + 1];
                    result.declarations.push_back({name, kind, line.toString().trimmed(), {}, QStringLiteral("当前正文词法声明；完整语义由检查器决定。"), lineNumber + 1, static_cast<int>(line.indexOf(name)) + 1});
                }
                // JASS 参数：takes 后按“类型 名称”成对出现，直到 returns；没有检查器时函数体内的参数也能着色。
                const int takes = !lua && kind == QStringLiteral("函数") && marker >= 0 ? static_cast<int>(words.indexOf(QStringLiteral("takes"))) : -1;
                for (int w = takes >= 0 ? takes + 2 : static_cast<int>(words.size()); w < words.size() && words[w - 1] != "returns" && words[w] != "returns"
                        && result.declarations.size() < 4096; w += 2)
                    result.declarations.push_back({words[w], QStringLiteral("参数"), line.toString().trimmed(), {}, QStringLiteral("当前正文词法声明；完整语义由检查器决定。"), lineNumber + 1, static_cast<int>(line.indexOf(words[w])) + 1});
            }
        }
        // 词法折叠只认结构词，字符串/注释里的伪关键字不参与；无配对的块不隐藏正文。
        for (int w = 0; w < words.size(); ++w) {
            const auto& word = words[w];
            QString closing, opening;
            if (lua) {
                if (word == "end") closing = QStringLiteral("end");
                if (word == "until") closing = QStringLiteral("repeat");
                if (word == "function" || word == "if" || word == "for" || word == "while") opening = QStringLiteral("end");
                if (word == "repeat") opening = QStringLiteral("repeat");
                if (word == "do" && !words.contains(QStringLiteral("for")) && !words.contains(QStringLiteral("while"))) opening = QStringLiteral("end");
            } else {
                if (word.startsWith("end")) closing = word.mid(3);
                if ((word == "function" && (w == 0 || words[w - 1] == "private" || words[w - 1] == "public"))
                    || word == "globals" || word == "if" || word == "loop"
                    || (language == CodeLanguage::VJass && QStringList{"library", "scope", "struct", "interface", "module", "method"}.contains(word))) opening = word;
            }
            if (!closing.isEmpty() && !folds.isEmpty() && folds.back().kind == closing) {
                const auto fold = folds.takeLast();
                if (fold.line < lineNumber) result.lines[fold.line].foldEnd = lineNumber;
            }
            if (!opening.isEmpty()) {
                if (folds.size() >= 512) { result.budgetExceeded = true; result.reason = QStringLiteral("折叠索引达到深度预算；正文没有修改。"); return result; }
                folds.push_back({opening, lineNumber});
            }
        }
        if (newline < 0) break;
        offset = end + 1; ++lineNumber;
    }
    return result;
}

CodeTokenKind CodeSymbolKindFor(const QString& kind, const QString& signature) {
    if (kind == QStringLiteral("原生函数")) return CodeTokenKind::Native;
    if (kind == QStringLiteral("函数")) return CodeTokenKind::Function;
    if (kind == QStringLiteral("类型")) return CodeTokenKind::Type;
    if (kind == QStringLiteral("局部变量")) return CodeTokenKind::Local;
    if (kind == QStringLiteral("参数")) return CodeTokenKind::Parameter;
    return signature.startsWith(QStringLiteral("constant ")) ? CodeTokenKind::Constant : CodeTokenKind::Global;
}

QColor CodeTokenColor(CodeTokenKind kind, bool light) {
    // Visual Studio 2022 深色/浅色主题的语义色；浅色版按纸色底调深到对比度不低于 4.5。
    switch (kind) {
    case CodeTokenKind::Keyword: return QColor(light ? "#0000ff" : "#569cd6");
    case CodeTokenKind::ControlKeyword: return QColor(light ? "#8f08c4" : "#d8a0df");
    case CodeTokenKind::String: return QColor(light ? "#a31515" : "#d69d85");
    case CodeTokenKind::Comment: return QColor(light ? "#006400" : "#57a64a");
    case CodeTokenKind::Number: return QColor(light ? "#06704a" : "#b5cea8");
    case CodeTokenKind::Type: return QColor(light ? "#1f6f8b" : "#4ec9b0");
    case CodeTokenKind::Function: case CodeTokenKind::Native: return QColor(light ? "#74531f" : "#dcdcaa");
    case CodeTokenKind::Local: case CodeTokenKind::Parameter: return QColor(light ? "#1f377f" : "#9cdcfe");
    case CodeTokenKind::Global: return QColor(light ? "#3b3b3b" : "#dadada");
    case CodeTokenKind::Constant: return QColor(light ? "#005a9e" : "#4fc1ff");
    case CodeTokenKind::Operator: return QColor(light ? "#3b3b3b" : "#b4b4b4");
    }
    return {};
}

namespace {
const QSet<QString>& ControlKeywords(CodeLanguage language) {
    static const QSet<QString> jass = [] {
        QSet<QString> result;
        for (const auto& word : QStringLiteral("if then elseif else endif loop endloop exitwhen return").split(' ')) result.insert(word);
        return result;
    }();
    static const QSet<QString> lua = [] {
        QSet<QString> result;
        for (const auto& word : QStringLiteral("if then elseif else end for while do repeat until return break goto in").split(' ')) result.insert(word);
        return result;
    }();
    return language == CodeLanguage::Lua ? lua : jass;
}
bool PrimitiveType(QStringView word) {
    return word == u"integer" || word == u"real" || word == u"boolean" || word == u"string"
        || word == u"handle" || word == u"code";
}
} // namespace

QList<QTextLayout::FormatRange> CodeFormats(const CodeLexLine& line, QStringView text,
    CodeLanguage language, const CodeSymbolKinds& symbols, const QPalette& palette) {
    QList<QTextLayout::FormatRange> result;
    const bool light = palette.color(QPalette::Base).lightnessF() > 0.5;
    const bool lua = language == CodeLanguage::Lua;
    const auto& control = ControlKeywords(language);
    const auto add = [&](int start, int length, CodeTokenKind kind) {
        QTextCharFormat format; format.setForeground(CodeTokenColor(kind, light));
        if (kind == CodeTokenKind::Keyword || kind == CodeTokenKind::ControlKeyword) format.setFontWeight(QFont::DemiBold);
        result.push_back({start, length, format});
    };
    // 声明位置的局部规则：takes 列表里“类型 名称”成对出现，local/constant/returns/extends 后紧跟类型。
    QString previousWord;
    bool inTakes = false, expectTypeInTakes = false, previousWasType = false;
    const auto firstWord = text.trimmed().left(text.trimmed().indexOf(' '));
    const bool lineLocal = firstWord == u"local", lineConstant = firstWord == u"constant";
    const auto classifyGap = [&](int from, int to) {
        for (int i = from; i < to;) {
            const QChar ch = text[i];
            if (ch.isLetter() || ch == '_') {
                const int begin = i;
                while (i < to && (text[i].isLetterOrNumber() || text[i] == '_')) ++i;
                const auto word = text.mid(begin, i - begin);
                int next = i; while (next < text.size() && (text[next] == ' ' || text[next] == '\t')) ++next;
                const bool call = next < text.size() && text[next] == '(';
                const auto found = symbols.constFind(word.toString());
                std::optional<CodeTokenKind> kind;
                if (found != symbols.constEnd()) kind = *found;
                else if (!lua && PrimitiveType(word)) kind = CodeTokenKind::Type;
                else if (call || previousWord == u"call" || previousWord == u"function" || previousWord == u"native"
                    || previousWord == u"method") kind = CodeTokenKind::Function;
                else if (!lua && inTakes) kind = expectTypeInTakes ? CodeTokenKind::Type : CodeTokenKind::Parameter;
                else if (!lua && (previousWord == u"local" || previousWord == u"constant" || previousWord == u"returns"
                    || previousWord == u"extends" || previousWord == u"type")) kind = CodeTokenKind::Type;
                else if (!lua && previousWasType)
                    kind = lineLocal ? CodeTokenKind::Local : lineConstant ? CodeTokenKind::Constant : CodeTokenKind::Global;
                else if (lua && previousWord == u"local") kind = CodeTokenKind::Local;
                if (kind) add(begin, i - begin, *kind);
                previousWasType = kind == CodeTokenKind::Type;
                if (inTakes) expectTypeInTakes = !expectTypeInTakes;
                previousWord = word.toString();
                continue;
            }
            if (ch.isDigit()) { ++i; continue; }
            if (ch == ',') { if (inTakes) expectTypeInTakes = true; previousWord.clear(); previousWasType = false; }
            if (QStringLiteral("+-*/=<>!%#~^&|.:").contains(ch)) {
                const int begin = i; while (i < to && QStringLiteral("+-*/=<>!%#~^&|").contains(text[i])) ++i;
                if (i == begin) ++i;
                add(begin, i - begin, CodeTokenKind::Operator);
                previousWord.clear(); previousWasType = false;
                continue;
            }
            ++i;
        }
    };
    int cursor = 0;
    for (const auto& span : line.spans) {
        if (span.start > cursor) classifyGap(cursor, qMin<int>(span.start, text.size()));
        CodeTokenKind kind = span.kind;
        if (kind == CodeTokenKind::Keyword) {
            const auto word = text.mid(span.start, span.length);
            if (control.contains(word.toString())) kind = CodeTokenKind::ControlKeyword;
            else if (!lua && word == u"nothing") kind = CodeTokenKind::Type;
            if (word == u"takes") { inTakes = true; expectTypeInTakes = true; }
            else if (word == u"returns") inTakes = false;
            previousWord = word.toString(); previousWasType = word == u"array" ? previousWasType : false;
        } else { previousWord.clear(); previousWasType = false; }
        add(span.start, span.length, kind);
        cursor = span.start + span.length;
    }
    if (cursor < text.size()) classifyGap(cursor, static_cast<int>(text.size()));
    return result;
}
} // namespace War3Map::UiKit
