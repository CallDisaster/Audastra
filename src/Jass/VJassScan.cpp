#include "VJassModel.h"

#include <array>

// vJass 结构解析：库、全局块、原生函数、函数、结构（成员 / 方法）、keyword。函数体只定位范围。
// 遇到尚未支持的特性时记录原因（首个），由翻译入口决定改用 jasshelper。
namespace War3Map::Jass::detail {
namespace {

constexpr std::array<std::string_view, 10> kUnsupportedWords = {
    "module", "endmodule", "implement", "interface", "endinterface", "hook", "delegate", "stub",
    "textmacro", "runtextmacro"};

// 方法调用、成员访问中尚未支持的 vJass 内建成员。
constexpr std::array<std::string_view, 7> kUnsupportedMembers = {"execute", "evaluate", "name", "exists", "getType", "typeid", "super"};

std::uint32_t EndOf(const Token& token) {
    return token.offset + static_cast<std::uint32_t>(token.text.size());
}

bool IsWord(const Token& token, std::string_view word) {
    return token.kind == TokenKind::Identifier && token.text == word;
}

class Scanner {
public:
    explicit Scanner(std::string_view text, CheckBudget* budget) : text_(text), budget_(budget) {
        program_.text = text;
    }

    VProgram Run() {
        if (budget_) budget_->RegisterSource(text_.size());
        ScanDirectives();
        LexOptions options;
        options.blockComments = true;
        if (budget_) {
            auto lexed = LexJassBounded(text_, options, budget_->RemainingTokens(), budget_);
            if (!lexed.Succeeded()) budget_->RejectLexical(std::move(lexed.diagnostics));
            budget_->ConsumeTokens(lexed.value->size());
            program_.tokens = std::move(*lexed.value);
            // 有限名称防止作用域/结构限定名在改写时反复放大；只拒绝，不改源字节。
            for (const auto& token : program_.tokens) {
                budget_->Poll();
                if (token.kind == TokenKind::Identifier && token.text.size() > 4096) budget_->Fail(CheckFailure::SourceBytes);
            }
        } else program_.tokens = LexJass(text_, options);
        StripDebugLines();
        ScanUnsupportedTokens();
        Parser parser(program_.tokens, program_.pool, program_.errors, true, budget_);
        parser_ = &parser;
        TopLevel();
        parser_ = nullptr;
        return std::move(program_);
    }

private:
    template<class T> void Push(std::vector<T>& values, T value) {
        if (budget_) {
            budget_->ConsumeAst();
            ReserveBounded(values, values.size() + 1, budget_->AstLimit(), *budget_);
        }
        values.push_back(std::move(value));
    }
    Parser& P() { return *parser_; }

    void Unsupported(std::uint32_t line, std::string what) {
        if (program_.unsupported.empty()) program_.unsupported = "第 " + std::to_string(line) + " 行：" + DiagnosticText(what);
    }

    // //! 指令写在注释里，词法阶段看不到，按行预扫描。
    void ScanDirectives() {
        std::uint32_t line = 1;
        std::size_t at = 0;
        while (at < text_.size()) {
            if (budget_) budget_->Poll();
            std::size_t end = at;
            while (end < text_.size() && text_[end] != '\n' && text_[end] != '\r') { if (budget_) budget_->PollOccasionally(end); ++end; }
            std::size_t first = at;
            while (first < end && (text_[first] == ' ' || text_[first] == '\t')) ++first;
            if (text_.substr(first, end - first).starts_with("//!")) {
                auto directive = text_.substr(first + 3, end - first - 3);
                while (!directive.empty() && (directive.front() == ' ' || directive.front() == '\t')) directive.remove_prefix(1);
                Unsupported(line, "//! " + std::string(directive.substr(0, directive.find_first_of(" \t"))) + " 指令");
                return;
            }
            at = end;
            if (at < text_.size() && text_[at] == '\r') ++at;
            if (at < text_.size() && text_[at] == '\n') ++at;
            ++line;
        }
    }

