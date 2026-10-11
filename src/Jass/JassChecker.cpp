#include "War3MapCore/Jass/JassChecker.h"

#include "JassCheckContext.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <thread>

// 检查流程：按文件顺序解析顶层声明并建立符号表（顺序敏感，单线程）；
// 之后各函数体独立解析与检查（只读共享符号表，多线程），最后按文件、行号合并诊断。
namespace War3Map::Jass {
namespace {

using namespace detail;

constexpr std::string_view kSubsystem = "Jass.Check";

struct Located {
    std::uint32_t file = 0;
    std::uint32_t line = 0;
    Issue issue;
};

struct FunctionWork {
    std::uint32_t file = 0;
    const ProgramFile* program = nullptr;
    const FunctionDecl* decl = nullptr;
    const FunctionSymbol* symbol = nullptr;
    std::vector<Issue> issues;
    CheckFlags flags;
    SymbolCollector symbols;
};

class Declarations {
public:
    Declarations(CheckContext& context, const CheckFlags& flags, std::vector<Located>& issues, CheckBudget& budget,
        std::vector<SymbolCollector>* symbols)
        : context_(context), flags_(flags), issues_(issues), budget_(budget), symbols_(symbols) {}

    void Add(std::uint32_t fileIndex, const ProgramFile& file, std::vector<FunctionWork>& work) {
        file_ = fileIndex;
        collector_ = symbols_ ? &(*symbols_)[fileIndex] : nullptr;
        for (const auto& error : file.errors) {
            budget_.ConsumeDiagnosticBytes(error.message.size() + std::string_view("JASS_SYNTAX").size());
            Push(error.line, {error.line, "JASS_SYNTAX", error.message, false});
        }
        for (const auto& declaration : file.order) {
            budget_.Poll();
            const auto order = next_++;
            switch (declaration.kind) {
            case Declaration::Kind::Type: AddType(file.types[declaration.index]); break;
            case Declaration::Kind::Global: AddGlobal(file, file.globals[declaration.index], order); break;
            case Declaration::Kind::Native: AddNative(file.natives[declaration.index], order); break;
            case Declaration::Kind::Function: {
                const auto& decl = file.functions[declaration.index];
                if (const auto* symbol = AddFunction(decl.name, decl.params, decl.returns, decl.constant, false, decl.line, order)) {
                    ReserveBounded(work, work.size() + 1, budget_.AstLimit(), budget_);
                    work.push_back({fileIndex, &file, &decl, symbol, {}, ApplyPragmas(flags_, decl.pragmas, &budget_)});
                }
                break;
            }
            }
        }
    }

private:
    void Push(std::uint32_t line, Issue issue) {
        ReserveBounded(issues_, issues_.size() + 1, budget_.DiagnosticLimit(), budget_);
        issues_.push_back({file_, line, std::move(issue)});
    }
    void Error(std::uint32_t line, std::string code, std::string message) {
        message = budget_.Message(message);
        budget_.ConsumeDiagnostic(code.size() + message.size());
        Push(line, {line, std::move(code), std::move(message), true});
    }

    TypeId ResolveType(std::string_view name, std::uint32_t line, bool allowNothing = false) {
        if (allowNothing && name.empty()) return kNothing;
        const auto type = context_.types.Find(name);
        if (type == kUnknownType) {
            Error(line, "JASS_UNKNOWN_TYPE", "未知的类型 “" + DiagnosticText(name) + "”");
            return kErrorType;
        }
        return type;
    }

    bool NameTaken(std::string_view name, std::uint32_t line) {
        if (context_.globals.contains(name)) {
            Error(line, "JASS_REDECLARED", "“" + DiagnosticText(name) + "” 已被声明为全局变量");
            return true;
        }
        if (context_.functions.contains(name)) {
            Error(line, "JASS_REDECLARED", "“" + DiagnosticText(name) + "” 已被声明为函数");
            return true;
        }
        return false;
    }

    void AddType(const TypeDecl& decl) {
        const auto parent = context_.types.Find(decl.parent);
        if (parent == kUnknownType || !context_.types.IsHandle(parent)) {
            Error(decl.line, "JASS_UNKNOWN_TYPE", "类型 “" + DiagnosticText(decl.name) + "” 的父类型 “" + DiagnosticText(decl.parent) + "” 未知或不是句柄类型");
            return;
        }
        if (context_.types.Declare(decl.name, parent) == kUnknownType) {
            Error(decl.line, "JASS_REDECLARED", "类型 “" + DiagnosticText(decl.name) + "” 重复声明");
        } else if (collector_) (void)collector_->Define(decl.name, decl.parent, SymbolKind::Type);
    }

