#include "Markdown/TexRenderer.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

// 排版规则取 TeX 的简化版：四级样式（display/text/script/scriptscript）、原子类间距、
// 分式轴线、上下标位移和伸缩定界符。度量全部取实际字体的紧包围盒，绘制与测量共用同一字体。
namespace War3Map::UiKit {
namespace {

enum class Cls { None, Ord, Op, Bin, Rel, Open, Close, Punct };
enum class Face { Italic, Upright, Bold, BoldItalic, Text, TextBold };
enum class Level { Display, Text, Script, ScriptScript };

struct Node;
using NodePtr = std::shared_ptr<Node>;

struct Node {
    enum Kind { Row, Atom, Frac, Sqrt, Scripts, Fenced, Array, Accent, Space, Stack, Boxed, Colored, Styled };
    Kind kind = Row;
    std::vector<NodePtr> children;
    QString text;            // Atom 文本；Fenced 左定界；Accent 种类；Stack 种类
    QString text2;           // Fenced 右定界
    Cls cls = Cls::Ord;
    Face face = Face::Italic;
    bool error = false;
    bool bigOp = false;
    bool limitsByDefault = false;
    int limits = 0;          // 0 按默认，1 \limits，2 \nolimits
    NodePtr a, b, c;
    bool noBar = false;
    int fracStyle = 0;       // 1 \dfrac，2 \tfrac
    qreal em = 0;            // Space 宽度；Fenced 固定高度（\big 系列）
    std::vector<std::vector<NodePtr>> rows;
    QString align;
    qreal colGap = 0.8;
    bool alternateAlign = false;
    QColor color;
    Level level = Level::Display;
};

NodePtr MakeRow() { return std::make_shared<Node>(); }
NodePtr MakeAtom(const QString& text, Cls cls, Face face) {
    auto node = std::make_shared<Node>();
    node->kind = Node::Atom; node->text = text; node->cls = cls; node->face = face;
    return node;
}
NodePtr MakeError(const QString& text) {
    auto node = MakeAtom(text, Cls::Ord, Face::Text);
    node->error = true;
    return node;
}

struct Symbol { QString text; Cls cls; Face face = Face::Upright; };

const QHash<QString, Symbol>& Symbols() {
    static const QHash<QString, Symbol> table = [] {
        QHash<QString, Symbol> t;
        const auto ord = [&t](const char* name, const char16_t* text, Face face = Face::Upright) {
            t.insert(QString::fromLatin1(name), {QString::fromUtf16(text), Cls::Ord, face});
        };
        const auto add = [&t](const char* name, const char16_t* text, Cls cls) {
            t.insert(QString::fromLatin1(name), {QString::fromUtf16(text), cls, Face::Upright});
        };
        // 希腊字母：小写斜体，大写直立（与 TeX 一致）
        ord("alpha", u"α", Face::Italic); ord("beta", u"β", Face::Italic); ord("gamma", u"γ", Face::Italic);
        ord("delta", u"δ", Face::Italic); ord("epsilon", u"ϵ", Face::Italic); ord("varepsilon", u"ε", Face::Italic);
        ord("zeta", u"ζ", Face::Italic); ord("eta", u"η", Face::Italic); ord("theta", u"θ", Face::Italic);
        ord("vartheta", u"ϑ", Face::Italic); ord("iota", u"ι", Face::Italic); ord("kappa", u"κ", Face::Italic);
        ord("lambda", u"λ", Face::Italic); ord("mu", u"μ", Face::Italic); ord("nu", u"ν", Face::Italic);
        ord("xi", u"ξ", Face::Italic); ord("pi", u"π", Face::Italic); ord("varpi", u"ϖ", Face::Italic);
        ord("rho", u"ρ", Face::Italic); ord("varrho", u"ϱ", Face::Italic); ord("sigma", u"σ", Face::Italic);
        ord("varsigma", u"ς", Face::Italic); ord("tau", u"τ", Face::Italic); ord("upsilon", u"υ", Face::Italic);
        ord("phi", u"ϕ", Face::Italic); ord("varphi", u"φ", Face::Italic); ord("chi", u"χ", Face::Italic);
        ord("psi", u"ψ", Face::Italic); ord("omega", u"ω", Face::Italic);
        ord("Gamma", u"Γ"); ord("Delta", u"Δ"); ord("Theta", u"Θ"); ord("Lambda", u"Λ"); ord("Xi", u"Ξ");
        ord("Pi", u"Π"); ord("Sigma", u"Σ"); ord("Upsilon", u"Υ"); ord("Phi", u"Φ"); ord("Psi", u"Ψ"); ord("Omega", u"Ω");
        // 普通符号
        ord("infty", u"∞"); ord("partial", u"∂"); ord("nabla", u"∇"); ord("forall", u"∀"); ord("exists", u"∃");
        ord("nexists", u"∄"); ord("emptyset", u"∅"); ord("varnothing", u"∅"); ord("neg", u"¬"); ord("lnot", u"¬");
        ord("angle", u"∠"); ord("triangle", u"△"); ord("ell", u"ℓ"); ord("hbar", u"ℏ"); ord("Re", u"ℜ");
        ord("Im", u"ℑ"); ord("aleph", u"ℵ"); ord("prime", u"′"); ord("ldots", u"…"); ord("dots", u"…");
        ord("cdots", u"⋯"); ord("vdots", u"⋮"); ord("ddots", u"⋱"); ord("degree", u"°"); ord("top", u"⊤");
        ord("bot", u"⊥"); ord("square", u"□"); ord("blacksquare", u"■"); ord("checkmark", u"✓");
        ord("backslash", u"\\"); ord("|", u"‖"); ord("%", u"%"); ord("$", u"$"); ord("&", u"&"); ord("#", u"#");
        ord("_", u"_"); ord("surd", u"√"); ord("clubsuit", u"♣"); ord("spadesuit", u"♠"); ord("heartsuit", u"♡");
        ord("diamondsuit", u"♢"); ord("star", u"⋆");
        // 二元运算
        add("pm", u"±", Cls::Bin); add("mp", u"∓", Cls::Bin); add("times", u"×", Cls::Bin); add("div", u"÷", Cls::Bin);
        add("cdot", u"·", Cls::Bin); add("ast", u"∗", Cls::Bin); add("circ", u"∘", Cls::Bin); add("bullet", u"∙", Cls::Bin);
        add("oplus", u"⊕", Cls::Bin); add("ominus", u"⊖", Cls::Bin); add("otimes", u"⊗", Cls::Bin); add("odot", u"⊙", Cls::Bin);
        add("cup", u"∪", Cls::Bin); add("cap", u"∩", Cls::Bin); add("wedge", u"∧", Cls::Bin); add("land", u"∧", Cls::Bin);
        add("vee", u"∨", Cls::Bin); add("lor", u"∨", Cls::Bin); add("setminus", u"∖", Cls::Bin); add("amalg", u"⨿", Cls::Bin);
        // 关系
        add("le", u"≤", Cls::Rel); add("leq", u"≤", Cls::Rel); add("ge", u"≥", Cls::Rel); add("geq", u"≥", Cls::Rel);
        add("leqslant", u"⩽", Cls::Rel); add("geqslant", u"⩾", Cls::Rel);
        add("ne", u"≠", Cls::Rel); add("neq", u"≠", Cls::Rel); add("approx", u"≈", Cls::Rel); add("equiv", u"≡", Cls::Rel);
        add("sim", u"∼", Cls::Rel); add("simeq", u"≃", Cls::Rel); add("cong", u"≅", Cls::Rel); add("propto", u"∝", Cls::Rel);
        add("ll", u"≪", Cls::Rel); add("gg", u"≫", Cls::Rel); add("in", u"∈", Cls::Rel); add("notin", u"∉", Cls::Rel);
        add("ni", u"∋", Cls::Rel); add("subset", u"⊂", Cls::Rel); add("subseteq", u"⊆", Cls::Rel); add("supset", u"⊃", Cls::Rel);
        add("supseteq", u"⊇", Cls::Rel); add("to", u"→", Cls::Rel); add("rightarrow", u"→", Cls::Rel);
        add("leftarrow", u"←", Cls::Rel); add("gets", u"←", Cls::Rel); add("Rightarrow", u"⇒", Cls::Rel);
        add("Leftarrow", u"⇐", Cls::Rel); add("Leftrightarrow", u"⇔", Cls::Rel); add("leftrightarrow", u"↔", Cls::Rel);
        add("implies", u"⟹", Cls::Rel); add("iff", u"⟺", Cls::Rel); add("impliedby", u"⟸", Cls::Rel);
        add("mapsto", u"↦", Cls::Rel); add("longrightarrow", u"⟶", Cls::Rel); add("longleftarrow", u"⟵", Cls::Rel);
        add("Longrightarrow", u"⟹", Cls::Rel); add("uparrow", u"↑", Cls::Rel); add("downarrow", u"↓", Cls::Rel);
        add("perp", u"⊥", Cls::Rel); add("parallel", u"∥", Cls::Rel); add("mid", u"∣", Cls::Rel); add("models", u"⊨", Cls::Rel);
        add("vdash", u"⊢", Cls::Rel); add("coloneqq", u"≔", Cls::Rel); add("triangleq", u"≜", Cls::Rel);
        add("prec", u"≺", Cls::Rel); add("succ", u"≻", Cls::Rel);
        // 定界符
        add("langle", u"⟨", Cls::Open); add("rangle", u"⟩", Cls::Close); add("lfloor", u"⌊", Cls::Open);
        add("rfloor", u"⌋", Cls::Close); add("lceil", u"⌈", Cls::Open); add("rceil", u"⌉", Cls::Close);
        add("lbrace", u"{", Cls::Open); add("rbrace", u"}", Cls::Close); add("{", u"{", Cls::Open); add("}", u"}", Cls::Close);
        add("vert", u"|", Cls::Ord); add("Vert", u"‖", Cls::Ord); add("lvert", u"|", Cls::Open); add("rvert", u"|", Cls::Close);
        add("lVert", u"‖", Cls::Open); add("rVert", u"‖", Cls::Close);
        add("colon", u":", Cls::Punct); add(",", u"", Cls::None);
        return t;
    }();
    return table;
}

struct BigOp { QString text; bool limits; };
const QHash<QString, BigOp>& BigOps() {
    static const QHash<QString, BigOp> table{
        {QStringLiteral("sum"), {QStringLiteral("∑"), true}}, {QStringLiteral("prod"), {QStringLiteral("∏"), true}},
        {QStringLiteral("coprod"), {QStringLiteral("∐"), true}}, {QStringLiteral("bigcup"), {QStringLiteral("⋃"), true}},
        {QStringLiteral("bigcap"), {QStringLiteral("⋂"), true}}, {QStringLiteral("bigoplus"), {QStringLiteral("⨁"), true}},
        {QStringLiteral("bigotimes"), {QStringLiteral("⨂"), true}}, {QStringLiteral("bigvee"), {QStringLiteral("⋁"), true}},
        {QStringLiteral("bigwedge"), {QStringLiteral("⋀"), true}}, {QStringLiteral("int"), {QStringLiteral("∫"), false}},
        {QStringLiteral("iint"), {QStringLiteral("∬"), false}}, {QStringLiteral("iiint"), {QStringLiteral("∭"), false}},
        {QStringLiteral("oint"), {QStringLiteral("∮"), false}}};
    return table;
}

// 直立函数名；第二项表示 display 下上下限居中（lim/max/min 类）。
const QHash<QString, bool>& Functions() {
    static const QHash<QString, bool> table{
        {QStringLiteral("sin"), false}, {QStringLiteral("cos"), false}, {QStringLiteral("tan"), false},
        {QStringLiteral("cot"), false}, {QStringLiteral("sec"), false}, {QStringLiteral("csc"), false},
        {QStringLiteral("arcsin"), false}, {QStringLiteral("arccos"), false}, {QStringLiteral("arctan"), false},
        {QStringLiteral("sinh"), false}, {QStringLiteral("cosh"), false}, {QStringLiteral("tanh"), false},
        {QStringLiteral("log"), false}, {QStringLiteral("ln"), false}, {QStringLiteral("lg"), false},
        {QStringLiteral("exp"), false}, {QStringLiteral("deg"), false}, {QStringLiteral("dim"), false},
        {QStringLiteral("arg"), false}, {QStringLiteral("ker"), false}, {QStringLiteral("hom"), false},
        {QStringLiteral("lim"), true}, {QStringLiteral("liminf"), true}, {QStringLiteral("limsup"), true},
        {QStringLiteral("max"), true}, {QStringLiteral("min"), true}, {QStringLiteral("sup"), true},
        {QStringLiteral("inf"), true}, {QStringLiteral("det"), true}, {QStringLiteral("gcd"), true},
        {QStringLiteral("Pr"), true}, {QStringLiteral("argmax"), true}, {QStringLiteral("argmin"), true}};
    return table;
}

QString FromUcs4(char32_t code) { return QString::fromUcs4(&code, 1); }

// \mathbb / \mathcal / \mathfrak：Unicode 数学字母区，含历史保留位的例外。
QString MapAlphabet(const QString& variant, QChar ch) {
    const char16_t c = ch.unicode();
    const bool upper = c >= u'A' && c <= u'Z';
    const bool lower = c >= u'a' && c <= u'z';
    if (variant == QStringLiteral("mathbb")) {
        static const QHash<char16_t, char32_t> special{{u'C', U'ℂ'}, {u'H', U'ℍ'}, {u'N', U'ℕ'}, {u'P', U'ℙ'},
            {u'Q', U'ℚ'}, {u'R', U'ℝ'}, {u'Z', U'ℤ'}};
        if (special.contains(c)) return FromUcs4(special.value(c));
        if (upper) return FromUcs4(U'\U0001D538' + (c - u'A'));
        if (lower) return FromUcs4(U'\U0001D552' + (c - u'a'));
        if (c >= u'0' && c <= u'9') return FromUcs4(U'\U0001D7D8' + (c - u'0'));
    } else if (variant == QStringLiteral("mathcal") || variant == QStringLiteral("mathscr")) {
        static const QHash<char16_t, char32_t> special{{u'B', U'ℬ'}, {u'E', U'ℰ'}, {u'F', U'ℱ'}, {u'H', U'ℋ'},
            {u'I', U'ℐ'}, {u'L', U'ℒ'}, {u'M', U'ℳ'}, {u'R', U'ℛ'}, {u'e', U'ℯ'}, {u'g', U'ℊ'}, {u'o', U'ℴ'}};
        if (special.contains(c)) return FromUcs4(special.value(c));
        if (upper) return FromUcs4(U'\U0001D49C' + (c - u'A'));
        if (lower) return FromUcs4(U'\U0001D4B6' + (c - u'a'));
    } else if (variant == QStringLiteral("mathfrak")) {
        static const QHash<char16_t, char32_t> special{{u'C', U'ℭ'}, {u'H', U'ℌ'}, {u'I', U'ℑ'}, {u'R', U'ℜ'}, {u'Z', U'ℨ'}};
        if (special.contains(c)) return FromUcs4(special.value(c));
        if (upper) return FromUcs4(U'\U0001D504' + (c - u'A'));
        if (lower) return FromUcs4(U'\U0001D51E' + (c - u'a'));
    }
    return QString(ch);
}

void ApplyFace(const NodePtr& node, Face face) {
    if (!node) return;
    if (node->kind == Node::Atom && !node->error && node->face != Face::Text && node->face != Face::TextBold) {
        const bool letterLike = !node->text.isEmpty() && (node->text.front().isLetterOrNumber());
        if (letterLike || face == Face::Bold || face == Face::BoldItalic) node->face = face;
    }
    for (const auto& child : node->children) ApplyFace(child, face);
    ApplyFace(node->a, face); ApplyFace(node->b, face); ApplyFace(node->c, face);
    for (const auto& row : node->rows) for (const auto& cell : row) ApplyFace(cell, face);
}

void ApplyAlphabet(const NodePtr& node, const QString& variant) {
    if (!node) return;
    if (node->kind == Node::Atom && !node->error && node->text.size() == 1) {
        node->text = MapAlphabet(variant, node->text.front());
        node->face = Face::Upright;
    }
    for (const auto& child : node->children) ApplyAlphabet(child, variant);
}

// ---------------------------------------------------------------- 解析

class Parser {
public:
    explicit Parser(const QString& source) : s_(source) {}

