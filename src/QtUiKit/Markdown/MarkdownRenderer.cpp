#include "Markdown/MarkdownRenderer.h"

#include "Markdown/TexRenderer.h"

#include <QHash>
#include <QFontMetricsF>
#include <QRegularExpression>
#include <QStringList>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QPainter>
#include <QTextCursor>
#include <QTextFrame>
#include <QTextTable>
#include <QTextList>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <vector>

// 规则来源：DSH ui-primitives/markdown（micromark GFM + math + cjkFriendlyStrong）。
// Qt 没有浏览器内核，这里用原生解析生成 Qt 富文本子集 HTML；块级按行扫描，行内按
// CommonMark 定界符栈处理强调。公式交给 TexRenderer 排成图片。
namespace War3Map::UiKit {
namespace {

QString Esc(const QString& text) { return text.toHtmlEscaped(); }
QString Css(const QColor& color) { return color.name(QColor::HexRgb); }

int IndentOf(const QString& line) {
    int width = 0;
    for (const QChar ch : line) {
        if (ch == u' ') ++width;
        else if (ch == u'\t') width += 4 - width % 4;
        else break;
    }
    return width;
}

QString StripIndent(const QString& line, int count) {
    int removed = 0, i = 0;
    while (i < line.size() && removed < count) {
        if (line.at(i) == u' ') { ++removed; ++i; }
        else if (line.at(i) == u'\t') { removed += 4 - removed % 4; ++i; }
        else break;
    }
    return line.mid(i);
}

bool Blank(const QString& line) { return line.trimmed().isEmpty(); }

bool IsCjk(QChar ch) {
    switch (ch.script()) {
    case QChar::Script_Han: case QChar::Script_Hiragana: case QChar::Script_Katakana:
    case QChar::Script_Hangul: case QChar::Script_Bopomofo: return true;
    default: return false;
    }
}
bool IsPunct(QChar ch) { return ch.isPunct() || ch.isSymbol(); }
bool IsWide(QChar ch) {
    const auto c = ch.unicode();
    return IsCjk(ch) || (c >= 0x3000 && c <= 0x303f) || (c >= 0xff00 && c <= 0xffef);
}

const QRegularExpression& FenceRe() { static const QRegularExpression re(QStringLiteral(R"(^( {0,3})(`{3,}|~{3,})(.*)$)")); return re; }
const QRegularExpression& HeadingRe() { static const QRegularExpression re(QStringLiteral(R"(^ {0,3}(#{1,6})(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$)")); return re; }
const QRegularExpression& RuleRe() { static const QRegularExpression re(QStringLiteral(R"(^ {0,3}([-*_])(?:[ \t]*\1){2,}[ \t]*$)")); return re; }
const QRegularExpression& QuoteRe() { static const QRegularExpression re(QStringLiteral(R"(^ {0,3}> ?(.*)$)")); return re; }
const QRegularExpression& ListRe() { static const QRegularExpression re(QStringLiteral(R"(^( *)([-*+]|\d{1,9}[.)])(?:([ \t]+)(.*))?$)")); return re; }
const QRegularExpression& TaskRe() { static const QRegularExpression re(QStringLiteral(R"(^\[([ xX])\][ \t]+(.*)$)")); return re; }
const QRegularExpression& DelimiterRowRe() {
    static const QRegularExpression re(QStringLiteral(R"(^ {0,3}\|?[ \t]*:?-+:?[ \t]*(?:\|[ \t]*:?-+:?[ \t]*)*\|?[ \t]*$)"));
    return re;
}
const QRegularExpression& FootnoteDefRe() { static const QRegularExpression re(QStringLiteral(R"(^\[\^([^\]\s]+)\]:[ \t]*(.*)$)")); return re; }

struct Context {
    const MarkdownTheme& theme;
    bool math = false;
    const MathImageFactory& mathImage;
    QStringList footnoteOrder;
    QHash<QString, QString> footnotes;
    int inlinePixelSize = 0;
    int listDepth = 0;
};

// ---------------------------------------------------------------- 代码高亮

struct Language {
    QStringList keywords;
    QStringList literals;
    QString line;            // 行注释
    bool block = false;      // /* */ 注释
    bool hashComment = false;
    bool json = false;
};

Language LanguageFor(const QString& info) {
    const QString name = info.toLower();
    Language lang;
    if (name == QStringLiteral("json") || name == QStringLiteral("jsonc")) { lang.json = true; lang.literals = {QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("null")}; lang.line = QStringLiteral("//"); return lang; }
    if (name == QStringLiteral("jass") || name == QStringLiteral("vjass") || name == QStringLiteral("zinc") || name == QStringLiteral("j")) {
        lang.keywords = QStringLiteral("function endfunction takes returns nothing local set call if then else elseif endif loop endloop exitwhen return globals endglobals native constant type extends array and or not library endlibrary struct endstruct method endmethod static private public requires needs uses initializer scope endscope module endmodule implement interface endinterface debug thistype this").split(u' ');
        lang.literals = {QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("null")};
        lang.line = QStringLiteral("//"); lang.block = true; return lang;
    }
    if (name == QStringLiteral("lua")) {
        lang.keywords = QStringLiteral("and break do else elseif end for function goto if in local not or repeat return then until while").split(u' ');
        lang.literals = {QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("nil")};
        lang.line = QStringLiteral("--"); return lang;
    }
    if (name == QStringLiteral("python") || name == QStringLiteral("py")) {
        lang.keywords = QStringLiteral("and as assert async await break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield").split(u' ');
        lang.literals = {QStringLiteral("True"), QStringLiteral("False"), QStringLiteral("None")};
        lang.hashComment = true; return lang;
    }
    if (name == QStringLiteral("bash") || name == QStringLiteral("sh") || name == QStringLiteral("shell") || name == QStringLiteral("powershell")
        || name == QStringLiteral("ps1") || name == QStringLiteral("ini") || name == QStringLiteral("toml") || name == QStringLiteral("yaml") || name == QStringLiteral("yml")) {
        lang.keywords = QStringLiteral("if then else elif fi for in do done while case esac function return export local foreach param").split(u' ');
        lang.literals = {QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("null"), QStringLiteral("$true"), QStringLiteral("$false")};
        lang.hashComment = true; return lang;
    }
    if (name.isEmpty() || name == QStringLiteral("text") || name == QStringLiteral("txt") || name == QStringLiteral("plain")) return lang;
    // C/C++/JS/TS/C#/Java/Rust/Go 共用
    lang.keywords = QStringLiteral("auto break case catch class const constexpr continue default delete do else enum explicit export extern for friend function goto if import inline interface let namespace new operator private protected public return static struct switch template this throw try typedef typename union using var virtual void volatile while async await yield fn impl mut pub use match loop where trait type package func go defer range chan select int float double char bool long short unsigned signed").split(u' ');
    lang.literals = {QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("null"), QStringLiteral("nullptr"), QStringLiteral("undefined"), QStringLiteral("None")};
    lang.line = QStringLiteral("//"); lang.block = true;
    return lang;
}

QString Highlight(const QString& code, const QString& info, bool dark) {
    const Language lang = LanguageFor(info);
    if (lang.keywords.isEmpty() && lang.literals.isEmpty() && !lang.json) return Esc(code);
    const QString keyword = dark ? QStringLiteral("#c792ea") : QStringLiteral("#8e44ad");
    const QString string = dark ? QStringLiteral("#a5d6a7") : QStringLiteral("#2e7d32");
    const QString number = dark ? QStringLiteral("#f2b482") : QStringLiteral("#b35c00");
    const QString comment = dark ? QStringLiteral("#7c8783") : QStringLiteral("#7a8380");
    const QString literal = dark ? QStringLiteral("#7fd6c2") : QStringLiteral("#00796b");
    const QString key = dark ? QStringLiteral("#8ec5ff") : QStringLiteral("#1f5fae");
    const auto span = [](const QString& color, const QString& text, bool italic = false) {
        return QStringLiteral("<span style=\"color:%1;%3\">%2</span>").arg(color, Esc(text), italic ? QStringLiteral(" font-style:italic;") : QString());
    };
    QString out;
    const int n = static_cast<int>(code.size());
    int i = 0;
    while (i < n) {
        const QChar c = code.at(i);
        if (!lang.line.isEmpty() && code.mid(i, lang.line.size()) == lang.line) {
            const int end = code.indexOf(u'\n', i);
            const int stop = end < 0 ? n : end;
            out += span(comment, code.mid(i, stop - i), true);
            i = stop;
            continue;
        }
        if (lang.hashComment && c == u'#') {
            const int end = code.indexOf(u'\n', i);
            const int stop = end < 0 ? n : end;
            out += span(comment, code.mid(i, stop - i), true);
            i = stop;
            continue;
        }
        if (lang.block && c == u'/' && i + 1 < n && code.at(i + 1) == u'*') {
            const int end = code.indexOf(QStringLiteral("*/"), i + 2);
            const int stop = end < 0 ? n : end + 2;
            out += span(comment, code.mid(i, stop - i), true);
            i = stop;
            continue;
        }
        if (c == u'"' || c == u'\'' || c == u'`') {
            int k = i + 1;
            while (k < n && code.at(k) != c && code.at(k) != u'\n') { if (code.at(k) == u'\\') ++k; ++k; }
            const int stop = std::min(n, k + 1);
            QString text = code.mid(i, stop - i);
            bool isKey = false;
            if (lang.json && c == u'"') {
                int j = stop;
                while (j < n && (code.at(j) == u' ' || code.at(j) == u'\t')) ++j;
                isKey = j < n && code.at(j) == u':';
            }
            out += span(isKey ? key : string, text);
            i = stop;
            continue;
        }
        if (c.isDigit() || (c == u'-' && lang.json && i + 1 < n && code.at(i + 1).isDigit())) {
            const bool wordStart = i == 0 || !(code.at(i - 1).isLetterOrNumber() || code.at(i - 1) == u'_');
            if (wordStart) {
                int k = i + 1;
                while (k < n && (code.at(k).isLetterOrNumber() || code.at(k) == u'.' || code.at(k) == u'_')) ++k;
                out += span(number, code.mid(i, k - i));
                i = k;
                continue;
            }
        }
        if (c.isLetter() || c == u'_' || c == u'$') {
            int k = i + 1;
            while (k < n && (code.at(k).isLetterOrNumber() || code.at(k) == u'_')) ++k;
            const QString word = code.mid(i, k - i);
            if (lang.keywords.contains(word)) out += span(keyword, word);
            else if (lang.literals.contains(word)) out += span(literal, word);
            else out += Esc(word);
            i = k;
            continue;
        }
        out += Esc(QString(c));
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------- 行内

QString InlineCode(const QString& code, const Context& ctx) {
    const auto& theme = ctx.theme;
    const int size = ctx.inlinePixelSize > 0 ? ctx.inlinePixelSize : std::max(11, theme.pixelSize - 1);
    return QStringLiteral("<code style=\"font-family:'%1'; font-size:%2px; color:%3;\">%4</code>")
        .arg(theme.codeFamily).arg(size).arg(Css(theme.inlineCodeAccent), Esc(code));
}

QString MathHtml(const QString& tex, bool display, Context& ctx) {
    if (ctx.mathImage) {
        const QString html = ctx.mathImage(tex.trimmed(), display);
        if (!html.isEmpty()) return html;
    }
    return InlineCode(display ? QStringLiteral("$$") + tex + QStringLiteral("$$") : QLatin1Char('$') + tex + QLatin1Char('$'), ctx);
}

QString LinkHtml(const QString& url, const QString& label, const Context& ctx) {
    const QUrl parsed(url);
    const QString scheme = parsed.scheme().toLower();
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https") || scheme == QStringLiteral("mailto")) {
        return QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none;\">%3</a>")
            .arg(Esc(parsed.toString(QUrl::FullyEncoded)), Css(ctx.theme.link), label);
    }
    // 非网页链接（本地路径等）不可点击，只以链接色显示文字
    return QStringLiteral("<span style=\"color:%1;\">%2</span>").arg(Css(ctx.theme.link), label);
}

int MatchBracket(const QString& text, int open) {
    int depth = 0;
    for (int i = open; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch == u'\\') { ++i; continue; }
        if (ch == u'`') { const int close = text.indexOf(u'`', i + 1); if (close > 0) { i = close; continue; } }
        if (ch == u'[') ++depth;
        else if (ch == u']' && --depth == 0) return i;
    }
    return -1;
}

int MatchParen(const QString& text, int open) {
    int depth = 0;
    for (int i = open; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch == u'\\') { ++i; continue; }
        if (ch == u'(') ++depth;
        else if (ch == u')' && --depth == 0) return i;
        else if (ch == u'\n') return -1;
    }
    return -1;
}

