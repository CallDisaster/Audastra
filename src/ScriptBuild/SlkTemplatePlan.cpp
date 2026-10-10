#include "War3MapCore/Script/SlkTemplatePlan.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

// 独立的闭集语法和求值器。这里的 capability 是内部标签，不是 Lua 模块或宿主对象。
namespace War3Map::Script {
namespace {

constexpr std::int64_t kExactInteger = 9007199254740991LL;

struct Failure {
    std::string code;
    std::string message;
    SlkTemplateSource source;
};

[[noreturn]] void Fail(std::string code, std::string message, SlkTemplateSource source) {
    throw Failure{std::move(code), std::move(message), source};
}

void Spend(std::size_t& used, std::size_t amount, std::size_t limit, SlkTemplateSource source,
    std::string_view name) {
    if (used > limit || amount > limit - used) {
        Fail("slk-plan.budget", "超出预算：" + std::string(name), source);
    }
    used += amount;
}

bool Alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool Digit(char c) { return c >= '0' && c <= '9'; }
bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
bool Rawcode(std::string_view text) {
    return text.size() == 4 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}
bool StaticAbilityId(std::string_view text) {
    return text.size() == 4 && std::all_of(text.begin(), text.end(), [](char c) {
        return Digit(c) || (Alpha(c) && c != '_');
    });
}

enum class TokenKind { End, Name, Number, String, Symbol };
struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    SlkTemplateSource source;
};

using NumberLiteral = std::variant<std::int64_t, double>;
NumberLiteral ReadNumber(const Token& token) {
    const auto& text = token.text;
    if (text.find_first_of(".eE") == std::string::npos) {
        std::int64_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > kExactInteger) {
            Fail("slk-plan.integer", "整数字面量超出精确范围", token.source);
        }
        return value;
    }
    double value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) {
        Fail("slk-plan.nonfinite", "实数字面量不是可表示的有限数", token.source);
    }
    return value;
}

class Lexer {
public:
    Lexer(std::string_view source, const SlkTemplatePlanLimits& limits) : source_(source), limits_(limits) {}