    NodePtr Parse() {
        auto row = ParseRow(Stop::Top);
        while (pos_ < s_.size()) {        // 多余的 }：报错后继续
            complete = false;
            row->children.push_back(MakeError(QString(s_.at(pos_))));
            ++pos_;
            auto rest = ParseRow(Stop::Top);
            for (auto& child : rest->children) row->children.push_back(child);
        }
        return row;
    }

    bool complete = true;

private:
    enum class Stop { Top, Group, Cell, Right, Bracket };

    void SkipSpace() { while (pos_ < s_.size() && s_.at(pos_).isSpace()) ++pos_; }
    [[nodiscard]] bool AtEnd() const { return pos_ >= s_.size(); }

    QString ReadCommandName() {
        // 已越过反斜杠
        if (AtEnd()) return {};
        if (!s_.at(pos_).isLetter()) return QString(s_.at(pos_++));
        const int start = pos_;
        while (pos_ < s_.size() && s_.at(pos_).isLetter() && s_.at(pos_).unicode() < 128) ++pos_;
        return s_.mid(start, pos_ - start);
    }

    [[nodiscard]] bool PeekCommand(QStringView name) const {
        if (pos_ + 1 + name.size() > s_.size() || s_.at(pos_) != u'\\') return false;
        if (QStringView(s_).mid(pos_ + 1, name.size()) != name) return false;
        const int after = pos_ + 1 + static_cast<int>(name.size());
        return after >= s_.size() || !s_.at(after).isLetter();
    }

    // 读取 {…} 中的原文（\text 等），支持嵌套括号
    QString ReadRawGroup() {
        SkipSpace();
        if (AtEnd()) { complete = false; return {}; }
        if (s_.at(pos_) != u'{') return QString(s_.at(pos_++));
        ++pos_;
        int depth = 1;
        const int start = pos_;
        while (pos_ < s_.size()) {
            const QChar ch = s_.at(pos_);
            if (ch == u'\\' && pos_ + 1 < s_.size()) { pos_ += 2; continue; }
            if (ch == u'{') ++depth;
            else if (ch == u'}' && --depth == 0) break;
            ++pos_;
        }
        QString text = s_.mid(start, pos_ - start);
        if (pos_ < s_.size()) ++pos_; else complete = false;
        // \text 内的转义字符还原
        static const QString escapes = QStringLiteral("{}%$&#_ ");
        QString out;
        for (int i = 0; i < text.size(); ++i) {
            if (text.at(i) == u'\\' && i + 1 < text.size() && escapes.contains(text.at(i + 1))) { out += text.at(++i); continue; }
            out += text.at(i);
        }
        return out;
    }