struct Token {
    enum Kind { Text, Html, Delim };
    Kind kind = Text;
    QString text;
    QChar ch;
    int count = 0;
    int original = 0;
    bool canOpen = false;
    bool canClose = false;
    QString openTags;
    QString closeTags;
};

void ProcessEmphasis(std::vector<Token>& tokens) {
    for (std::size_t closer = 0; closer < tokens.size(); ++closer) {
        if (tokens[closer].kind != Token::Delim || !tokens[closer].canClose) continue;
        bool matched = true;
        while (tokens[closer].count > 0 && matched) {
            matched = false;
            for (std::size_t o = closer; o-- > 0;) {
                Token& opener = tokens[o];
                Token& close = tokens[closer];
                if (opener.kind != Token::Delim || opener.ch != close.ch || !opener.canOpen || opener.count == 0) continue;
                if (close.ch == u'~' && (opener.count < 2 || close.count < 2)) continue;
                if ((opener.canClose || close.canOpen) && (opener.original + close.original) % 3 == 0
                    && !(opener.original % 3 == 0 && close.original % 3 == 0)) continue;
                const int use = close.ch == u'~' ? 2 : (opener.count >= 2 && close.count >= 2 ? 2 : 1);
                const QString tag = close.ch == u'~' ? QStringLiteral("s") : use == 2 ? QStringLiteral("b") : QStringLiteral("i");
                opener.openTags.prepend(QLatin1Char('<') + tag + QLatin1Char('>'));
                close.closeTags.append(QStringLiteral("</") + tag + QLatin1Char('>'));
                opener.count -= use;
                close.count -= use;
                for (std::size_t k = o + 1; k < closer; ++k)
                    if (tokens[k].kind == Token::Delim) tokens[k].canOpen = tokens[k].canClose = false;
                matched = true;
                break;
            }
        }
    }
}

