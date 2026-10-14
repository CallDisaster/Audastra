#include "LuaCodeModelInternal.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace War3Map::Lua::Detail {
namespace {
constexpr std::uint32_t kOpen = std::numeric_limits<std::uint32_t>::max();
constexpr int kDepthLimit = 180;

struct Expr {
    Type type;
    std::uint32_t begin = 0, end = 0;
    bool multi = false; // 末尾是函数调用或 ...，个数不定
    std::optional<std::string> literal; // 字符串字面量的值
};
// 赋值目标：名字或表字段。
struct Access {
    enum class Kind : std::uint8_t { None, Name, Field } kind = Kind::None;
    std::string name;
    Type owner;
    std::uint32_t begin = 0, end = 0;
};

bool Integral(std::string_view number) {
    if (number.size() > 1 && number[0] == '0' && (number[1] == 'x' || number[1] == 'X'))
        return number.find_first_of(".pP") == std::string_view::npos;
    return number.find_first_of(".eE") == std::string_view::npos;
}
std::string Describe(const Type& type) {
    switch (type.kind) {
    case TypeKind::Nil: return "nil";
    case TypeKind::Boolean: return "布尔";
    case TypeKind::Integer: return "整数";
    case TypeKind::Float: return "小数";
    case TypeKind::Number: return "数字";
    case TypeKind::String: return "文本";
    case TypeKind::Function: return "函数";
    case TypeKind::Table: return "表";
    case TypeKind::Module: case TypeKind::Library: return "模块 " + type.name;
    case TypeKind::Jass: return type.name;
    default: return "未知";
    }
}
// 实参能否传给需要 parameter 类型的 native；不能确定时视为可以。
std::optional<std::string> Mismatch(const Type& argument, std::string_view parameter, const ApiCatalog& catalog) {
    const auto kind = argument.kind;
    if (kind == TypeKind::Unknown) return std::nullopt;
    const auto reject = [&] { return std::optional<std::string>(Describe(argument)); };
    if (parameter == "integer") {
        if (kind == TypeKind::Integer || kind == TypeKind::Number) return std::nullopt;
        return kind == TypeKind::Float ? std::optional<std::string>("小数") : reject();
    }
    if (parameter == "real") return kind == TypeKind::Integer || kind == TypeKind::Float || kind == TypeKind::Number ? std::nullopt : reject();
    if (parameter == "boolean") return kind == TypeKind::Boolean || kind == TypeKind::Nil ? std::nullopt : reject();
    if (parameter == "string") return kind == TypeKind::String || kind == TypeKind::Nil ? std::nullopt : reject();
    if (parameter == "code") return kind == TypeKind::Function || kind == TypeKind::Nil ? std::nullopt : reject();
    if (!catalog.IsJassType(parameter)) return std::nullopt;
    if (kind == TypeKind::Nil) return std::nullopt;
    if (kind == TypeKind::Jass) return catalog.Extends(argument.name, parameter) ? std::nullopt : reject();
    return reject();
}

class Parser {
public:
    Parser(std::string_view source, const ParseOptions& options, Model& model, std::vector<Token> comments, bool warn)
        : source_(source), options_(options), model_(model), tokens_(model.tokens), comments_(std::move(comments)), warn_(warn) {}

    void Run() {
        OpenScope();
        while (!AtEnd()) {
            const auto before = position_;
            Block();
            if (position_ == before) ++position_; // 多余的 end / until 等，跳过继续
        }
        CloseScope(static_cast<std::uint32_t>(source_.size()));
    }

private:
    std::string_view source_;
    const ParseOptions& options_;
    Model& model_;
    const std::vector<Token>& tokens_;
    std::vector<Token> comments_;
    bool warn_ = false;
    std::size_t position_ = 0;
    std::vector<std::size_t> active_;
    std::vector<std::size_t> scopes_;
    std::vector<Type*> returns_;
    int depth_ = 0;