    // 与 jasshelper 相同：非调试模式下，以 debug 开头的行整行删除（逐行处理，不按语句块）。
    void StripDebugLines() {
        auto& tokens = program_.tokens;
        std::vector<Token> kept;
        if (!budget_) kept.reserve(tokens.size());
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            if (budget_) budget_->Poll();
            const bool lineStart = kept.empty() || kept.back().kind == TokenKind::Newline || kept.back().kind == TokenKind::Pragma;
            if (!lineStart || !tokens[i].Is(Keyword::Debug)) {
                Push(kept, tokens[i]);
                continue;
            }
            const auto begin = LineStart(tokens[i].offset);
            while (i < tokens.size() && tokens[i].kind != TokenKind::Newline && tokens[i].kind != TokenKind::End) {
                if (budget_) budget_->PollOccasionally(i);
                ++i;
            }
            if (i < tokens.size() && tokens[i].kind == TokenKind::End) {
                Push(kept, tokens[i]);
                Push(program_.debugLines, LineRange{begin, tokens[i].offset});
                break;
            }
            Push(program_.debugLines, LineRange{begin, i < tokens.size() ? EndOf(tokens[i]) : static_cast<std::uint32_t>(text_.size())});
        }
        tokens = std::move(kept);
    }

    void ScanUnsupportedTokens() {
        const auto& tokens = program_.tokens;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            if (budget_) budget_->Poll();
            const Token& token = tokens[i];
            if (token.kind != TokenKind::Identifier) continue;
            if (token.text == "static" && i + 1 < tokens.size() && tokens[i + 1].Is(Keyword::If)) {
                Unsupported(token.line, "static if");
                return;
            }
            if (i > 0 && tokens[i - 1].IsOperator(".")) {
                for (const auto member : kUnsupportedMembers) {
                    if (budget_) budget_->Poll();
                    if (token.text == member) {
                        Unsupported(token.line, "." + std::string(member));
                        return;
                    }
                }
            }
            if (token.text == "super") {
                Unsupported(token.line, std::string(token.text));
                return;
            }
        }
    }

    std::uint32_t LineStart(std::uint32_t offset) const {
        while (offset > 0 && text_[offset - 1] != '\n' && text_[offset - 1] != '\r') { if (budget_) budget_->PollOccasionally(offset); --offset; }
        return offset;
    }

    // 已消耗的最后一个换行之后的位置（当前条目所在行的结束）。
    std::uint32_t ConsumedLineEnd() const {
        const auto position = parser_->Position();
        if (position > 0 && program_.tokens[position - 1].kind == TokenKind::Newline) return EndOf(program_.tokens[position - 1]);
        return program_.tokens[position].offset;
    }

    Visibility Modifiers(std::int32_t library) {
        Visibility visibility = Visibility::Default;
        for (;;) {
            if (budget_) budget_->Poll();
            const Token& token = P().Peek();
            if (IsWord(token, "private") || IsWord(token, "public")) {
                if (library < 0) P().Error(token.line, std::string(token.text) + " 只能用在 library 之内");
                visibility = token.text == "private" ? Visibility::Private : Visibility::Public;
                P().Take();
                continue;
            }
            return visibility;
        }
    }

    void AddScopedName(std::int32_t library, std::string_view name, Visibility visibility) {
        if (library >= 0 && visibility != Visibility::Default) Push(program_.scopedNames, VProgram::ScopedName{library, name, visibility});
    }

    void TopLevel() {
        for (;;) {
            if (budget_) budget_->Poll();
            P().SkipBlankLines();
            if (P().AtEnd()) return;
            const Token& token = P().Peek();
            const std::uint32_t begin = LineStart(token.offset);
            if (IsWord(token, "library") || IsWord(token, "library_once")) {
                Library(begin);
                continue;
            }
            if (IsWord(token, "scope")) {
                Scope(begin, -1);
                continue;
            }
            if (IsWord(token, "endscope")) {
                P().Error(token.line, "多余的 endscope");
                P().SkipLine();
                continue;
            }
            if (IsWord(token, "endlibrary")) {
                P().Error(token.line, "多余的 endlibrary");
                P().SkipLine();
                continue;
            }
            VItem item;
            if (Declaration(-1, begin, item)) Push(program_.top, VTopItem{VTopItem::Kind::Item, item, -1});
        }
    }