QString Inline(const QString& source, Context& ctx);

QString FootnoteRef(const QString& id, Context& ctx) {
    if (!ctx.footnoteOrder.contains(id)) ctx.footnoteOrder << id;
    const auto number = ctx.footnoteOrder.indexOf(id) + 1;
    return QStringLiteral("<sup style=\"color:%1;\">[%2]</sup>").arg(Css(ctx.theme.link)).arg(number);
}

QString Inline(const QString& source, Context& ctx) {
    std::vector<Token> tokens;
    QString pending;
    const auto flush = [&] {
        if (pending.isEmpty()) return;
        Token token; token.kind = Token::Text; token.text = pending;
        tokens.push_back(std::move(token));
        pending.clear();
    };
    const auto html = [&](const QString& text) {
        flush();
        Token token; token.kind = Token::Html; token.text = text;
        tokens.push_back(std::move(token));
    };
    const int n = static_cast<int>(source.size());
    int i = 0;
    while (i < n) {
        const QChar c = source.at(i);
        if (c == u'\\' && i + 1 < n) {
            const QChar next = source.at(i + 1);
            if (ctx.math && (next == u'(' || next == u'[')) {
                const QString close = next == u'(' ? QStringLiteral("\\)") : QStringLiteral("\\]");
                const int end = source.indexOf(close, i + 2);
                if (end > i + 2) { html(MathHtml(source.mid(i + 2, end - i - 2), next == u'[', ctx)); i = end + 2; continue; }
            }
            if (next.unicode() < 128 && IsPunct(next)) { pending += next; i += 2; continue; }
            pending += c; ++i;
            continue;
        }
        if (c == u'`') {
            int run = 0;
            while (i + run < n && source.at(i + run) == u'`') ++run;
            int k = i + run, found = -1;
            while (k < n) {
                if (source.at(k) == u'`') {
                    int other = 0;
                    while (k + other < n && source.at(k + other) == u'`') ++other;
                    if (other == run) { found = k; break; }
                    k += other;
                } else ++k;
            }
            if (found >= 0) {
                QString code = source.mid(i + run, found - i - run);
                code.replace(u'\n', u' ');
                if (code.size() >= 2 && code.startsWith(u' ') && code.endsWith(u' ') && !code.trimmed().isEmpty()) code = code.mid(1, code.size() - 2);
                html(InlineCode(code, ctx));
                i = found + run;
            } else { pending += QString(run, u'`'); i += run; }
            continue;
        }
        if (c == u'$' && ctx.math) {
            if (i + 1 < n && source.at(i + 1) == u'$') {
                const int end = source.indexOf(QStringLiteral("$$"), i + 2);
                if (end > i + 2) { html(MathHtml(source.mid(i + 2, end - i - 2), true, ctx)); i = end + 2; continue; }
                pending += QStringLiteral("$$"); i += 2;
                continue;
            }
            // 单个 $：开头后不是空白，结尾前不是空白、后面不是数字（避免把 $5 和 $6 当公式）
            if (i + 1 < n && !source.at(i + 1).isSpace()) {
                int k = i + 1, found = -1;
                while (k < n) {
                    if (source.at(k) == u'\\') { k += 2; continue; }
                    if (source.at(k) == u'\n' && k + 1 < n && source.at(k + 1) == u'\n') break;
                    if (source.at(k) == u'$') {
                        if (!source.at(k - 1).isSpace() && !(k + 1 < n && source.at(k + 1).isDigit())) found = k;
                        break;
                    }
                    ++k;
                }
                if (found > i + 1) { html(MathHtml(source.mid(i + 1, found - i - 1), false, ctx)); i = found + 1; continue; }
            }
            pending += c; ++i;
            continue;
        }
        if (c == u'*' || c == u'_' || c == u'~') {
            int run = 0;
            while (i + run < n && source.at(i + run) == c) ++run;
            const QChar before = i > 0 ? source.at(i - 1) : QChar(u' ');
            const QChar after = i + run < n ? source.at(i + run) : QChar(u' ');
            const bool beforeSpace = before.isSpace(), afterSpace = after.isSpace();
            const bool beforePunct = IsPunct(before), afterPunct = IsPunct(after);
            const bool left = !afterSpace && (!afterPunct || beforeSpace || beforePunct);
            const bool right = !beforeSpace && (!beforePunct || afterSpace || afterPunct);
            Token token;
            token.kind = Token::Delim; token.ch = c; token.count = token.original = run;
            if (c == u'*') {
                // DSH cjkFriendlyStrong：标点后紧接中日韩文字时 ** 也可闭合；对称地允许中日韩文字后紧接标点时开启
                token.canOpen = left || (run >= 2 && afterPunct && !afterSpace && IsCjk(before));
                token.canClose = right || (run >= 2 && beforePunct && !beforeSpace && IsCjk(after));
            } else if (c == u'_') {
                token.canOpen = left && (!right || beforePunct);
                token.canClose = right && (!left || afterPunct);
            } else {
                if (run != 2) { pending += QString(run, c); i += run; continue; }
                token.canOpen = left; token.canClose = right;
            }
            flush();
            tokens.push_back(std::move(token));
            i += run;
            continue;
        }
        if (c == u'!' && i + 1 < n && source.at(i + 1) == u'[') {
            const int close = MatchBracket(source, i + 1);
            if (close > 0 && close + 1 < n && source.at(close + 1) == u'(') {
                const int end = MatchParen(source, close + 1);
                if (end > 0) {
                    const QString alt = source.mid(i + 2, close - i - 2);
                    html(QStringLiteral("<span style=\"color:%1;\">[图片：%2]</span>").arg(Css(ctx.theme.muted), Esc(alt)));
                    i = end + 1;
                    continue;
                }
            }
        }
        if (c == u'[') {
            if (i + 1 < n && source.at(i + 1) == u'^') {
                const int close = source.indexOf(u']', i + 2);
                if (close > i + 2) {
                    const QString id = source.mid(i + 2, close - i - 2);
                    if (!id.contains(u' ') && !(close + 1 < n && source.at(close + 1) == u':')) { html(FootnoteRef(id, ctx)); i = close + 1; continue; }
                }
            }
            const int close = MatchBracket(source, i);
            if (close > 0 && close + 1 < n && source.at(close + 1) == u'(') {
                const int end = MatchParen(source, close + 1);
                if (end > 0) {
                    QString target = source.mid(close + 2, end - close - 2).trimmed();
                    if (target.startsWith(u'<') && target.contains(u'>')) target = target.mid(1, target.indexOf(u'>') - 1);
                    else target = target.section(u' ', 0, 0);
                    html(LinkHtml(target, Inline(source.mid(i + 1, close - i - 1), ctx), ctx));
                    i = end + 1;
                    continue;
                }
            }
            pending += c; ++i;
            continue;
        }
        if (c == u'<') {
            const int close = source.indexOf(u'>', i + 1);
            if (close > 0) {
                const QString inner = source.mid(i + 1, close - i - 1);
                if ((inner.startsWith(QStringLiteral("http://")) || inner.startsWith(QStringLiteral("https://"))
                        || inner.startsWith(QStringLiteral("mailto:"))) && !inner.contains(u' ')) {
                    html(LinkHtml(inner, Esc(inner), ctx));
                    i = close + 1;
                    continue;
                }
            }
            pending += c; ++i;
            continue;
        }
        if ((c == u'h' || c == u'H') && (i == 0 || !source.at(i - 1).isLetterOrNumber())
            && (source.mid(i, 7).compare(QStringLiteral("http://"), Qt::CaseInsensitive) == 0
                || source.mid(i, 8).compare(QStringLiteral("https://"), Qt::CaseInsensitive) == 0)) {
            int k = i;
            while (k < n && !source.at(k).isSpace() && !IsWide(source.at(k)) && source.at(k) != u'<' && source.at(k) != u'>'
                && source.at(k) != u'"' && source.at(k) != u'`') ++k;
            while (k > i && QStringLiteral(".,;:!?'\"").contains(source.at(k - 1))) --k;
            if (k > i && source.at(k - 1) == u')' && source.mid(i, k - i).count(u'(') < source.mid(i, k - i).count(u')')) --k;
            const QString url = source.mid(i, k - i);
            if (url.size() > 8) { html(LinkHtml(url, Esc(url), ctx)); i = k; continue; }
        }
        if (c == u'\n') { html(QStringLiteral("<br/>")); ++i; continue; }
        pending += c;
        ++i;
    }
    flush();
    ProcessEmphasis(tokens);
    QString out;
    for (const auto& token : tokens) {
        switch (token.kind) {
        case Token::Text: out += Esc(token.text); break;
        case Token::Html: out += token.text; break;
        case Token::Delim: out += token.closeTags + Esc(QString(token.count, token.ch)) + token.openTags; break;
        }
    }
    return out;
}