    // ── 记号 ──
    const Token& Peek(std::size_t ahead = 0) const {
        const auto index = (std::min)(position_ + ahead, tokens_.size() - 1);
        return tokens_[index];
    }
    std::string_view Text(const Token& token) const { return token.Text(source_); }
    bool AtEnd() const { return Peek().kind == TokenKind::End; }
    bool Check(std::string_view text) const { return Peek().Is(source_, text); }
    bool Accept(std::string_view text) { if (!Check(text)) return false; ++position_; return true; }
    void Expect(std::string_view text) { (void)Accept(text); }
    bool BlockEnd() const {
        return AtEnd() || Check("end") || Check("else") || Check("elseif") || Check("until");
    }
    std::uint32_t LastEnd() const { return position_ == 0 ? 0 : tokens_[(std::min)(position_, tokens_.size()) - 1].end; }
    struct DepthGuard {
        Parser& parser;
        explicit DepthGuard(Parser& owner) : parser(owner) { ++parser.depth_; }
        ~DepthGuard() { --parser.depth_; }
        [[nodiscard]] bool Exceeded() const { return parser.depth_ > kDepthLimit; }
    };

    // ── 诊断 ──
    void Warn(std::string code, std::string message, std::uint32_t begin, std::uint32_t end) {
        if (!warn_ || model_.warnings.size() >= 1024) return;
        const auto line = std::upper_bound(model_.lineStarts.begin(), model_.lineStarts.end(), begin) - model_.lineStarts.begin();
        const auto start = model_.lineStarts[static_cast<std::size_t>((std::max)(line, static_cast<decltype(line)>(1)) - 1)];
        model_.warnings.push_back({std::move(code), std::move(message), static_cast<std::uint32_t>(line),
            begin - start + 1, end > begin ? end - begin : 0});
    }

    // ── 文档注释：紧贴在声明上方、独占一行的 -- 注释 ──
    std::string DocBefore(std::uint32_t line) const {
        std::vector<std::string> lines;
        auto expected = line;
        const auto after = std::lower_bound(comments_.begin(), comments_.end(), line,
            [](const Token& token, std::uint32_t value) { return token.line < value; });
        for (auto at = std::make_reverse_iterator(after); at != comments_.rend(); ++at) {
            const auto lastLine = at->line + static_cast<std::uint32_t>(std::count(source_.begin() + at->begin, source_.begin() + at->end, '\n'));
            if (lastLine + 1 != expected) break;
            const auto lineStart = model_.lineStarts[at->line - 1];
            const auto prefix = source_.substr(lineStart, at->begin - lineStart);
            if (prefix.find_first_not_of(" \t") != std::string_view::npos) break;
            auto text = std::string(Text(*at));
            text.erase(0, text.find_first_not_of('-'));
            if (text.starts_with("[")) {
                const auto open = text.find('[', 1);
                if (open != std::string::npos) text.erase(0, open + 1);
                const auto close = text.rfind(']');
                if (close != std::string::npos && close > 0) text.erase(text.find_last_not_of("]=", close) + 1);
            }
            text.erase(0, text.find_first_not_of(" \t"));
            while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\t')) text.pop_back();
            lines.push_back(std::move(text));
            expected = at->line;
            if (lines.size() >= 24) break;
        }
        std::reverse(lines.begin(), lines.end());
        std::string result;
        for (const auto& text : lines) {
            if (!result.empty()) result += '\n';
            result += text;
        }
        return result;
    }

    // ── 作用域 ──
    void OpenScope() { scopes_.push_back(active_.size()); }
    void CloseScope(std::uint32_t end) {
        const auto mark = scopes_.empty() ? 0 : scopes_.back();
        if (!scopes_.empty()) scopes_.pop_back();
        for (auto index = mark; index < active_.size(); ++index) model_.locals[active_[index]].scopeEnd = end;
        active_.resize(mark);
    }
    std::size_t Declare(std::string name, Type type, std::uint32_t declared, bool parameter = false, std::string documentation = {},
        std::uint32_t nameEnd = 0) {
        if (type.function && type.function->name.empty())
            std::const_pointer_cast<FunctionInfo>(type.function)->name = name;
        model_.locals.push_back({std::move(name), std::move(type), declared, kOpen, parameter, std::move(documentation), nameEnd});
        active_.push_back(model_.locals.size() - 1);
        return model_.locals.size() - 1;
    }
    std::optional<std::size_t> Lookup(std::string_view name) const {
        for (auto at = active_.rbegin(); at != active_.rend(); ++at) if (model_.locals[*at].name == name) return *at;
        return std::nullopt;
    }
    Type GlobalType(std::string_view name) const {
        if (const auto found = model_.globals.find(name); found != model_.globals.end()) return found->second.type;
        if (!options_.catalog) return {};
        const auto* member = options_.catalog->Global(name);
        if (!member) return {};
        if (member->kind == ApiKind::Module) return {TypeKind::Library, member->table, {}, {}, member};
        if (member->kind == ApiKind::Function) return {TypeKind::Function, {}, {}, {}, member};
        auto type = TypeFromName(member->type, options_.catalog);
        type.api = member;
        return type;
    }