    NodePtr ParseGroupBody() {
        // 已越过 {
        auto row = ParseRow(Stop::Group);
        if (!AtEnd() && s_.at(pos_) == u'}') ++pos_; else complete = false;
        return row;
    }

    NodePtr ParseArg() {
        SkipSpace();
        if (AtEnd()) { complete = false; return MakeRow(); }
        const QChar ch = s_.at(pos_);
        if (ch == u'{') { ++pos_; return ParseGroupBody(); }
        auto row = MakeRow();
        if (ch == u'\\') { ++pos_; ParseCommand(*row); }
        else { ++pos_; row->children.push_back(CharAtom(ch)); }
        return row->children.size() == 1 ? row->children.front() : row;
    }

    QString ParseDelimiter() {
        SkipSpace();
        if (AtEnd()) { complete = false; return QStringLiteral("."); }
        const QChar ch = s_.at(pos_++);
        if (ch != u'\\') return ch == u'<' ? QStringLiteral("⟨") : ch == u'>' ? QStringLiteral("⟩") : QString(ch);
        const QString name = ReadCommandName();
        const auto& symbols = Symbols();
        if (symbols.contains(name)) return symbols.value(name).text;
        complete = false;
        return QStringLiteral(".");
    }

    static NodePtr CharAtom(QChar ch) {
        const char16_t c = ch.unicode();
        if ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z')) return MakeAtom(QString(ch), Cls::Ord, Face::Italic);
        if ((c >= u'0' && c <= u'9') || c == u'.') return MakeAtom(QString(ch), Cls::Ord, Face::Upright);
        switch (c) {
        case u'+': return MakeAtom(QStringLiteral("+"), Cls::Bin, Face::Upright);
        case u'-': return MakeAtom(QStringLiteral("−"), Cls::Bin, Face::Upright);
        case u'*': return MakeAtom(QStringLiteral("∗"), Cls::Bin, Face::Upright);
        case u'=': case u'<': case u'>': case u':': return MakeAtom(QString(ch), Cls::Rel, Face::Upright);
        case u',': case u';': return MakeAtom(QString(ch), Cls::Punct, Face::Upright);
        case u'(': case u'[': return MakeAtom(QString(ch), Cls::Open, Face::Upright);
        case u')': case u']': case u'!': case u'?': return MakeAtom(QString(ch), Cls::Close, Face::Upright);
        case u'~': return nullptr;
        default: break;
        }
        if (c >= 0x370 && c <= 0x3ff) return MakeAtom(QString(ch), Cls::Ord, Face::Italic);
        if (c < 128) return MakeAtom(QString(ch), Cls::Ord, Face::Upright);
        if (c >= 0x2190 && c <= 0x22ff) return MakeAtom(QString(ch), Cls::Rel, Face::Upright);
        return MakeAtom(QString(ch), Cls::Ord, Face::Text);
    }

    NodePtr ParseRow(Stop stop) {
        auto row = MakeRow();
        while (true) {
            SkipSpace();
            if (AtEnd()) { if (stop == Stop::Group || stop == Stop::Right || stop == Stop::Bracket) complete = false; return row; }
            const QChar ch = s_.at(pos_);
            if (ch == u'}') { if (stop == Stop::Group || stop == Stop::Top) return row; complete = false; return row; }
            if (stop == Stop::Bracket && ch == u']') return row;
            if (stop == Stop::Cell && (ch == u'&' || (ch == u'\\' && pos_ + 1 < s_.size() && s_.at(pos_ + 1) == u'\\')
                || PeekCommand(u"end"))) return row;
            if (stop == Stop::Right && PeekCommand(u"right")) return row;
            if (ch == u'&') { ++pos_; continue; }             // 数组外的 & 忽略
            if (ch == u'^' || ch == u'_') {
                ++pos_;
                AttachScript(*row, ch == u'^', ParseArg());
                continue;
            }
            if (ch == u'\'') {
                ++pos_;
                QString primes = QStringLiteral("′");
                while (!AtEnd() && s_.at(pos_) == u'\'') { primes += QStringLiteral("′"); ++pos_; }
                AttachScript(*row, true, MakeAtom(primes, Cls::Ord, Face::Upright));
                continue;
            }
            if (ch == u'{') { ++pos_; row->children.push_back(ParseGroupBody()); continue; }
            if (ch == u'\\') {
                ++pos_;
                if (!AtEnd() && s_.at(pos_) == u'\\') { ++pos_; continue; }   // 顶层 \\ 换行：忽略
                const int before = pos_;
                if (ParseCommand(*row, stop)) continue;
                pos_ = before;
                return row;
            }
            ++pos_;
            if (auto atom = CharAtom(ch)) row->children.push_back(atom);
            else { auto space = std::make_shared<Node>(); space->kind = Node::Space; space->em = 0.28; row->children.push_back(space); }
        }
    }

    static void AttachScript(Node& row, bool sup, NodePtr script) {
        NodePtr base;
        if (!row.children.empty()) base = row.children.back();
        if (base && base->kind == Node::Scripts && !(sup ? base->b : base->c)) {
            (sup ? base->b : base->c) = script;
            return;
        }
        auto node = std::make_shared<Node>();
        node->kind = Node::Scripts;
        node->a = base ? base : MakeRow();
        (sup ? node->b : node->c) = script;
        if (base) row.children.back() = node; else row.children.push_back(node);
    }