    std::vector<Token> Run() {
        std::vector<Token> tokens;
        if (source_.starts_with("\xef\xbb\xbf")) Advance(3);
        while (true) {
            Skip();
            Token token;
            token.source = Here();
            if (at_ == source_.size()) {
                tokens.push_back(std::move(token));
                return tokens;
            }
            if (tokens.size() >= limits_.tokens) Fail("slk-plan.budget", "超出预算：词法记号", token.source);
            const auto begin = at_;
            const char c = source_[at_];
            if (Alpha(c)) {
                token.kind = TokenKind::Name;
                do { Advance(); } while (at_ < source_.size() && (Alpha(source_[at_]) || Digit(source_[at_])));
            } else if (Digit(c) || (c == '.' && at_ + 1 < source_.size() && Digit(source_[at_ + 1]))) {
                token.kind = TokenKind::Number;
                while (at_ < source_.size() && Digit(source_[at_])) Advance();
                if (at_ < source_.size() && source_[at_] == '.') {
                    Advance();
                    while (at_ < source_.size() && Digit(source_[at_])) Advance();
                }
                if (at_ < source_.size() && (source_[at_] == 'e' || source_[at_] == 'E')) {
                    Advance();
                    if (at_ < source_.size() && (source_[at_] == '+' || source_[at_] == '-')) Advance();
                    const auto exponent = at_;
                    while (at_ < source_.size() && Digit(source_[at_])) Advance();
                    if (exponent == at_) Fail("slk-plan.number", "指数缺少数字", token.source);
                }
                // Lua 5.3 read_numeral 将相邻点纳入数字；1..2 是非法数字，不能拆成 1 .. 2。
                // https://www.lua.org/source/5.3/llex.c.html#read_numeral
                if (at_ < source_.size() && (Alpha(source_[at_]) || source_[at_] == '.')) {
                    Fail("slk-plan.number", "不支持此数字字面量；数字与连接运算符之间需要分隔", token.source);
                }
            } else if (c == '\'' || c == '"') {
                token.kind = TokenKind::String;
                Advance();
                while (at_ < source_.size() && source_[at_] != c) {
                    char value = source_[at_];
                    if (value == '\n' || value == '\r' || value == '\0') Fail("slk-plan.string", "字符串包含换行或 NUL", Here());
                    Advance();
                    if (value == '\\') {
                        if (at_ == source_.size()) Fail("slk-plan.string", "未结束的字符串转义", token.source);
                        value = source_[at_];
                        Advance();
                        switch (value) {
                        case 'n': value = '\n'; break;
                        case 'r': value = '\r'; break;
                        case 't': value = '\t'; break;
                        case '\\': case '\'': case '"': break;
                        default: Fail("slk-plan.string", "不支持此字符串转义", token.source);
                        }
                    }
                    if (token.text.size() >= limits_.stringBytes) Fail("slk-plan.budget", "超出预算：字符串长度", token.source);
                    token.text.push_back(value);
                }
                if (at_ == source_.size()) Fail("slk-plan.string", "字符串未闭合", token.source);
                Advance();
            } else {
                token.kind = TokenKind::Symbol;
                static constexpr std::string_view pairs[] = {"<?", "?>", "..", "==", "~=", "<=", ">="};
                bool paired = false;
                for (const auto pair : pairs) {
                    if (source_.substr(at_, 2) == pair) { Advance(2); paired = true; break; }
                }
                if (!paired) {
                    if (std::string_view("+-*/%^=<>.,:;(){}[]").find(c) == std::string_view::npos) {
                        Fail("slk-plan.syntax", "不支持的字符", token.source);
                    }
                    Advance();
                }
            }
            token.source.length = at_ - begin;
            if (token.kind != TokenKind::String) token.text = std::string(source_.substr(begin, at_ - begin));
            tokens.push_back(std::move(token));
        }
    }

private:
    SlkTemplateSource Here() const { return {at_, 1, line_, column_}; }
    void Advance(std::size_t count = 1) {
        while (count-- != 0) {
            if (source_[at_++] == '\n') { ++line_; column_ = 1; } else ++column_;
        }
    }
    void Skip() {
        while (at_ < source_.size()) {
            if (Space(source_[at_])) { Advance(); continue; }
            if (source_.substr(at_, 2) != "--") return;
            const auto start = Here();
            Advance(2);
            if (source_.substr(at_, 2) == "[[") {
                Advance(2);
                const auto end = source_.find("]]", at_);
                if (end == std::string_view::npos) Fail("slk-plan.syntax", "长注释未闭合", start);
                Advance(end + 2 - at_);
            } else {
                if (at_ < source_.size() && source_[at_] == '[') Fail("slk-plan.syntax", "仅支持 --[[...]] 长注释", start);
                while (at_ < source_.size() && source_[at_] != '\n') Advance();
            }
        }
    }
    std::string_view source_;
    const SlkTemplatePlanLimits& limits_;
    std::size_t at_ = 0, line_ = 1, column_ = 1;
};

enum class ExpressionKind { Number, String, Boolean, Variable, Array, Index, Unary, Binary, Capability, NewAbility };
struct Expression {
    ExpressionKind kind = ExpressionKind::Number;
    Token token;
    std::string extra;
    std::vector<std::size_t> children;
    std::size_t height = 1;
};
enum class StatementKind { Local, Assign, Field, For, If };
struct Statement {
    StatementKind kind = StatementKind::Local;
    Token token;
    std::string field;
    std::vector<std::size_t> expressions;
    std::vector<std::vector<std::size_t>> blocks;
};
struct Syntax {
    std::vector<Expression> expressions;
    std::vector<Statement> statements;
    std::vector<std::size_t> root;
};