    void AddGlobal(const ProgramFile& file, const VarDecl& decl, std::uint32_t order) {
        TypeId type = ResolveType(decl.type, decl.line);
        if (type == kNothing || type == kNull) type = kErrorType;
        if (type == kCode && decl.array) Error(decl.line, "JASS_CODE", "不能声明 code 数组");
        if (decl.init >= 0) {
            std::vector<Issue> issues;
            CodeChecker checker(context_, file.globalPool, flags_, issues, &budget_, collector_);
            checker.SetScope(order);
            checker.CheckInitializer(decl.init, type, decl.name, decl.line);
            for (auto& issue : issues) Push(issue.line, std::move(issue));
        }
        if (NameTaken(decl.name, decl.line)) return;
        const auto definition = collector_ ? collector_->Define(decl.name, decl.type, SymbolKind::Global,
            decl.array, decl.constant) : std::optional<SymbolKey>{};
        context_.globals.emplace(decl.name, GlobalSymbol{type, decl.array, decl.constant, order, decl.line, definition, file_});
    }

    void AddNative(const NativeDecl& decl, std::uint32_t order) {
        AddFunction(decl.name, decl.params, decl.returns, decl.constant, true, decl.line, order);
    }

    const FunctionSymbol* AddFunction(std::string_view name, const std::vector<Param>& params, std::string_view returns, bool constant,
        bool native, std::uint32_t line, std::uint32_t order) {
        FunctionSymbol symbol;
        for (const auto& param : params) {
            budget_.Poll();
            auto type = ResolveType(param.type, line);
            if (type == kNothing || type == kNull) type = kErrorType;
            ReserveBounded(symbol.params, symbol.params.size() + 1, budget_.AstLimit(), budget_);
            ReserveBounded(symbol.paramNames, symbol.paramNames.size() + 1, budget_.AstLimit(), budget_);
            symbol.params.push_back(type);
            symbol.paramNames.push_back(param.name);
        }
        symbol.returns = ResolveType(returns, line, true);
        symbol.constant = constant;
        symbol.native = native;
        symbol.order = order;
        symbol.file = file_;
        symbol.line = line;
        if (NameTaken(name, line)) return nullptr;
        if (collector_) symbol.definition = collector_->Define(name, returns.empty() ? "nothing" : returns,
            native ? SymbolKind::Native : SymbolKind::Function, false, constant);
        if (collector_ && symbol.definition)
            for (const auto& param : params) collector_->Parameter(*symbol.definition, param.type, param.name);
        return &context_.functions.emplace(name, std::move(symbol)).first->second;
    }