    void Library(std::uint32_t begin) {
        VLibrary library;
        const Token& keyword = P().Take();
        library.once = keyword.text == "library_once";
        library.line = keyword.line;
        if (!P().Name(library.name, "库名")) {
            P().SkipLine();
            return;
        }
        while (!P().AtLineEnd() && P().Peek().kind != TokenKind::Pragma) {
            if (budget_) budget_->Poll();
            const Token& word = P().Peek();
            if (IsWord(word, "initializer")) {
                P().Take();
                if (!P().Name(library.initializer, "初始化函数名")) break;
            } else if (IsWord(word, "requires") || IsWord(word, "needs") || IsWord(word, "uses")) {
                P().Take();
                do {
                    VRequirement requirement;
                    requirement.line = word.line;
                    requirement.optional = P().AcceptWord("optional");
                    if (!P().Name(requirement.name, "依赖的库名")) break;
                    Push(library.requirements, requirement);
                } while (P().AcceptOperator(","));
            } else {
                P().Error(word.line, "library 声明中无法识别的“" + std::string(word.text) + "”");
                break;
            }
        }
        P().ExpectLineEnd("library 声明");
        library.header = {begin, ConsumedLineEnd()};
        for (const auto& existing : program_.libraries) {
            if (budget_) budget_->Poll();
            if (existing.name != library.name) continue;
            if (library.once) {
                library.duplicate = true;
            } else {
                P().Error(library.line, "库 " + std::string(library.name) + " 重复定义");
            }
        }
        const auto index = static_cast<std::int32_t>(program_.libraries.size());
        Push(program_.libraries, library);
        for (;;) {
            if (budget_) budget_->Poll();
            P().SkipBlankLines();
            const Token& token = P().Peek();
            if (token.kind == TokenKind::End) {
                P().Error(token.line, "库 " + std::string(library.name) + " 缺少 endlibrary（第 " + std::to_string(library.line) + " 行开始）");
                break;
            }
            const std::uint32_t itemBegin = LineStart(token.offset);
            if (IsWord(token, "endlibrary")) {
                P().Take();
                P().ExpectLineEnd("endlibrary");
                program_.libraries[static_cast<std::size_t>(index)].footer = {itemBegin, ConsumedLineEnd()};
                break;
            }
            if (IsWord(token, "library") || IsWord(token, "library_once")) {
                P().Error(token.line, "库 " + std::string(library.name) + " 缺少 endlibrary（第 " + std::to_string(library.line) + " 行开始）");
                break;
            }
            VItem item;
            if (Declaration(index, itemBegin, item)) Push(program_.libraries[static_cast<std::size_t>(index)].items, item);
        }
        auto& stored = program_.libraries[static_cast<std::size_t>(index)];
        if (stored.footer.end == 0) stored.footer = {ConsumedLineEnd(), ConsumedLineEnd()};
        VTopItem top;
        top.kind = VTopItem::Kind::Library;
        top.item.range = {stored.header.begin, stored.footer.end};
        top.library = index;
        Push(program_.top, top);
    }