class Parser {
public:
    Parser(const std::vector<Token>& tokens, const SlkTemplatePlanLimits& limits) : tokens_(tokens), limits_(limits) {}
    Syntax Run() {
        const bool wrapped = Take("<?");
        syntax_.root = Block(1);
        if (wrapped) Expect("?>");
        if (Current().kind != TokenKind::End) Fail("slk-plan.syntax", "模板块末尾存在未知语句或多余内容", Current().source);
        return std::move(syntax_);
    }
private:
    const Token& Current() const { return tokens_[at_]; }
    bool Is(std::string_view text) const { return Current().kind != TokenKind::String && Current().text == text; }
    bool Take(std::string_view text) { if (!Is(text)) return false; ++at_; return true; }
    void Expect(std::string_view text) {
        if (!Take(text)) Fail("slk-plan.syntax", "预期 " + std::string(text), Current().source);
    }
    Token Name(bool localName = true) {
        static constexpr std::string_view reserved[] = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function",
            "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while", "require"};
        const auto result = Current();
        if (result.kind != TokenKind::Name || std::find(std::begin(reserved), std::end(reserved), result.text) != std::end(reserved)) {
            Fail("slk-plan.syntax", "预期局部名称；不支持此语句或关键字", result.source);
        }
        // Lua 的自由名称经 _ENV 查找；不能把被替换环境中的 require 当成固定 capability。
        if (localName && result.text == "_ENV")
            Fail("slk-plan.environment", "不支持读取、绑定或替换 Lua _ENV 环境", result.source);
        ++at_;
        return result;
    }
    void Depth(std::size_t depth, SlkTemplateSource source) const {
        if (depth > limits_.depth) Fail("slk-plan.budget", "超出预算：语法深度", source);
    }
    std::size_t Add(Expression expression) {
        // 无论运行时分支是否命中，非法数字字面量也不能藏在语法树中。
        if (expression.kind == ExpressionKind::Number) (void)ReadNumber(expression.token);
        for (const auto child : expression.children) expression.height = std::max(expression.height, syntax_.expressions[child].height + 1);
        Depth(expression.height, expression.token.source);
        Spend(nodes_, 1, limits_.syntaxNodes, expression.token.source, "语法节点");
        syntax_.expressions.push_back(std::move(expression));
        return syntax_.expressions.size() - 1;
    }
    std::size_t Add(Statement statement) {
        Spend(nodes_, 1, limits_.syntaxNodes, statement.token.source, "语法节点");
        syntax_.statements.push_back(std::move(statement));
        return syntax_.statements.size() - 1;
    }
    std::vector<std::size_t> Block(std::size_t depth) {
        Depth(depth, Current().source);
        std::vector<std::size_t> result;
        while (Current().kind != TokenKind::End && !Is("end") && !Is("else") && !Is("elseif") && !Is("?>")) {
            if (Take(";")) continue;
            result.push_back(ParseStatement(depth));
        }
        return result;
    }
    std::size_t ParseStatement(std::size_t depth) {
        Statement statement;
        statement.token = Current();
        if (Take("local")) {
            statement.kind = StatementKind::Local;
            statement.token = Name();
            Expect("=");
            statement.expressions.push_back(Initializer(depth + 1));
        } else if (Take("for")) {
            statement.kind = StatementKind::For;
            statement.token = Name();
            Expect("=");
            statement.expressions.push_back(Expr(0, depth + 1));
            Expect(",");
            statement.expressions.push_back(Expr(0, depth + 1));
            if (Take(",")) statement.expressions.push_back(Expr(0, depth + 1));
            Expect("do");
            statement.blocks.push_back(Block(depth + 1));
            Expect("end");
        } else if (Take("if")) {
            statement.kind = StatementKind::If;
            do {
                statement.expressions.push_back(Expr(0, depth + 1));
                Expect("then");
                statement.blocks.push_back(Block(depth + 1));
            } while (Take("elseif"));
            if (Take("else")) statement.blocks.push_back(Block(depth + 1));
            Expect("end");
        } else {
            statement.token = Name();
            statement.kind = StatementKind::Assign;
            if (Take(".")) { statement.kind = StatementKind::Field; statement.field = Name(false).text; }
            Expect("=");
            statement.expressions.push_back(Expr(0, depth + 1));
        }
        return Add(std::move(statement));
    }
    std::size_t Initializer(std::size_t depth) {
        Depth(depth, Current().source);
        if (Is("require")) {
            const auto token = Current();
            ++at_;
            const bool parenthesis = Take("(");
            if (Current().kind != TokenKind::String || Current().text != "slk") Fail("slk-plan.capability", "只允许局部 require 'slk' capability", Current().source);
            ++at_;
            if (parenthesis) Expect(")");
            return Add({ExpressionKind::Capability, token, {}, {}, 1});
        }
        if (Current().kind == TokenKind::Name && tokens_[at_ + 1].text == ".") {
            const auto capability = Name();
            Expect("."); Expect("ability"); Expect(".");
            const auto parent = Name(false);
            if (!Rawcode(parent.text)) Fail("slk-plan.rawcode", "父能力 ID 必须是四字节 rawcode", parent.source);
            Expect(":"); Expect("new"); Expect("(");
            const auto id = Expr(0, depth + 1);
            Expect(")");
            return Add({ExpressionKind::NewAbility, capability, parent.text, {id}, 1});
        }
        return Expr(0, depth);
    }
    static int Binding(std::string_view token) {
        if (token == "==" || token == "~=" || token == "<" || token == "<=" || token == ">" || token == ">=") return 3;
        if (token == "..") return 4;
        if (token == "+" || token == "-") return 5;
        if (token == "*" || token == "/" || token == "%") return 6;
        if (token == "^") return 8;
        return -1;
    }
    std::size_t Expr(int minimum, std::size_t depth) {
        Depth(depth, Current().source);
        const auto token = Current();
        std::size_t left = 0;
        bool prefix = false;
        if (Take("-")) {
            left = Add({ExpressionKind::Unary, token, {}, {Expr(7, depth + 1)}, 1});
        } else if (Take("(")) {
            left = Expr(0, depth + 1);
            Expect(")");
            prefix = true;
        } else if (Take("{")) {
            Expression array{ExpressionKind::Array, token, {}, {}, 1};
            if (!Take("}")) {
                do {
                    array.children.push_back(Expr(0, depth + 1));
                    if (Is("}")) break;
                    Expect(",");
                } while (!Is("}"));
                Expect("}");
            }
            left = Add(std::move(array));
        } else if (token.kind == TokenKind::Number || token.kind == TokenKind::String || Is("true") || Is("false")) {
            ++at_;
            left = Add({token.kind == TokenKind::Number ? ExpressionKind::Number
                : token.kind == TokenKind::String ? ExpressionKind::String : ExpressionKind::Boolean, token, {}, {}, 1});
        } else {
            left = Add({ExpressionKind::Variable, Name(), {}, {}, 1});
            prefix = true;
        }
        while (true) {
            if (Is("[")) {
                // Lua 后缀索引仅属于 prefixexp；表构造器须写成 ({...})[i]。
                if (!prefix) Fail("slk-plan.syntax", "后缀索引只允许名称或括号表达式", Current().source);
                const auto indexToken = Current();
                ++at_;
                const auto index = Expr(0, depth + 1);
                Expect("]");
                left = Add({ExpressionKind::Index, indexToken, {}, {left, index}, 1});
                continue;
            }
            const auto op = Current();
            const int binding = op.kind == TokenKind::Symbol ? Binding(op.text) : -1;
            if (binding < minimum) break;
            ++at_;
            const bool rightAssociative = op.text == "^" || op.text == "..";
            const auto right = Expr(binding + (rightAssociative ? 0 : 1), depth + 1);
            left = Add({ExpressionKind::Binary, op, {}, {left, right}, 1});
            prefix = false;
        }
        return left;
    }
    const std::vector<Token>& tokens_;
    const SlkTemplatePlanLimits& limits_;
    std::size_t at_ = 0, nodes_ = 0;
    Syntax syntax_;
};