// ---------------------------------------------------------------- 块级

QStringList SplitRow(const QString& line) {
    QString row = line.trimmed();
    if (row.startsWith(u'|')) row.remove(0, 1);
    if (row.endsWith(u'|') && !row.endsWith(QStringLiteral("\\|"))) row.chop(1);
    QStringList cells;
    QString cell;
    bool code = false;
    for (int i = 0; i < row.size(); ++i) {
        const QChar ch = row.at(i);
        if (ch == u'\\' && i + 1 < row.size() && row.at(i + 1) == u'|') { cell += u'|'; ++i; continue; }
        if (ch == u'`') code = !code;
        if (ch == u'|' && !code) { cells << cell.trimmed(); cell.clear(); continue; }
        cell += ch;
    }
    cells << cell.trimmed();
    return cells;
}

bool StartsBlock(const QString& line, const Context& ctx) {
    if (FenceRe().match(line).hasMatch() || HeadingRe().match(line).hasMatch() || RuleRe().match(line).hasMatch()
        || QuoteRe().match(line).hasMatch()) return true;
    const auto list = ListRe().match(line);
    if (list.hasMatch() && !list.captured(4).isEmpty()) {
        const QString marker = list.captured(2);
        if (!marker.front().isDigit() || marker.chopped(1) == QStringLiteral("1")) return true;
    }
    const QString trimmed = line.trimmed();
    return ctx.math && (trimmed.startsWith(QStringLiteral("$$")) || trimmed.startsWith(QStringLiteral("\\[")));
}

