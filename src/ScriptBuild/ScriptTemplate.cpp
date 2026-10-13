#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Script/ScriptSaveBundle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <variant>

// 模板求值：独立实现的受限表达式解释器，只覆盖函数库模板实际使用的子集。
// 不执行任何外部脚本；超出子集或未闭合的模板报错，不能删除未知副作用后继续构建。
namespace War3Map::Script {
namespace {

struct Nil {};
struct Boolean { bool value = false; };
// import(名) 的结果：等待第二次调用给出文本或路径。
struct ImportTarget { std::string member; };
using Value = std::variant<Nil, std::int64_t, double, std::string, Boolean, ImportTarget>;

constexpr std::size_t kImportFileBytes = 16u * 1024u * 1024u;
constexpr std::size_t kImportTotalBytes = 64u * 1024u * 1024u;
constexpr std::size_t kImportCount = 4096;

std::string NumberText(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.14g", value);
    std::string text = buffer;
    if (text.find_first_of(".eEn") == std::string::npos) text += ".0";
    return text;
}

std::optional<std::string> ToText(const Value& value) {
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return std::to_string(*integer);
    if (const auto* real = std::get_if<double>(&value)) return NumberText(*real);
    return std::nullopt;
}

std::optional<double> ToNumber(const Value& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return static_cast<double>(*integer);
    if (const auto* real = std::get_if<double>(&value)) return *real;
    if (const auto* text = std::get_if<std::string>(&value)) {
        char* end = nullptr;
        const double parsed = std::strtod(text->c_str(), &end);
        if (end != text->c_str() && *end == '\0') return parsed;
    }
    return std::nullopt;
}

// 整数值的浮点数按整数返回（与 math.floor 等一致）。
Value Integral(double value) {
    if (std::isfinite(value) && std::floor(value) == value && std::fabs(value) < 9.0e15) return static_cast<std::int64_t>(value);
    return value;
}

enum class Token { End, Name, Number, String, Symbol };

struct Lexeme {
    Token kind = Token::End;
    std::string text;
};

class Lexer {
public:
    explicit Lexer(std::string_view code) : code_(code) {}

    Lexeme Next() {
        SkipSpace();
        if (at_ >= code_.size()) return {};
        const char c = code_[at_];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const std::size_t start = at_;
            while (at_ < code_.size() && (std::isalnum(static_cast<unsigned char>(code_[at_])) || code_[at_] == '_')) ++at_;
            return {Token::Name, std::string(code_.substr(start, at_ - start))};
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && at_ + 1 < code_.size() && std::isdigit(static_cast<unsigned char>(code_[at_ + 1])))) {
            const std::size_t start = at_;
            if (c == '0' && at_ + 1 < code_.size() && (code_[at_ + 1] == 'x' || code_[at_ + 1] == 'X')) {
                at_ += 2;
                while (at_ < code_.size() && std::isxdigit(static_cast<unsigned char>(code_[at_]))) ++at_;
            } else {
                while (at_ < code_.size() && (std::isdigit(static_cast<unsigned char>(code_[at_])) || code_[at_] == '.')) ++at_;
                if (at_ < code_.size() && (code_[at_] == 'e' || code_[at_] == 'E')) {
                    ++at_;
                    if (at_ < code_.size() && (code_[at_] == '+' || code_[at_] == '-')) ++at_;
                    while (at_ < code_.size() && std::isdigit(static_cast<unsigned char>(code_[at_]))) ++at_;
                }
            }
            return {Token::Number, std::string(code_.substr(start, at_ - start))};
        }
        if (c == '"' || c == '\'') return QuotedString(c);
        if (c == '[') {
            std::string content;
            const auto bracket = LongBracket(content);
            if (bracket == Bracket::Unterminated) return Fail();
            if (bracket == Bracket::Ok) return {Token::String, std::move(content)};
        }
        static constexpr std::string_view kSymbols[] = {"...", "..", "==", "~=", "<=", ">=", "//", "::"};
        for (const auto symbol : kSymbols) {
            if (code_.substr(at_, symbol.size()) == symbol) {
                at_ += symbol.size();
                return {Token::Symbol, std::string(symbol)};
            }
        }
        ++at_;
        return {Token::Symbol, std::string(1, c)};
    }