    // ── 语句 ──
    void Block() {
        DepthGuard guard(*this);
        if (guard.Exceeded()) { while (!AtEnd()) ++position_; return; }
        while (!BlockEnd()) {
            const auto before = position_;
            Statement();
            if (position_ == before) ++position_;
        }
    }
    void ScopedBlock() {
        OpenScope();
        Block();
        CloseScope(Peek().begin);
    }
    void Statement() {
        if (Accept(";")) return;
        if (Check("::")) { position_ += 3; return; }
        if (Accept("break")) return;
        if (Accept("goto")) { ++position_; return; }
        if (Accept("do")) { ScopedBlock(); Expect("end"); return; }
        if (Accept("while")) { (void)Expression(); Expect("do"); ScopedBlock(); Expect("end"); return; }
        if (Accept("repeat")) {
            OpenScope(); Block(); Expect("until"); (void)Expression(); CloseScope(LastEnd());
            return;
        }
        if (Accept("if")) {
            (void)Expression(); Expect("then"); ScopedBlock();
            while (Accept("elseif")) { (void)Expression(); Expect("then"); ScopedBlock(); }
            if (Accept("else")) ScopedBlock();
            Expect("end");
            return;
        }
        if (Accept("for")) { ForStatement(); return; }
        if (Check("function")) { FunctionStatement(); return; }
        if (Accept("local")) {
            if (Accept("function")) LocalFunction(); else LocalStatement();
            return;
        }
        if (Check("return")) { ReturnStatement(); return; }
        ExpressionStatement();
    }
    void ForStatement() {
        std::vector<std::string> names;
        do {
            if (Peek().kind != TokenKind::Name) break;
            names.emplace_back(Text(Peek())); ++position_;
        } while (Accept(","));
        const bool numeric = Accept("=");
        if (!numeric) Expect("in");
        (void)ExpressionList();
        Expect("do");
        OpenScope();
        const auto declared = LastEnd();
        for (std::size_t index = 0; index < names.size(); ++index)
            Declare(names[index], numeric ? Type{TypeKind::Number} : Type{}, declared);
        Block();
        CloseScope(Peek().begin);
        Expect("end");
    }
    std::shared_ptr<FunctionInfo> FunctionBody(std::string name, bool method, const Type* self, std::string documentation) {
        auto info = std::make_shared<FunctionInfo>();
        info->name = std::move(name); info->method = method; info->documentation = std::move(documentation);
        Expect("(");
        std::vector<std::pair<std::string, std::uint32_t>> parameters;
        while (!AtEnd() && !Check(")")) {
            if (Accept("...")) { info->variadic = true; break; }
            if (Peek().kind != TokenKind::Name) break;
            parameters.emplace_back(Text(Peek()), Peek().end); ++position_;
            if (!Accept(",")) break;
        }
        Expect(")");
        for (const auto& parameter : parameters) info->parameters.push_back(parameter.first);
        OpenScope();
        const auto declared = LastEnd();
        if (self) Declare("self", *self, declared, true);
        for (const auto& parameter : parameters) Declare(parameter.first, {}, declared, true, {}, parameter.second);
        returns_.push_back(&info->returns);
        Block();
        returns_.pop_back();
        CloseScope(Peek().begin);
        Expect("end");
        return info;
    }
    void LocalFunction() {
        if (Peek().kind != TokenKind::Name) return;
        const auto& nameToken = Peek();
        const std::string name(Text(nameToken));
        const auto documentation = DocBefore(nameToken.line);
        ++position_;
        // 名字在函数体内已经可见（递归调用）。
        auto placeholder = std::make_shared<FunctionInfo>();
        placeholder->name = name;
        const auto index = Declare(name, {TypeKind::Function, {}, {}, placeholder}, nameToken.end, false, documentation, nameToken.end);
        auto info = FunctionBody(name, false, nullptr, documentation);
        *placeholder = std::move(*info);
        model_.locals[index].type.function = placeholder;
    }
    void LocalStatement() {
        std::vector<std::pair<std::string, std::uint32_t>> names;
        const auto line = tokens_[position_ - 1].line;
        do {
            if (Peek().kind != TokenKind::Name) break;
            names.emplace_back(Text(Peek()), Peek().end); ++position_;
            if (Accept("<")) { ++position_; Expect(">"); } // Lua 5.4 属性，按名字处理
        } while (Accept(","));
        std::vector<Expr> values;
        if (Accept("=")) values = ExpressionList();
        const auto declared = LastEnd();
        const auto documentation = DocBefore(line);
        // 没有对应值的名字按未知处理（local x 之后通常会再赋值）。
        for (std::size_t index = 0; index < names.size(); ++index)
            Declare(names[index].first, index < values.size() ? values[index].type : Type{}, declared, false, documentation,
                names[index].second);
    }
    void FunctionStatement() {
        const auto line = Peek().line;
        ++position_;
        if (Peek().kind != TokenKind::Name) return;
        std::vector<std::string> path{std::string(Text(Peek()))};
        auto nameEnd = Peek().end;
        ++position_;
        bool method = false;
        while (Check(".") || Check(":")) {
            method = Check(":");
            ++position_;
            if (Peek().kind != TokenKind::Name) break;
            path.emplace_back(Text(Peek())); nameEnd = Peek().end; ++position_;
            if (method) break;
        }
        std::string fullName = path.front();
        for (std::size_t index = 1; index < path.size(); ++index) fullName += (method && index + 1 == path.size() ? ":" : ".") + path[index];
        const auto documentation = DocBefore(line);
        // 先解析所有者，定义时即写入，函数体内的递归调用与 self 都能看到。
        Type owner;
        if (path.size() > 1) {
            if (const auto local = Lookup(path.front())) owner = model_.locals[*local].type;
            else owner = GlobalType(path.front());
            for (std::size_t index = 1; index + 1 < path.size(); ++index) owner = MemberOf(owner, path[index], options_.catalog);
        }
        auto placeholder = std::make_shared<FunctionInfo>();
        placeholder->name = fullName;
        const Type type{TypeKind::Function, {}, {}, placeholder};
        if (path.size() == 1) {
            if (const auto local = Lookup(path.front())) { if (!model_.locals[*local].type.Known()) model_.locals[*local].type = type; }
            else if (const auto found = model_.globals.find(path.front()); found == model_.globals.end() || !found->second.type.Known())
                model_.globals[path.front()] = {type, documentation, nameEnd};
        } else if (owner.kind == TypeKind::Table && owner.shape) {
            auto& field = owner.shape->fields[path.back()];
            if (!field.type.Known()) field = {type, documentation, nameEnd};
        }
        auto info = FunctionBody(fullName, method, method ? &owner : nullptr, documentation);
        *placeholder = std::move(*info);
    }
    void ReturnStatement() {
        ++position_;
        if (BlockEnd() || Check(";")) { Accept(";"); return; }
        const auto values = ExpressionList();
        Accept(";");
        if (values.empty()) return;
        auto* target = returns_.empty() ? &model_.exports : returns_.back();
        if (!target->Known()) *target = values.front().type;
    }
    void ExpressionStatement() {
        Access first;
        const auto start = position_;
        (void)Suffixed(&first);
        if (position_ == start) return;
        if (!Check("=") && !Check(",")) return;
        std::vector<Access> targets{first};
        while (Accept(",")) {
            Access next;
            (void)Suffixed(&next);
            targets.push_back(std::move(next));
        }
        const auto line = tokens_[start].line;
        Expect("=");
        const auto values = ExpressionList();
        const auto documentation = DocBefore(line);
        for (std::size_t index = 0; index < targets.size(); ++index)
            Assign(targets[index], index < values.size() ? values[index].type : Type{}, documentation);
    }
    void Assign(const Access& target, const Type& type, const std::string& documentation) {
        if (target.kind == Access::Kind::Name) {
            if (const auto local = Lookup(target.name)) {
                auto& symbol = model_.locals[*local];
                if (!symbol.type.Known() || symbol.type.kind == TypeKind::Nil) symbol.type = type;
                return;
            }
            const auto found = model_.globals.find(target.name);
            if (found == model_.globals.end() || !found->second.type.Known())
                model_.globals[target.name] = {type, documentation, target.end};
            return;
        }
        if (target.kind == Access::Kind::Field && target.owner.kind == TypeKind::Table && target.owner.shape) {
            auto& field = target.owner.shape->fields[target.name];
            if (!field.type.Known()) field = {type, documentation, target.end};
            if (field.type.function && field.type.function->name.empty())
                std::const_pointer_cast<FunctionInfo>(field.type.function)->name = target.name;
        }
    }