QString RenderBlocks(const QStringList& lines, Context& ctx, bool tight = false);

QString Paragraph(const QString& inlineHtml, const Context& ctx, bool tight) {
    if (tight) return inlineHtml;
    return QStringLiteral("<p style=\"margin-top:0px; margin-bottom:%1px;\">%2</p>")
        .arg(ctx.theme.pixelSize * 0.75, 0, 'f', 2).arg(inlineHtml);
}

QString CodeBlock(const QString& code, const QString& info, const Context& ctx) {
    const auto& theme = ctx.theme;
    QString label;
    if (!info.isEmpty()) {
        label = QStringLiteral("<p style=\"margin-top:0px; margin-bottom:6px; color:%1; font-size:%2px;\">%3</p>")
            .arg(Css(theme.muted)).arg(std::max(11, theme.pixelSize - 2)).arg(Esc(info));
    }
    return QStringLiteral("<table width=\"100%\" border=\"0\" cellspacing=\"0\" cellpadding=\"12\" style=\"margin-top:2px; margin-bottom:12px;\">"
                          "<tr><td bgcolor=\"%1\">%2<pre style=\"margin:0px; font-family:'%3'; font-size:%4px; color:%5; white-space:pre-wrap;\">%6</pre></td></tr></table>")
        .arg(Css(theme.codeBackground), label, theme.codeFamily).arg(std::max(11, theme.pixelSize - 1))
        .arg(Css(theme.codeText), Highlight(code, info, theme.dark));
}

QString Table(const QStringList& header, const QStringList& aligns, const std::vector<QStringList>& rows, Context& ctx) {
    const auto& theme = ctx.theme;
    QString html = QStringLiteral("<table border=\"1\" cellspacing=\"0\" cellpadding=\"7\" style=\"border-collapse:collapse; border-style:solid; border-color:%1; margin-top:2px; margin-bottom:12px;\">")
        .arg(Css(theme.border));
    const auto align = [&aligns](int column) {
        const QString a = column < aligns.size() ? aligns.at(column) : QString();
        return a.isEmpty() ? QString() : QStringLiteral(" align=\"%1\"").arg(a);
    };
    html += QStringLiteral("<tr>");
    for (int c = 0; c < header.size(); ++c)
        html += QStringLiteral("<th bgcolor=\"%1\"%2>%3</th>").arg(Css(theme.tableHeader), align(c), Inline(header.at(c), ctx));
    html += QStringLiteral("</tr>");
    for (const auto& row : rows) {
        html += QStringLiteral("<tr>");
        for (int c = 0; c < header.size(); ++c)
            html += QStringLiteral("<td%1>%2</td>").arg(align(c), c < row.size() ? Inline(row.at(c), ctx) : QString());
        html += QStringLiteral("</tr>");
    }
    return html + QStringLiteral("</table>");
}

QString Quote(const QString& inner, const Context& ctx) {
    // Qt 富文本没有单边边框，用一个窄色块单元格做引用竖线
    return QStringLiteral("<table border=\"0\" cellspacing=\"0\" cellpadding=\"0\" style=\"margin-top:2px; margin-bottom:10px;\">"
                          "<tr><td width=\"3\" bgcolor=\"%1\"></td><td style=\"padding-left:12px; color:%2;\">%3</td></tr></table>")
        .arg(Css(ctx.theme.quoteBar), Css(ctx.theme.muted), inner);
}