    [[nodiscard]] bool Failed() const noexcept { return failed_; }

private:
    void SkipSpace() {
        while (at_ < code_.size()) {
            const char c = code_[at_];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++at_;
            } else if (code_.substr(at_, 2) == "--") {
                at_ += 2;
                std::string ignored;
                const auto bracket = LongBracket(ignored);
                if (bracket == Bracket::Unterminated) { Fail(); return; }
                if (bracket == Bracket::None) {
                    const std::size_t end = code_.find('\n', at_);
                    at_ = end == std::string_view::npos ? code_.size() : end + 1;
                }
            } else {
                break;
            }
        }
    }

    // Lua 长括号 [[...]]、[==[...]==]（字符串与注释共用）。与 Lua 相同：紧跟开括号的
    // 第一个换行不计入内容，内容中的 \r\n / \n\r / \r 统一成 \n。
    enum class Bracket { None, Ok, Unterminated };
    Bracket LongBracket(std::string& content) {
        if (at_ >= code_.size() || code_[at_] != '[') return Bracket::None;
        std::size_t cursor = at_ + 1, level = 0;
        while (cursor < code_.size() && code_[cursor] == '=') { ++cursor; ++level; }
        if (cursor >= code_.size() || code_[cursor] != '[') return Bracket::None;
        const std::string close = "]" + std::string(level, '=') + "]";
        const std::size_t end = code_.find(close, cursor + 1);
        if (end == std::string_view::npos) return Bracket::Unterminated;
        std::size_t start = cursor + 1;
        if (start < end && (code_[start] == '\r' || code_[start] == '\n')) {
            const char first = code_[start++];
            if (start < end && (code_[start] == '\r' || code_[start] == '\n') && code_[start] != first) ++start;
        }
        content.clear();
        for (std::size_t index = start; index < end; ++index) {
            const char ch = code_[index];
            if (ch == '\r' || ch == '\n') {
                if (index + 1 < end && (code_[index + 1] == '\r' || code_[index + 1] == '\n') && code_[index + 1] != ch) ++index;
                content.push_back('\n');
            } else content.push_back(ch);
        }
        at_ = end + close.size();
        return Bracket::Ok;
    }

    Lexeme QuotedString(char quote) {
        std::string text;
        ++at_;
        while (at_ < code_.size() && code_[at_] != quote) {
            char c = code_[at_++];
            if (c == '\n') return Fail();
            if (c == '\\' && at_ < code_.size()) {
                const char escaped = code_[at_++];
                switch (escaped) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '0': c = '\0'; break;
                case '\\': case '"': case '\'': c = escaped; break;
                case '\n': c = '\n'; break;
                default: return Fail();
                }
            }
            text.push_back(c);
        }
        if (at_ >= code_.size()) return Fail();
        ++at_;
        return {Token::String, std::move(text)};
    }

    Lexeme Fail() {
        failed_ = true;
        at_ = code_.size();
        return {};
    }

    std::string_view code_;
    std::size_t at_ = 0;
    bool failed_ = false;
};

// 递归下降求值器：直接在解析时求值（无副作用函数，短路无需特殊处理）。
class Evaluator {
public:
    Evaluator(const TemplateContext& context, std::unordered_map<std::string, Value>& variables, std::vector<Diagnostic>& diagnostics,
        std::vector<MemberReplacement>& imports)
        : context_(context), variables_(variables), diagnostics_(diagnostics), imports_(imports) {}

    std::optional<Value> Expression(std::string_view code, std::string& error) {
        Start(code);
        auto value = Concat();
        if (!error_.empty() || lexer_->Failed() || current_.kind != Token::End) {
            error = error_.empty() ? "无法解析表达式" : error_;
            return std::nullopt;
        }
        return value;
    }

    // 只接受 [local] NAME = expr 与 import 调用语句；其余语句返回 false，由调用者报错并拒绝构建。
    bool Statements(std::string_view code, std::string& error) {
        importFailed_ = false;
        rejected_ = !StatementList(code, error);
        return !rejected_;
    }
    [[nodiscard]] bool Rejected() const noexcept { return rejected_; }
    // 上一次拒绝发生在已识别的 import 语句中（名称、路径或读取失败）。
    [[nodiscard]] bool ImportFailed() const noexcept { return importFailed_; }

private:
    bool StatementList(std::string_view code, std::string& error) {
        Start(code);
        while (current_.kind != Token::End) {
            if (current_.kind == Token::Symbol && current_.text == ";") {
                Advance();
                continue;
            }
            if (current_.kind == Token::Name && current_.text == "import") {
                const auto value = Concat();
                if (error_.empty() && std::holds_alternative<ImportTarget>(value))
                    Fail("import(名) 之后缺少要写入的文本或文件路径");
                if (!error_.empty() || lexer_->Failed()) {
                    error = error_.empty() ? "无法解析 import 语句" : error_;
                    importFailed_ = true;
                    return false;
                }
                continue;
            }
            if (current_.kind == Token::Name && current_.text == "local") Advance();
            if (current_.kind != Token::Name || IsKeyword(current_.text)) return false;
            const std::string name = current_.text;
            Advance();
            if (current_.kind != Token::Symbol || current_.text != "=") return false;
            Advance();
            auto value = Concat();
            if (!error_.empty() || lexer_->Failed()) {
                error = error_.empty() ? "无法解析语句" : error_;
                return false;
            }
            variables_[name] = std::move(value);
        }
        return true;
    }