    // 返回 false 表示遇到属于外层的 \end / \right（调用方回退位置）。
    bool ParseCommand(Node& row, Stop stop = Stop::Top) {
        const QString name = ReadCommandName();
        if (name.isEmpty()) { complete = false; return true; }
        if ((name == QStringLiteral("end") && stop == Stop::Cell) || (name == QStringLiteral("right") && stop == Stop::Right)) return false;
        auto push = [&row](NodePtr node) { row.children.push_back(std::move(node)); };
        auto space = [&](qreal em) { auto node = std::make_shared<Node>(); node->kind = Node::Space; node->em = em; push(node); };

        if (name == QStringLiteral(",")) { space(3.0 / 18); return true; }
        if (name == QStringLiteral(":") || name == QStringLiteral(">")) { space(4.0 / 18); return true; }
        if (name == QStringLiteral(";")) { space(5.0 / 18); return true; }
        if (name == QStringLiteral("!")) { space(-3.0 / 18); return true; }
        if (name == QStringLiteral(" ")) { space(0.28); return true; }
        if (name == QStringLiteral("quad")) { space(1.0); return true; }
        if (name == QStringLiteral("qquad")) { space(2.0); return true; }
        if (name == QStringLiteral("enspace")) { space(0.5); return true; }

        if (const auto& symbols = Symbols(); symbols.contains(name)) {
            const auto& symbol = symbols.value(name);
            if (symbol.cls != Cls::None) push(MakeAtom(symbol.text, symbol.cls, symbol.face));
            return true;
        }
        if (const auto& ops = BigOps(); ops.contains(name)) {
            auto node = MakeAtom(ops.value(name).text, Cls::Op, Face::Upright);
            node->bigOp = true; node->limitsByDefault = ops.value(name).limits;
            push(node);
            return true;
        }
        if (const auto& functions = Functions(); functions.contains(name)) {
            QString text = name;
            if (name == QStringLiteral("liminf")) text = QStringLiteral("lim inf");
            else if (name == QStringLiteral("limsup")) text = QStringLiteral("lim sup");
            else if (name == QStringLiteral("argmax")) text = QStringLiteral("arg max");
            else if (name == QStringLiteral("argmin")) text = QStringLiteral("arg min");
            auto node = MakeAtom(text, Cls::Op, Face::Upright);
            node->limitsByDefault = functions.value(name);
            push(node);
            return true;
        }
        if (name == QStringLiteral("limits") || name == QStringLiteral("nolimits")) {
            if (!row.children.empty()) row.children.back()->limits = name == QStringLiteral("limits") ? 1 : 2;
            return true;
        }
        if (name == QStringLiteral("frac") || name == QStringLiteral("dfrac") || name == QStringLiteral("tfrac")
            || name == QStringLiteral("cfrac") || name == QStringLiteral("binom") || name == QStringLiteral("dbinom")
            || name == QStringLiteral("tbinom")) {
            auto node = std::make_shared<Node>();
            node->kind = Node::Frac;
            node->a = ParseArg(); node->b = ParseArg();
            node->noBar = name.endsWith(QStringLiteral("binom"));
            node->fracStyle = name.startsWith(u'd') || name == QStringLiteral("cfrac") ? 1 : name.startsWith(u't') ? 2 : 0;
            if (node->noBar) {
                auto fenced = std::make_shared<Node>();
                fenced->kind = Node::Fenced; fenced->text = QStringLiteral("("); fenced->text2 = QStringLiteral(")");
                fenced->a = node;
                push(fenced);
            } else push(node);
            return true;
        }
        if (name == QStringLiteral("sqrt")) {
            auto node = std::make_shared<Node>();
            node->kind = Node::Sqrt;
            SkipSpace();
            if (!AtEnd() && s_.at(pos_) == u'[') {
                ++pos_;
                node->b = ParseRow(Stop::Bracket);
                if (!AtEnd() && s_.at(pos_) == u']') ++pos_; else complete = false;
            }
            node->a = ParseArg();
            push(node);
            return true;
        }
        if (name == QStringLiteral("text") || name == QStringLiteral("textrm") || name == QStringLiteral("textnormal")
            || name == QStringLiteral("mbox") || name == QStringLiteral("textit") || name == QStringLiteral("textbf")
            || name == QStringLiteral("textsf") || name == QStringLiteral("texttt")) {
            push(MakeAtom(ReadRawGroup(), Cls::Ord, name == QStringLiteral("textbf") ? Face::TextBold : Face::Text));
            return true;
        }
        if (name == QStringLiteral("operatorname")) {
            SkipSpace();
            if (!AtEnd() && s_.at(pos_) == u'*') ++pos_;
            push(MakeAtom(ReadRawGroup(), Cls::Op, Face::Upright));
            return true;
        }
        if (name == QStringLiteral("mathrm") || name == QStringLiteral("mathbf") || name == QStringLiteral("mathit")
            || name == QStringLiteral("mathsf") || name == QStringLiteral("mathtt") || name == QStringLiteral("boldsymbol")
            || name == QStringLiteral("bm") || name == QStringLiteral("mathnormal")) {
            auto arg = ParseArg();
            ApplyFace(arg, name == QStringLiteral("mathbf") ? Face::Bold
                : name == QStringLiteral("boldsymbol") || name == QStringLiteral("bm") ? Face::BoldItalic
                : name == QStringLiteral("mathit") || name == QStringLiteral("mathnormal") ? Face::Italic : Face::Upright);
            push(arg);
            return true;
        }
        if (name == QStringLiteral("mathbb") || name == QStringLiteral("mathcal") || name == QStringLiteral("mathscr")
            || name == QStringLiteral("mathfrak")) {
            auto arg = ParseArg();
            ApplyAlphabet(arg, name);
            push(arg);
            return true;
        }
        if (name == QStringLiteral("left")) {
            auto node = std::make_shared<Node>();
            node->kind = Node::Fenced;
            node->text = ParseDelimiter();
            node->a = ParseRow(Stop::Right);
            if (PeekCommand(u"right")) { pos_ += 6; node->text2 = ParseDelimiter(); }
            else { complete = false; node->text2 = QStringLiteral("."); }
            push(node);
            return true;
        }
        if (name == QStringLiteral("right")) { complete = false; (void)ParseDelimiter(); return true; }
        if (name == QStringLiteral("middle")) { push(MakeAtom(ParseDelimiter(), Cls::Rel, Face::Upright)); return true; }
        if (name == QStringLiteral("big") || name == QStringLiteral("Big") || name == QStringLiteral("bigg")
            || name == QStringLiteral("Bigg") || name.startsWith(QStringLiteral("bigl")) || name.startsWith(QStringLiteral("bigr"))
            || name.startsWith(QStringLiteral("Bigl")) || name.startsWith(QStringLiteral("Bigr"))
            || name.startsWith(QStringLiteral("biggl")) || name.startsWith(QStringLiteral("biggr"))
            || name.startsWith(QStringLiteral("Biggl")) || name.startsWith(QStringLiteral("Biggr"))) {
            const QString base = name.startsWith(QStringLiteral("Bigg")) ? QStringLiteral("Bigg")
                : name.startsWith(QStringLiteral("bigg")) ? QStringLiteral("bigg")
                : name.startsWith(QStringLiteral("Big")) ? QStringLiteral("Big") : QStringLiteral("big");
            auto node = std::make_shared<Node>();
            node->kind = Node::Fenced;
            const QString delim = ParseDelimiter();
            const bool right = name.endsWith(u'r');
            node->text = right ? QStringLiteral(".") : delim;
            node->text2 = right ? delim : QStringLiteral(".");
            node->a = MakeRow();
            node->em = base == QStringLiteral("big") ? 1.2 : base == QStringLiteral("Big") ? 1.8
                : base == QStringLiteral("bigg") ? 2.4 : 3.0;
            node->cls = right ? Cls::Close : Cls::Open;
            push(node);
            return true;
        }
        if (name == QStringLiteral("overline") || name == QStringLiteral("underline") || name == QStringLiteral("bar")
            || name == QStringLiteral("hat") || name == QStringLiteral("widehat") || name == QStringLiteral("tilde")
            || name == QStringLiteral("widetilde") || name == QStringLiteral("vec") || name == QStringLiteral("overrightarrow")
            || name == QStringLiteral("dot") || name == QStringLiteral("ddot") || name == QStringLiteral("check")
            || name == QStringLiteral("overbrace") || name == QStringLiteral("underbrace")) {
            auto node = std::make_shared<Node>();
            node->kind = Node::Accent;
            node->text = name;
            node->a = ParseArg();
            if (name == QStringLiteral("overbrace") || name == QStringLiteral("underbrace")) {
                node->bigOp = true; node->limits = 1; node->cls = Cls::Op;
            }
            push(node);
            return true;
        }
        if (name == QStringLiteral("overset") || name == QStringLiteral("underset") || name == QStringLiteral("stackrel")) {
            auto node = std::make_shared<Node>();
            node->kind = Node::Stack;
            node->text = name;
            node->b = ParseArg();     // 附加文字
            node->a = ParseArg();     // 主体
            node->cls = name == QStringLiteral("stackrel") ? Cls::Rel : Cls::Ord;
            if (node->a && node->a->kind == Node::Atom) node->cls = name == QStringLiteral("stackrel") ? Cls::Rel : node->a->cls;
            push(node);
            return true;
        }
        if (name == QStringLiteral("boxed") || name == QStringLiteral("fbox")) {
            auto node = std::make_shared<Node>();
            node->kind = Node::Boxed;
            node->a = name == QStringLiteral("fbox") ? MakeAtom(ReadRawGroup(), Cls::Ord, Face::Text) : ParseArg();
            push(node);
            return true;
        }
        if (name == QStringLiteral("color") || name == QStringLiteral("textcolor")) {
            const QColor color(ReadRawGroup().trimmed());
            auto node = std::make_shared<Node>();
            node->kind = Node::Colored;
            node->color = color.isValid() ? color : QColor();
            if (name == QStringLiteral("textcolor")) node->a = ParseArg();
            else node->a = ParseRow(stop == Stop::Bracket ? Stop::Group : stop);
            push(node);
            return true;
        }
        if (name == QStringLiteral("displaystyle") || name == QStringLiteral("textstyle")
            || name == QStringLiteral("scriptstyle") || name == QStringLiteral("scriptscriptstyle")) {
            auto node = std::make_shared<Node>();
            node->kind = Node::Styled;
            node->level = name == QStringLiteral("displaystyle") ? Level::Display : name == QStringLiteral("textstyle") ? Level::Text
                : name == QStringLiteral("scriptstyle") ? Level::Script : Level::ScriptScript;
            node->a = ParseRow(stop == Stop::Bracket ? Stop::Group : stop);
            push(node);
            return true;
        }
        if (name == QStringLiteral("not")) {
            SkipSpace();
            const QString next = !AtEnd() && s_.at(pos_) == u'=' ? (++pos_, QStringLiteral("="))
                : !AtEnd() && s_.at(pos_) == u'\\' ? (++pos_, ReadCommandName()) : QString();
            if (next == QStringLiteral("=")) push(MakeAtom(QStringLiteral("≠"), Cls::Rel, Face::Upright));
            else if (next == QStringLiteral("in")) push(MakeAtom(QStringLiteral("∉"), Cls::Rel, Face::Upright));
            else if (next == QStringLiteral("subset")) push(MakeAtom(QStringLiteral("⊄"), Cls::Rel, Face::Upright));
            else if (next == QStringLiteral("equiv")) push(MakeAtom(QStringLiteral("≢"), Cls::Rel, Face::Upright));
            else push(MakeAtom(QStringLiteral("/"), Cls::Rel, Face::Upright));
            return true;
        }
        if (name == QStringLiteral("bmod") || name == QStringLiteral("mod")) { push(MakeAtom(QStringLiteral("mod"), Cls::Bin, Face::Upright)); return true; }
        if (name == QStringLiteral("pmod")) {
            auto fenced = std::make_shared<Node>();
            fenced->kind = Node::Fenced; fenced->text = QStringLiteral("("); fenced->text2 = QStringLiteral(")");
            auto inner = MakeRow();
            inner->children.push_back(MakeAtom(QStringLiteral("mod"), Cls::Ord, Face::Upright));
            auto gap = std::make_shared<Node>(); gap->kind = Node::Space; gap->em = 0.33; inner->children.push_back(gap);
            inner->children.push_back(ParseArg());
            fenced->a = inner;
            space(0.5);
            push(fenced);
            return true;
        }
        if (name == QStringLiteral("begin")) { push(ParseEnvironment(ReadRawGroup().trimmed())); return true; }
        if (name == QStringLiteral("hline") || name == QStringLiteral("nonumber") || name == QStringLiteral("notag")
            || name == QStringLiteral("label") || name == QStringLiteral("tag")) {
            if (name == QStringLiteral("label") || name == QStringLiteral("tag")) (void)ReadRawGroup();
            return true;
        }
        complete = false;
        push(MakeError(QLatin1Char('\\') + name));
        return true;
    }