int RenderList(const QStringList& lines, int i, Context& ctx, QString& html) {
    const auto first = ListRe().match(lines.at(i));
    const int baseIndent = static_cast<int>(first.captured(1).size());
    const QString firstMarker = first.captured(2);
    const bool ordered = firstMarker.front().isDigit();
    const QChar kind = ordered ? firstMarker.back() : firstMarker.front();
    const int start = ordered ? firstMarker.chopped(1).toInt() : 1;
    struct Item { QStringList lines; int task = -1; };
    std::vector<Item> items;
    bool loose = false;
    const int n = static_cast<int>(lines.size());
    bool done = false;
    while (i < n && !done) {
        const auto match = ListRe().match(lines.at(i));
        if (!match.hasMatch() || RuleRe().match(lines.at(i)).hasMatch()) break;
        const int indent = static_cast<int>(match.captured(1).size());
        const QString marker = match.captured(2);
        const bool itemOrdered = marker.front().isDigit();
        if (indent < baseIndent || indent > baseIndent + 3 || itemOrdered != ordered
            || (itemOrdered ? marker.back() : marker.front()) != kind) break;
        const int spacing = std::clamp(static_cast<int>(match.captured(3).size()), 1, 4);
        const int contentIndent = indent + static_cast<int>(marker.size()) + spacing;
        Item item;
        QString text = match.captured(4);
        if (const auto task = TaskRe().match(text); task.hasMatch()) {
            item.task = task.captured(1) == QStringLiteral(" ") ? 0 : 1;
            text = task.captured(2);
        }
        item.lines << text;
        ++i;
        while (i < n) {
            const QString& line = lines.at(i);
            if (Blank(line)) {
                int k = i + 1;
                while (k < n && Blank(lines.at(k))) ++k;
                if (k < n && IndentOf(lines.at(k)) > baseIndent && !(ListRe().match(lines.at(k)).hasMatch() && IndentOf(lines.at(k)) <= baseIndent)) {
                    item.lines << QString();
                    i = k;
                    loose = true;
                    continue;
                }
                const auto next = k < n ? ListRe().match(lines.at(k)) : QRegularExpressionMatch();
                if (next.hasMatch() && IndentOf(lines.at(k)) == baseIndent) { loose = true; i = k; break; }
                i = k;
                done = true;
                break;
            }
            const int lineIndent = IndentOf(line);
            if (lineIndent > baseIndent) { item.lines << StripIndent(line, std::min(lineIndent, contentIndent)); ++i; continue; }
            if (ListRe().match(line).hasMatch() || StartsBlock(line, ctx)) break;
            item.lines << line.trimmed();        // 段落惰性续行
            ++i;
        }
        items.push_back(std::move(item));
    }
    // 列表用“标记列 + 内容列”的两列布局，不用 Qt 原生列表：原生标记按段落标记字体单独绘制，
    // 与 1.65 倍行距的正文基线对不齐（圆点下沉、序号被裁）。两列同字体同行距、顶端对齐即可对齐首行。
    const bool taskList = !ordered && std::all_of(items.begin(), items.end(), [](const Item& item) { return item.task >= 0; });
    const int depth = ++ctx.listDepth;
    const int lastNumber = start + static_cast<int>(items.size()) - 1;
    const int markerWidth = ordered ? (lastNumber >= 100 ? 34 : lastNumber >= 10 ? 28 : 22) : (taskList ? 24 : 18);
    static const QStringList bullets{QStringLiteral("•"), QStringLiteral("◦"), QStringLiteral("▪")};
    html += QStringLiteral("<table border=\"0\" cellspacing=\"0\" cellpadding=\"0\" style=\"margin-top:0px; margin-bottom:%1px;\">")
        .arg(depth > 1 ? 4 : 12);
    int number = start;
    for (const auto& item : items) {
        const QString content = RenderBlocks(item.lines, ctx, !loose);
        QString marker;
        if (item.task >= 0) {
            marker = QStringLiteral("<img src=\"forge-check:%1\" width=\"14\" height=\"14\" style=\"vertical-align:middle;\" title=\"%2\"/>")
                .arg(item.task).arg(item.task ? QStringLiteral("已完成") : QStringLiteral("未完成"));
        } else {
            marker = ordered ? QStringLiteral("%1%2").arg(number).arg(kind) : bullets.at((depth - 1) % bullets.size());
        }
        ++number;
        html += QStringLiteral("<tr><td width=\"%1\" valign=\"top\" style=\"color:%2; padding-right:8px;\">%3</td>"
                               "<td valign=\"top\" style=\"padding-bottom:%4px;\">%5</td></tr>")
            .arg(markerWidth).arg(Css(ctx.theme.muted), marker).arg(loose ? 8 : 4).arg(content);
    }
    html += QStringLiteral("</table>");
    --ctx.listDepth;
    return i;
}