    CheckContext& context_;
    CheckFlags flags_;
    std::vector<Located>& issues_;
    CheckBudget& budget_;
    std::vector<SymbolCollector>* symbols_ = nullptr;
    SymbolCollector* collector_ = nullptr;
    std::uint32_t file_ = 0;
    std::uint32_t next_ = 0;
};

// 固定工作池；任何来源/函数的异常都在本线程收敛，不逃出 jthread 触发 terminate。
template<class Task>
void RunWorkers(std::size_t count, unsigned threads, CheckBudget& budget, Task task) {
    std::atomic<std::size_t> next{0};
    const auto worker = [&] {
        try {
            for (std::size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
                budget.Poll();
                task(i);
            }
        } catch (const CheckAborted& error) {
            budget.MarkFailure(error.Cause());
        } catch (const std::bad_alloc&) {
            budget.MarkFailure(CheckFailure::Allocation);
        } catch (...) {
            budget.MarkFailure(CheckFailure::Worker);
        }
    };
    std::vector<std::jthread> pool;
    try {
        pool.reserve(threads > 0 ? threads - 1 : 0);
        for (unsigned i = 1; i < threads; ++i) pool.emplace_back(worker);
    } catch (const std::bad_alloc&) {
        budget.MarkFailure(CheckFailure::Allocation);
    } catch (...) {
        budget.MarkFailure(CheckFailure::Worker);
    }
    worker();
}

void CheckFunctions(const CheckContext& context, std::vector<FunctionWork>& work, unsigned threads, CheckBudget& budget, bool syntaxOnly) {
    RunWorkers(work.size(), threads, budget, [&](std::size_t i) {
        auto& item = work[i];
        const auto body = ParseFunctionBody(item.program->tokens, *item.decl, false, &budget);
        for (const auto& error : body.errors) {
            budget.ConsumeDiagnosticBytes(error.message.size() + std::string_view("JASS_SYNTAX").size());
            ReserveBounded(item.issues, item.issues.size() + 1, budget.DiagnosticLimit(), budget);
            item.issues.push_back({error.line, "JASS_SYNTAX", error.message, false});
        }
        if (syntaxOnly) return;
        CodeChecker checker(context, body.pool, item.flags, item.issues, &budget,
            item.symbols.shared ? &item.symbols : nullptr);
        checker.SetScope(item.symbol->order, item.decl->name);
        checker.CheckFunction(*item.decl, *item.symbol, body);
    });
}

Diagnostic HardDiagnostic(CheckFailure failure) {
    std::string code, message;
    switch (failure) {
    case CheckFailure::Sources: code = "JASS_SOURCE_COUNT_BUDGET"; message = "来源数量超过检查预算。"; break;
    case CheckFailure::SourceBytes: code = "JASS_SOURCE_BYTES_BUDGET"; message = "单个来源字节数超过检查预算或偏移表示上限。"; break;
    case CheckFailure::TotalBytes: code = "JASS_TOTAL_BYTES_BUDGET"; message = "来源合计字节数超过检查预算。"; break;
    case CheckFailure::Tokens: code = "JASS_TOKEN_BUDGET"; message = "来源合计词法记号超过检查预算（包含 End）。"; break;
    case CheckFailure::AstNodes: code = "JASS_AST_NODE_BUDGET"; message = "语法节点及参数边超过检查预算。"; break;
    case CheckFailure::PoolNodes: code = "JASS_POOL_NODE_BUDGET"; message = "单个函数或全局语法池超过检查预算。"; break;
    case CheckFailure::AstDepth: code = "JASS_AST_DEPTH_BUDGET"; message = "语法树真实子树深度超过检查预算。"; break;
    case CheckFailure::Nesting: code = "JASS_NESTING_BUDGET"; message = "解析递归结构超过检查预算。"; break;
    case CheckFailure::Diagnostics: code = "JASS_DIAGNOSTIC_BUDGET"; message = "产生的诊断超过检查预算。"; break;
    case CheckFailure::DiagnosticBytes: code = "JASS_DIAGNOSTIC_BYTES_BUDGET"; message = "诊断文本字节超过检查预算。"; break;
    case CheckFailure::Lexical: code = "JASS_SOURCE_LEXER"; message = "来源词法未能完整读取。"; break;
    case CheckFailure::Cancelled: code = "JASS_CHECK_CANCELLED"; message = "检查已取消。"; break;
    case CheckFailure::Deadline: code = "JASS_CHECK_DEADLINE"; message = "检查超过截止时间。"; break;
    case CheckFailure::Allocation: code = "JASS_CHECK_ALLOCATION"; message = "检查分配失败。"; break;
    case CheckFailure::InvalidTree: code = "JASS_CHECK_INTERNAL_TREE"; message = "检查器检测到无效的内部语法引用。"; break;
    default: code = "JASS_CHECK_WORKER"; message = "检查工作者未能完成。"; break;
    }
    message += "检查未完整执行，不能作为脚本通过证据。";
    return {DiagnosticSeverity::Error, std::string(kSubsystem), std::move(code), std::move(message), std::nullopt};
}

} // namespace

bool JassCheckResult::Succeeded() const noexcept {
    return completed && std::none_of(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& diagnostic) { return diagnostic.severity == DiagnosticSeverity::Error; });
}