struct Capability {};
struct AbilityReference { std::size_t index = 0; };
using Array = std::vector<SlkTemplateScalar>;
using Value = std::variant<std::int64_t, double, std::string, bool, std::shared_ptr<const Array>, Capability, AbilityReference>;
struct BindingValue { Value value; bool readOnly = false; };

class Planner {
public:
    Planner(const Syntax& syntax, const SlkTemplatePlanLimits& limits) : syntax_(syntax), limits_(limits) {}
    SlkTemplatePlan Run() { Block(syntax_.root, 1); return std::move(plan_); }
private:
    void Depth(std::size_t depth, SlkTemplateSource source) const {
        if (depth > limits_.depth) Fail("slk-plan.budget", "超出预算：求值深度", source);
    }
    void Step(SlkTemplateSource source) { Spend(plan_.executedSteps, 1, limits_.executedSteps, source, "求值步数"); }
    void TextBudget(std::size_t size, SlkTemplateSource source) {
        if (size > limits_.stringBytes) Fail("slk-plan.budget", "超出预算：字符串长度", source);
        Spend(stringBytes_, size, limits_.totalStringBytes, source, "累计字符串字节");
    }
    Value Copy(const Value& value, SlkTemplateSource source) {
        if (const auto* text = std::get_if<std::string>(&value)) TextBudget(text->size(), source);
        return value;
    }
    BindingValue& Find(const Token& name) {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            const auto found = scope->find(name.text);
            if (found != scope->end()) return found->second;
        }
        Fail("slk-plan.local", "找不到局部名称：" + name.text, name.source);
    }
    void Declare(const Token& name, Value value, bool readOnly = false) {
        Spend(bindings_, 1, limits_.localBindings, name.source, "局部变量创建数");
        scopes_.back().insert_or_assign(name.text, BindingValue{std::move(value), readOnly});
    }
    static bool IsNumber(const Value& value) { return std::holds_alternative<std::int64_t>(value) || std::holds_alternative<double>(value); }
    static double Number(const Value& value, SlkTemplateSource source) {
        if (const auto* integer = std::get_if<std::int64_t>(&value)) return static_cast<double>(*integer);
        if (const auto* real = std::get_if<double>(&value)) return *real;
        Fail("slk-plan.type", "预期数值（不隐式转换字符串或 boolean）", source);
    }
    static std::int64_t Integer(const Value& value, SlkTemplateSource source) {
        const auto number = Number(value, source);
        if (!std::isfinite(number) || std::trunc(number) != number || std::abs(number) > static_cast<double>(kExactInteger)) {
            Fail("slk-plan.integer", "需要可精确表示的有限整数", source);
        }
        return static_cast<std::int64_t>(number);
    }
    static Value Numeric(double value, bool integer, SlkTemplateSource source) {
        if (!std::isfinite(value)) Fail("slk-plan.nonfinite", "数值运算产生非有限值", source);
        if (integer) {
            if (std::abs(value) > static_cast<double>(kExactInteger)) Fail("slk-plan.integer", "整数运算超出精确范围", source);
            return static_cast<std::int64_t>(value);
        }
        return value;
    }
    SlkTemplateScalar Scalar(Value value, SlkTemplateSource source) {
        if (const auto* integer = std::get_if<std::int64_t>(&value)) return *integer;
        if (const auto* real = std::get_if<double>(&value)) return *real;
        if (auto* text = std::get_if<std::string>(&value)) return std::move(*text);
        if (const auto* boolean = std::get_if<bool>(&value)) return *boolean;
        Fail("slk-plan.type", "只允许整数、实数、字符串或 boolean 标量", source);
    }
    Value FromScalar(const SlkTemplateScalar& value, SlkTemplateSource source) {
        return std::visit([&](const auto& scalar) -> Value {
            if constexpr (std::is_same_v<std::decay_t<decltype(scalar)>, std::string>) TextBudget(scalar.size(), source);
            return scalar;
        }, value);
    }
    std::string ConcatText(const Value& value, SlkTemplateSource source) {
        if (const auto* text = std::get_if<std::string>(&value)) { TextBudget(text->size(), source); return *text; }
        char bytes[128];
        std::to_chars_result converted;
        if (const auto* integer = std::get_if<std::int64_t>(&value)) converted = std::to_chars(std::begin(bytes), std::end(bytes), *integer);
        else if (std::holds_alternative<double>(value)) {
            // Lua 浮点转字符串的格式由目标运行时决定；未经对照验证不得用 to_chars 猜测。
            Fail("slk-plan.float-concat", "尚未验证目标 Lua 的浮点转字符串格式；连接只接受字符串或整数", source);
        } else Fail("slk-plan.type", "连接只接受字符串或整数", source);
        if (converted.ec != std::errc{}) Fail("slk-plan.number", "数值无法转换为字符串", source);
        const auto length = static_cast<std::size_t>(converted.ptr - bytes);
        TextBudget(length, source);
        return std::string(bytes, length);
    }
    Value Binary(const Expression& expression, Value left, Value right) {
        const auto source = expression.token.source;
        const auto& op = expression.token.text;
        if (op == "..") {
            auto text = ConcatText(left, source);
            const auto tail = ConcatText(right, source);
            if (tail.size() > limits_.stringBytes - text.size()) Fail("slk-plan.budget", "超出预算：连接字符串长度", source);
            TextBudget(text.size() + tail.size(), source);
            text += tail;
            return text;
        }
        if (op == "==" || op == "~=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            bool equal = false, less = false, greater = false;
            if (IsNumber(left) && IsNumber(right)) {
                const auto a = Number(left, source), b = Number(right, source);
                equal = a == b; less = a < b; greater = a > b;
            } else if (const auto* a = std::get_if<std::string>(&left); a && std::holds_alternative<std::string>(right)) {
                const auto& b = std::get<std::string>(right);
                equal = *a == b; less = *a < b; greater = *a > b;
            } else if ((op == "==" || op == "~=") && std::holds_alternative<bool>(left) && std::holds_alternative<bool>(right)) {
                equal = std::get<bool>(left) == std::get<bool>(right);
            } else Fail("slk-plan.type", "比较只接受同类标量或整数/实数对", source);
            if (op == "==") return equal;
            if (op == "~=") return !equal;
            if (op == "<") return less;
            if (op == "<=") return less || equal;
            if (op == ">") return greater;
            return greater || equal;
        }
        const double a = Number(left, source), b = Number(right, source);
        const bool integer = std::holds_alternative<std::int64_t>(left) && std::holds_alternative<std::int64_t>(right);
        if (op == "+") return Numeric(a + b, integer, source);
        if (op == "-") return Numeric(a - b, integer, source);
        if (op == "*") return Numeric(a * b, integer, source);
        if (op == "/" || op == "%") {
            if (b == 0) Fail("slk-plan.number", "除数不能为零", source);
            if (op == "/") return Numeric(a / b, false, source);
            // Lua 取模与 floor 除法同号；fmod 避免 a/b 极大时中间值溢出。
            double remainder = std::fmod(a, b);
            if (remainder != 0 && ((remainder < 0) != (b < 0))) remainder += b;
            return Numeric(remainder, integer, source);
        }
        // Lua 5.3 的幂始终产生 float，包括 2^0 之类结果恰为整数的情况。
        // https://www.lua.org/manual/5.3/manual.html#3.4.1
        return Numeric(std::pow(a, b), false, source);
    }
    Value Eval(std::size_t index, std::size_t depth) {
        const auto& expression = syntax_.expressions[index];
        const auto source = expression.token.source;
        Depth(depth, source); Step(source);
        const auto child = [&](std::size_t ordinal) { return Eval(expression.children[ordinal], depth + 1); };
        switch (expression.kind) {
        case ExpressionKind::Number:
            return std::visit([](const auto value) -> Value { return value; }, ReadNumber(expression.token));
        case ExpressionKind::String:
            TextBudget(expression.token.text.size(), source);
            return expression.token.text;
        case ExpressionKind::Boolean: return expression.token.text == "true";
        case ExpressionKind::Variable: return Copy(Find(expression.token).value, source);
        case ExpressionKind::Capability: return Capability{};
        case ExpressionKind::Array: {
            Spend(arrayElements_, expression.children.size(), limits_.arrayElements, source, "数组元素");
            auto values = std::make_shared<Array>();
            values->reserve(expression.children.size());
            for (const auto item : expression.children) values->push_back(Scalar(Eval(item, depth + 1), source));
            return std::shared_ptr<const Array>(std::move(values));
        }
        case ExpressionKind::Index: {
            const auto array = child(0);
            const auto indexValue = Integer(child(1), source);
            const auto* values = std::get_if<std::shared_ptr<const Array>>(&array);
            if (!values) Fail("slk-plan.type", "索引只允许局部密集数组", source);
            if (indexValue < 1 || static_cast<std::uint64_t>(indexValue) > (*values)->size()) Fail("slk-plan.index", "数组索引越界（从 1 开始）", source);
            return FromScalar((**values)[static_cast<std::size_t>(indexValue - 1)], source);
        }
        case ExpressionKind::Unary: {
            const auto value = child(0);
            return Numeric((expression.token.text == "-" ? -1 : 1) * Number(value, source), std::holds_alternative<std::int64_t>(value), source);
        }
        case ExpressionKind::Binary: {
            auto left = child(0);
            auto right = child(1);
            return Binary(expression, std::move(left), std::move(right));
        }
        case ExpressionKind::NewAbility: {
            if (!std::holds_alternative<Capability>(Find(expression.token).value)) Fail("slk-plan.capability", "能力创建的接收者不是局部 slk capability", source);
            const auto value = child(0);
            const auto* id = std::get_if<std::string>(&value);
            if (!id || !StaticAbilityId(*id)) Fail("slk-plan.rawcode", "新能力 ID 必须是四个 ASCII 字母或数字；拒绝 slk:new 动态标签", source);
            if (*id == expression.extra) Fail("slk-plan.rawcode", "新能力 ID 不能与父 ID 相同", source);
            if (!ids_.insert(*id).second) Fail("slk-plan.duplicate-id", "能力 ID 重复：" + *id, source);
            if (plan_.abilities.size() >= limits_.abilities) Fail("slk-plan.budget", "超出预算：能力数量", source);
            plan_.abilities.push_back({expression.extra, *id, source, {}});
            return AbilityReference{plan_.abilities.size() - 1};
        }
        }
        Fail("slk-plan.syntax", "未知表达式", source);
    }
    void Block(const std::vector<std::size_t>& block, std::size_t depth) {
        const auto source = block.empty() ? SlkTemplateSource{} : syntax_.statements[block.front()].token.source;
        Depth(depth, source);
        scopes_.emplace_back();
        for (const auto index : block) Execute(syntax_.statements[index], depth);
        scopes_.pop_back();
    }
    void Execute(const Statement& statement, std::size_t depth) {
        const auto source = statement.token.source;
        Step(source);
        const auto evaluate = [&](std::size_t ordinal) { return Eval(statement.expressions[ordinal], depth + 1); };
        switch (statement.kind) {
        case StatementKind::Local: Declare(statement.token, evaluate(0)); break;
        case StatementKind::Assign: {
            auto value = evaluate(0);
            auto& binding = Find(statement.token);
            if (binding.readOnly) Fail("slk-plan.loop-control", "不能赋值给循环控制变量", source);
            binding.value = std::move(value);
            break;
        }
        case StatementKind::Field: {
            const auto* reference = std::get_if<AbilityReference>(&Find(statement.token).value);
            if (!reference) Fail("slk-plan.capability", "字段写入目标不是本计划创建的能力", source);
            const auto object = reference->index;
            if (statement.field.size() > 256) Fail("slk-plan.budget", "超出预算：字段名称长度", source);
            TextBudget(statement.field.size(), source); // 展开后保留的每条键也占用累计预算。
            auto value = Scalar(evaluate(0), source);
            Spend(fieldWrites_, 1, limits_.fieldWrites, source, "字段写入数");
            plan_.abilities[object].writes.push_back({statement.field, std::move(value), source});
            break;
        }
        case StatementKind::If: {
            bool selected = false;
            for (std::size_t branch = 0; branch < statement.expressions.size(); ++branch) {
                const auto value = evaluate(branch);
                const auto* condition = std::get_if<bool>(&value);
                if (!condition) Fail("slk-plan.type", "if 条件必须是 boolean 或比较表达式", source);
                if (*condition) { Block(statement.blocks[branch], depth + 1); selected = true; break; }
            }
            if (!selected && statement.blocks.size() > statement.expressions.size()) Block(statement.blocks.back(), depth + 1);
            break;
        }
        case StatementKind::For: {
            const auto initialValue = evaluate(0);
            const auto first = Integer(initialValue, source);
            const auto last = Integer(evaluate(1), source);
            const auto stepValue = statement.expressions.size() == 3 ? evaluate(2) : Value{std::int64_t{1}};
            const auto increment = Integer(stepValue, source);
            // Lua 5.3 只有整数初值及步长才走整数循环；不能把浮点循环变量转成可拼接整数。
            // https://www.lua.org/source/5.3/lvm.c.html#luaV_execute
            if (!std::holds_alternative<std::int64_t>(initialValue) || !std::holds_alternative<std::int64_t>(stepValue)) {
                Fail("slk-plan.integer", "仅支持整数类型的循环初值和步长；不能隐式改写浮点循环", source);
            }
            if (increment == 0) Fail("slk-plan.loop-control", "循环步长不能为零", source);
            // 预检总次数；即使循环体为空也不能绕过运行预算。
            const auto distance = increment > 0 ? last - first : first - last;
            const auto stride = increment > 0 ? increment : -increment;
            const auto count = distance < 0 ? 0 : static_cast<std::uint64_t>(distance / stride) + 1;
            if (count > limits_.loopIterations - plan_.loopIterations) Fail("slk-plan.budget", "超出预算：循环迭代数", source);
            auto counter = first;
            for (std::uint64_t iteration = 0; iteration < count; ++iteration) {
                Spend(plan_.loopIterations, 1, limits_.loopIterations, source, "循环迭代数");
                scopes_.emplace_back();
                Declare(statement.token, counter, true);
                Block(statement.blocks.front(), depth + 1);
                scopes_.pop_back();
                if (iteration + 1 < count) counter += increment;
            }
            break;
        }
        }
    }
    const Syntax& syntax_;
    const SlkTemplatePlanLimits& limits_;
    SlkTemplatePlan plan_;
    std::vector<std::unordered_map<std::string, BindingValue>> scopes_;
    std::unordered_set<std::string> ids_;
    std::size_t bindings_ = 0, arrayElements_ = 0, stringBytes_ = 0, fieldWrites_ = 0;
};