QString RenderBlocks(const QStringList& lines, Context& ctx, bool tight) {
    QString html;
    const int n = static_cast<int>(lines.size());
    int i = 0;
    bool firstParagraph = true;
    while (i < n) {
        const QString& line = lines.at(i);
        if (Blank(line)) { ++i; continue; }
        if (const auto fence = FenceRe().match(line); fence.hasMatch()
            && !(fence.captured(2).front() == u'`' && fence.captured(3).contains(u'`'))) {
            const QString marker = fence.captured(2);
            const int indent = static_cast<int>(fence.captured(1).size());
            const QString info = fence.captured(3).trimmed().section(u' ', 0, 0);
            QStringList code;
            ++i;
            while (i < n) {
                const QString trimmed = lines.at(i).trimmed();
                if (IndentOf(lines.at(i)) <= 3 && trimmed.size() >= marker.size()
                    && trimmed == QString(trimmed.size(), marker.front())) { ++i; break; }
                code << StripIndent(lines.at(i), indent);
                ++i;
            }
            html += CodeBlock(code.join(u'\n'), info, ctx);
            continue;
        }
        const QString trimmed = line.trimmed();
        if (ctx.math && (trimmed.startsWith(QStringLiteral("$$")) || trimmed.startsWith(QStringLiteral("\\[")))) {
            const QString close = trimmed.startsWith(QStringLiteral("$$")) ? QStringLiteral("$$") : QStringLiteral("\\]");
            QString body = trimmed.mid(2);
            int j = i;
            bool closed = false;
            if (const int end = body.indexOf(close); end >= 0) { body = body.left(end); closed = true; ++j; }
            else {
                QStringList parts;
                if (!body.trimmed().isEmpty()) parts << body;
                ++j;
                while (j < n) {
                    const int stop = lines.at(j).indexOf(close);
                    if (stop >= 0) { parts << lines.at(j).left(stop); closed = true; ++j; break; }
                    parts << lines.at(j);
                    ++j;
                }
                body = parts.join(u'\n');
            }
            if (closed) {
                html += QStringLiteral("<p align=\"center\" style=\"margin-top:4px; margin-bottom:10px;\">%1</p>").arg(MathHtml(body, true, ctx));
                i = j;
                continue;
            }
        }
        if (const auto heading = HeadingRe().match(line); heading.hasMatch()) {
            const int level = static_cast<int>(heading.captured(1).size());
            const qreal scale = level == 1 ? 1.45 : level == 2 ? 1.28 : level == 3 ? 1.14 : 1.0;
            const int headingSize = qRound(ctx.theme.pixelSize * scale);
            const int previousInlineSize = ctx.inlinePixelSize;
            ctx.inlinePixelSize = headingSize;
            const QString headingHtml = Inline(heading.captured(2), ctx);
            ctx.inlinePixelSize = previousInlineSize;
            html += QStringLiteral("<h%1 style=\"font-size:%2px; font-weight:700; color:%5; margin-top:%3px; margin-bottom:8px;\">%4</h%1>")
                .arg(level).arg(headingSize).arg(firstParagraph ? 2 : 14).arg(headingHtml, Css(ctx.theme.headingText));
            ++i;
            firstParagraph = false;
            continue;
        }
        if (RuleRe().match(line).hasMatch()) {
            html += QStringLiteral("<hr style=\"margin-top:6px; margin-bottom:12px;\"/>");
            ++i;
            continue;
        }
        if (i + 1 < n && line.contains(u'|') && DelimiterRowRe().match(lines.at(i + 1)).hasMatch()) {
            const QStringList header = SplitRow(line);
            QStringList aligns;
            for (const QString& cell : SplitRow(lines.at(i + 1))) {
                const bool left = cell.startsWith(u':'), right = cell.endsWith(u':');
                aligns << (left && right ? QStringLiteral("center") : right ? QStringLiteral("right") : left ? QStringLiteral("left") : QString());
            }
            if (aligns.size() == header.size()) {
                std::vector<QStringList> rows;
                int j = i + 2;
                while (j < n && !Blank(lines.at(j)) && lines.at(j).contains(u'|')) rows.push_back(SplitRow(lines.at(j++)));
                html += Table(header, aligns, rows, ctx);
                i = j;
                continue;
            }
        }
        if (QuoteRe().match(line).hasMatch()) {
            QStringList inner;
            while (i < n) {
                const auto quote = QuoteRe().match(lines.at(i));
                if (quote.hasMatch()) { inner << quote.captured(1); ++i; }
                else if (!Blank(lines.at(i)) && !inner.isEmpty() && !Blank(inner.last()) && !StartsBlock(lines.at(i), ctx)) { inner << lines.at(i); ++i; }
                else break;
            }
            html += Quote(RenderBlocks(inner, ctx), ctx);
            continue;
        }
        if (const auto list = ListRe().match(line); list.hasMatch()) {
            i = RenderList(lines, i, ctx, html);
            continue;
        }
        if (const auto note = FootnoteDefRe().match(line); note.hasMatch()) {
            QString text = note.captured(2);
            ++i;
            while (i < n && !Blank(lines.at(i)) && IndentOf(lines.at(i)) >= 2) text += u'\n' + lines.at(i++).trimmed();
            ctx.footnotes.insert(note.captured(1), text);
            continue;
        }
        QStringList paragraph{trimmed};
        ++i;
        while (i < n && !Blank(lines.at(i)) && !StartsBlock(lines.at(i), ctx)
            && !(i + 1 < n && lines.at(i).contains(u'|') && DelimiterRowRe().match(lines.at(i + 1)).hasMatch())) {
            paragraph << lines.at(i).trimmed();
            ++i;
        }
        if (tight && !html.isEmpty() && !html.endsWith(QStringLiteral("</ul>")) && !html.endsWith(QStringLiteral("</ol>"))) html += QStringLiteral("<br/>");
        html += Paragraph(Inline(paragraph.join(u'\n'), ctx), ctx, tight);
        firstParagraph = false;
    }
    return html;
}

QImage CheckboxImage(bool done, const MarkdownTheme& theme) {
    const qreal ratio = std::max<qreal>(1, theme.devicePixelRatio);
    QImage image(QSize(14, 14) * ratio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF box(1.5, 1.5, 11, 11);
    if (done) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.link);
        painter.drawRoundedRect(box, 3, 3);
        QPen pen(theme.codeBackground, 1.8);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawPolyline(QPolygonF{QPointF(4.3, 7.2), QPointF(6.2, 9.1), QPointF(9.8, 5.0)});
    } else {
        painter.setPen(QPen(theme.muted, 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(box, 3, 3);
    }
    return image;
}

void StyleTables(QTextFrame* frame, const MarkdownTheme& theme) {
    if (auto* table = qobject_cast<QTextTable*>(frame); table != nullptr && table->format().border() > 0) {
        QTextTableFormat format = table->format();
        format.setBorder(1);
        format.setBorderBrush(theme.border);
        format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        format.setBorderCollapse(true);
        format.setCellSpacing(0);
        format.setCellPadding(6);
        table->setFormat(format);
        // 合并边框模式下 Qt 只画单元格自身的边框，逐格设置细线
        for (int row = 0; row < table->rows(); ++row) {
            for (int column = 0; column < table->columns(); ++column) {
                QTextTableCell cell = table->cellAt(row, column);
                QTextTableCellFormat cellFormat = cell.format().toTableCellFormat();
                cellFormat.setBorder(1);
                cellFormat.setBorderBrush(theme.border);
                cellFormat.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                cell.setFormat(cellFormat);
            }
        }
    }
    for (QTextFrame* child : frame->childFrames()) StyleTables(child, theme);
}

} // namespace