    static bool IsKeyword(std::string_view name) {
        static constexpr std::string_view kKeywords[] = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function",
            "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
        for (const auto keyword : kKeywords) {
            if (name == keyword) return true;
        }
        return false;
    }

    void Start(std::string_view code) {
        lexer_.emplace(code);
        error_.clear();
        Advance();
    }

    void Advance() { current_ = lexer_->Next(); }

    bool IsSymbol(std::string_view symbol) const { return current_.kind == Token::Symbol && current_.text == symbol; }

    Value Fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
        return Nil{};
    }

    // .. 右结合，优先级低于加减。
    Value Concat() {
        Value left = Additive();
        if (!IsSymbol("..")) return left;
        Advance();
        Value right = Concat();
        const auto a = ToText(left);
        const auto b = ToText(right);
        if (!a || !b) return Fail("连接运算的操作数为空值");
        return *a + *b;
    }

    Value Additive() {
        Value left = Multiplicative();
        while (IsSymbol("+") || IsSymbol("-")) {
            const bool add = current_.text == "+";
            Advance();
            left = Arithmetic(left, Multiplicative(), add ? '+' : '-');
        }
        return left;
    }

    Value Multiplicative() {
        Value left = Unary();
        while (IsSymbol("*") || IsSymbol("/") || IsSymbol("//") || IsSymbol("%")) {
            const std::string op = current_.text;
            Advance();
            left = Arithmetic(left, Unary(), op == "*" ? '*' : op == "/" ? '/' : op == "//" ? 'd' : '%');
        }
        return left;
    }

    Value Unary() {
        if (IsSymbol("-")) {
            Advance();
            return Arithmetic(std::int64_t{0}, Unary(), '-');
        }
        return Power();
    }

    Value Power() {
        Value base = Primary();
        if (!IsSymbol("^")) return base;
        Advance();
        return Arithmetic(base, Unary(), '^');
    }

    Value Arithmetic(const Value& left, const Value& right, char op) {
        const auto* li = std::get_if<std::int64_t>(&left);
        const auto* ri = std::get_if<std::int64_t>(&right);
        if (li != nullptr && ri != nullptr && op != '/' && op != '^') {
            switch (op) {
            case '+': return *li + *ri;
            case '-': return *li - *ri;
            case '*': return *li * *ri;
            case 'd':
                if (*ri == 0) return Fail("整数除以零");
                return static_cast<std::int64_t>(std::floor(static_cast<double>(*li) / static_cast<double>(*ri)));
            case '%': {
                if (*ri == 0) return Fail("整数取模零");
                const std::int64_t m = *li % *ri;
                return (m != 0 && ((m < 0) != (*ri < 0))) ? m + *ri : m;
            }
            default: break;
            }
        }
        const auto a = ToNumber(left);
        const auto b = ToNumber(right);
        if (!a || !b) return Fail("算术运算的操作数不是数字");
        switch (op) {
        case '+': return *a + *b;
        case '-': return *a - *b;
        case '*': return *a * *b;
        case '/': return *a / *b;
        case 'd': return std::floor(*a / *b);
        case '%': return *a - std::floor(*a / *b) * *b;
        default: return std::pow(*a, *b);
        }
    }

    Value Primary() {
        if (current_.kind == Token::Number) {
            const std::string text = current_.text;
            Advance();
            if (text.size() > 2 && (text[1] == 'x' || text[1] == 'X')) return static_cast<std::int64_t>(std::stoull(text.substr(2), nullptr, 16));
            if (text.find_first_of(".eE") == std::string::npos) return static_cast<std::int64_t>(std::stoll(text));
            return std::stod(text);
        }
        if (current_.kind == Token::String) {
            std::string text = current_.text;
            Advance();
            return text;
        }
        if (IsSymbol("(")) {
            Advance();
            Value inner = Concat();
            if (!IsSymbol(")")) return Fail("缺少右括号");
            Advance();
            return inner;
        }
        if (current_.kind == Token::Name && (current_.text == "true" || current_.text == "false" || current_.text == "nil")) {
            const std::string literal = current_.text;
            Advance();
            return literal == "nil" ? Value(Nil{}) : Value(Boolean{literal == "true"});
        }
        if (current_.kind != Token::Name || IsKeyword(current_.text)) return Fail("不支持的表达式：" + current_.text);
        std::string name = current_.text;
        Advance();
        if (name == "import") return ImportChain();
        while (IsSymbol(".")) {
            Advance();
            if (current_.kind != Token::Name) return Fail("成员名无效");
            name += "." + current_.text;
            Advance();
        }
        if (!IsSymbol("(")) {
            const auto found = variables_.find(name);
            if (found == variables_.end()) return Fail("未定义的模板变量：" + name);
            return found->second;
        }
        Advance();
        std::vector<Value> arguments;
        while (!IsSymbol(")")) {
            arguments.push_back(Concat());
            if (!error_.empty()) return Nil{};
            if (IsSymbol(",")) Advance();
            else if (!IsSymbol(")")) return Fail("参数列表无效");
        }
        Advance();
        return Call(name, arguments);
    }

    // 调用参数：( 表达式列表 )，或 Lua 的单个字符串实参写法 f "x" / f [[x]]。
    std::optional<std::vector<Value>> CallArguments() {
        std::vector<Value> arguments;
        if (current_.kind == Token::String) {
            arguments.emplace_back(current_.text);
            Advance();
            return arguments;
        }
        if (!IsSymbol("(")) { Fail("缺少调用参数"); return std::nullopt; }
        Advance();
        while (!IsSymbol(")")) {
            arguments.push_back(Concat());
            if (!error_.empty()) return std::nullopt;
            if (IsSymbol(",")) Advance();
            else if (!IsSymbol(")")) { Fail("参数列表无效"); return std::nullopt; }
        }
        Advance();
        return arguments;
    }

    // YDWE：import(名) 返回一个函数，再以 (文本) 或 (相对路径, true) 调用一次完成写入，返回空值。
    Value ImportChain() {
        const auto first = CallArguments();
        if (!first) return Nil{};
        const auto member = first->empty() ? std::nullopt : ToText(first->front());
        if (!member) return Fail("import 需要文件名");
        const auto name = ImportMemberName(*member);
        if (!name) return Nil{};
        if (!IsSymbol("(") && current_.kind != Token::String) return ImportTarget{*name};
        const auto second = CallArguments();
        if (!second) return Nil{};
        return Import(*name, *second);
    }

    static std::string Lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        return text;
    }

    std::optional<std::string> ImportMemberName(std::string name) {
        std::replace(name.begin(), name.end(), '/', '\\');
        if (name.empty() || name.size() > 240 || name.front() == '\\' || name.find(':') != std::string::npos
            || name.find("..") != std::string::npos
            || std::any_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; })) {
            Fail("import 的文件名必须是地图内相对路径：" + name);
            return std::nullopt;
        }
        if (IsTemplateImportReserved(name)) {
            Fail("import 不能覆盖地图脚本、物编或归档内部文件：" + name);
            return std::nullopt;
        }
        return name;
    }

    Value Import(const std::string& member, const std::vector<Value>& arguments) {
        const auto content = arguments.empty() ? std::nullopt : ToText(arguments[0]);
        if (!content) return Fail("import(" + member + ") 需要文本内容或文件路径");
        const auto* flag = arguments.size() > 1 ? std::get_if<Boolean>(&arguments[1]) : nullptr;
        const bool path = flag && flag->value;
        // 只读检查场景：核对写法与名称，不读文件、不产生成员。
        if (!context_.importRoot) return Nil{};
        std::vector<std::byte> bytes;
        if (path) {
            auto relative = *content;
            std::replace(relative.begin(), relative.end(), '\\', '/');
            const std::filesystem::path file(std::u8string(relative.begin(), relative.end()));
            if (relative.empty() || file.is_absolute() || file.has_root_name() || file.has_root_directory()
                || std::any_of(file.begin(), file.end(), [](const auto& part) { return part == ".."; }))
                return Fail("import(" + member + ") 的文件路径必须位于地图所在目录内：" + *content);
            auto read = IO::ReadFileBytes(*context_.importRoot / file, kImportFileBytes);
            if (!read.Succeeded() || !read.value)
                return Fail("import(" + member + ") 无法读取地图所在目录下的 " + *content + "（不存在、不是普通文件或超过 16 MiB）");
            bytes = std::move(*read.value);
        } else {
            const auto* first = reinterpret_cast<const std::byte*>(content->data());
            bytes.assign(first, first + content->size());
        }
        // 同名后者覆盖前者，与 YDWE 依次覆盖复制一致。
        const auto key = Lower(member);
        const auto existing = std::find_if(imports_.begin(), imports_.end(), [&](const auto& item) { return Lower(item.name) == key; });
        if (existing != imports_.end()) {
            importBytes_ -= existing->data.size();
            diagnostics_.push_back({DiagnosticSeverity::Warning, "Script.Template", "TEMPLATE_IMPORT_DUPLICATE",
                "模板多次 import 同一文件 " + member + "，以最后一次为准", {}});
        } else if (imports_.size() >= kImportCount) {
            return Fail("模板 import 的文件超过 4096 个");
        }
        if (bytes.size() > kImportTotalBytes - importBytes_) return Fail("模板 import 的文件合计超过 64 MiB");
        importBytes_ += bytes.size();
        if (existing != imports_.end()) existing->data = std::move(bytes);
        else imports_.push_back({member, std::move(bytes)});
        return Nil{};
    }

    Value Call(const std::string& name, const std::vector<Value>& arguments) {
        const auto number = [&](std::size_t index) -> std::optional<double> {
            return index < arguments.size() ? ToNumber(arguments[index]) : std::nullopt;
        };
        const auto text = [&](std::size_t index) -> std::optional<std::string> {
            return index < arguments.size() ? ToText(arguments[index]) : std::nullopt;
        };
        if (name == "StringHash") {
            const auto value = text(0);
            if (!value) return Fail("StringHash 需要字符串参数");
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "0x%08X", StormStringHash(*value));
            return std::string(buffer);
        }
        if (name == "abilityOrder") {
            const auto ability = text(0);
            const auto type = text(1);
            if (!ability || !type) return Fail("abilityOrder 需要两个参数");
            std::optional<std::int64_t> order;
            if (context_.abilityOrder) order = context_.abilityOrder(*ability, *type);
            if (!order) {
                diagnostics_.push_back({DiagnosticSeverity::Warning, "Script.Template", "ABILITY_ORDER_UNKNOWN",
                    "无法确定技能 " + *ability + " 的命令 ID（" + *type + "），按 0 处理", {}});
                return std::int64_t{0};
            }
            return *order;
        }
        if (name == "tostring") {
            if (const auto* flag = arguments.empty() ? nullptr : std::get_if<Boolean>(&arguments[0]))
                return std::string(flag->value ? "true" : "false");
            const auto value = text(0);
            return value ? Value(*value) : Value(std::string("nil"));
        }
        if (name == "tonumber") {
            const auto value = number(0);
            return value ? Integral(*value) : Value(Nil{});
        }
        if (name == "math.modf" || name == "math.floor" || name == "math.ceil" || name == "math.abs") {
            const auto value = number(0);
            if (!value) return Fail(name + " 需要数字参数");
            if (name == "math.modf") return Integral(std::trunc(*value));
            if (name == "math.floor") return Integral(std::floor(*value));
            if (name == "math.ceil") return Integral(std::ceil(*value));
            if (const auto* integer = std::get_if<std::int64_t>(&arguments[0])) return *integer < 0 ? -*integer : *integer;
            return std::fabs(*value);
        }
        if (name == "math.max" || name == "math.min") {
            if (arguments.empty()) return Fail(name + " 需要参数");
            Value best = arguments[0];
            for (std::size_t index = 1; index < arguments.size(); ++index) {
                const auto a = ToNumber(best);
                const auto b = number(index);
                if (!a || !b) return Fail(name + " 需要数字参数");
                if (name == "math.max" ? *b > *a : *b < *a) best = arguments[index];
            }
            return best;
        }
        return Fail("不支持的模板函数：" + name);
    }

    const TemplateContext& context_;
    std::unordered_map<std::string, Value>& variables_;
    std::vector<Diagnostic>& diagnostics_;
    std::vector<MemberReplacement>& imports_;
    std::size_t importBytes_ = 0;
    bool rejected_ = false, importFailed_ = false;
    std::optional<Lexer> lexer_;
    Lexeme current_;
    std::string error_;
};