    void Scope(std::uint32_t begin, std::int32_t parent) {
        NestingGuard guard(budget_, nesting_);
        VScope scope;
        const Token& keyword = P().Take();
        scope.line = keyword.line;
        scope.parent = parent;
        scope.order = static_cast<std::uint32_t>(program_.scopes.size());
        if (!P().Name(scope.name, "作用域名")) {
            P().SkipLine();
            return;
        }
        if (P().AcceptWord("initializer") || P().AcceptWord("requires") || P().AcceptWord("needs") || P().AcceptWord("uses")) {
            Unsupported(keyword.line, "scope 的依赖或 initializer");
            P().SkipLine();
            return;
        }
        P().ExpectLineEnd("scope 声明");
        scope.header = {begin, ConsumedLineEnd()};
        std::string path = std::string(scope.name);
        if (parent >= 0) path = program_.scopes[static_cast<std::size_t>(parent)].path + "__" + path;
        if (budget_ && path.size() > 4096) budget_->Fail(CheckFailure::SourceBytes);
        scope.path = path;
        const auto index = static_cast<std::int32_t>(program_.scopes.size());
        Push(program_.scopes, scope);
        if (parent >= 0) Push(program_.scopes[static_cast<std::size_t>(parent)].children, index);
        for (;;) {
            if (budget_) budget_->Poll();
            P().SkipBlankLines();
            const Token& token = P().Peek();
            if (token.kind == TokenKind::End) {
                P().Error(token.line, "作用域 " + path + " 缺少 endscope");
                break;
            }
            const std::uint32_t itemBegin = LineStart(token.offset);
            if (IsWord(token, "endscope")) {
                P().Take();
                P().ExpectLineEnd("endscope");
                program_.scopes[static_cast<std::size_t>(index)].footer = {itemBegin, ConsumedLineEnd()};
                break;
            }
            if (IsWord(token, "scope")) {
                const auto before = program_.scopes.size();
                Scope(itemBegin, index);
                if (program_.scopes.size() > before) {
                    VItem child;
                    child.kind = ItemKind::Keyword;
                    child.index = static_cast<std::int32_t>(before);
                    child.range = program_.scopes[before].header;
                    child.range.end = program_.scopes[before].footer.end;
                    Push(program_.scopes[static_cast<std::size_t>(index)].items, child);
                }
                continue;
            }
            if (IsWord(token, "library") || IsWord(token, "library_once") || IsWord(token, "struct")) {
                Unsupported(token.line, "scope 内的 " + std::string(token.text));
                P().SkipLine();
                continue;
            }
            VItem item;
            if (Declaration(-1, itemBegin, item, index)) Push(program_.scopes[static_cast<std::size_t>(index)].items, item);
        }
        auto& stored = program_.scopes[static_cast<std::size_t>(index)];
        if (stored.footer.end == 0) stored.footer = {ConsumedLineEnd(), ConsumedLineEnd()};
        if (parent < 0) {
            VTopItem top;
            top.kind = VTopItem::Kind::Library;
            top.item.range = {stored.header.begin, stored.footer.end};
            top.library = -(index + 2);
            Push(program_.top, top);
        }
    }

    bool Declaration(std::int32_t library, std::uint32_t begin, VItem& item, std::int32_t scope = -1) {
        const auto visibility = Modifiers(library);
        const Token& token = P().Peek();
        item.range.begin = begin;
        if (token.kind == TokenKind::Identifier) {
            for (const auto word : kUnsupportedWords) {
                if (budget_) budget_->Poll();
                if (token.text == word) {
                    Unsupported(token.line, std::string(word));
                    P().SkipLine();
                    return false;
                }
            }
        }
        if (token.Is(Keyword::Globals)) {
            Globals(library, scope);
            item.kind = ItemKind::Globals;
        } else if (token.Is(Keyword::Native) || (token.Is(Keyword::Constant) && P().Peek(1).Is(Keyword::Native))) {
            item.kind = ItemKind::Native;
            item.index = Native();
            if (item.index < 0) return false;
        } else if (token.Is(Keyword::Function) || (token.Is(Keyword::Constant) && P().Peek(1).Is(Keyword::Function))) {
            if (IsWord(P().Peek(token.Is(Keyword::Constant) ? 2 : 1), "interface")) {
                Unsupported(token.line, "function interface");
                P().SkipLine();
                return false;
            }
            item.kind = ItemKind::Function;
            item.index = Function(library, visibility, -1, false, scope);
            if (item.index < 0) return false;
        } else if (IsWord(token, "struct")) {
            item.kind = ItemKind::Struct;
            item.index = Struct(library, visibility);
            if (item.index < 0) return false;
        } else if (IsWord(token, "keyword")) {
            P().Take();
            std::string_view name;
            if (P().Name(name, "名称")) AddScopedName(library, name, visibility);
            P().ExpectLineEnd("keyword");
            item.kind = ItemKind::Keyword;
        } else if (token.Is(Keyword::Type)) {
            Unsupported(token.line, "type 声明");
            P().SkipLine();
            return false;
        } else {
            P().Error(token.line, "此处需要声明（library / globals / native / function / struct），实际为“" + std::string(token.text) + "”");
            P().SkipLine();
            return false;
        }
        item.range.end = ConsumedLineEnd();
        return true;
    }