QString MarkdownToHtml(const QString& markdown, const MarkdownTheme& theme, bool math, const MathImageFactory& mathImage) {
    Context ctx{theme, math, mathImage, {}, {}};
    QString text = markdown;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(u'\r', u'\n');
    QString html = RenderBlocks(text.split(u'\n'), ctx);
    QStringList order = ctx.footnoteOrder;
    for (auto it = ctx.footnotes.constBegin(); it != ctx.footnotes.constEnd(); ++it) if (!order.contains(it.key())) order << it.key();
    if (!order.isEmpty() && !ctx.footnotes.isEmpty()) {
        html += QStringLiteral("<hr style=\"margin-top:8px; margin-bottom:8px;\"/>"
                               "<table border=\"0\" cellspacing=\"0\" cellpadding=\"0\" style=\"font-size:%1px; color:%2;\">")
            .arg(std::max(11, theme.pixelSize - 1)).arg(Css(theme.muted));
        int number = 0;
        for (const QString& id : order) {
            const QString body = ctx.footnotes.value(id);
            html += QStringLiteral("<tr><td width=\"28\" valign=\"top\" style=\"padding-right:6px;\">[%1]</td><td valign=\"top\">%2</td></tr>")
                .arg(++number).arg(body.isEmpty() ? Esc(id) : Inline(body, ctx));
        }
        html += QStringLiteral("</table>");
    }
    return QStringLiteral("<body style=\"color:%1; font-size:%2px;\">%3</body>")
        .arg(Css(theme.text)).arg(theme.pixelSize).arg(html);
}

void RenderMarkdown(QTextDocument& document, const QString& markdown, const MarkdownTheme& theme, bool math) {
    struct Pending { QUrl url; QImage image; };
    std::vector<Pending> images;
    // 公式图片缓存：同一公式在流式重绘、窗口重排时不重复排版
    static QHash<QString, TexImage> cache;
    int serial = 0;
    const MathImageFactory factory = [&](const QString& tex, bool display) -> QString {
        if (tex.isEmpty()) return {};
        TexStyle style;
        style.color = theme.text;
        style.errorColor = theme.mathError;
        style.textFamily = theme.textFamily;
        style.pixelSize = theme.pixelSize * (display ? 1.12 : 1.06);
        style.devicePixelRatio = theme.devicePixelRatio;
        style.display = display;
        const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(display).arg(style.pixelSize).arg(style.devicePixelRatio)
            .arg(Css(style.color), tex);
        TexImage rendered;
        if (const auto found = cache.constFind(key); found != cache.constEnd()) rendered = found.value();
        else {
            rendered = RenderTex(tex, style);
            if (cache.size() > 400) cache.clear();
            cache.insert(key, rendered);
        }
        if (rendered.image.isNull()) return {};
        const QUrl url(QStringLiteral("forge-math:%1").arg(++serial));
        images.push_back({url, rendered.image});
        QString tip = tex.toHtmlEscaped();
        if (!rendered.complete) tip.prepend(QStringLiteral("部分命令未识别：\n"));
        return QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%3\" style=\"vertical-align:middle;\" title=\"%4\"/>")
            .arg(url.toString()).arg(rendered.width).arg(rendered.height).arg(tip);
    };
    const QString html = MarkdownToHtml(markdown, theme, math, factory);
    QFont bodyFont = document.defaultFont();
    if (!theme.textFamily.isEmpty()) bodyFont.setFamily(theme.textFamily);
    bodyFont.setPixelSize(theme.pixelSize);
    document.setDefaultFont(bodyFont);
    document.setIndentWidth(20);
    document.setHtml(html);
    for (const auto& pending : images) document.addResource(QTextDocument::ImageResource, pending.url, QVariant(pending.image));
    if (html.contains(QStringLiteral("forge-check:"))) {
        for (int done = 0; done < 2; ++done) {
            document.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("forge-check:%1").arg(done)),
                QVariant(CheckboxImage(done != 0, theme)));
        }
    }
    // GFM 表格（HTML 里 border=1 的表）统一细线网格；代码块与引用用的布局表不画边框
    for (QTextFrame* frame : document.rootFrame()->childFrames()) StyleTables(frame, theme);
    // 正文、引用与表格采用同一行距，代码块保留等宽排版的紧凑行距。
    QTextCursor cursor(&document);
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        cursor.setPosition(block.position());
        const bool code = block.charFormat().fontFixedPitch() || block.blockFormat().nonBreakableLines();
        format.setLineHeight(code ? 135 : 165, QTextBlockFormat::ProportionalHeight);
        // Qt 的 HTML reader 把 px margin 读成整数；保留 0.75em 的真实段距。
        const qreal paragraphGap = theme.pixelSize * 0.75;
        if (!code && !block.textList() && format.headingLevel() == 0
            && format.bottomMargin() == static_cast<int>(paragraphGap)) {
            format.setBottomMargin(paragraphGap);
        }
        cursor.setBlockFormat(format);
    }
    if (!images.empty()) document.markContentsDirty(0, document.characterCount());
}

} // namespace War3Map::UiKit