    NodePtr ParseEnvironment(const QString& env) {
        auto node = std::make_shared<Node>();
        node->kind = Node::Array;
        QString name = env;
        if (name.endsWith(u'*')) name.chop(1);
        if (name == QStringLiteral("array")) {
            for (const QChar ch : ReadRawGroup()) if (ch == u'l' || ch == u'c' || ch == u'r') node->align += ch;
        }
        if (name == QStringLiteral("pmatrix")) { node->text = QStringLiteral("("); node->text2 = QStringLiteral(")"); }
        else if (name == QStringLiteral("bmatrix")) { node->text = QStringLiteral("["); node->text2 = QStringLiteral("]"); }
        else if (name == QStringLiteral("Bmatrix")) { node->text = QStringLiteral("{"); node->text2 = QStringLiteral("}"); }
        else if (name == QStringLiteral("vmatrix")) { node->text = QStringLiteral("|"); node->text2 = QStringLiteral("|"); }
        else if (name == QStringLiteral("Vmatrix")) { node->text = QStringLiteral("‖"); node->text2 = QStringLiteral("‖"); }
        else if (name == QStringLiteral("cases")) { node->text = QStringLiteral("{"); node->text2 = QStringLiteral("."); node->align = QStringLiteral("ll"); node->colGap = 1.0; }
        else if (name == QStringLiteral("rcases")) { node->text = QStringLiteral("."); node->text2 = QStringLiteral("}"); node->align = QStringLiteral("ll"); node->colGap = 1.0; }
        else if (name == QStringLiteral("aligned") || name == QStringLiteral("align") || name == QStringLiteral("alignat")
            || name == QStringLiteral("split") || name == QStringLiteral("eqnarray")) { node->alternateAlign = true; node->colGap = 0; node->level = Level::Display; }
        else if (name == QStringLiteral("gathered") || name == QStringLiteral("gather")) { node->align = QStringLiteral("c"); node->level = Level::Display; }
        else if (name != QStringLiteral("matrix") && name != QStringLiteral("smallmatrix") && name != QStringLiteral("array")) complete = false;
        if (node->text.isEmpty()) { node->text = QStringLiteral("."); node->text2 = QStringLiteral("."); }
        std::vector<NodePtr> current;
        while (true) {
            current.push_back(ParseRow(Stop::Cell));
            if (AtEnd()) { complete = false; break; }
            const QChar ch = s_.at(pos_);
            if (ch == u'&') { ++pos_; continue; }
            if (ch == u'\\' && pos_ + 1 < s_.size() && s_.at(pos_ + 1) == u'\\') {
                pos_ += 2;
                SkipSpace();
                if (!AtEnd() && s_.at(pos_) == u'[') { while (!AtEnd() && s_.at(pos_) != u']') ++pos_; if (!AtEnd()) ++pos_; }
                node->rows.push_back(std::move(current));
                current.clear();
                continue;
            }
            if (PeekCommand(u"end")) { pos_ += 4; (void)ReadRawGroup(); break; }
            ++pos_;
        }
        const bool trailingEmpty = current.size() == 1 && current.front()->kind == Node::Row && current.front()->children.empty();
        if (!current.empty() && !trailingEmpty) node->rows.push_back(std::move(current));
        return node;
    }

    QString s_;
    int pos_ = 0;
};

// ---------------------------------------------------------------- 排版

struct Box {
    qreal w = 0, asc = 0, desc = 0;
    qreal ink = -1;          // 单个字形的墨迹右缘；下标贴墨迹放置（斜体字形的步进宽于墨迹）
    std::function<void(QPainter&, QPointF)> paint;
};

Level Smaller(Level level) {
    switch (level) {
    case Level::Display: case Level::Text: return Level::Script;
    default: return Level::ScriptScript;
    }
}
Level FracChild(Level level) {
    switch (level) {
    case Level::Display: return Level::Text;
    case Level::Text: return Level::Script;
    default: return Level::ScriptScript;
    }
}

struct Ctx {
    Level level = Level::Text;
    QColor color;
};

class Engine {
public:
    explicit Engine(const TexStyle& style) : style_(style) {
        static const QStringList installed = [] {
            QStringList found;
            const QStringList available = QFontDatabase::families();
            for (const auto* family : {"Cambria", "Cambria Math", "STIX Two Math", "Times New Roman"})
                if (available.contains(QString::fromLatin1(family))) found << QString::fromLatin1(family);
            return found;
        }();
        mathFamilies_ = installed;
        textFamily_ = style.textFamily.isEmpty() ? QGuiApplication::font().family() : style.textFamily;
        mathFamilies_ << textFamily_;
        reference_ = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
    }

    Box Layout(const NodePtr& node, const Ctx& ctx) {
        if (!node) return {};
        switch (node->kind) {
        case Node::Row: return LayoutRow(node->children, ctx);
        case Node::Atom: return LayoutAtom(*node, ctx);
        case Node::Frac: return LayoutFrac(*node, ctx);
        case Node::Sqrt: return LayoutSqrt(*node, ctx);
        case Node::Scripts: return LayoutScripts(*node, ctx);
        case Node::Fenced: return LayoutFenced(*node, ctx);
        case Node::Array: return LayoutArray(*node, ctx);
        case Node::Accent: return LayoutAccent(*node, ctx);
        case Node::Stack: return LayoutStack(*node, ctx);
        case Node::Boxed: return LayoutBoxed(*node, ctx);
        case Node::Colored: { Ctx inner = ctx; if (node->color.isValid()) inner.color = node->color; return Layout(node->a, inner); }
        case Node::Styled: { Ctx inner = ctx; inner.level = node->level; return Layout(node->a, inner); }
        case Node::Space: { Box box; box.w = node->em * Size(ctx.level); return box; }
        }
        return {};
    }

    qreal Size(Level level) const {
        const qreal factor = level == Level::Script ? 0.72 : level == Level::ScriptScript ? 0.55 : 1.0;
        return std::max<qreal>(7.0, style_.pixelSize * factor);
    }
    qreal Rule(Level level) const { return std::max<qreal>(0.9, Size(level) * 0.05); }

    QFont FontFor(Face face, qreal px) const {
        QFont font;
        if (face == Face::Text || face == Face::TextBold) font.setFamilies({textFamily_});
        else font.setFamilies(mathFamilies_);
        font.setPointSizeF(px * 0.75);      // 参考设备与图片均为 96 DPI，逻辑像素与磅值换算固定
        font.setItalic(face == Face::Italic || face == Face::BoldItalic);
        font.setBold(face == Face::Bold || face == Face::BoldItalic || face == Face::TextBold);
        font.setStyleStrategy(QFont::PreferAntialias);
        return font;
    }

    qreal Axis(Level level) {
        const QFontMetricsF metrics(FontFor(Face::Upright, Size(level)), &reference_);
        const QRectF minus = metrics.tightBoundingRect(QStringLiteral("−"));
        return minus.isEmpty() ? Size(level) * 0.25 : -minus.center().y();
    }
    qreal XHeight(Level level) {
        const QFontMetricsF metrics(FontFor(Face::Italic, Size(level)), &reference_);
        return metrics.xHeight();
    }

    Box Glyph(const QString& text, Face face, qreal px, const QColor& color) {
        const QFont font = FontFor(face, px);
        const QFontMetricsF metrics(font, &reference_);
        const QRectF tight = metrics.tightBoundingRect(text);
        Box box;
        box.w = metrics.horizontalAdvance(text);
        if (face == Face::Italic || face == Face::BoldItalic) box.w = std::max(box.w, tight.right() + px * 0.04);
        if (!tight.isEmpty()) box.ink = tight.right();
        if (tight.isEmpty()) { box.asc = metrics.xHeight(); box.desc = 0; }
        else { box.asc = std::max<qreal>(-tight.top(), 0); box.desc = std::max<qreal>(tight.bottom(), 0); }
        const qreal shift = tight.left() < 0 && !tight.isEmpty() ? -tight.left() : 0;
        box.w += shift;
        if (box.ink >= 0) box.ink += shift;
        box.paint = [font, text, color, shift](QPainter& painter, QPointF at) {
            painter.setFont(font);
            painter.setPen(color);
            painter.drawText(QPointF(at.x() + shift, at.y()), text);
        };
        return box;
    }

    Box LayoutAtom(const Node& node, const Ctx& ctx) {
        if (node.error) {
            incomplete = true;
            return Glyph(node.text, Face::Text, Size(ctx.level), style_.errorColor);
        }
        qreal px = Size(ctx.level);
        if (node.bigOp && node.kind == Node::Atom) {
            const bool integral = !node.limitsByDefault;
            px *= ctx.level == Level::Display ? (integral ? 1.9 : 1.55) : (integral ? 1.3 : 1.15);
            Box box = Glyph(node.text, Face::Upright, px, ctx.color);
            // 大型运算符相对数学轴居中
            const qreal axis = Axis(ctx.level);
            const qreal center = (box.asc - box.desc) / 2;
            const qreal dy = center - axis;
            Box shifted;
            shifted.w = box.w; shifted.asc = box.asc - dy; shifted.desc = box.desc + dy;
            shifted.paint = [box, dy](QPainter& painter, QPointF at) { box.paint(painter, QPointF(at.x(), at.y() + dy)); };
            return shifted;
        }
        return Glyph(node.text, node.face, px, ctx.color);
    }