    void Globals(std::int32_t library, std::int32_t scope = -1) {
        const std::uint32_t line = P().Take().line;
        P().ExpectLineEnd("globals");
        for (;;) {
            if (budget_) budget_->Poll();
            P().SkipBlankLines();
            if (P().Accept(Keyword::EndGlobals)) {
                P().ExpectLineEnd("endglobals");
                return;
            }
            const Token& next = P().Peek();
            if (next.kind == TokenKind::End || next.Is(Keyword::Function) || next.Is(Keyword::Globals) || IsWord(next, "endlibrary")) {
                P().Error(next.line, "globals 缺少 endglobals（第 " + std::to_string(line) + " 行开始）");
                return;
            }
            const auto visibility = Modifiers(library);
            if (IsWord(P().Peek(), "key")) {
                Unsupported(P().Peek().line, "key 声明");
                P().SkipLine();
                continue;
            }
            VGlobal global;
            if (!P().Variable(global.decl)) continue;
            if (global.decl.init < 0 && P().Peek().kind == TokenKind::Identifier) {
                // 与 jasshelper 相同：名称之后多余的单词（如 GUI 变量名中的空格）忽略，仍读取 = 之后的初始值。
                const Token& extra = P().Peek();
                Push(program_.warnings, SyntaxError{extra.line, "全局变量 " + std::string(global.decl.name) + " 之后多余的“" + std::string(extra.text) + "”已忽略"});
                while (P().Peek().kind == TokenKind::Identifier) P().Take();
                if (P().AcceptOperator("=")) global.decl.init = P().Expression();
            }
            global.visibility = visibility;
            global.library = library;
            global.scope = scope;
            AddScopedName(library, global.decl.name, visibility);
            Push(program_.globals, global);
            P().ExpectLineEnd("全局变量声明");
        }
    }

    std::int32_t Native() {
        NativeDecl decl;
        const Token& first = P().Peek();
        decl.line = first.line;
        decl.constant = P().Accept(Keyword::Constant);
        P().Accept(Keyword::Native);
        const bool ok = P().Name(decl.name, "函数名") && P().Accept(Keyword::Takes) && P().Params(decl.params) &&
            P().Accept(Keyword::Returns) && P().ReturnType(decl.returns);
        if (!ok) {
            P().Error(decl.line, "原生函数声明格式应为 native 名称 takes 参数 returns 类型");
            P().SkipLine();
            return -1;
        }
        P().ExpectLineEnd("原生函数声明");
        Push(program_.natives, LineRange{LineStart(first.offset), ConsumedLineEnd()});
        Push(program_.nativeDecls, std::move(decl));
        return static_cast<std::int32_t>(program_.natives.size() - 1);
    }