JassCheckResult CheckJass(std::span<const JassSource> sources, const JassCheckOptions& options) {
    JassCheckResult result;
    CheckBudget budget(options);
    try {
        budget.Poll();
        if (options.maxSourceWorkers == 0 || options.maxBodyWorkers == 0) {
            result.completed = false;
            result.diagnostics.push_back({DiagnosticSeverity::Error, std::string(kSubsystem), "JASS_CHECK_OPTIONS",
                "工作者数量上限必须大于零；检查未执行。", std::nullopt});
            result.diagnosticOriginExact.push_back(false);
            return result;
        }
        if (sources.size() > (std::min)(options.maxSources,
            static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()))) budget.Fail(CheckFailure::Sources);
        for (const auto& source : sources) {
            if (source.name.size() > 4096) budget.Fail(CheckFailure::SourceBytes);
            budget.RegisterSource(source.text.size());
        }
        CheckFlags flags;
        flags.returnBug = options.returnBug;
        flags.shadow = options.shadow;
        flags.filter = options.filter;

        // 词法先按来源序领取全局剩余额度，不能让多个源各自取完整 token 预算。
        std::vector<ProgramFile> files(sources.size());
        for (std::size_t i = 0; i < sources.size(); ++i) {
            budget.Poll();
            auto tokens = LexJassBounded(sources[i].text, {}, options.maxTokens - budget.Tokens(), &budget);
            if (!tokens.Succeeded()) {
                for (auto& diagnostic : tokens.diagnostics) {
                    if (diagnostic.origin) diagnostic.origin->source = sources[i].name;
                }
                budget.RejectLexical(std::move(tokens.diagnostics));
            }
            budget.ConsumeTokens(tokens.value->size());
            files[i].tokens = std::move(*tokens.value);
        }
        result.sourceThreadsUsed = static_cast<unsigned>((std::max)(std::size_t{1},
            (std::min)({sources.size(), static_cast<std::size_t>(options.maxSourceWorkers), std::size_t{64}})));
        RunWorkers(sources.size(), result.sourceThreadsUsed, budget, [&](std::size_t i) {
            files[i] = ParseProgram(std::move(files[i].tokens), &budget);
        });
        budget.Poll();

        CheckContext context;
        context.options = &options;
        std::vector<Located> located;
        std::vector<FunctionWork> work;
        SymbolCollectionBudget symbolBudget(sources, options.maxSymbolDefinitions,
            options.maxSymbolReferences, options.maxSymbolBytes);
        std::vector<SymbolCollector> collectors;
        if (options.collectSymbols) {
            collectors.resize(sources.size());
            for (std::size_t i = 0; i < collectors.size(); ++i) {
                collectors[i].shared = &symbolBudget; collectors[i].file = static_cast<std::uint32_t>(i);
            }
        }
        Declarations declarations(context, flags, located, budget, options.collectSymbols ? &collectors : nullptr);
        for (std::size_t i = 0; i < files.size(); ++i) {
            context.files.push_back(sources[i].name);
            if (!options.syntaxOnly) {
                declarations.Add(static_cast<std::uint32_t>(i), files[i], work);
            } else {
                for (const auto& error : files[i].errors) {
                    budget.ConsumeDiagnosticBytes(error.message.size() + std::string_view("JASS_SYNTAX").size());
                    ReserveBounded(located, located.size() + 1, budget.DiagnosticLimit(), budget);
                    located.push_back({static_cast<std::uint32_t>(i), error.line, {error.line, "JASS_SYNTAX", error.message, false}});
                }
                for (const auto& decl : files[i].functions) {
                    budget.Poll();
                    ReserveBounded(work, work.size() + 1, budget.AstLimit(), budget);
                    work.push_back({static_cast<std::uint32_t>(i), &files[i], &decl, nullptr, {}, ApplyPragmas(flags, decl.pragmas, &budget)});
                }
            }
            result.lines += files[i].lines;
        }
        result.functions = work.size();
        if (options.collectSymbols && !options.syntaxOnly) {
            for (auto& item : work) {
                item.symbols.shared = &symbolBudget; item.symbols.file = item.file;
                item.symbols.scope = item.symbol->definition;
                if (item.symbol->definition)
                    item.symbols.Scope(*item.symbol->definition, item.decl->line, item.decl->endLine);
            }
        }

        unsigned threads = options.threads != 0 ? options.threads : std::max(1u, std::thread::hardware_concurrency());
        threads = static_cast<unsigned>((std::min)({static_cast<std::size_t>(threads), work.size() / 64 + 1,
            static_cast<std::size_t>(options.maxBodyWorkers), std::size_t{64}}));
        result.threadsUsed = threads;
        CheckFunctions(context, work, threads, budget, options.syntaxOnly);
        budget.Poll();
        result.semanticChecked = !options.syntaxOnly;

        for (auto& item : work) {
            for (auto& issue : item.issues) {
                budget.Poll();
                const bool ignored = issue.semantic ? item.flags.noSemanticErrors : item.flags.noSyntaxErrors;
                if (ignored) {
                    ++result.errorsIgnored;
                    continue;
                }
                ReserveBounded(located, located.size() + 1, budget.DiagnosticLimit(), budget);
                located.push_back({item.file, issue.line, std::move(issue)});
            }
        }
        std::stable_sort(located.begin(), located.end(),
            [](const Located& a, const Located& b) { return a.file != b.file ? a.file < b.file : a.line < b.line; });
        if (options.collectSymbols) {
            for (auto& item : work) collectors.push_back(std::move(item.symbols));
            FinishSymbolIndex(result.symbols, symbolBudget, collectors, budget,
                !options.syntaxOnly && located.empty() && result.errorsIgnored == 0);
        }
        // 已按文件/词法行号排序：每个来源只向前扫一次，重复错误复用当前行边界。
        std::size_t sourceFile = sources.size(), lineStart = 0, lineEnd = 0;
        std::uint32_t sourceLine = 1;
        for (const auto& item : located) {
            budget.Poll();
            if (result.diagnostics.size() >= options.maxErrors) {
                result.diagnostics.push_back({DiagnosticSeverity::Error, std::string(kSubsystem), "JASS_TOO_MANY_ERRORS",
                    "错误过多，只显示前 " + std::to_string(options.maxErrors) + " 个（共 " + std::to_string(located.size()) + " 个）", std::nullopt});
                break;
            }
            const auto text = sources[item.file].text;
            const auto endOfLine = [&text](std::size_t start) {
                const auto end = text.find_first_of("\r\n", start);
                return end == std::string_view::npos ? text.size() : end;
            };
            if (sourceFile != item.file) {
                sourceFile = item.file; sourceLine = 1; lineStart = 0; lineEnd = endOfLine(0);
            }
            while (sourceLine < item.line && lineEnd < text.size()) {
                budget.PollOccasionally(sourceLine);
                lineStart = lineEnd + 1;
                if (text[lineEnd] == '\r' && lineStart < text.size() && text[lineStart] == '\n') ++lineStart;
                ++sourceLine; lineEnd = endOfLine(lineStart);
            }
            Diagnostic diagnostic{DiagnosticSeverity::Error, std::string(kSubsystem), item.issue.code,
                context.DisplayLocation(static_cast<std::uint32_t>(item.file), item.line) + ": " + item.issue.message, std::nullopt};
            bool exactOrigin = false;
            if (sourceLine == item.line) {
                diagnostic.origin = OriginSpan{context.files[item.file], lineStart, lineEnd - lineStart, {}};
                if (item.issue.byteSpan) {
                    const auto [offset, length] = *item.issue.byteSpan;
                    // 跨行字面量/表达式的诊断行可位于运算符处；范围仍是实际AST，不能限定为整行内。
                    if (offset > text.size() || length > text.size() - offset)
                        budget.Fail(CheckFailure::InvalidTree);
                    diagnostic.origin = OriginSpan{context.files[item.file], offset, length, {}};
                    exactOrigin = true;
                }
                auto end = std::min(lineEnd, lineStart + std::min<std::size_t>(512, text.size() - lineStart));
                // 有效 UTF-8 摘录不能在延续字节中间截断；不解析或重新检查原文。
                while (end < lineEnd && end > lineStart && (static_cast<unsigned char>(text[end]) & 0xc0u) == 0x80u) --end;
                diagnostic.message += "\n原文：" + std::string(text.substr(lineStart, end - lineStart));
                if (end < lineEnd) diagnostic.message += "…";
            }
            diagnostic.message = budget.Message(diagnostic.message);
            budget.ConsumeDiagnosticBytes(diagnostic.code.size() + diagnostic.message.size()
                + (diagnostic.origin ? context.files[item.file].size() : 0));
            result.diagnostics.push_back(std::move(diagnostic));
            ReserveBounded(result.diagnosticOriginExact, result.diagnostics.size(), budget.DiagnosticLimit() + 1, budget);
            result.diagnosticOriginExact.resize(result.diagnostics.size(), false);
            result.diagnosticOriginExact.back() = exactOrigin;
        }
        budget.Poll();
    } catch (const CheckAborted& error) {
        budget.MarkFailure(error.Cause());
    } catch (const std::bad_alloc&) {
        budget.MarkFailure(CheckFailure::Allocation);
    } catch (...) {
        budget.MarkFailure(CheckFailure::Worker);
    }
    result.tokens = budget.Tokens();
    result.astNodes = budget.AstNodes();
    result.maxAstDepthObserved = budget.MaxDepth();
    result.diagnosticBytes = budget.DiagnosticBytes();
    if (budget.Failure() != CheckFailure::None) {
        result.completed = false;
        result.symbols.completed = false;
        if (options.collectSymbols) result.symbols.reason = "检查被取消、超时或触及硬预算，引用索引不可作为完整结果。";
        for (auto& diagnostic : budget.TakeLexicalDiagnostics()) result.diagnostics.push_back(std::move(diagnostic));
        // 独立硬错误通道：maxErrors=0 和忽略 pragma 均不能清除此失败。
        result.diagnostics.push_back(HardDiagnostic(budget.Failure()));
    }
    result.diagnosticOriginExact.resize(result.diagnostics.size(), false);
    return result;
}

} // namespace War3Map::Jass