    static Cls ClassOf(const NodePtr& node) {
        if (!node) return Cls::None;
        switch (node->kind) {
        case Node::Atom: return node->cls;
        case Node::Scripts: return ClassOf(node->a);
        case Node::Space: return Cls::None;
        case Node::Accent: return node->bigOp ? Cls::Op : Cls::Ord;
        case Node::Stack: return node->cls;
        case Node::Fenced: return node->cls == Cls::Open || node->cls == Cls::Close ? node->cls : Cls::Ord;
        case Node::Colored: case Node::Styled: return ClassOf(node->a);
        case Node::Row: return node->children.empty() ? Cls::None : Cls::Ord;
        default: return Cls::Ord;
        }
    }

    static int Mu(Cls left, Cls right) {
        if (left == Cls::None || right == Cls::None) return 0;
        if (left == Cls::Open || right == Cls::Close || right == Cls::Punct) return left == Cls::Bin ? 4 : left == Cls::Rel ? 5 : left == Cls::Punct ? 3 : 0;
        if (left == Cls::Bin || right == Cls::Bin) return 4;
        if (left == Cls::Rel || right == Cls::Rel) return left == right ? 0 : 5;
        if (left == Cls::Punct) return 3;
        if (left == Cls::Op || right == Cls::Op) return 3;
        return 0;
    }

    Box LayoutRow(const std::vector<NodePtr>& items, const Ctx& ctx) {
        std::vector<Box> boxes;
        std::vector<Cls> classes;
        boxes.reserve(items.size());
        for (const auto& item : items) { boxes.push_back(Layout(item, ctx)); classes.push_back(ClassOf(item)); }
        // 二元运算符在行首或关系/开括号/标点之后、或在关系/闭括号之前时按普通符号处理（负号等）
        Cls previous = Cls::None;
        for (std::size_t i = 0; i < classes.size(); ++i) {
            if (classes[i] == Cls::None) continue;
            if (classes[i] == Cls::Bin && (previous == Cls::None || previous == Cls::Bin || previous == Cls::Op
                    || previous == Cls::Rel || previous == Cls::Open || previous == Cls::Punct)) classes[i] = Cls::Ord;
            previous = classes[i];
        }
        for (std::size_t i = classes.size(); i-- > 0;) {
            if (classes[i] != Cls::Bin) continue;
            std::size_t next = i + 1;
            while (next < classes.size() && classes[next] == Cls::None) ++next;
            if (next == classes.size() || classes[next] == Cls::Rel || classes[next] == Cls::Close || classes[next] == Cls::Punct)
                classes[i] = Cls::Ord;
        }
        const bool tight = ctx.level == Level::Script || ctx.level == Level::ScriptScript;
        const qreal muUnit = Size(ctx.level) / 18;
        Box row;
        std::vector<std::pair<qreal, Box>> placed;
        Cls last = Cls::None;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (classes[i] != Cls::None) {
                int mu = Mu(last, classes[i]);
                if (tight && classes[i] != Cls::Op && last != Cls::Op) mu = 0;
                row.w += mu * muUnit;
                last = classes[i];
            }
            placed.emplace_back(row.w, boxes[i]);
            row.w += boxes[i].w;
            row.asc = std::max(row.asc, boxes[i].asc);
            row.desc = std::max(row.desc, boxes[i].desc);
        }
        row.paint = [placed](QPainter& painter, QPointF at) {
            for (const auto& [x, box] : placed) if (box.paint) box.paint(painter, QPointF(at.x() + x, at.y()));
        };
        return row;
    }

    Box LayoutFrac(const Node& node, const Ctx& ctx) {
        Level outer = ctx.level;
        if (node.fracStyle == 1) outer = Level::Display;
        else if (node.fracStyle == 2) outer = Level::Text;
        Ctx child = ctx; child.level = FracChild(outer);
        const Box num = Layout(node.a, child), den = Layout(node.b, child);
        const qreal px = Size(ctx.level), t = Rule(ctx.level), axis = Axis(ctx.level);
        const bool display = outer == Level::Display;
        const qreal gap = node.noBar ? (display ? 7 * t : 3 * t) : (display ? 3 * t : 1.4 * t);
        const qreal u = std::max(display ? 0.68 * px : 0.40 * px, axis + t / 2 + gap + num.desc);
        const qreal v = std::max(display ? 0.69 * px : 0.35 * px, den.asc + gap + t / 2 - axis);
        const qreal pad = px * 0.12;
        Box box;
        box.w = std::max(num.w, den.w) + 2 * pad;
        box.asc = u + num.asc;
        box.desc = v + den.desc;
        const QColor color = ctx.color;
        const bool bar = !node.noBar;
        box.paint = [num, den, u, v, pad, t, axis, color, bar, width = box.w](QPainter& painter, QPointF at) {
            num.paint(painter, QPointF(at.x() + (width - num.w) / 2, at.y() - u));
            den.paint(painter, QPointF(at.x() + (width - den.w) / 2, at.y() + v));
            if (bar) painter.fillRect(QRectF(at.x() + pad * 0.5, at.y() - axis - t / 2, width - pad, t), color);
        };
        return box;
    }

    Box LayoutSqrt(const Node& node, const Ctx& ctx) {
        const Box body = Layout(node.a, ctx);
        const qreal px = Size(ctx.level), t = Rule(ctx.level);
        const qreal gap = ctx.level == Level::Display ? t + px * 0.18 : t + px * 0.1;
        const qreal top = -(std::max(body.asc, px * 0.5) + gap + t / 2);
        const qreal bottom = std::max(body.desc, px * 0.05) + px * 0.04;
        const qreal height = bottom - top;
        const qreal rw = px * 0.42 + height * 0.12;
        const qreal tickY = bottom - height * 0.42;
        Box index;
        qreal offset = 0;
        if (node.b) {
            Ctx small = ctx; small.level = Level::ScriptScript;
            index = Layout(node.b, small);
            offset = std::max<qreal>(0, index.w - rw * 0.55);
        }
        Box box;
        box.w = offset + rw + body.w + px * 0.12;
        box.asc = -top + t / 2 + px * 0.04;
        box.desc = bottom + t / 2;
        if (node.b) box.asc = std::max(box.asc, -(tickY - px * 0.12) + index.asc + index.desc);
        const QColor color = ctx.color;
        box.paint = [body, index, offset, rw, top, bottom, tickY, t, px, color, hasIndex = bool(node.b)](QPainter& painter, QPointF at) {
            const qreal x = at.x() + offset, y = at.y();
            QPainterPath path;
            path.moveTo(x, y + tickY);
            path.lineTo(x + rw * 0.25, y + tickY - px * 0.07);
            path.lineTo(x + rw * 0.6, y + bottom);
            path.lineTo(x + rw, y + top);
            path.lineTo(x + rw + body.w + px * 0.08, y + top);
            QPen pen(color, t); pen.setJoinStyle(Qt::MiterJoin); pen.setCapStyle(Qt::FlatCap);
            painter.setPen(pen); painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            body.paint(painter, QPointF(x + rw + px * 0.04, y));
            if (hasIndex) index.paint(painter, QPointF(at.x() + offset + rw * 0.55 - index.w, y + tickY - px * 0.12 - index.desc));
        };
        return box;
    }

    static bool UsesLimits(const NodePtr& base, Level level) {
        if (!base) return false;
        if (base->limits == 1) return true;
        if (base->limits == 2) return false;
        return base->limitsByDefault && level == Level::Display;
    }

    Box LayoutScripts(const Node& node, const Ctx& ctx) {
        const Box base = Layout(node.a, ctx);
        Ctx small = ctx; small.level = Smaller(ctx.level);
        const Box sup = node.b ? Layout(node.b, small) : Box{};
        const Box sub = node.c ? Layout(node.c, small) : Box{};
        const qreal px = Size(ctx.level), t = Rule(ctx.level);
        Box box;
        if (UsesLimits(node.a, ctx.level)) {
            const qreal gap = px * 0.14;
            box.w = std::max({base.w, node.b ? sup.w : 0.0, node.c ? sub.w : 0.0});
            const qreal supShift = base.asc + gap + sup.desc;
            const qreal subShift = base.desc + gap + sub.asc;
            box.asc = base.asc + (node.b ? gap + sup.asc + sup.desc : 0);
            box.desc = base.desc + (node.c ? gap + sub.asc + sub.desc : 0);
            box.paint = [base, sup, sub, supShift, subShift, width = box.w, hasSup = bool(node.b), hasSub = bool(node.c)](QPainter& painter, QPointF at) {
                base.paint(painter, QPointF(at.x() + (width - base.w) / 2, at.y()));
                if (hasSup) sup.paint(painter, QPointF(at.x() + (width - sup.w) / 2, at.y() - supShift));
                if (hasSub) sub.paint(painter, QPointF(at.x() + (width - sub.w) / 2, at.y() + subShift));
            };
            return box;
        }
        const qreal xh = XHeight(ctx.level);
        const bool simpleBase = node.a && node.a->kind == Node::Atom && !node.a->bigOp;
        qreal u = std::max({simpleBase ? 0.0 : base.asc - px * 0.3, ctx.level == Level::Display ? 0.41 * px : 0.36 * px, sup.desc + 0.25 * xh});
        qreal v = std::max({simpleBase ? 0.0 : base.desc + px * 0.05, 0.15 * px, sub.asc - 0.8 * xh});
        if (node.b && node.c) {
            v = std::max(v, 0.25 * px);
            const qreal clearance = (u - sup.desc) - (sub.asc - v);
            if (clearance < 4 * t) v += 4 * t - clearance;
        }
        const qreal kern = px * 0.05;
        const qreal italic = node.a && node.a->kind == Node::Atom && node.a->bigOp && !node.a->limitsByDefault ? px * 0.1 : 0;
        // 下标贴近墨迹右缘（TeX 中下标不加斜体校正），上标放在步进之后
        const qreal subX = base.ink >= 0 ? std::min(base.w, base.ink + px * 0.02) : base.w;
        box.w = std::max({base.w, node.b ? base.w + sup.w + italic + kern : 0.0, node.c ? subX + sub.w + kern : 0.0});
        box.asc = std::max(base.asc, node.b ? u + sup.asc : 0.0);
        box.desc = std::max(base.desc, node.c ? v + sub.desc : 0.0);
        box.paint = [base, sup, sub, u, v, italic, subX, hasSup = bool(node.b), hasSub = bool(node.c)](QPainter& painter, QPointF at) {
            base.paint(painter, at);
            if (hasSup) sup.paint(painter, QPointF(at.x() + base.w + italic, at.y() - u));
            if (hasSub) sub.paint(painter, QPointF(at.x() + subX, at.y() + v));
        };
        return box;
    }