    // ── 表达式 ──
    std::vector<Expr> ExpressionList() {
        std::vector<Expr> values;
        do {
            if (BlockEnd() || Check(")") || Check(";")) break;
            values.push_back(Expression());
        } while (Accept(","));
        return values;
    }
    static int LeftPriority(std::string_view op) {
        if (op == "or") return 1;
        if (op == "and") return 2;
        if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "~=" || op == "==") return 3;
        if (op == "|") return 4;
        if (op == "~") return 5;
        if (op == "&") return 6;
        if (op == "<<" || op == ">>") return 7;
        if (op == "..") return 9;
        if (op == "+" || op == "-") return 10;
        if (op == "*" || op == "/" || op == "//" || op == "%") return 11;
        if (op == "^") return 14;
        return 0;
    }
    static int RightPriority(std::string_view op) {
        if (op == "..") return 8;
        if (op == "^") return 13;
        return LeftPriority(op);
    }
    static Type Combine(std::string_view op, const Type& left, const Type& right) {
        if (op == "==" || op == "~=" || op == "<" || op == ">" || op == "<=" || op == ">=") return {TypeKind::Boolean};
        if (op == "..") return {TypeKind::String};
        if (op == "and") return right;
        if (op == "or") return left.Known() && left.kind != TypeKind::Nil ? left : right;
        if (op == "|" || op == "~" || op == "&" || op == "<<" || op == ">>") return {TypeKind::Integer};
        const bool integers = left.kind == TypeKind::Integer && right.kind == TypeKind::Integer;
        if (op == "/" || op == "^") return {TypeKind::Float};
        if (op == "//") return {integers ? TypeKind::Integer : TypeKind::Number};
        return {integers ? TypeKind::Integer : TypeKind::Number};
    }
    Expr Expression(int limit = 0) {
        DepthGuard guard(*this);
        if (guard.Exceeded()) { while (!AtEnd()) ++position_; return {}; }
        Expr left;
        const auto begin = Peek().begin;
        if (Check("not") || Check("#") || Check("-") || Check("~")) {
            const auto op = Text(Peek());
            ++position_;
            auto operand = Expression(12);
            left.type = op == "not" ? Type{TypeKind::Boolean} : op == "#" ? Type{TypeKind::Integer}
                : op == "~" ? Type{TypeKind::Integer} : operand.type;
            if (op == "-" && left.type.kind != TypeKind::Integer && left.type.kind != TypeKind::Float) left.type = {TypeKind::Number};
            left.begin = begin; left.end = operand.end;
        } else {
            left = Simple();
        }
        while (true) {
            const auto& token = Peek();
            if (token.kind != TokenKind::Symbol && token.kind != TokenKind::Keyword) break;
            const auto op = Text(token);
            const auto priority = LeftPriority(op);
            if (priority == 0 || priority <= limit) break;
            ++position_;
            auto right = Expression(RightPriority(op));
            left.type = Combine(op, left.type, right.type);
            left.end = right.end; left.multi = false; left.literal.reset();
        }
        return left;
    }
    Expr Simple() {
        const auto& token = Peek();
        Expr result;
        result.begin = token.begin; result.end = token.end;
        if (token.kind == TokenKind::Number) {
            result.type = {token.rawcode || Integral(Text(token)) ? TypeKind::Integer : TypeKind::Float};
            ++position_; return result;
        }
        if (token.kind == TokenKind::String) {
            result.type = {TypeKind::String}; result.literal = StringValue(Text(token));
            ++position_; return result;
        }
        if (Accept("nil")) { result.type = {TypeKind::Nil}; return result; }
        if (Accept("true") || Accept("false")) { result.type = {TypeKind::Boolean}; return result; }
        if (Accept("...")) { result.multi = true; return result; }
        if (Check("function")) {
            const auto documentation = DocBefore(token.line);
            ++position_;
            auto info = FunctionBody({}, false, nullptr, documentation);
            result.type = {TypeKind::Function, {}, {}, info};
            result.end = LastEnd();
            return result;
        }
        if (Check("{")) return Constructor();
        return Suffixed(nullptr);
    }
    Expr Constructor() {
        Expr result;
        result.begin = Peek().begin;
        ++position_;
        auto shape = std::make_shared<Shape>();
        while (!AtEnd() && !Check("}")) {
            const auto before = position_;
            const auto line = Peek().line;
            if (Peek().kind == TokenKind::Name && Peek(1).Is(source_, "=")) {
                const std::string name(Text(Peek()));
                const auto end = Peek().end;
                position_ += 2;
                const auto value = Expression();
                if (value.type.function && value.type.function->name.empty())
                    std::const_pointer_cast<FunctionInfo>(value.type.function)->name = name;
                auto& field = shape->fields[name];
                if (!field.type.Known()) field = {value.type, DocBefore(line), end};
            } else if (Accept("[")) {
                const auto key = Expression();
                Expect("]"); Expect("=");
                const auto value = Expression();
                if (key.literal) shape->fields[*key.literal] = {value.type, DocBefore(line), key.end};
            } else {
                (void)Expression();
            }
            if (!Accept(",") && !Accept(";")) {
                if (position_ == before) ++position_;
                if (!Check("}")) break;
            }
        }
        Expect("}");
        result.end = LastEnd();
        result.type = {TypeKind::Table, {}, shape};
        return result;
    }
    std::vector<Expr> Arguments(bool& parsed) {
        parsed = true;
        if (Peek().kind == TokenKind::String) return {Simple()};
        if (Check("{")) return {Constructor()};
        if (!Accept("(")) { parsed = false; return {}; }
        auto values = ExpressionList();
        Expect(")");
        return values;
    }
    // 前缀表达式及其后缀（字段、索引、调用）。access 记录最后一步，供赋值使用。
    Expr Suffixed(Access* access) {
        Expr result;
        Access last;
        const auto& first = Peek();
        result.begin = first.begin;
        if (first.kind == TokenKind::Name) {
            const std::string name(Text(first));
            ++position_;
            if (const auto local = Lookup(name)) result.type = model_.locals[*local].type;
            else result.type = GlobalType(name);
            last = {Access::Kind::Name, name, {}, first.begin, first.end};
        } else if (Accept("(")) {
            auto inner = Expression();
            Expect(")");
            result.type = inner.type;
        } else {
            return result;
        }
        result.end = LastEnd();
        while (true) {
            DepthGuard guard(*this);
            if (guard.Exceeded()) break;
            if (Check(".") || Check("[")) {
                std::string key;
                std::uint32_t keyBegin = 0, keyEnd = 0;
                bool known = false;
                if (Accept(".")) {
                    if (Peek().kind != TokenKind::Name) { result.type = {}; last = {}; break; }
                    key = Text(Peek()); keyBegin = Peek().begin; keyEnd = Peek().end; known = true;
                    ++position_;
                } else {
                    ++position_;
                    const auto index = Expression();
                    Expect("]");
                    if (index.literal) { key = *index.literal; keyBegin = index.begin; keyEnd = index.end; known = true; }
                }
                const auto owner = result.type;
                result.type = known ? MemberOf(owner, key, options_.catalog) : Type{};
                if (known) CheckMember(owner, key, keyBegin, keyEnd);
                last = known ? Access{Access::Kind::Field, key, owner, keyBegin, keyEnd} : Access{};
                result.end = LastEnd();
                continue;
            }
            if (Check(":")) {
                ++position_;
                if (Peek().kind != TokenKind::Name) { result.type = {}; last = {}; break; }
                const std::string key(Text(Peek()));
                const auto keyToken = Peek();
                ++position_;
                const auto callee = MemberOf(result.type, key, options_.catalog, true);
                bool parsed = false;
                auto arguments = Arguments(parsed);
                result.type = CallResult(callee, arguments, keyToken.begin, keyToken.end, true);
                result.multi = true; result.end = LastEnd(); last = {};
                continue;
            }
            if (Check("(") || Check("{") || Peek().kind == TokenKind::String) {
                // 换行后的 ( 在 Lua 中仍是调用；按调用处理。
                const auto callee = result.type;
                const auto calleeBegin = last.kind != Access::Kind::None ? last.begin : result.begin;
                const auto calleeEnd = last.kind != Access::Kind::None ? last.end : result.end;
                const bool require = last.kind == Access::Kind::Name && last.name == "require" && callee.api && callee.api->name == "require";
                bool parsed = false;
                auto arguments = Arguments(parsed);
                if (!parsed) break;
                if (require && !arguments.empty() && arguments.front().literal) result.type = Require(*arguments.front().literal, arguments.front());
                else if (last.kind == Access::Kind::Name && last.name == "setmetatable" && callee.api && !arguments.empty())
                    result.type = arguments.front().type;
                else result.type = CallResult(callee, arguments, calleeBegin, calleeEnd, false);
                result.multi = true; result.end = LastEnd(); last = {};
                continue;
            }
            break;
        }
        if (last.kind != Access::Kind::None) result.multi = false;
        if (access) *access = last;
        return result;
    }
    Type Require(const std::string& name, const Expr& argument) {
        if (options_.catalog) {
            if (const auto* module = options_.catalog->Module(name); module && module->requirable) return {TypeKind::Module, name};
            if (name.starts_with("jass.")) return {};
        }
        if (options_.moduleExists && !options_.moduleExists(name))
            Warn("LUA_MODULE_NOT_FOUND", "地图里没有 Lua 模块 " + name + "（子目录用点号，如 lessons.hello）", argument.begin, argument.end);
        return options_.resolve ? options_.resolve(name) : Type{};
    }
    // 访问 jass.* 模块上不存在的成员。
    void CheckMember(const Type& owner, std::string_view key, std::uint32_t begin, std::uint32_t end) {
        if (owner.kind != TypeKind::Module || !options_.catalog) return;
        const auto* module = options_.catalog->Module(owner.name);
        if (!module || !module->closed || module->Find(key)) return;
        if (owner.name == "jass.common") {
            // jass.common 找不到 native 时按全局读取；只有被调用时才能确定写错了。
            if (Check("(") || Check("{") || Peek().kind == TokenKind::String)
                Warn("LUA_UNKNOWN_NATIVE", "common.j 里没有 native " + std::string(key), begin, end);
            return;
        }
        Warn("LUA_UNKNOWN_FIELD", owner.name + " 没有 " + std::string(key), begin, end);
    }
    Type CallResult(const Type& callee, const std::vector<Expr>& arguments, std::uint32_t begin, std::uint32_t end, bool method) {
        if (callee.function) return callee.function->returns;
        if (!callee.api || callee.api->kind != ApiKind::Function) return {};
        const auto& api = *callee.api;
        if (api.module == "jass.common" && options_.catalog && !method) CheckNativeCall(api, arguments, begin, end);
        return TypeFromName(api.returns, options_.catalog);
    }
    void CheckNativeCall(const ApiMember& api, const std::vector<Expr>& arguments, std::uint32_t begin, std::uint32_t end) {
        const auto expected = api.parameters.size();
        const auto given = arguments.size();
        const bool open = !arguments.empty() && arguments.back().multi;
        if (given > expected || (!open && given < expected))
            Warn("LUA_NATIVE_ARGUMENT_COUNT", api.name + " 需要 " + std::to_string(expected) + " 个参数，这里是 "
                + std::to_string(given) + (open ? " 个以上" : " 个"), begin, end);
        for (std::size_t index = 0; index < given && index < expected; ++index) {
            const auto& parameter = api.parameters[index];
            if (const auto actual = Mismatch(arguments[index].type, parameter.type, *options_.catalog))
                Warn("LUA_NATIVE_ARGUMENT_TYPE", api.name + " 的参数 " + std::to_string(index + 1) + "（" + parameter.type + " "
                    + parameter.name + "）需要 " + parameter.type + "，这里是 " + *actual, arguments[index].begin, arguments[index].end);
        }
    }
};
} // namespace