std::size_t LineOf(std::string_view text, std::size_t offset) {
    std::size_t line = 1;
    for (std::size_t index = 0; index < offset && index < text.size(); ++index) {
        if (text[index] == '\n') ++line;
    }
    return line;
}

} // namespace

bool TemplateResult::Succeeded() const noexcept {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error) return false;
    }
    return true;
}

TemplateResult EvaluateTemplates(std::string_view text, const TemplateContext& context) {
    TemplateResult result;
    result.text.reserve(text.size());
    std::unordered_map<std::string, Value> variables;
    Evaluator evaluator(context, variables, result.diagnostics, result.imports);
    std::size_t at = 0;
    std::size_t errors = 0;
    // 行号按需增量计算，避免对大文件反复从头扫描。
    std::size_t lineAt = 0;
    std::size_t line = 1;
    const auto lineOf = [&](std::size_t offset) {
        line += LineOf(text.substr(lineAt, offset - lineAt), offset - lineAt) - 1;
        lineAt = offset;
        return line;
    };
    while (at < text.size()) {
        const std::size_t open = text.find("<?", at);
        if (open == std::string_view::npos) {
            result.text.append(text.substr(at));
            break;
        }
        result.text.append(text.substr(at, open - at));
        const std::size_t close = text.find("?>", open + 2);
        if (close == std::string_view::npos) {
            // 未闭合时无法确定模板边界，保留输入片段并阻止后续构建。
            result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Template", "TEMPLATE_UNTERMINATED",
                "第 " + std::to_string(lineOf(open)) + " 行的模板标记未闭合，已阻止脚本构建", {}});
            result.text.append(text.substr(open));
            break;
        }
        const bool expression = open + 2 < text.size() && text[open + 2] == '=';
        const std::string_view code = text.substr(open + (expression ? 3 : 2), close - open - (expression ? 3 : 2));
        const auto outputBegin = result.text.size();
        std::string error;
        if (expression) {
            ++result.expressions;
            const auto value = evaluator.Expression(code, error);
            const auto rendered = value ? ToText(*value) : std::nullopt;
            if (!rendered) {
                if (errors++ < 50) {
                    result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Template", "TEMPLATE_EXPRESSION",
                        "第 " + std::to_string(lineOf(open)) + " 行：" + (error.empty() ? "表达式结果为空值" : error) + "：" + std::string(code.substr(0, 120)), {}});
                }
            } else {
                result.text.append(*rendered);
            }
        } else if (!evaluator.Statements(code, error) && evaluator.ImportFailed()) {
            // import 写法已识别但无法完成（名称、路径或读取失败）：明确报错，不交给整份模板执行。
            result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Template", "TEMPLATE_IMPORT_FAILED",
                "第 " + std::to_string(lineOf(open)) + " 行：" + error, {}});
            result.text.append(text.substr(open, close + 2 - open));
        } else if (evaluator.Rejected()) {
            ++result.skippedBlocks;
            const bool slk = code.find("slk") != std::string_view::npos;
            // 不依赖字符串分类决定是否安全：别名或拼接模块名同样可能隐藏物编写入。
            result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Template", slk ? "TEMPLATE_SLK_SKIPPED" : "TEMPLATE_BLOCK_SKIPPED",
                "第 " + std::to_string(lineOf(open)) + " 行的模板代码块未执行" +
                    (slk ? "（物编生成块尚未支持；未验证或应用其物编修改，已阻止脚本构建）"
                         : "（无法确定其副作用，已阻止脚本构建）" + (error.empty() ? std::string{} : "：" + error)), {}});
            // 失败结果仅供诊断；保留原块，避免独立模板命令输出看似完整但缺少行为的脚本。
            result.text.append(text.substr(open, close + 2 - open));
        }
        result.spans.push_back({open, close + 2, outputBegin, result.text.size()});
        at = close + 2;
    }
    if (!result.imports.empty()) {
        std::string names;
        for (std::size_t index = 0; index < result.imports.size() && index < 5; ++index)
            names += (index ? "、" : "") + result.imports[index].name;
        if (result.imports.size() > 5) names += " 等";
        result.diagnostics.push_back({DiagnosticSeverity::Info, "Script.Template", "TEMPLATE_IMPORTED",
            "模板 import 将写入 " + std::to_string(result.imports.size()) + " 个文件：" + names, {}});
    }
    return result;
}

} // namespace War3Map::Script