    // 伸缩定界符：按高度用路径绘制，保证线宽一致；矮的直接用字形。
    Box Delimiter(const QString& delim, qreal height, const Ctx& ctx, bool forcePath) {
        const qreal px = Size(ctx.level);
        if (delim == QStringLiteral(".") || delim.isEmpty()) { Box empty; empty.w = px * 0.12; return empty; }
        if (!forcePath && height <= px * 1.18) return Glyph(delim, Face::Upright, px, ctx.color);
        const qreal axis = Axis(ctx.level);
        const qreal top = -axis - height / 2, bottom = -axis + height / 2;
        const qreal stroke = std::max(Rule(ctx.level) * 1.15, px * 0.055);
        const qreal w = std::min(px * 0.5, px * 0.28 + height * 0.06);
        Box box;
        box.w = w + px * 0.08;
        box.asc = -top + stroke / 2;
        box.desc = bottom + stroke / 2;
        const QColor color = ctx.color;
        box.paint = [delim, top, bottom, w, stroke, color, px](QPainter& painter, QPointF at) {
            const qreal x0 = at.x() + px * 0.04, x1 = x0 + w, yt = at.y() + top, yb = at.y() + bottom, ym = (yt + yb) / 2;
            QPainterPath path;
            const QString d = delim;
            if (d == QStringLiteral("(")) { path.moveTo(x1, yt); path.cubicTo(x0, yt + (yb - yt) * 0.22, x0, yb - (yb - yt) * 0.22, x1, yb); }
            else if (d == QStringLiteral(")")) { path.moveTo(x0, yt); path.cubicTo(x1, yt + (yb - yt) * 0.22, x1, yb - (yb - yt) * 0.22, x0, yb); }
            else if (d == QStringLiteral("[")) { path.moveTo(x1, yt); path.lineTo(x0 + w * 0.3, yt); path.lineTo(x0 + w * 0.3, yb); path.lineTo(x1, yb); }
            else if (d == QStringLiteral("]")) { path.moveTo(x0, yt); path.lineTo(x1 - w * 0.3, yt); path.lineTo(x1 - w * 0.3, yb); path.lineTo(x0, yb); }
            else if (d == QStringLiteral("⌊")) { path.moveTo(x0 + w * 0.3, yt); path.lineTo(x0 + w * 0.3, yb); path.lineTo(x1, yb); }
            else if (d == QStringLiteral("⌋")) { path.moveTo(x1 - w * 0.3, yt); path.lineTo(x1 - w * 0.3, yb); path.lineTo(x0, yb); }
            else if (d == QStringLiteral("⌈")) { path.moveTo(x1, yt); path.lineTo(x0 + w * 0.3, yt); path.lineTo(x0 + w * 0.3, yb); }
            else if (d == QStringLiteral("⌉")) { path.moveTo(x0, yt); path.lineTo(x1 - w * 0.3, yt); path.lineTo(x1 - w * 0.3, yb); }
            else if (d == QStringLiteral("{") || d == QStringLiteral("}")) {
                const bool left = d == QStringLiteral("{");
                const qreal tip = left ? x0 : x1, end = left ? x1 : x0, mid = (x0 + x1) / 2;
                const qreal h = yb - yt;
                path.moveTo(end, yt);
                path.cubicTo(mid, yt, mid, yt, mid, yt + h * 0.12);
                path.lineTo(mid, ym - h * 0.1);
                path.cubicTo(mid, ym - h * 0.02, mid, ym, tip, ym);
                path.cubicTo(mid, ym, mid, ym + h * 0.02, mid, ym + h * 0.1);
                path.lineTo(mid, yb - h * 0.12);
                path.cubicTo(mid, yb, mid, yb, end, yb);
            } else if (d == QStringLiteral("⟨")) { path.moveTo(x1, yt); path.lineTo(x0, ym); path.lineTo(x1, yb); }
            else if (d == QStringLiteral("⟩")) { path.moveTo(x0, yt); path.lineTo(x1, ym); path.lineTo(x0, yb); }
            else if (d == QStringLiteral("‖")) { path.moveTo(x0 + w * 0.3, yt); path.lineTo(x0 + w * 0.3, yb); path.moveTo(x1 - w * 0.3, yt); path.lineTo(x1 - w * 0.3, yb); }
            else if (d == QStringLiteral("/")) { path.moveTo(x1, yt); path.lineTo(x0, yb); }
            else { path.moveTo((x0 + x1) / 2, yt); path.lineTo((x0 + x1) / 2, yb); }   // | 及其他
            QPen pen(color, stroke); pen.setCapStyle(Qt::FlatCap); pen.setJoinStyle(Qt::MiterJoin);
            painter.setPen(pen); painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
        };
        return box;
    }

    Box LayoutFenced(const Node& node, const Ctx& ctx) {
        const Box inner = Layout(node.a, ctx);
        const qreal px = Size(ctx.level), axis = Axis(ctx.level);
        const bool fixed = node.em > 0;
        const qreal height = fixed ? node.em * px
            : std::max(2 * std::max(inner.asc - axis, inner.desc + axis) + px * 0.12, px * 1.0);
        const Box left = Delimiter(node.text, height, ctx, fixed);
        const Box right = Delimiter(node.text2, height, ctx, fixed);
        Box box;
        box.w = left.w + inner.w + right.w;
        box.asc = std::max({left.asc, inner.asc, right.asc});
        box.desc = std::max({left.desc, inner.desc, right.desc});
        box.paint = [left, inner, right](QPainter& painter, QPointF at) {
            if (left.paint) left.paint(painter, at);
            if (inner.paint) inner.paint(painter, QPointF(at.x() + left.w, at.y()));
            if (right.paint) right.paint(painter, QPointF(at.x() + left.w + inner.w, at.y()));
        };
        return box;
    }

    Box LayoutArray(const Node& node, const Ctx& ctx) {
        Ctx cell = ctx;
        cell.level = node.level == Level::Display && ctx.level == Level::Display ? Level::Display
            : ctx.level == Level::Display ? Level::Text : ctx.level;
        const qreal px = Size(ctx.level);
        std::size_t columns = 0;
        for (const auto& row : node.rows) columns = std::max(columns, row.size());
        std::vector<std::vector<Box>> boxes;
        std::vector<qreal> widths(columns, 0), ascents, descents;
        for (const auto& row : node.rows) {
            std::vector<Box> line;
            qreal asc = px * 0.7, desc = px * 0.25;
            for (std::size_t c = 0; c < row.size(); ++c) {
                line.push_back(Layout(row[c], cell));
                widths[c] = std::max(widths[c], line.back().w);
                asc = std::max(asc, line.back().asc);
                desc = std::max(desc, line.back().desc);
            }
            boxes.push_back(std::move(line));
            ascents.push_back(asc);
            descents.push_back(desc);
        }
        const qreal rowGap = px * (node.alternateAlign ? 0.3 : 0.35);
        std::vector<qreal> xs(columns, 0);
        qreal width = 0;
        for (std::size_t c = 0; c < columns; ++c) {
            if (c > 0) width += node.alternateAlign ? (c % 2 == 0 ? px * 1.2 : 0) : px * node.colGap;
            xs[c] = width;
            width += widths[c];
        }
        qreal height = 0;
        for (std::size_t r = 0; r < boxes.size(); ++r) height += ascents[r] + descents[r] + (r ? rowGap : 0);
        const qreal axis = Axis(ctx.level);
        const qreal top = -axis - height / 2;
        std::vector<qreal> baselines;
        qreal y = top;
        for (std::size_t r = 0; r < boxes.size(); ++r) {
            if (r) y += rowGap;
            baselines.push_back(y + ascents[r]);
            y += ascents[r] + descents[r];
        }
        QString align = node.align;
        Box body;
        body.w = width;
        body.asc = -top;
        body.desc = height + top;
        body.paint = [boxes, xs, widths, baselines, align, alternate = node.alternateAlign](QPainter& painter, QPointF at) {
            for (std::size_t r = 0; r < boxes.size(); ++r) {
                for (std::size_t c = 0; c < boxes[r].size(); ++c) {
                    const auto& box = boxes[r][c];
                    if (!box.paint) continue;
                    const QChar a = alternate ? QChar(c % 2 == 0 ? u'r' : u'l') : c < static_cast<std::size_t>(align.size()) ? align.at(static_cast<int>(c)) : QChar(u'c');
                    const qreal dx = a == u'l' ? 0 : a == u'r' ? widths[c] - box.w : (widths[c] - box.w) / 2;
                    box.paint(painter, QPointF(at.x() + xs[c] + dx, at.y() + baselines[r]));
                }
            }
        };
        if (node.text == QStringLiteral(".") && node.text2 == QStringLiteral(".")) return body;
        const qreal delimHeight = height + px * 0.2;
        const Box left = Delimiter(node.text, delimHeight, ctx, true);
        const Box right = Delimiter(node.text2, delimHeight, ctx, true);
        const qreal padLeft = node.text == QStringLiteral(".") ? 0 : px * 0.12;
        const qreal padRight = node.text2 == QStringLiteral(".") ? 0 : px * 0.12;
        Box box;
        box.w = left.w + padLeft + body.w + padRight + right.w;
        box.asc = std::max({left.asc, body.asc, right.asc});
        box.desc = std::max({left.desc, body.desc, right.desc});
        box.paint = [left, body, right, padLeft, padRight](QPainter& painter, QPointF at) {
            if (left.paint) left.paint(painter, at);
            body.paint(painter, QPointF(at.x() + left.w + padLeft, at.y()));
            if (right.paint) right.paint(painter, QPointF(at.x() + left.w + padLeft + body.w + padRight, at.y()));
        };
        return box;
    }