    // 函数或方法：声明行已在当前位置（方法的修饰词已消耗）。返回 VFunction 下标。
    std::int32_t Function(std::int32_t library, Visibility visibility, std::int32_t structIndex, bool isStatic, std::int32_t scope = -1) {
        VFunction function;
        const Token& first = P().Peek();
        function.headerBegin = LineStart(first.offset);
        function.decl.line = first.line;
        function.library = library;
        function.visibility = visibility;
        function.structIndex = structIndex;
        function.isStatic = isStatic;
        const bool method = structIndex >= 0;
        function.isMethod = method;
        if (method) {
            P().Take(); // method
            if (P().AcceptWord("operator")) {
                if (P().AcceptOperator("[")) {
                    const Token& close = P().Peek();
                    if (!P().AcceptOperator("]")) {
                        P().Error(close.line, "method operator [] 格式错误");
                        P().SkipLine();
                        return -1;
                    }
                    function.decl.name = close.text;
                    function.op = P().AcceptOperator("=") ? OperatorKind::IndexSet : OperatorKind::Index;
                } else if (P().Peek().kind == TokenKind::Identifier) {
                    function.decl.name = P().Take().text;
                    function.op = P().AcceptOperator("=") ? OperatorKind::Set : OperatorKind::Get;
                } else {
                    Unsupported(first.line, "method operator " + std::string(P().Peek().text));
                    P().SkipLine();
                    return -1;
                }
            } else if (!P().Name(function.decl.name, "方法名")) {
                P().SkipLine();
                return -1;
            }
        } else {
            function.decl.constant = P().Accept(Keyword::Constant);
            P().Accept(Keyword::Function);
            if (!P().Name(function.decl.name, "函数名")) {
                P().SkipLine();
                return -1;
            }
        }
        const bool ok = P().Accept(Keyword::Takes) && P().Params(function.decl.params) && P().Accept(Keyword::Returns) &&
            P().ReturnType(function.decl.returns);
        if (!ok) {
            P().Error(first.line, method ? "方法声明格式应为 method 名称 takes 参数 returns 类型" : "函数声明格式应为 function 名称 takes 参数 returns 类型");
            P().SkipLine();
        } else {
            P().ExpectLineEnd(method ? "方法声明" : "函数声明");
        }
        function.headerEnd = ConsumedLineEnd();
        function.decl.bodyBegin = P().Position();
        // 找行首的结束标记；先遇到下一个声明说明缺少结束标记。
        const auto& tokens = program_.tokens;
        std::size_t end = function.decl.bodyBegin;
        bool found = false;
        for (; end < tokens.size() && tokens[end].kind != TokenKind::End; ++end) {
            if (budget_) budget_->Poll();
            const Token& token = tokens[end];
            if (!AtLineStart(tokens, end)) continue;
            if (method ? IsWord(token, "endmethod") : token.Is(Keyword::EndFunction)) {
                found = true;
                break;
            }
            const bool header = token.Is(Keyword::Function) || token.Is(Keyword::Globals) || token.Is(Keyword::Native) ||
                IsWord(token, "method") || IsWord(token, "endstruct") || IsWord(token, "struct") || IsWord(token, "library") ||
                IsWord(token, "endlibrary") || IsWord(token, "private") || IsWord(token, "public") ||
                (method && token.Is(Keyword::EndFunction)) || (!method && IsWord(token, "endmethod")) ||
                (token.Is(Keyword::Constant) && end + 1 < tokens.size() && tokens[end + 1].Is(Keyword::Function));
            if (header) break;
        }
        function.decl.bodyEnd = end;
        function.decl.endLine = tokens[end].line;
        function.endToken = tokens[end].offset;
        P().Seek(end);
        if (found) {
            P().Take();
            P().ExpectLineEnd(method ? "endmethod" : "endfunction");
        } else {
            P().Error(function.decl.endLine, std::string(method ? "方法 " : "函数 ") + std::string(function.decl.name) + " 缺少 " +
                (method ? "endmethod" : "endfunction") + "（第 " + std::to_string(function.decl.line) + " 行开始）");
        }
        function.endLineEnd = ConsumedLineEnd();
        if (!method) AddScopedName(library, function.decl.name, visibility);
        function.scope = scope;
        Push(program_.functions, std::move(function));
        return static_cast<std::int32_t>(program_.functions.size() - 1);
    }