std::string TypeName(const Type& type) {
    switch (type.kind) {
    case TypeKind::Nil: return "nil";
    case TypeKind::Boolean: return "boolean";
    case TypeKind::Integer: return "integer";
    case TypeKind::Float: case TypeKind::Number: return "number";
    case TypeKind::String: return "string";
    case TypeKind::Function: return "function";
    case TypeKind::Table: return "table";
    case TypeKind::Module: case TypeKind::Library: case TypeKind::Jass: return type.name;
    default: return {};
    }
}

Type TypeFromName(std::string_view name, const ApiCatalog* catalog) {
    if (name == "integer") return {TypeKind::Integer};
    if (name == "real") return {TypeKind::Float};
    if (name == "number") return {TypeKind::Number};
    if (name == "boolean") return {TypeKind::Boolean};
    if (name == "string") return {TypeKind::String};
    if (name == "code" || name == "function") return {TypeKind::Function};
    if (name == "table") return {TypeKind::Table};
    if (name.empty() || name == "nothing" || name == "any") return {};
    if (name == "handle" || (catalog && catalog->IsJassType(name))) return {TypeKind::Jass, std::string(name)};
    return {};
}

Type MemberOf(const Type& base, std::string_view name, const ApiCatalog* catalog, bool method) {
    const auto fromApi = [&](const ApiModule* module) -> Type {
        const auto* member = module ? module->Find(name) : nullptr;
        if (!member) return {};
        if (member->kind == ApiKind::Function) return {TypeKind::Function, {}, {}, {}, member};
        auto type = TypeFromName(member->type, catalog);
        type.api = member;
        return type;
    };
    switch (base.kind) {
    case TypeKind::Module: case TypeKind::Library:
        return catalog ? fromApi(catalog->Module(base.name)) : Type{};
    case TypeKind::String:
        return method && catalog ? fromApi(catalog->Module("string")) : Type{};
    case TypeKind::Table:
        if (base.shape) if (const auto found = base.shape->fields.find(name); found != base.shape->fields.end()) return found->second.type;
        return {};
    default:
        return {};
    }
}

std::shared_ptr<Model> ParseModel(std::string_view source, const ParseOptions& options) {
    auto all = Tokenize(source, true);
    std::vector<Token> comments;
    auto model = std::make_shared<Model>();
    model->lineStarts.push_back(0);
    for (std::size_t at = 0; at < source.size(); ++at) {
        if (source[at] == '\n') model->lineStarts.push_back(static_cast<std::uint32_t>(at + 1));
        else if (source[at] == '\r' && (at + 1 >= source.size() || source[at + 1] != '\n')) model->lineStarts.push_back(static_cast<std::uint32_t>(at + 1));
    }
    for (auto& token : all) (token.kind == TokenKind::Comment ? comments : model->tokens).push_back(token);
    // 第一遍收集全局定义，第二遍让先使用后定义的全局也有类型。
    {
        Model first;
        first.tokens = model->tokens; first.lineStarts = model->lineStarts;
        Parser(source, options, first, comments, false).Run();
        model->globals = std::move(first.globals);
    }
    Parser(source, options, *model, std::move(comments), options.warnings).Run();
    return model;
}

} // namespace War3Map::Lua::Detail