bool ValidLimits(const SlkTemplatePlanLimits& limits) {
    const SlkTemplatePlanLimits maximum;
    return limits.sourceBytes <= maximum.sourceBytes && limits.tokens <= maximum.tokens
        && limits.syntaxNodes <= maximum.syntaxNodes && limits.depth <= maximum.depth
        && limits.executedSteps <= maximum.executedSteps && limits.loopIterations <= maximum.loopIterations
        && limits.localBindings <= maximum.localBindings && limits.arrayElements <= maximum.arrayElements
        && limits.stringBytes <= maximum.stringBytes && limits.totalStringBytes <= maximum.totalStringBytes
        && limits.abilities <= maximum.abilities && limits.fieldWrites <= maximum.fieldWrites;
}

} // namespace

Result<SlkTemplatePlan> ParseSlkTemplatePlan(std::string_view source, const SlkTemplatePlanLimits& limits) {
    try {
        if (!ValidLimits(limits)) Fail("slk-plan.limits", "调用方只能收紧 SLK 计划硬预算", {});
        if (source.size() > limits.sourceBytes) Fail("slk-plan.budget", "超出预算：源码字节", {});
        const auto tokens = Lexer(source, limits).Run();
        const auto syntax = Parser(tokens, limits).Run();
        return Result<SlkTemplatePlan>::Success(Planner(syntax, limits).Run());
    } catch (const Failure& failure) {
        auto result = Result<SlkTemplatePlan>::Failure("Script.SlkPlan", failure.code,
            "第 " + std::to_string(failure.source.line) + " 行，第 " + std::to_string(failure.source.column)
                + " 列：" + failure.message);
        result.diagnostics.front().origin = OriginSpan{{}, failure.source.offset, failure.source.length, {}};
        return result;
    }
}

} // namespace War3Map::Script
