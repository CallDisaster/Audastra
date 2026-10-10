#include "PpExpression.h"
#include "JassCheckBudget.h"

#include <cctype>
#include <cstdlib>

namespace War3Map::Jass::detail {
namespace {

class Parser {
public:
    explicit Parser(std::span<const PpToken> tokens, CheckBudget* budget) : tokens_(tokens), budget_(budget) {}

    PpEvaluation Run() {
        PpEvaluation result;
        if (Peek() == nullptr) {
            result.error = "表达式为空";
            return result;
        }
        result.value = Conditional();
        if (error_.empty() && Peek() != nullptr) error_ = "多余的记号“" + DiagnosticText(Peek()->text) + "”";
        result.error = error_;
        return result;
    }

private:
    const PpToken* Peek() const { if (budget_) budget_->Poll(); return at_ < tokens_.size() ? &tokens_[at_] : nullptr; }
    bool Accept(std::string_view punct) {
        if (const auto* token = Peek(); token != nullptr && token->Is(punct)) {
            ++at_;
            return true;
        }
        return false;
    }
    // 短路 / 未选中的分支中只做语法检查：语义错误（除以零）不报告。
    PpValue Fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
        at_ = tokens_.size();
        return {};
    }
    PpValue SemanticFail(std::string message) {
        if (suppress_ > 0) return {};
        return Fail(std::move(message));
    }
    PpValue Suppressed(bool suppress, PpValue (Parser::*parse)()) {
        if (suppress) ++suppress_;
        const PpValue value = (this->*parse)();
        if (suppress) --suppress_;
        return value;
    }

    static PpValue Make(std::int64_t value, bool isUnsigned) { return {value, isUnsigned}; }
    static PpValue Bool(bool value) { return {value ? 1 : 0, false}; }

    PpValue Conditional() {
        NestingGuard guard(budget_, nesting_);
        const PpValue condition = Binary(0);
        if (!Accept("?")) return condition;
        // 两个分支都要解析（语法检查），只取一个的值。
        const PpValue whenTrue = Suppressed(condition.value == 0, &Parser::Conditional);
        if (!Accept(":")) return Fail("条件表达式缺少“:”");
        const PpValue whenFalse = Suppressed(condition.value != 0, &Parser::Conditional);
        const bool isUnsigned = whenTrue.isUnsigned || whenFalse.isUnsigned;
        return Make(condition.value != 0 ? whenTrue.value : whenFalse.value, isUnsigned);
    }

    // 优先级由低到高：|| && | ^ & (== !=) (< > <= >=) (<< >>) (+ -) (* / %)
    static int Precedence(std::string_view op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "|") return 3;
        if (op == "^") return 4;
        if (op == "&") return 5;
        if (op == "==" || op == "!=") return 6;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 7;
        if (op == "<<" || op == ">>") return 8;
        if (op == "+" || op == "-") return 9;
        if (op == "*" || op == "/" || op == "%") return 10;
        return 0;
    }

    PpValue Binary(int minimum) {
        NestingGuard guard(budget_, nesting_);
        PpValue left = Unary();
        while (const auto* token = Peek()) {
            if (token->kind != PpKind::Punct) break;
            const int precedence = Precedence(token->text);
            if (precedence == 0 || precedence <= minimum) break;
            const std::string_view op = token->text;
            ++at_;
            const bool shortCircuit = (op == "&&" && left.value == 0) || (op == "||" && left.value != 0);
            if (shortCircuit) ++suppress_;
            const PpValue right = Binary(precedence);
            if (shortCircuit) --suppress_;
            if (!error_.empty()) return {};
            left = Apply(op, left, right);
        }
        return left;
    }

    PpValue Apply(std::string_view op, PpValue left, PpValue right) {
        const bool isUnsigned = left.isUnsigned || right.isUnsigned;
        const auto ul = static_cast<std::uint64_t>(left.value);
        const auto ur = static_cast<std::uint64_t>(right.value);
        if (op == "||") return Bool(left.value != 0 || right.value != 0);
        if (op == "&&") return Bool(left.value != 0 && right.value != 0);
        if (op == "|") return Make(left.value | right.value, isUnsigned);
        if (op == "^") return Make(left.value ^ right.value, isUnsigned);
        if (op == "&") return Make(left.value & right.value, isUnsigned);
        if (op == "==") return Bool(left.value == right.value);
        if (op == "!=") return Bool(left.value != right.value);
        if (op == "<") return Bool(isUnsigned ? ul < ur : left.value < right.value);
        if (op == ">") return Bool(isUnsigned ? ul > ur : left.value > right.value);
        if (op == "<=") return Bool(isUnsigned ? ul <= ur : left.value <= right.value);
        if (op == ">=") return Bool(isUnsigned ? ul >= ur : left.value >= right.value);
        if (op == "<<") return Make(static_cast<std::int64_t>(ul << (ur & 63)), left.isUnsigned);
        if (op == ">>") {
            return Make(left.isUnsigned ? static_cast<std::int64_t>(ul >> (ur & 63)) : left.value >> (ur & 63), left.isUnsigned);
        }
        if (op == "+") return Make(static_cast<std::int64_t>(ul + ur), isUnsigned);
        if (op == "-") return Make(static_cast<std::int64_t>(ul - ur), isUnsigned);
        if (op == "*") return Make(static_cast<std::int64_t>(ul * ur), isUnsigned);
        if (right.value == 0) return SemanticFail("#if 表达式除以零");
        if (!isUnsigned && left.value == INT64_MIN && right.value == -1) return SemanticFail("#if 有符号除法超过64位范围");
        if (op == "/") return isUnsigned ? Make(static_cast<std::int64_t>(ul / ur), true) : Make(left.value / right.value, false);
        return isUnsigned ? Make(static_cast<std::int64_t>(ul % ur), true) : Make(left.value % right.value, false);
    }