    std::int32_t Struct(std::int32_t library, Visibility visibility) {
        VStruct data;
        const Token& keyword = P().Take();
        data.line = keyword.line;
        data.library = library;
        data.visibility = visibility;
        if (!P().Name(data.name, "结构名")) {
            P().SkipLine();
            return -1;
        }
        if (P().Peek().IsOperator("[")) {
            Unsupported(keyword.line, "指定容量的 struct");
        }
        if (P().Accept(Keyword::Extends)) {
            if (P().Peek().Is(Keyword::Array)) {
                Unsupported(keyword.line, "struct extends array");
            } else {
                P().Name(data.parentName, "父结构名");
            }
        }
        P().ExpectLineEnd("struct 声明");
        AddScopedName(library, data.name, visibility);
        const auto index = static_cast<std::int32_t>(program_.structs.size());
        Push(program_.structs, data);
        for (;;) {
            if (budget_) budget_->Poll();
            P().SkipBlankLines();
            const Token& token = P().Peek();
            if (token.kind == TokenKind::End || IsWord(token, "endlibrary") || IsWord(token, "struct") || token.Is(Keyword::Function)) {
                P().Error(token.line, "结构 " + std::string(data.name) + " 缺少 endstruct（第 " + std::to_string(data.line) + " 行开始）");
                break;
            }
            if (IsWord(token, "endstruct")) {
                P().Take();
                P().ExpectLineEnd("endstruct");
                break;
            }
            bool isStatic = false;
            for (;;) {
                if (budget_) budget_->Poll();
                const Token& word = P().Peek();
                if (IsWord(word, "private") || IsWord(word, "public") || IsWord(word, "readonly")) {
                    P().Take();
                } else if (IsWord(word, "static")) {
                    P().Take();
                    isStatic = true;
                } else {
                    break;
                }
            }
            const Token& head = P().Peek();
            if (head.kind == TokenKind::Identifier) {
                bool unsupported = false;
                for (const auto word : kUnsupportedWords) unsupported = unsupported || head.text == word;
                if (unsupported || head.text == "optional") {
                    Unsupported(head.line, std::string(head.text));
                    P().SkipLine();
                    continue;
                }
            }
            if (IsWord(head, "method")) {
                const auto method = Function(library, Visibility::Default, index, isStatic);
                if (method < 0) continue;
                auto& stored = program_.structs[static_cast<std::size_t>(index)];
                Push(stored.methods, method);
                const auto& function = program_.functions[static_cast<std::size_t>(method)];
                if (function.op == OperatorKind::None && function.decl.name == "onDestroy" && !isStatic) stored.onDestroy = method;
                if (function.op == OperatorKind::None && function.decl.name == "create" && isStatic) stored.create = method;
                if (function.op == OperatorKind::None && function.decl.name == "onInit") Unsupported(head.line, "onInit");
                continue;
            }
            VMember member;
            member.isStatic = isStatic;
            if (!P().Variable(member.decl)) continue;
            if (!isStatic && member.decl.array) Unsupported(member.decl.line, "结构的数组成员");
            Push(program_.structs[static_cast<std::size_t>(index)].members, member);
            P().ExpectLineEnd("结构成员声明");
        }
        return index;
    }

    std::string_view text_;
    VProgram program_;
    Parser* parser_ = nullptr;
    CheckBudget* budget_ = nullptr;
    std::size_t nesting_ = 0;
};

} // namespace

VProgram ScanVJass(std::string_view text, CheckBudget* budget) {
    return Scanner(text, budget).Run();
}

std::string ScopedFinalName(std::string_view library, std::string_view name, Visibility visibility) {
    switch (visibility) {
    case Visibility::Private: return std::string(library) + "__" + std::string(name);
    case Visibility::Public: return std::string(library) + "_" + std::string(name);
    default: return std::string(name);
    }
}

} // namespace War3Map::Jass::detail