    Box LayoutAccent(const Node& node, const Ctx& ctx) {
        const Box body = Layout(node.a, ctx);
        const qreal px = Size(ctx.level), t = Rule(ctx.level);
        const QString kind = node.text;
        const bool below = kind == QStringLiteral("underline") || kind == QStringLiteral("underbrace");
        const bool brace = kind.endsWith(QStringLiteral("brace"));
        const qreal gap = px * (brace ? 0.12 : 0.08);
        const qreal mark = brace ? px * 0.32 : kind == QStringLiteral("overline") || kind == QStringLiteral("underline")
            || kind == QStringLiteral("bar") ? t : px * 0.2;
        Box box;
        box.w = std::max(body.w, px * 0.5);
        box.asc = body.asc + (below ? 0 : gap + mark);
        box.desc = body.desc + (below ? gap + mark : 0);
        const QColor color = ctx.color;
        const qreal bodyX = (box.w - body.w) / 2;
        box.paint = [body, kind, below, gap, mark, t, px, color, bodyX, width = box.w](QPainter& painter, QPointF at) {
            body.paint(painter, QPointF(at.x() + bodyX, at.y()));
            const qreal y0 = below ? at.y() + body.desc + gap : at.y() - body.asc - gap - mark;   // 记号区顶部
            const bool narrow = kind == QStringLiteral("hat") || kind == QStringLiteral("tilde") || kind == QStringLiteral("dot")
                || kind == QStringLiteral("ddot") || kind == QStringLiteral("check") || kind == QStringLiteral("bar");
            const qreal span = narrow ? std::min(width, px * 0.55) : width;
            const qreal x0 = at.x() + (width - span) / 2, x1 = x0 + span, xm = (x0 + x1) / 2;
            QPen pen(color, std::max(t, px * 0.05)); pen.setCapStyle(Qt::RoundCap); pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen); painter.setBrush(Qt::NoBrush);
            QPainterPath path;
            if (kind == QStringLiteral("overline") || kind == QStringLiteral("underline") || kind == QStringLiteral("bar")) {
                painter.fillRect(QRectF(x0, y0, span, t), color);
                return;
            }
            if (kind == QStringLiteral("hat") || kind == QStringLiteral("widehat")) { path.moveTo(x0, y0 + mark); path.lineTo(xm, y0); path.lineTo(x1, y0 + mark); }
            else if (kind == QStringLiteral("check")) { path.moveTo(x0, y0); path.lineTo(xm, y0 + mark); path.lineTo(x1, y0); }
            else if (kind == QStringLiteral("tilde") || kind == QStringLiteral("widetilde")) {
                path.moveTo(x0, y0 + mark * 0.75);
                path.cubicTo(x0 + span * 0.3, y0 - mark * 0.2, x0 + span * 0.7, y0 + mark * 1.2, x1, y0 + mark * 0.25);
            } else if (kind == QStringLiteral("vec") || kind == QStringLiteral("overrightarrow")) {
                const qreal ym = y0 + mark * 0.55;
                path.moveTo(x0, ym); path.lineTo(x1, ym);
                path.moveTo(x1 - px * 0.15, ym - px * 0.1); path.lineTo(x1, ym); path.lineTo(x1 - px * 0.15, ym + px * 0.1);
            } else if (kind == QStringLiteral("dot") || kind == QStringLiteral("ddot")) {
                painter.setBrush(color); painter.setPen(Qt::NoPen);
                const qreal r = px * 0.06;
                if (kind == QStringLiteral("dot")) painter.drawEllipse(QPointF(xm, y0 + mark * 0.6), r, r);
                else { painter.drawEllipse(QPointF(xm - px * 0.12, y0 + mark * 0.6), r, r); painter.drawEllipse(QPointF(xm + px * 0.12, y0 + mark * 0.6), r, r); }
                return;
            } else if (kind == QStringLiteral("overbrace") || kind == QStringLiteral("underbrace")) {
                const qreal edge = below ? y0 : y0 + mark, tipY = below ? y0 + mark : y0, midY = (edge + tipY) / 2;
                path.moveTo(x0, edge);
                path.quadTo(x0, midY, x0 + px * 0.2, midY);
                path.lineTo(xm - px * 0.2, midY);
                path.quadTo(xm, midY, xm, tipY);
                path.quadTo(xm, midY, xm + px * 0.2, midY);
                path.lineTo(x1 - px * 0.2, midY);
                path.quadTo(x1, midY, x1, edge);
            }
            painter.drawPath(path);
        };
        return box;
    }

    Box LayoutStack(const Node& node, const Ctx& ctx) {
        const Box body = Layout(node.a, ctx);
        Ctx small = ctx; small.level = Smaller(ctx.level);
        const Box extra = Layout(node.b, small);
        const qreal gap = Size(ctx.level) * 0.1;
        const bool below = node.text == QStringLiteral("underset");
        Box box;
        box.w = std::max(body.w, extra.w);
        box.asc = body.asc + (below ? 0 : gap + extra.asc + extra.desc);
        box.desc = body.desc + (below ? gap + extra.asc + extra.desc : 0);
        box.paint = [body, extra, gap, below, width = box.w](QPainter& painter, QPointF at) {
            body.paint(painter, QPointF(at.x() + (width - body.w) / 2, at.y()));
            const qreal y = below ? at.y() + body.desc + gap + extra.asc : at.y() - body.asc - gap - extra.desc;
            extra.paint(painter, QPointF(at.x() + (width - extra.w) / 2, y));
        };
        return box;
    }

    Box LayoutBoxed(const Node& node, const Ctx& ctx) {
        const Box body = Layout(node.a, ctx);
        const qreal pad = Size(ctx.level) * 0.22, t = Rule(ctx.level);
        Box box;
        box.w = body.w + 2 * pad;
        box.asc = body.asc + pad;
        box.desc = body.desc + pad;
        const QColor color = ctx.color;
        box.paint = [body, pad, t, color, box](QPainter& painter, QPointF at) {
            body.paint(painter, QPointF(at.x() + pad, at.y()));
            painter.setPen(QPen(color, t)); painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(at.x() + t / 2, at.y() - box.asc + t / 2, box.w - t, box.asc + box.desc - t));
        };
        return box;
    }

    bool incomplete = false;

private:
    TexStyle style_;
    QStringList mathFamilies_;
    QString textFamily_;
    QImage reference_;
};

} // namespace

TexImage RenderTex(const QString& source, const TexStyle& style) {
    TexImage result;
    constexpr int kMaxSource = 8000;
    Parser parser(source.left(kMaxSource));
    const NodePtr root = parser.Parse();
    Engine engine(style);
    Ctx ctx;
    ctx.level = style.display ? Level::Display : Level::Text;
    ctx.color = style.color;
    const Box box = engine.Layout(root, ctx);
    const qreal margin = std::max<qreal>(1.0, style.pixelSize * 0.08);
    const qreal dpr = std::clamp<qreal>(style.devicePixelRatio, 1.0, 4.0);
    const int width = std::max(1, static_cast<int>(std::ceil(box.w + 2 * margin)));
    const int height = std::max(1, static_cast<int>(std::ceil(box.asc + box.desc + 2 * margin)));
    constexpr int kMaxPixels = 8192;
    if (width * dpr > kMaxPixels || height * dpr > kMaxPixels) {
        result.complete = false;
        return result;
    }
    result.image = QImage(QSize(static_cast<int>(std::ceil(width * dpr)), static_cast<int>(std::ceil(height * dpr))),
        QImage::Format_ARGB32_Premultiplied);
    result.image.setDevicePixelRatio(dpr);
    result.image.fill(Qt::transparent);
    {
        QPainter painter(&result.image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        if (box.paint) box.paint(painter, QPointF(margin, margin + box.asc));
    }
    result.width = width;
    result.height = height;
    result.baseline = static_cast<int>(std::round(margin + box.asc));
    result.complete = parser.complete && !engine.incomplete && source.size() <= kMaxSource;
    return result;
}

} // namespace War3Map::UiKit