    PpValue Unary() {
        NestingGuard guard(budget_, nesting_);
        const auto* token = Peek();
        if (token == nullptr) return Fail("表达式不完整");
        if (token->kind == PpKind::Punct) {
            if (Accept("+")) return Unary();
            if (Accept("-")) {
                const PpValue value = Unary();
                return Make(static_cast<std::int64_t>(0 - static_cast<std::uint64_t>(value.value)), value.isUnsigned);
            }
            if (Accept("~")) {
                const PpValue value = Unary();
                return Make(~value.value, value.isUnsigned);
            }
            if (Accept("!")) return Bool(Unary().value == 0);
            if (Accept("(")) {
                const PpValue value = Conditional();
                if (!Accept(")")) return Fail("缺少“)”");
                return value;
            }
            return Fail("无效的记号“" + DiagnosticText(token->text) + "”");
        }
        ++at_;
        switch (token->kind) {
        case PpKind::Number: return Number(token->text);
        case PpKind::Char: return Character(token->text);
        case PpKind::Identifier:
            if (token->text == "true" || token->text == "false") return Fail("#if 表达式不能使用 " + DiagnosticText(token->text));
            return Make(0, false);
        default: return Fail("无效的记号“" + DiagnosticText(token->text) + "”");
        }
    }

    PpValue Number(std::string_view text) {
        std::size_t end = text.size();
        bool isUnsigned = false;
        while (end > 0) {
            if (budget_) budget_->PollOccasionally(end);
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[end - 1])));
            if (c == 'u') isUnsigned = true;
            else if (c != 'l') break;
            --end;
        }
        if (budget_) budget_->Poll();
        const auto digits = text.substr(0, end);
        int base = 10;
        std::size_t start = 0;
        if (digits.size() > 1 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
            base = 16;
            start = 2;
        } else if (digits.size() > 1 && digits[0] == '0') {
            base = 8;
            start = 1;
        }
        if (start >= digits.size() && base == 16) return Fail("无效的数字“" + DiagnosticText(text) + "”");
        std::uint64_t value = 0;
        bool overflow = false;
        for (std::size_t i = start; i < digits.size(); ++i) {
            if (budget_) budget_->PollOccasionally(i);
            const auto c = static_cast<unsigned char>(digits[i]);
            const unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 255;
            if (d >= static_cast<unsigned>(base)) return Fail("#if 表达式中的数字“" + DiagnosticText(text) + "”无效");
            if (value > (UINT64_MAX - d) / static_cast<unsigned>(base)) overflow = true;
            if (!overflow) value = value * static_cast<unsigned>(base) + d;
        }
        if (overflow) value = UINT64_MAX; // 与 strtoull 溢出饱和值一致，不借助不可取消的大数字副本。
        if (value > static_cast<std::uint64_t>(INT64_MAX)) isUnsigned = true;
        return Make(static_cast<std::int64_t>(value), isUnsigned);
    }

    // 字符常量：单字符取其值；多字符按大端拼接（与常见实现一致）。
    PpValue Character(std::string_view text) {
        std::int64_t value = 0;
        std::size_t ticks = 0;
        for (std::size_t i = 1; i + 1 < text.size(); ++i) {
            if (budget_) budget_->PollOccasionally(ticks++);
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c == '\\' && i + 2 < text.size()) {
                const char escaped = text[++i];
                switch (escaped) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '0': c = 0; break;
                default: c = static_cast<unsigned char>(escaped); break;
                }
            }
            value = static_cast<std::int64_t>((static_cast<std::uint64_t>(value) << 8) | c);
        }
        return Make(value, false);
    }

    std::span<const PpToken> tokens_;
    std::size_t at_ = 0;
    int suppress_ = 0;
    std::string error_;
    CheckBudget* budget_ = nullptr;
    std::size_t nesting_ = 0;
};

} // namespace

PpEvaluation EvaluatePpExpression(std::span<const PpToken> tokens, CheckBudget* budget) {
    return Parser(tokens, budget).Run();
}

} // namespace War3Map::Jass::detail
