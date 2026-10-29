#include "War3MapCore/Jass/Preprocessor.h"

#include "PpExpression.h"
#include "PpLexer.h"
#include "JassCheckBudget.h"

#include <algorithm>
#include <deque>
#include <exception>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

// 预处理器实现：宏展开按 Prosser 的隐藏集算法（C99 标准附录中的参考算法）。
// 未展开的原文按源偏移整段复制（保留注释与排版）；展开结果按记号拼写输出，必要时补空格防止记号相连。
namespace War3Map::Jass {
namespace {

using detail::PpKind;
using detail::PpToken;
using TokenList = std::vector<PpToken>;
using detail::CheckBudget;
using detail::CheckFailure;
using detail::DiagnosticText;
using detail::NestingGuard;
using detail::ReserveBounded;

constexpr std::size_t kMaxErrors = 100;
constexpr std::string_view kSubsystem = "Jass.Preprocess";

struct Lexed {
    std::string text;
    SourceLexicalView physicalView, logicalView;
    detail::PpSplicedSource logical;
    TokenList tokens;
    [[nodiscard]] std::size_t SpliceStorage() const {
        const auto dynamic = [](const SourceLexicalView& view) {
            std::size_t bytes = 0;
            for (const auto& span : view.opaque) bytes += span.sourceSha256.capacity() + span.origin.fingerprint.capacity() +
                span.origin.source.native().capacity() * sizeof(std::filesystem::path::value_type);
            return bytes;
        };
        const auto opaque = physicalView.text.empty() && physicalView.opaque.empty() ? std::size_t{0} :
            physicalView.text.capacity()+1+physicalView.opaque.capacity()*sizeof(SourceOpaqueSpan)+dynamic(physicalView);
        return opaque + (logical.splices.empty() ? 0 : logical.text.capacity()+1+
            logical.splices.capacity()*sizeof(detail::PpLineSplice)+logicalView.text.capacity()+1+
            logicalView.opaque.capacity()*sizeof(SourceOpaqueSpan)+dynamic(logicalView));
    }
};

std::uint64_t ContentHash(std::string_view text, CheckBudget& budget) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < text.size(); ++i) {
        budget.PollOccasionally(i);
        hash ^= static_cast<unsigned char>(text[i]);
        hash *= 0x100000001B3ull;
    }
    return hash ^ text.size();
}

std::string_view StripBom(std::string_view text) {
    return text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text;
}

std::optional<std::string> ReadFile(const std::filesystem::path& path, CheckBudget& budget, std::size_t limit) {
    budget.Poll();
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const auto length = input.tellg();
    if (length < 0) return std::nullopt;
    if (static_cast<std::uint64_t>(length) > limit) budget.Fail(CheckFailure::SourceBytes);
    std::string text(static_cast<std::size_t>(length), '\0');
    input.seekg(0);
    for (std::size_t at = 0; at < text.size();) {
        budget.Poll();
        const auto bytes = (std::min)(text.size() - at, std::size_t{65536});
        if (!input.read(text.data() + at, static_cast<std::streamsize>(bytes))) return std::nullopt;
        at += bytes;
    }
    return text;
}

std::string PathText(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// 隐藏集：宏编号的有序集合，按内容驻留，记号只存编号。
class HideSets {
public:
    explicit HideSets(CheckBudget& budget) : budget_(budget) { sets_.emplace_back(); }

    [[nodiscard]] bool Contains(std::uint32_t set, std::uint32_t macro) const {
        const auto& items = sets_[set];
        return std::binary_search(items.begin(), items.end(), macro);
    }
    std::uint32_t With(std::uint32_t set, std::uint32_t macro) {
        budget_.ConsumeAst(sets_[set].size() + 1);
        if (Contains(set, macro)) return set;
        auto items = sets_[set];
        items.insert(std::upper_bound(items.begin(), items.end(), macro), macro);
        return Intern(std::move(items));
    }
    std::uint32_t Union(std::uint32_t a, std::uint32_t b) {
        budget_.ConsumeAst(sets_[a].size() + sets_[b].size());
        if (a == b || b == 0) return a;
        if (a == 0) return b;
        std::vector<std::uint32_t> items;
        std::set_union(sets_[a].begin(), sets_[a].end(), sets_[b].begin(), sets_[b].end(), std::back_inserter(items));
        return Intern(std::move(items));
    }
    std::uint32_t Intersect(std::uint32_t a, std::uint32_t b) {
        budget_.ConsumeAst(sets_[a].size() + sets_[b].size());
        if (a == b) return a;
        if (a == 0 || b == 0) return 0;
        std::vector<std::uint32_t> items;
        std::set_intersection(sets_[a].begin(), sets_[a].end(), sets_[b].begin(), sets_[b].end(), std::back_inserter(items));
        return Intern(std::move(items));
    }

private:
    std::uint32_t Intern(std::vector<std::uint32_t> items) {
        budget_.ConsumeAst(items.size());
        if (items.empty()) return 0;
        const auto [found, inserted] = index_.try_emplace(items, static_cast<std::uint32_t>(sets_.size()));
        if (inserted) {
            ReserveBounded(sets_, sets_.size() + 1, budget_.AstLimit(), budget_);
            sets_.push_back(std::move(items));
        }
        return found->second;
    }

    std::vector<std::vector<std::uint32_t>> sets_;
    std::map<std::vector<std::uint32_t>, std::uint32_t> index_;
    CheckBudget& budget_;
};

struct Macro {
    std::string source;
    std::uint32_t sourceLine = 0;
    std::uint32_t id = 0;
    bool function = false;
    bool variadic = false;
    std::vector<std::string_view> params; // 可变参数宏最后一项为 __VA_ARGS__（或具名的 GNU 形式）
    TokenList body;
    enum class Builtin { None, Line, File } builtin = Builtin::None;
    std::string origin; // 定义位置（诊断用）
};

bool SameDefinition(const Macro& a, const Macro& b) {
    if (a.function != b.function || a.variadic != b.variadic || a.params != b.params || a.body.size() != b.body.size()) return false;
    for (std::size_t i = 0; i < a.body.size(); ++i) {
        if (a.body[i].text != b.body[i].text || (i > 0 && a.body[i].space != b.body[i].space)) return false;
    }
    return true;
}

// 正在处理的文件。
struct FileContext {
    const Lexed* lexed = nullptr;
    std::uint32_t fileId = 0;
    std::string name;
    std::filesystem::path directory;
    std::filesystem::path canonical; // #pragma once 的键
    std::uint32_t depth = 0;
    std::string_view stageText;
};

// 条件编译栈的一层。
struct Conditional {
    bool parentActive = true;
    bool active = false;
    bool taken = false;
    bool sawElse = false;
    std::uint32_t line = 0;
};

// 宏展开的记号流：先取 pending（待重扫的展开结果），再取当前文件的源记号（仅正文行）。
struct Stream {
    std::deque<PpToken> pending;
    std::size_t pendingBytes = 0;
    const TokenList* source = nullptr;
    std::size_t* cursor = nullptr;

    // 下一个记号（skipNewlines 时跨行查看源记号）；没有时为 nullptr。
    [[nodiscard]] const PpToken* Peek(bool skipNewlines) const {
        if (!pending.empty()) return &pending.front();
        if (source == nullptr) return nullptr;
        std::size_t at = *cursor;
        while (skipNewlines && (*source)[at].kind == PpKind::Newline) ++at;
        const auto& token = (*source)[at];
        return token.kind == PpKind::End || token.kind == PpKind::Newline ? nullptr : &token;
    }
    // 取出下一个记号；跨过的换行记为前导空白。
    bool Take(PpToken& token) {
        if (!pending.empty()) {
            token = pending.front();
            pending.pop_front();
            pendingBytes -= token.text.size();
            return true;
        }
        if (source == nullptr) return false;
        bool crossed = false;
        while ((*source)[*cursor].kind == PpKind::Newline) {
            ++*cursor;
            crossed = true;
        }
        if ((*source)[*cursor].kind == PpKind::End) return false;
        token = (*source)[(*cursor)++];
        if (crossed) token.space = true;
        return true;
    }
};

class Engine {
public:
    Engine(const PreprocessOptions& options, PreprocessCache::Impl* cache, PreprocessResult& result, CheckBudget& budget)
        : options_(options), cache_(cache), result_(result), budget_(budget), hideSets_(budget) {}

    void Run(std::string_view mainText, const std::string& mainName, const std::filesystem::path& mainDirectory);

private:
    template<class T> void Push(std::vector<T>& values, T value) {
        budget_.ConsumeAst();
        ReserveBounded(values, values.size() + 1, budget_.AstLimit(), budget_);
        values.push_back(std::move(value));
    }
    void CopyTokens(std::size_t count) { budget_.ConsumeAst(count); }
    void AddDiagnostic(Diagnostic diagnostic) {
        diagnostic.message = budget_.Message(diagnostic.message);
        budget_.ConsumeDiagnostic(diagnostic.message.size() + diagnostic.code.size());
        result_.diagnostics.push_back(std::move(diagnostic));
    }
    void Generated(std::size_t bytes) {
        budget_.Poll();
        if (generatedBytes_ > options_.check.maxTotalBytes || bytes > options_.check.maxTotalBytes - generatedBytes_)
            budget_.Fail(CheckFailure::TotalBytes);
        generatedBytes_ += bytes;
    }
    // —— 文件与缓存 ——
    // cacheable 为 false 时不进缓存（主文件每次都不同，缓存只会增长）。
    std::shared_ptr<const Lexed> Lex(std::string text, bool cacheable = true,
        const std::filesystem::path& source = {}, const SourceLexicalView* explicitView = nullptr);
    SourceLexicalView ViewOf(std::string_view text) const;
    std::optional<std::filesystem::path> FindInclude(const std::string& name, bool angled,
        const std::filesystem::path& currentDirectory, const FileContext& includer, std::uint32_t line,
        std::uint32_t offset = 0, std::uint32_t length = 0, bool forced = false);
    void IncludeFile(const std::filesystem::path& path, const FileContext& includer, std::uint32_t line);
    void ProcessFile(const FileContext& file);

    // —— 行 ——
    void TextLine(const FileContext& file, std::size_t& i);
    void Directive(const FileContext& file, std::size_t begin, std::size_t end);
    [[nodiscard]] bool Active() const { return conditionals_.empty() || conditionals_.back().active; }

    // —— 指令 ——
    void Define(const FileContext& file, std::span<const PpToken> tokens, std::uint32_t line);
    void Undefine(const FileContext& file, std::span<const PpToken> tokens, std::uint32_t line);
    void Include(const FileContext& file, std::size_t begin, std::size_t end, std::uint32_t line);
    std::optional<bool> Condition(const FileContext& file, std::span<const PpToken> tokens, std::uint32_t line);

    // —— 宏 ——
    std::uint32_t MacroId(std::string_view name);
    const Macro* FindMacro(const PpToken& token) const;
    bool TryExpand(const PpToken& token, Stream& stream, const FileContext& file, std::uint32_t* sourceEnd);
    bool CollectArguments(const Macro& macro, const PpToken& name, Stream& stream, std::vector<TokenList>& arguments,
        PpToken& closing, const FileContext& file, std::uint32_t* sourceEnd);
    TokenList Substitute(const Macro& macro, std::vector<TokenList>& arguments);
    void AuditMacro(const Macro& macro, std::string_view name, const FileContext& file, std::uint32_t line, bool expanded);
    TokenList ExpandIsolated(std::span<const PpToken> tokens, const FileContext& file);
    PpToken Stringize(const TokenList& argument, bool space);
    void Paste(TokenList& out, const PpToken& right);
    std::string_view Store(std::string text);

    // —— 输出 ——
    void Append(std::string_view text);
    bool CheckOutputBudget(std::size_t used, std::size_t added);
    void Flush(const FileContext& file, std::uint32_t from, std::uint32_t to);
    void Emit(const PpToken& token);
    void EnsureNewline();
    void Sync(std::uint32_t fileId, std::uint32_t line) { result_.sourceMap.Map(outputLine_, {fileId, line}); }

    void Report(DiagnosticSeverity severity, std::string code, const FileContext& file, std::uint32_t line, std::string message);

    const PreprocessOptions& options_;
    PreprocessCache::Impl* cache_;
    PreprocessResult& result_;
    CheckBudget& budget_;
    std::size_t generatedBytes_ = 0, providedBytes_ = 0, nesting_ = 0, macroNesting_ = 0;
    std::vector<std::shared_ptr<const Lexed>> alive_;
    std::size_t sourceStorageBytes_=0; // 本次运行额外逻辑原文/续接表累计预算；重复缓存引用只计一次。
    std::deque<std::string> arena_;
    std::unordered_map<std::string_view, Macro> macros_;
    std::unordered_map<std::string_view, std::uint32_t> macroIds_;
    std::deque<std::string> macroNames_;
    HideSets hideSets_;
    std::vector<Conditional> conditionals_;
    std::set<std::filesystem::path> pragmaOnce_;
    std::map<std::filesystem::path, std::string> includeContents_;
    bool includeSearchFailed_ = false;
    bool stopped_ = false;
    std::size_t macroAuditBytes_ = 0;
    std::uint32_t outputLine_ = 1;
    bool lastWasExpansion_ = false;
    std::uint32_t currentLine_ = 0;
    const FileContext* currentFile_ = nullptr; // 嵌套展开（实参展开）的诊断与 __FILE__ 归属
    std::size_t errors_ = 0;
};

} // namespace

// ---- 缓存 ----

struct PreprocessCache::Impl {
    std::unordered_map<std::uint64_t, std::vector<std::shared_ptr<const Lexed>>> entries;
};

PreprocessCache::PreprocessCache() : impl_(std::make_unique<Impl>()) {}
PreprocessCache::~PreprocessCache() = default;

std::size_t PreprocessCache::Entries() const noexcept {
    std::size_t count = 0;
    for (const auto& [hash, list] : impl_->entries) count += list.size();
    return count;
}

void PreprocessCache::Clear() { impl_->entries.clear(); }

bool PreprocessResult::Succeeded() const noexcept {
    return completed && std::none_of(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& diagnostic) { return diagnostic.severity == DiagnosticSeverity::Error; });
}

static Status ValidateSourceLexicalViewImpl(std::string_view original, const SourceLexicalView& view, CheckBudget* budget) {
    const auto fail = [](std::string message, const SourceOpaqueSpan* span = nullptr) {
        auto result = Status::Failure("Jass.Preprocess", "PP_LEXICAL_VIEW_INVALID", std::move(message));
        if (span) result.diagnostics.back().origin = span->origin;
        return result;
    };
    if (original.size() != view.text.size() || original.size() > 16u * 1024u * 1024u || view.opaque.size() > 262144)
        return fail("原字节与词法视图长度或跨度预算不一致。");
    auto lex = budget ? detail::LexPpBounded(view.text, *budget, Text::SourceEncoding::Utf8)
        : detail::LexPp(view.text, Text::SourceEncoding::Utf8);
    if (!lex.Succeeded()) return {std::move(lex.diagnostics)};
    if (!view.opaque.empty()) {
        const auto raw = budget ? detail::LexPpBounded(original, *budget, Text::SourceEncoding::Utf8)
            : detail::LexPp(original, Text::SourceEncoding::Utf8);
        if (!raw.Succeeded()) return {raw.diagnostics};
        if (raw.value->size() != lex.value->size()) return fail("词法视图改变了原字节记号边界。");
        for (std::size_t i = 0; i < raw.value->size(); ++i) {
            const auto& a = (*raw.value)[i]; const auto& b = (*lex.value)[i];
            if (a.offset != b.offset || a.End() != b.End() || a.kind != b.kind)
                return fail("词法视图改变了原字节转义或记号边界。");
        }
    }
    std::size_t cursor = 0, tokenIndex = 0;
    for (const auto& span : view.opaque) {
        if (budget) budget->Poll();
        if (span.offset < cursor || span.offset > original.size() || span.length > original.size() - span.offset ||
            span.sourceSha256.size() != 64 || !std::all_of(span.sourceSha256.begin(), span.sourceSha256.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }) || span.origin.fingerprint != span.sourceSha256 || span.origin.source.empty() || span.origin.length != span.length ||
            span.origin.offset > 16u * 1024u * 1024u)
            return fail("不透明跨度未有界、未排序或缺少原来源身份。", &span);
        if (original.substr(cursor, span.offset - cursor) != std::string_view(view.text).substr(cursor, span.offset - cursor))
            return fail("词法视图改变了未获资格的原字节。", &span);
        const auto scalar = Text::ReadSourceScalar(original, span.offset, Text::SourceEncoding::Gbk);
        if (!scalar.Succeeded() || scalar.value->bytes != span.length || static_cast<unsigned char>(original[span.offset]) < 0x80 ||
            !std::all_of(view.text.begin() + span.offset, view.text.begin() + span.offset + span.length, [](char c) { return c == 'x'; }))
            return fail("屏蔽跨度不是精确CP936标量，或等长视图含未知替换。", &span);
        while (tokenIndex < lex.value->size() && (*lex.value)[tokenIndex].End() <= span.offset) ++tokenIndex;
        auto kind = SourceOpaqueKind::Comment;
        if (tokenIndex < lex.value->size() && (*lex.value)[tokenIndex].offset <= span.offset) {
            const auto& token = (*lex.value)[tokenIndex];
            if (token.kind == detail::PpKind::String) kind = SourceOpaqueKind::String;
            else if (token.kind == detail::PpKind::Char) kind = SourceOpaqueKind::Rawcode;
            else return fail("词法视图不得隐藏非ASCII代码token或标识符。", &span);
        }
        if (kind != span.kind) return fail("不透明跨度的字符串、注释或rawcode类别不匹配。", &span);
        cursor = span.offset + span.length;
    }
    if (original.substr(cursor) != std::string_view(view.text).substr(cursor)) return fail("词法视图改变了未获资格的尾部原字节。");
    return Status::Ok();
}

Status ValidateSourceLexicalView(std::string_view original, const SourceLexicalView& view) {
    return ValidateSourceLexicalViewImpl(original, view, nullptr);
}

namespace {

std::shared_ptr<const Lexed> Engine::Lex(std::string text, bool cacheable,
    const std::filesystem::path& source, const SourceLexicalView* explicitView) {
    budget_.RegisterSource(text.size());
    SourceLexicalView qualified;
    const bool qualifiedMode = static_cast<bool>(options_.lexicalViewProvider);
    if (qualifiedMode) {
        if (explicitView) qualified = *explicitView;
        else {
            auto view = options_.lexicalViewProvider(text, source);
            if (!view.Succeeded()) {
                for (auto& diagnostic : view.diagnostics) AddDiagnostic(std::move(diagnostic));
                stopped_ = true; ++errors_;
                auto empty = std::make_shared<Lexed>(); empty->tokens.emplace_back(); alive_.push_back(empty); return empty;
            }
            qualified = std::move(*view.value);
        }
        const auto valid = ValidateSourceLexicalViewImpl(text, qualified, &budget_);
        if (!valid.Succeeded()) {
            for (auto diagnostic : valid.diagnostics) AddDiagnostic(std::move(diagnostic));
            stopped_ = true; ++errors_;
            auto empty = std::make_shared<Lexed>(); empty->tokens.emplace_back(); alive_.push_back(empty); return empty;
        }
    }
    const auto hash = ContentHash(text, budget_);
    const auto storage=options_.maxOutputBytes ? (std::min)(options_.maxOutputBytes,std::size_t{64u*1024u*1024u}) : 64u*1024u*1024u;
    if (cacheable && cache_ != nullptr) {
        for (const auto& entry : cache_->entries[hash]) {
            budget_.Poll();
            if (entry->text == text && entry->physicalView.text == qualified.text && entry->physicalView.opaque == qualified.opaque) {
                budget_.ConsumeTokens(entry->tokens.size()); // cache 只免分词，不免本次重复来源/执行预算。
                if (std::find(alive_.begin(),alive_.end(),entry)==alive_.end()) {
                    if (entry->SpliceStorage()>storage-sourceStorageBytes_) {
                        AddDiagnostic({DiagnosticSeverity::Error,"Jass.Preprocess","PP_SOURCE_STORAGE_LIMIT",
                            "已缓存的行续接原文和位置表超过本次运行累计预算。",std::nullopt});
                        stopped_=true; ++errors_;
                    } else sourceStorageBytes_+=entry->SpliceStorage();
                }
                ++result_.cacheHits;
                alive_.push_back(entry);
                return entry;
            }
        }
    }
    auto lexed = std::make_shared<Lexed>();
    lexed->text = std::move(text);
    lexed->physicalView = std::move(qualified);
    const auto lexical = qualifiedMode ? std::string_view(lexed->physicalView.text) : std::string_view(lexed->text);
    auto spliced=detail::SplicePpLines(lexical,storage-sourceStorageBytes_, {}, &budget_);
    if (!spliced.Succeeded()) {
        for (auto& diagnostic:spliced.diagnostics) AddDiagnostic(std::move(diagnostic));
        stopped_=true; ++errors_;
        lexed->tokens.emplace_back();
    } else {
        lexed->logical=std::move(*spliced.value);
        if (qualifiedMode && !lexed->logical.splices.empty()) {
            lexed->logicalView.text = lexed->logical.text;
            std::string rawLogical;
            std::uint32_t begin = 0;
            for (const auto& splice : lexed->logical.splices) {
                budget_.Poll();
                rawLogical.append(lexed->text.substr(begin, splice.physicalOffset - begin));
                begin = splice.physicalOffset + splice.length;
            }
            rawLogical.append(lexed->text.substr(begin)); lexed->logical.text = std::move(rawLogical);
            for (auto span : lexed->physicalView.opaque) {
                budget_.Poll();
                std::uint32_t removed = 0;
                for (const auto& splice : lexed->logical.splices) {
                    budget_.Poll();
                    if (splice.physicalOffset >= span.offset) break;
                    removed += splice.length;
                }
                span.offset -= removed; lexed->logicalView.opaque.push_back(std::move(span));
            }
        }
        if (lexed->SpliceStorage() > storage-sourceStorageBytes_) {
            AddDiagnostic({DiagnosticSeverity::Error,"Jass.Preprocess","PP_SOURCE_STORAGE_LIMIT",
                "原字节词法视图与续接位置累计超过有界存储预算。",OriginSpan{source}});
            stopped_=true; ++errors_; lexed->tokens.emplace_back(); alive_.push_back(lexed); return lexed;
        }
        sourceStorageBytes_+=lexed->SpliceStorage();
        const auto raw = lexed->logical.splices.empty() ? std::string_view(lexed->text) : std::string_view(lexed->logical.text);
        const auto view = lexed->logical.splices.empty() ? lexical : qualifiedMode ? std::string_view(lexed->logicalView.text) : raw;
        auto tokens = detail::LexPpBounded(view, budget_);
        if (!tokens.Succeeded()) budget_.RejectLexical(std::move(tokens.diagnostics));
        lexed->tokens = std::move(*tokens.value);
        if (qualifiedMode) for (auto& token : lexed->tokens) token.text = raw.substr(token.offset, token.text.size());
        lexed->logical.MapOrigins(lexed->tokens, &budget_);
        if (cacheable && cache_ != nullptr) cache_->entries[hash].push_back(lexed);
    }
    alive_.push_back(lexed);
    return lexed;
}

SourceLexicalView Engine::ViewOf(std::string_view text) const {
    SourceLexicalView result; result.text = text;
    if (!options_.lexicalViewProvider || text.empty()) return result;
    const auto address = reinterpret_cast<std::uintptr_t>(text.data());
    for (const auto& lexed : alive_) {
        budget_.Poll();
        const auto find = [&](const std::string& raw, const SourceLexicalView& view) {
            const auto begin = reinterpret_cast<std::uintptr_t>(raw.data());
            if (address < begin || address - begin > raw.size() || text.size() > raw.size() - (address - begin) || view.text.size() != raw.size()) return false;
            const auto offset = static_cast<std::uint32_t>(address - begin);
            result.text = view.text.substr(offset, text.size());
            auto found = std::lower_bound(view.opaque.begin(), view.opaque.end(), offset,
                [](const SourceOpaqueSpan& span, std::uint32_t value) { return span.offset < value; });
            for (; found != view.opaque.end() && found->offset + found->length <= offset + text.size(); ++found) {
                budget_.Poll();
                auto span = *found; span.offset -= offset; result.opaque.push_back(std::move(span));
            }
            return true;
        };
        if (find(lexed->text, lexed->physicalView) || find(lexed->logical.text, lexed->logicalView)) return result;
    }
    return result;
}

void Engine::Report(DiagnosticSeverity severity, std::string code, const FileContext& file, std::uint32_t line, std::string message) {
    budget_.Poll();
    if (severity == DiagnosticSeverity::Error && ++errors_ > kMaxErrors) return;
    auto text = budget_.Message(DiagnosticText(file.name) + ":" + std::to_string(line) + ": " + budget_.Message(message));
    AddDiagnostic({severity, std::string(kSubsystem), std::move(code), std::move(text), std::nullopt});
}

std::string_view Engine::Store(std::string text) {
    Generated(text.size());
    arena_.push_back(std::move(text));
    return arena_.back();
}

std::uint32_t Engine::MacroId(std::string_view name) {
    budget_.Poll();
    if (name.size() > 4096) budget_.Fail(CheckFailure::SourceBytes);
    if (const auto found = macroIds_.find(name); found != macroIds_.end()) return found->second;
    budget_.ConsumeAst();
    Generated(name.size());
    macroNames_.emplace_back(name);
    const auto id = static_cast<std::uint32_t>(macroNames_.size());
    macroIds_.emplace(macroNames_.back(), id);
    return id;
}

const Macro* Engine::FindMacro(const PpToken& token) const {
    if (token.kind != PpKind::Identifier) return nullptr;
    const auto found = macros_.find(token.text);
    if (found == macros_.end()) return nullptr;
    if (token.hide != 0 && hideSets_.Contains(token.hide, found->second.id)) return nullptr;
    return &found->second;
}

// ---- 输出 ----

bool Engine::CheckOutputBudget(std::size_t used, std::size_t added) {
    budget_.Poll();
    if (stopped_) return false;
    const auto limit = options_.maxOutputBytes == 0 ? options_.check.maxTotalBytes
        : (std::min)(options_.maxOutputBytes, options_.check.maxTotalBytes);
    if (used <= limit && added <= limit - used) return true;
    stopped_ = true;
    ++errors_;
    result_.completed = false;
    AddDiagnostic({DiagnosticSeverity::Error, std::string(kSubsystem), "PP_OUTPUT_BUDGET",
        (currentFile_ != nullptr ? currentFile_->name + ":" + std::to_string(currentLine_) + ": " : "") +
            "预处理输出或单次宏展开超过 " + std::to_string(limit) + " 字节预算", std::nullopt});
    return false;
}

// 输出统一使用 \n 或 \r\n 换行：单独的 \r（旧式 Mac 换行）改为 \r\n，后续阶段与行号映射按 \n 计行。
void Engine::Append(std::string_view text) {
    if (!CheckOutputBudget(result_.text.size(), text.size())) return;
    {
        std::size_t normalizedSize = text.size();
        for (std::size_t i = 0; i < text.size(); ++i) {
            budget_.Poll();
            if (text[i] == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n')) {
                if (!CheckOutputBudget(result_.text.size(), normalizedSize + 1)) return;
                ++normalizedSize;
            }
        }
    }
    if (options_.lexicalViewProvider) {
        const auto view = ViewOf(text);
        const auto base = static_cast<std::uint32_t>(result_.text.size());
        for (auto span : view.opaque) {
            budget_.Poll();
            std::uint32_t inserted = 0;
            for (std::size_t i = 0; i < span.offset; ++i) if (text[i] == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n')) ++inserted;
            span.offset += base + inserted; result_.lexicalView.opaque.push_back(std::move(span));
        }
        for (std::size_t i = 0; i < text.size(); ++i) {
            budget_.Poll();
            result_.lexicalView.text += view.text[i];
            if (text[i] == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n')) result_.lexicalView.text += '\n';
        }
    }
    if (text.find('\r') == std::string_view::npos) {
        result_.text.append(text);
        outputLine_ += static_cast<std::uint32_t>(std::count(text.begin(), text.end(), '\n'));
        return;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        budget_.Poll();
        result_.text += text[i];
        if (text[i] == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n')) result_.text += '\n';
        if (result_.text.back() == '\n') ++outputLine_;
    }
}

void Engine::Flush(const FileContext& file, std::uint32_t from, std::uint32_t to) {
    if (stopped_ || from >= to) return;
    const std::string_view text = file.lexed->logical.Slice(file.lexed->text,from,to);
    if (text.empty()) return;
    if (lastWasExpansion_ && !result_.text.empty() && detail::WouldMerge(result_.text.back(), text.front())) Append(" ");
    Append(text);
    lastWasExpansion_ = false;
}

void Engine::Emit(const PpToken& token) {
    if (stopped_ || token.text.empty()) return;
    if (!result_.text.empty()) {
        const char last = result_.text.back();
        const bool blank = last == ' ' || last == '\t' || last == '\n';
        if (!blank && (token.space || detail::WouldMerge(last, token.text.front()))) Append(" ");
    }
    Append(token.text);
    lastWasExpansion_ = true;
}

void Engine::EnsureNewline() {
    if (!result_.text.empty() && result_.text.back() != '\n') Append("\r\n");
    lastWasExpansion_ = false;
}

// ---- 文件 ----

std::optional<std::filesystem::path> Engine::FindInclude(const std::string& name, bool angled,
    const std::filesystem::path& currentDirectory, const FileContext& includer, std::uint32_t line,
    std::uint32_t offset, std::uint32_t length, bool forced) {
    budget_.Poll();
    if (name.size() > 4096) budget_.Fail(CheckFailure::SourceBytes);
    includeSearchFailed_ = false;
    const std::filesystem::path relative(std::u8string(name.begin(), name.end()));
    std::error_code error;
    std::uint32_t ordinal = 0;
    std::optional<std::filesystem::path> skipped;
    const auto readProvided = [&](const std::filesystem::path& path) -> std::optional<std::filesystem::path> {
        const auto candidate = path.lexically_normal();
        try {
            budget_.Poll();
            auto provided = options_.includeProvider(candidate);
            budget_.Poll();
            const bool succeeded = provided.Succeeded();
            bool hasError = false;
            for (auto& diagnostic : provided.diagnostics) {
                budget_.Poll();
                if (diagnostic.severity == DiagnosticSeverity::Error) {
                    hasError = true;
                    ++errors_;
                    diagnostic.message += " [include请求 " + DiagnosticText(includer.name) + ":" + std::to_string(line) +
                        " → " + DiagnosticText(PathText(candidate)) + "]";
                }
                AddDiagnostic(std::move(diagnostic));
            }
            if (!succeeded) {
                includeSearchFailed_ = true;
                if (!hasError) Report(DiagnosticSeverity::Error, "PP_INCLUDE_PROVIDER_FAILED", includer, line,
                    "包含提供器未返回有效结果：" + PathText(candidate));
                return std::nullopt;
            }
            if (!provided.value->exists) return std::nullopt;
            const auto bytes = provided.value->text.size();
            if (bytes > options_.check.maxSourceBytes) budget_.Fail(CheckFailure::SourceBytes);
            if (providedBytes_ > options_.check.maxTotalBytes || bytes > options_.check.maxTotalBytes - providedBytes_)
                budget_.Fail(CheckFailure::TotalBytes);
            providedBytes_ += bytes;
            includeContents_.insert_or_assign(candidate, std::move(provided.value->text));
            return candidate;
        } catch (const detail::CheckAborted&) {
            throw;
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& exception) {
            includeSearchFailed_ = true;
            Report(DiagnosticSeverity::Error, "PP_INCLUDE_PROVIDER_EXCEPTION", includer, line,
                "包含提供器异常：" + PathText(candidate) + "：" + exception.what());
        } catch (...) {
            includeSearchFailed_ = true;
            Report(DiagnosticSeverity::Error, "PP_INCLUDE_PROVIDER_EXCEPTION", includer, line,
                "包含提供器发生未知异常：" + PathText(candidate));
        }
        return std::nullopt;
    };
    const auto probe = [&](const std::filesystem::path& directory, IncludeSearchRole role) -> std::optional<std::filesystem::path> {
        const auto originalCandidate = directory / relative;
        const auto candidate = originalCandidate.lexically_normal();
        IncludeCandidateRequest request;
        request.requester = includer.canonical.empty() ? includer.directory / includer.name : includer.canonical;
        request.requesterStageText = includer.stageText;
        request.searchDirectory = directory; request.candidate = candidate; request.spelling = name;
        request.requesterLine = line; request.requesterOffset = offset; request.requesterLength = length;
        request.searchOrdinal = ordinal++; request.searchRole = role; request.angled = angled; request.forced = forced;
        budget_.Poll();
        auto eligibility = CheckIncludeCandidate(options_, request);
        budget_.Poll();
        if (!eligibility.Succeeded()) {
            includeSearchFailed_ = true;
            for (auto& diagnostic : eligibility.diagnostics) {
                budget_.Poll();
                if (diagnostic.severity == DiagnosticSeverity::Error) ++errors_;
                diagnostic.message += " [include请求 " + DiagnosticText(includer.name) + ":" + std::to_string(line) +
                    " → " + DiagnosticText(PathText(candidate)) + "]";
                AddDiagnostic(std::move(diagnostic));
            }
            return std::nullopt;
        }
        if (*eligibility.value == IncludeCandidateEligibility::SkippedOutOfScope) {
            if (!skipped) skipped = candidate;
            return std::nullopt;
        }
        if (options_.includeProvider) return readProvided(candidate);
        if (std::filesystem::is_regular_file(options_.includeCandidatePolicy ? candidate : originalCandidate, error)) return candidate;
        return std::nullopt;
    };
    if (relative.is_absolute() && !options_.includeCandidatePolicy) {
        if (options_.includeProvider) return readProvided(relative);
        if (std::filesystem::is_regular_file(relative, error)) return relative;
        return std::nullopt;
    }
    if (relative.is_absolute()) {
        if (auto found = probe({}, IncludeSearchRole::Absolute)) return found;
    } else if (!angled) {
        if (!currentDirectory.empty()) {
            if (auto found = probe(currentDirectory, IncludeSearchRole::CurrentFile)) return found;
            if (includeSearchFailed_) return std::nullopt;
        }
        for (const auto& directory : options_.userIncludes) {
            budget_.Poll();
            if (auto found = probe(directory, IncludeSearchRole::User)) return found;
            if (includeSearchFailed_) return std::nullopt;
        }
    }
    if (!relative.is_absolute()) {
        for (const auto& directory : options_.systemIncludes) {
            budget_.Poll();
            if (auto found = probe(directory, IncludeSearchRole::System)) return found;
            if (includeSearchFailed_) return std::nullopt;
        }
    }
    if (!includeSearchFailed_ && skipped) {
        includeSearchFailed_ = true;
        Report(DiagnosticSeverity::Error, "SCRIPT_INCLUDE_PATH_SCOPE", includer, line,
            "include请求 " + includer.name + ":" + std::to_string(line) +
            " 未选中来源；范围外候选未读取：" + PathText(*skipped));
        result_.diagnostics.back().origin = OriginSpan{*skipped};
    }
    return std::nullopt;
}

void Engine::IncludeFile(const std::filesystem::path& path, const FileContext& includer, std::uint32_t line) {
    if (stopped_) return;
    std::error_code error;
    auto canonical = options_.includeProvider ? path.lexically_normal() : std::filesystem::weakly_canonical(path, error);
    if (error) canonical = path;
    if (pragmaOnce_.contains(canonical)) return;
    const auto recursion = (std::min)(options_.check.maxNesting, std::size_t{128});
    const auto depthLimit = (std::min)(static_cast<std::size_t>(options_.maxIncludeDepth), recursion > 0 ? recursion - 1 : 0);
    if (includer.depth + 1 > depthLimit) {
        Report(DiagnosticSeverity::Error, "PP_INCLUDE_DEPTH", includer, line, "包含层数超过 " + std::to_string(depthLimit) + "（显式 include/硬递归预算中较小值，可能循环包含）");
        stopped_ = true;
        result_.completed = false;
        return;
    }
    std::optional<std::string> diskText;
    const std::string* text = nullptr;
    if (options_.includeProvider) {
        const auto found = includeContents_.find(path.lexically_normal());
        if (found != includeContents_.end()) text = &found->second;
    } else {
        diskText = ReadFile(path, budget_, options_.check.maxSourceBytes);
        if (diskText) text = &*diskText;
    }
    if (text == nullptr) {
        Report(DiagnosticSeverity::Error, "PP_INCLUDE_UNREADABLE", includer, line, "无法读取包含文件：" + PathText(path));
        return;
    }
    const auto lexed = Lex(std::string(StripBom(*text)), true, path);
    ++result_.filesIncluded;
    FileContext file;
    file.lexed = lexed.get();
    file.stageText = lexed->text;
    file.name = PathText(path);
    file.fileId = result_.sourceMap.AddFile(file.name);
    file.directory = path.parent_path();
    file.depth = includer.depth + 1;
    file.canonical = canonical;
    EnsureNewline();
    if (stopped_) return;
    ProcessFile(file);
    EnsureNewline();
}

void Engine::ProcessFile(const FileContext& file) {
    NestingGuard guard(&budget_, nesting_);
    const FileContext* outer = currentFile_;
    currentFile_ = &file;
    const auto& tokens = file.lexed->tokens;
    const std::size_t depth = conditionals_.size();
    std::size_t i = 0;
    while (!stopped_ && tokens[i].kind != PpKind::End) {
        budget_.Poll();
        currentLine_ = tokens[i].line;
        if (tokens[i].lineStart && tokens[i].Is("#")) {
            std::size_t end = i + 1;
            while (tokens[end].kind != PpKind::Newline && tokens[end].kind != PpKind::End) { budget_.PollOccasionally(end); ++end; }
            Directive(file, i, end);
            i = tokens[end].kind == PpKind::Newline ? end + 1 : end;
            continue;
        }
        if (!Active()) {
            while (tokens[i].kind != PpKind::Newline && tokens[i].kind != PpKind::End) { budget_.PollOccasionally(i); ++i; }
            if (tokens[i].kind == PpKind::Newline) ++i;
            continue;
        }
        TextLine(file, i);
    }
    if (conditionals_.size() > depth) {
        if (!stopped_) Report(DiagnosticSeverity::Error, "PP_UNTERMINATED_CONDITIONAL", file, conditionals_.back().line, "条件编译块缺少 #endif");
        conditionals_.resize(depth);
    }
    currentFile_ = outer;
}

// 一行正文：未展开的部分原样复制，遇到宏调用时展开（函数宏的参数可以跨行）。
void Engine::TextLine(const FileContext& file, std::size_t& i) {
    const auto& tokens = file.lexed->tokens;
    std::uint32_t copyFrom = i == 0 ? 0 : tokens[i - 1].End();
    Sync(file.fileId, tokens[i].line);
    lastWasExpansion_ = false;
    Stream stream;
    stream.source = &tokens;
    stream.cursor = &i;
    while (!stopped_) {
        budget_.Poll();
        if (stream.pending.empty()) {
            const PpToken& token = tokens[i];
            if (token.kind == PpKind::Newline) {
                Flush(file, copyFrom, token.End());
                ++i;
                return;
            }
            if (token.kind == PpKind::End) {
                Flush(file, copyFrom, token.offset);
                EnsureNewline();
                return;
            }
            if (FindMacro(token) != nullptr) {
                const PpToken name = token;
                const std::uint32_t before = copyFrom;
                ++i;
                std::uint32_t sourceEnd = name.End();
                currentLine_ = name.line;
                if (TryExpand(name, stream, file, &sourceEnd)) {
                    Flush(file, before, name.offset);
                    copyFrom = sourceEnd;
                    lastWasExpansion_ = true;
                } else {
                    copyFrom = before;
                }
                continue;
            }
            ++i;
            continue;
        }
        PpToken token = stream.pending.front();
        stream.pending.pop_front();
        stream.pendingBytes -= token.text.size();
        std::uint32_t sourceEnd = 0;
        if (FindMacro(token) != nullptr && TryExpand(token, stream, file, &sourceEnd)) {
            if (sourceEnd != 0) copyFrom = sourceEnd;
            continue;
        }
        Emit(token);
    }
}

// ---- 宏展开 ----

bool Engine::CollectArguments(const Macro& macro, const PpToken& name, Stream& stream, std::vector<TokenList>& arguments,
    PpToken& closing, const FileContext& file, std::uint32_t* sourceEnd) {
    PpToken token;
    stream.Take(token); // "("
    if (token.source && sourceEnd != nullptr) *sourceEnd = token.End();
    budget_.ConsumeAst();
    arguments.emplace_back();
    int depth = 0;
    std::size_t argumentBytes = 0;
    const std::size_t named = macro.variadic ? macro.params.size() - 1 : macro.params.size();
    while (true) {
        budget_.Poll();
        if (!stream.Take(token)) {
            Report(DiagnosticSeverity::Error, "PP_UNTERMINATED_ARGUMENTS", file, name.line ? name.line : currentLine_,
                "宏 " + DiagnosticText(name.text) + " 的参数列表没有结束");
            return false;
        }
        if (token.source && sourceEnd != nullptr) *sourceEnd = token.End();
        if (token.Is("(")) {
            ++depth;
        } else if (token.Is(")")) {
            if (depth == 0) {
                closing = token;
                break;
            }
            --depth;
        } else if (token.Is(",") && depth == 0 && !(macro.variadic && arguments.size() > named)) {
            budget_.ConsumeAst();
            arguments.emplace_back();
            continue;
        }
        if (arguments.back().empty()) token.space = false;
        if (!CheckOutputBudget(argumentBytes, token.text.size())) return false;
        argumentBytes += token.text.size();
        Push(arguments.back(), token);
    }
    // 参数个数：无参宏只接受 ()；可变参数宏可省略可变部分。
    if (macro.params.empty()) {
        if (arguments.size() == 1 && arguments[0].empty()) {
            arguments.clear();
            return true;
        }
    } else if (macro.variadic && arguments.size() == named) {
        budget_.ConsumeAst();
        arguments.emplace_back();
        return true;
    } else if (arguments.size() == macro.params.size()) {
        return true;
    }
    Report(DiagnosticSeverity::Error, "PP_MACRO_ARGUMENTS", file, name.line ? name.line : currentLine_,
        "宏 " + DiagnosticText(name.text) + " 需要 " + std::to_string(macro.params.size()) + " 个参数，实际 " +
            std::to_string(arguments.size()) + " 个");
    return false;
}

PpToken Engine::Stringize(const TokenList& argument, bool space) {
    std::string text;
    const auto append = [&](std::string_view part) {
        if (CheckOutputBudget(text.size(), part.size())) text.append(part);
    };
    append("\"");
    for (std::size_t i = 0; !stopped_ && i < argument.size(); ++i) {
        budget_.Poll();
        const auto& token = argument[i];
        const auto opaque = ViewOf(token.text).opaque;
        if (!opaque.empty()) {
            AddDiagnostic({DiagnosticSeverity::Error, "Jass.Preprocess", "PP_OPAQUE_STRINGIZE_UNSUPPORTED",
                PathText(opaque.front().origin.source) + ":" + std::to_string(opaque.front().sourceLine) +
                    ": 跨编码不透明字面值的宏字符串化尚未支持；原字节未改写。", opaque.front().origin});
            stopped_ = true; ++errors_; return {};
        }
        if (i > 0 && token.space) append(" ");
        if (token.kind == PpKind::String || token.kind == PpKind::Char) {
            for (const char c : token.text) {
                budget_.Poll();
                if (stopped_) break;
                if (c == '"' || c == '\\') append("\\");
                append(std::string_view(&c, 1));
            }
        } else {
            append(token.text);
        }
    }
    append("\"");
    if (stopped_) return {};
    PpToken token;
    token.kind = PpKind::String;
    token.text = Store(std::move(text));
    token.space = space;
    return token;
}

// a ## b：拼写相接后恰为一个记号则合并；否则保留为两个记号（Wave 对 “名称##(” 也不报错）。
void Engine::Paste(TokenList& out, const PpToken& right) {
    if (stopped_) return;
    if (out.empty()) {
        Push(out, right);
        return;
    }
    PpToken& left = out.back();
    auto opaque = ViewOf(left.text).opaque;
    if (opaque.empty()) opaque = ViewOf(right.text).opaque;
    if (!opaque.empty()) {
        AddDiagnostic({DiagnosticSeverity::Error, "Jass.Preprocess", "PP_OPAQUE_PASTE_UNSUPPORTED",
            PathText(opaque.front().origin.source) + ":" + std::to_string(opaque.front().sourceLine) +
                ": 跨编码不透明字面值的宏粘贴尚未支持；原字节未改写。", opaque.front().origin});
        stopped_ = true; ++errors_; return;
    }
    if (!CheckOutputBudget(left.text.size(), right.text.size())) return;
    const std::string joined = std::string(left.text) + std::string(right.text);
    PpKind kind{};
    if (detail::LexSingle(joined, kind, &budget_)) {
        left.text = Store(joined);
        left.kind = kind;
        left.source = false;
        return;
    }
    PpToken copy = right;
    copy.space = false;
    Push(out, copy);
}

TokenList Engine::Substitute(const Macro& macro, std::vector<TokenList>& arguments) {
    budget_.Poll();
    CopyTokens(arguments.size());
    const auto parameter = [&](const PpToken& token) -> int {
        if (token.kind != PpKind::Identifier) return -1;
        for (std::size_t i = 0; i < macro.params.size(); ++i) {
            budget_.Poll();
            if (macro.params[i] == token.text) return static_cast<int>(i);
        }
        return -1;
    };
    std::vector<std::optional<TokenList>> expanded(arguments.size());
    const auto& body = macro.body;
    TokenList out;
    std::size_t outputBytes = 0;
    const auto push = [&](PpToken token) {
        if (!CheckOutputBudget(outputBytes, token.text.size())) return;
        outputBytes += token.text.size();
        Push(out, token);
    };
    const auto paste = [&](const PpToken& token) {
        if (!CheckOutputBudget(outputBytes, token.text.size())) return;
        outputBytes += token.text.size();
        Paste(out, token);
    };
    for (std::size_t i = 0; !stopped_ && i < body.size(); ++i) {
        budget_.Poll();
        const auto& token = body[i];
        // # 参数：字符串化原始实参。
        if (token.Is("#") && i + 1 < body.size()) {
            if (const int index = parameter(body[i + 1]); index >= 0) {
                push(Stringize(arguments[static_cast<std::size_t>(index)], token.space));
                ++i;
                continue;
            }
        }
        // ## 右操作数：用原始实参（不展开）；空实参时左操作数保持不变。
        if (token.Is("##") && i + 1 < body.size()) {
            const auto& right = body[i + 1];
            if (const int index = parameter(right); index >= 0) {
                const auto& argument = arguments[static_cast<std::size_t>(index)];
                if (!argument.empty()) {
                    paste(argument.front());
                    for (std::size_t k = 1; !stopped_ && k < argument.size(); ++k) push(argument[k]);
                }
            } else {
                paste(right);
            }
            ++i;
            continue;
        }
        const int index = parameter(token);
        if (index >= 0) {
            auto& argument = arguments[static_cast<std::size_t>(index)];
            // 参数后跟 ##：用原始实参；空实参且右侧也是空参数时什么都不产生。
            if (i + 1 < body.size() && body[i + 1].Is("##")) {
                if (argument.empty()) {
                    const auto& right = i + 2 < body.size() ? body[i + 2] : body[i + 1];
                    if (const int other = parameter(right); other >= 0 && i + 2 < body.size()) {
                        const auto& rightArgument = arguments[static_cast<std::size_t>(other)];
                        for (std::size_t k = 0; !stopped_ && k < rightArgument.size(); ++k) {
                            budget_.Poll();
                            PpToken copy = rightArgument[k];
                            if (k == 0) copy.space = token.space;
                            push(copy);
                        }
                    } else if (i + 2 < body.size()) {
                        PpToken copy = right;
                        copy.space = token.space;
                        push(copy);
                    }
                    i += 2;
                    continue;
                }
                for (std::size_t k = 0; !stopped_ && k < argument.size(); ++k) {
                    budget_.Poll();
                    PpToken copy = argument[k];
                    if (k == 0) copy.space = token.space;
                    push(copy);
                }
                continue;
            }
            // 普通参数：用完全展开后的实参。
            auto& value = expanded[static_cast<std::size_t>(index)];
            if (!value) value = ExpandIsolated(argument, *currentFile_);
            for (std::size_t k = 0; !stopped_ && k < value->size(); ++k) {
                budget_.Poll();
                PpToken copy = (*value)[k];
                if (k == 0) copy.space = token.space;
                push(copy);
            }
            continue;
        }
        push(token);
    }
    return out;
}

void Engine::AuditMacro(const Macro& macro, std::string_view name, const FileContext& file, std::uint32_t line, bool expanded) {
    if (!options_.captureMacroAudit || macro.builtin != Macro::Builtin::None) return;
    const bool construction = std::any_of(macro.body.begin(), macro.body.end(), [](const auto& t) { return t.Is("#") || t.Is("##"); });
    if (expanded && !construction) return;
    PreprocessResult::MacroAudit record;
    record.name = name; record.source = expanded ? file.name : macro.source;
    record.line = expanded ? line : macro.sourceLine;
    record.expanded = expanded; record.tokenConstruction = construction;
    for (const auto& token : macro.body) { record.body += token.text; record.body += ' '; }
    const auto bytes = record.name.size() + record.body.size() + record.source.size();
    if (result_.macroAudit.size() >= 262144 || bytes > 64 * 1024 * 1024 - macroAuditBytes_) {
        Report(DiagnosticSeverity::Error, "PP_MACRO_AUDIT_BUDGET", file, line, "宏覆盖证据超出预算，未提供部分结果");
        stopped_ = true; return;
    }
    macroAuditBytes_ += bytes; result_.macroAudit.push_back(std::move(record));
}
bool Engine::TryExpand(const PpToken& name, Stream& stream, const FileContext& file, std::uint32_t* sourceEnd) {
    if (stopped_) return false;
    const Macro* macro = FindMacro(name);
    if (macro == nullptr) return false;
    TokenList body;
    std::uint32_t hide = 0;
    if (macro->builtin != Macro::Builtin::None) {
        PpToken token;
        if (macro->builtin == Macro::Builtin::Line) {
            token.kind = PpKind::Number;
            token.text = Store(std::to_string(name.line != 0 ? name.line : currentLine_));
        } else {
            std::string quoted = "\"";
            for (const char c : file.name) {
                budget_.Poll();
                if (c == '"' || c == '\\') quoted += '\\';
                quoted += c;
            }
            token.kind = PpKind::String;
            token.text = Store(quoted + "\"");
        }
        Push(body, token);
        hide = name.hide;
    } else if (!macro->function) {
        {
            std::size_t bytes = 0;
            for (const auto& token : macro->body) {
                budget_.Poll();
                if (!CheckOutputBudget(bytes, token.text.size())) return true;
                bytes += token.text.size();
            }
        }
        CopyTokens(macro->body.size());
        body = macro->body;
        hide = hideSets_.With(name.hide, macro->id);
    } else {
        const PpToken* next = stream.Peek(true);
        if (next == nullptr || !next->Is("(")) return false;
        std::vector<TokenList> arguments;
        PpToken closing;
        if (!CollectArguments(*macro, name, stream, arguments, closing, file, sourceEnd)) return true;
        body = Substitute(*macro, arguments);
        hide = hideSets_.With(hideSets_.Intersect(name.hide, closing.hide), macro->id);
    }
    AuditMacro(*macro, name.text, file, name.line, true);
    if (stopped_) return true;
    std::size_t bodyBytes = 0;
    for (const auto& token : body) {
        budget_.Poll();
        if (!CheckOutputBudget(bodyBytes, token.text.size())) return true;
        bodyBytes += token.text.size();
    }
    if (!CheckOutputBudget(stream.pendingBytes, bodyBytes)) return true;
    ++result_.macroExpansions;
    for (auto& token : body) {
        budget_.Poll();
        token.hide = hideSets_.Union(token.hide, hide);
        token.source = false;
        token.lineStart = false;
    }
    if (!body.empty()) body.front().space = name.source ? false : name.space;
    CopyTokens(body.size());
    stream.pending.insert(stream.pending.begin(), body.begin(), body.end());
    stream.pendingBytes += bodyBytes;
    return true;
}

TokenList Engine::ExpandIsolated(std::span<const PpToken> tokens, const FileContext& file) {
    NestingGuard guard(&budget_, macroNesting_);
    CopyTokens(tokens.size());
    Stream stream;
    for (const auto& token : tokens) {
        budget_.Poll();
        if (!CheckOutputBudget(stream.pendingBytes, token.text.size())) return {};
        stream.pendingBytes += token.text.size();
    }
    CopyTokens(tokens.size());
    stream.pending.assign(tokens.begin(), tokens.end());
    TokenList out;
    std::size_t outputBytes = 0;
    PpToken token;
    while (!stopped_ && stream.Take(token)) {
        budget_.Poll();
        if (FindMacro(token) != nullptr && TryExpand(token, stream, file, nullptr)) continue;
        if (!CheckOutputBudget(outputBytes, token.text.size())) break;
        outputBytes += token.text.size();
        Push(out, token);
    }
    return out;
}

// ---- 指令 ----

void Engine::Directive(const FileContext& file, std::size_t begin, std::size_t end) {
    const auto& tokens = file.lexed->tokens;
    const std::uint32_t line = tokens[begin].line;
    const std::span<const PpToken> rest(tokens.data() + begin + 1, end - begin - 1);
    if (rest.empty()) return; // 空指令
    const std::string_view name = rest.front().text;
    const auto args = rest.subspan(1);
    if (name == "if" || name == "ifdef" || name == "ifndef") {
        Conditional entry;
        entry.parentActive = Active();
        entry.line = line;
        if (entry.parentActive) {
            std::optional<bool> value;
            if (name == "if") {
                value = Condition(file, args, line);
            } else if (args.empty() || args.front().kind != PpKind::Identifier) {
                Report(DiagnosticSeverity::Error, "PP_DIRECTIVE", file, line, "#" + std::string(name) + " 需要宏名");
            } else {
                const bool defined = macros_.contains(args.front().text);
                value = name == "ifdef" ? defined : !defined;
            }
            entry.active = value.value_or(false);
            entry.taken = entry.active;
        }
        Push(conditionals_, entry);
        return;
    }
    if (name == "elif" || name == "else" || name == "endif") {
        if (conditionals_.empty()) {
            Report(DiagnosticSeverity::Error, "PP_UNBALANCED_CONDITIONAL", file, line, "#" + std::string(name) + " 没有对应的 #if");
            return;
        }
        auto& entry = conditionals_.back();
        if (name == "endif") {
            conditionals_.pop_back();
            return;
        }
        if (entry.sawElse) {
            Report(DiagnosticSeverity::Error, "PP_UNBALANCED_CONDITIONAL", file, line, "#" + std::string(name) + " 出现在 #else 之后");
            return;
        }
        if (name == "else") {
            entry.sawElse = true;
            entry.active = entry.parentActive && !entry.taken;
            entry.taken = entry.taken || entry.active;
            return;
        }
        if (!entry.parentActive || entry.taken) {
            entry.active = false;
            return;
        }
        entry.active = Condition(file, args, line).value_or(false);
        entry.taken = entry.active;
        return;
    }
    if (!Active()) return;
    if (name == "define") return Define(file, args, line);
    if (name == "undef") return Undefine(file, args, line);
    if (name == "include") return Include(file, begin + 2, end, line);
    if (name == "error" || name == "warning") {
        std::string message;
        if (!args.empty()) {
            const auto from = args.front().offset;
            message = file.lexed->logical.Slice(file.lexed->text,from,tokens[end].offset);
            while (!message.empty() && (message.back() == '\r' || message.back() == ' ')) message.pop_back();
        }
        Report(name == "error" ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
            name == "error" ? "PP_ERROR_DIRECTIVE" : "PP_WARNING_DIRECTIVE", file, line, "#" + std::string(name) + " " + message);
        return;
    }
    if (name == "pragma") {
        if (!args.empty() && args.front().text == "once") pragmaOnce_.insert(file.canonical);
        return;
    }
    if (name == "line" || name == "ident") return;
    if (rest.front().kind == PpKind::Number) return; // GNU 行标记 “# 12 "file"”
    Report(DiagnosticSeverity::Error, "PP_DIRECTIVE", file, line, "未知的预处理指令 #" + std::string(name));
}

void Engine::Define(const FileContext& file, std::span<const PpToken> tokens, std::uint32_t line) {
    if (tokens.empty() || tokens.front().kind != PpKind::Identifier) {
        Report(DiagnosticSeverity::Error, "PP_DIRECTIVE", file, line, "#define 需要宏名");
        return;
    }
    const std::string_view name = tokens.front().text;
    if (name == "defined") {
        Report(DiagnosticSeverity::Error, "PP_DIRECTIVE", file, line, "不能定义名为 defined 的宏");
        return;
    }
    Macro macro;
    macro.id = MacroId(name);
    macro.origin = DiagnosticText(file.name) + ":" + std::to_string(line);
    macro.source = file.name; macro.sourceLine = line;
    std::size_t at = 1;
    if (at < tokens.size() && tokens[at].Is("(") && !tokens[at].space) {
        macro.function = true;
        ++at;
        bool expectName = true;
        while (at < tokens.size() && !tokens[at].Is(")")) {
            budget_.Poll();
            const auto& token = tokens[at];
            if (token.Is("...")) {
                macro.variadic = true;
                Push(macro.params, std::string_view("__VA_ARGS__"));
            } else if (token.kind == PpKind::Identifier && expectName) {
                if (at + 1 < tokens.size() && tokens[at + 1].Is("...")) {
                    macro.variadic = true; // GNU 具名可变参数 name...
                    ++at;
                }
                Push(macro.params, token.text);
                expectName = false;
                ++at;
                continue;
            } else if (token.Is(",") && !expectName && !macro.variadic) {
                expectName = true;
                ++at;
                continue;
            } else {
                Report(DiagnosticSeverity::Error, "PP_DIRECTIVE", file, line, "宏 " + std::string(name) + " 的参数列表无效");
                return;
            }
            ++at;
        }
        if (at >= tokens.size()) {
            Report(DiagnosticSeverity::Error, "PP_DIRECTIVE", file, line, "宏 " + std::string(name) + " 的参数列表缺少“)”");
            return;
        }
        ++at;
    }
    CopyTokens(tokens.size() - at);
    macro.body.assign(tokens.begin() + static_cast<std::ptrdiff_t>(at), tokens.end());
    for (auto& token : macro.body) {
        budget_.Poll();
        token.lineStart = false;
        token.hide = 0;
    }
    if (!macro.body.empty()) macro.body.front().space = false;
    if (!macro.body.empty() && (macro.body.front().Is("##") || macro.body.back().Is("##"))) {
        Report(DiagnosticSeverity::Error, "PP_DIRECTIVE", file, line, "宏 " + std::string(name) + " 的 ## 不能位于开头或结尾");
        return;
    }
    const std::string_view key = macroNames_[macro.id - 1];
    AuditMacro(macro, name, file, line, false);
    if (stopped_) return;
    if (const auto found = macros_.find(key); found != macros_.end()) {
        if (!SameDefinition(found->second, macro)) {
            Report(DiagnosticSeverity::Warning, "PP_MACRO_REDEFINED", file, line,
                "宏 " + std::string(name) + " 被重新定义（先前定义于 " + found->second.origin + "）");
        }
        found->second = std::move(macro);
        return;
    }
    macros_.emplace(key, std::move(macro));
}

void Engine::Undefine(const FileContext& file, std::span<const PpToken> tokens, std::uint32_t line) {
    if (tokens.empty() || tokens.front().kind != PpKind::Identifier) {
        Report(DiagnosticSeverity::Error, "PP_DIRECTIVE", file, line, "#undef 需要宏名");
        return;
    }
    macros_.erase(tokens.front().text);
}

void Engine::Include(const FileContext& file, std::size_t begin, std::size_t end, std::uint32_t line) {
    const auto& tokens = file.lexed->tokens;
    std::string target;
    bool angled = false;
    const auto raw = [&]() -> std::string_view {
        if (begin >= end) return {};
        const auto from = tokens[begin].offset;
        return file.lexed->logical.Slice(file.lexed->text,from,tokens[end].offset);
    }();
    if (!raw.empty() && (raw.front() == '"' || raw.front() == '<')) {
        const char closing = raw.front() == '"' ? '"' : '>';
        const auto stop = raw.find(closing, 1);
        if (stop == std::string_view::npos) {
            Report(DiagnosticSeverity::Error, "PP_DIRECTIVE", file, line, "#include 的文件名没有结束");
            return;
        }
        target = std::string(raw.substr(1, stop - 1));
        angled = closing == '>';
    } else {
        // 计算得到的包含名（#include BOOST_PP_LOCAL_ITERATE() 等）。
        const auto expanded = ExpandIsolated(TokenList(tokens.begin() + static_cast<std::ptrdiff_t>(begin),
            tokens.begin() + static_cast<std::ptrdiff_t>(end)), file);
        if (stopped_) return;
        if (!expanded.empty() && expanded.front().kind == PpKind::String) {
            target = std::string(expanded.front().text.substr(1, expanded.front().text.size() - 2));
        } else if (!expanded.empty() && expanded.front().Is("<")) {
            std::size_t k = 1;
            for (; k < expanded.size() && !expanded[k].Is(">"); ++k) {
                budget_.Poll();
                if (k > 1 && expanded[k].space) target += ' ';
                target += expanded[k].text;
            }
            if (k == expanded.size()) target.clear();
            angled = true;
        }
        if (target.empty()) {
            Report(DiagnosticSeverity::Error, "PP_DIRECTIVE", file, line, "#include 需要 \"文件\" 或 <文件>");
            return;
        }
    }
    const auto offset = begin < end ? tokens[begin].offset : 0;
    const auto length = begin < end ? tokens[end - 1].End() - offset : 0;
    const auto found = FindInclude(target, angled, file.directory, file, line, offset, length);
    if (!found) {
        if (!includeSearchFailed_) Report(DiagnosticSeverity::Error, "PP_INCLUDE_NOT_FOUND", file, line, "找不到包含文件：" + target);
        return;
    }
    IncludeFile(*found, file, line);
}

std::optional<bool> Engine::Condition(const FileContext& file, std::span<const PpToken> tokens, std::uint32_t line) {
    // 先替换 defined（展开前后各一次：宏展开产生的 defined 也按 GCC 的做法求值）。
    const auto replaceDefined = [this](const TokenList& input) {
        TokenList output;
        for (std::size_t i = 0; i < input.size(); ++i) {
            budget_.Poll();
            if (input[i].kind == PpKind::Identifier && input[i].text == "defined") {
                std::size_t at = i + 1;
                const bool parenthesized = at < input.size() && input[at].Is("(");
                if (parenthesized) ++at;
                if (at < input.size() && input[at].kind == PpKind::Identifier &&
                    (!parenthesized || (at + 1 < input.size() && input[at + 1].Is(")")))) {
                    PpToken value;
                    value.kind = PpKind::Number;
                    value.text = macros_.contains(input[at].text) ? "1" : "0";
                    value.space = input[i].space;
                    Push(output, value);
                    i = parenthesized ? at + 1 : at;
                    continue;
                }
            }
            Push(output, input[i]);
        }
        return output;
    };
    currentLine_ = line;
    auto expanded = ExpandIsolated(replaceDefined(TokenList(tokens.begin(), tokens.end())), file);
    if (stopped_) return std::nullopt;
    expanded = replaceDefined(expanded);
    const auto evaluation = detail::EvaluatePpExpression(expanded, &budget_);
    if (!evaluation.error.empty()) {
        Report(DiagnosticSeverity::Error, "PP_IF_EXPRESSION", file, line, "#if 表达式无效：" + evaluation.error);
        return std::nullopt;
    }
    return evaluation.value.value != 0;
}

void Engine::Run(std::string_view mainText, const std::string& mainName, const std::filesystem::path& mainDirectory) {
    // 预定义宏：与 Wave 的 C99 模式一致，使 Boost.Preprocessor 选择相同配置。
    std::string predefined =
        "#define __WAVE__ 0x0200\n#define __WAVE_HAS_VARIADICS__ 1\n#define __STDC__ 1\n"
        "#define __STDC_VERSION__ 199901L\n#define __STDC_HOSTED__ 0\n";
    for (const auto& [name, value] : options_.defines) {
        budget_.Poll();
        if (name.size() > 4096 || value.size() > options_.check.maxSourceBytes) budget_.Fail(CheckFailure::SourceBytes);
        if (!CheckOutputBudget(predefined.size(), name.size() + value.size() + 16)) return;
        predefined += "#define " + name + " " + (value.empty() ? "1" : value) + "\n";
    }
    FileContext commandLine;
    const auto predefinedLexed = Lex(std::move(predefined));
    commandLine.lexed = predefinedLexed.get();
    commandLine.name = "<预定义>";
    ProcessFile(commandLine);
    if (stopped_) return;
    for (const auto* builtin : {"__LINE__", "__FILE__"}) {
        budget_.Poll();
        Macro macro;
        macro.id = MacroId(builtin);
        macro.builtin = std::string_view(builtin) == "__LINE__" ? Macro::Builtin::Line : Macro::Builtin::File;
        macros_.emplace(macroNames_[macro.id - 1], std::move(macro));
    }
    result_.text.clear();
    result_.lexicalView = {};
    outputLine_ = 1;

    FileContext main;
    main.name = mainName;
    main.directory = mainDirectory;
    main.stageText = StripBom(mainText);
    main.fileId = result_.sourceMap.AddFile(mainName);
    for (const auto& name : options_.forceIncludes) {
        budget_.Poll();
        if (stopped_) break;
        if (const auto found = FindInclude(name, false, mainDirectory, main, 0, 0, 0, true)) {
            IncludeFile(*found, main, 0);
        } else if (!includeSearchFailed_) {
            Report(DiagnosticSeverity::Error, "PP_INCLUDE_NOT_FOUND", main, 0, "找不到强制包含文件：" + name);
        }
    }
    if (stopped_) return;
    const auto lexed = Lex(std::string(StripBom(mainText)), false, mainDirectory / mainName,
        options_.mainLexicalView ? &*options_.mainLexicalView : nullptr);
    main.lexed = lexed.get();
    ProcessFile(main);
    EnsureNewline();
    if (errors_ > kMaxErrors) {
        AddDiagnostic({DiagnosticSeverity::Error, std::string(kSubsystem), "PP_TOO_MANY_ERRORS",
            "错误过多，只报告前 " + std::to_string(kMaxErrors) + " 个", std::nullopt});
    }
}

} // namespace

Result<IncludeCandidateEligibility> CheckIncludeCandidate(
    const PreprocessOptions& options, const IncludeCandidateRequest& request) {
    if (!options.includeCandidatePolicy) return Result<IncludeCandidateEligibility>::Success(IncludeCandidateEligibility::Allowed);
    try {
        auto result = options.includeCandidatePolicy(request);
        if (!result.Succeeded() && std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        })) result.diagnostics.push_back({DiagnosticSeverity::Error, "Jass.Preprocess", "PP_INCLUDE_POLICY_FAILED",
            "包含资格预检未返回有效结果，未读取候选。", OriginSpan{request.requester, request.requesterOffset, request.requesterLength, {}}});
        if (result.Succeeded() && *result.value != IncludeCandidateEligibility::Allowed &&
            *result.value != IncludeCandidateEligibility::SkippedOutOfScope)
            return Result<IncludeCandidateEligibility>::Failure("Jass.Preprocess", "PP_INCLUDE_POLICY_INVALID", "包含资格预检返回未知结果，未读取候选。");
        return result;
    } catch (const detail::CheckAborted&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        return Result<IncludeCandidateEligibility>::Failure("Jass.Preprocess", "PP_INCLUDE_POLICY_EXCEPTION", "包含路径资格预检异常，未读取候选。");
    } catch (...) {
        return Result<IncludeCandidateEligibility>::Failure("Jass.Preprocess", "PP_INCLUDE_POLICY_EXCEPTION", "包含路径资格预检发生未知异常，未读取候选。");
    }
}

PreprocessResult Preprocess(std::string_view mainText, const std::string& mainName, const std::filesystem::path& mainDirectory,
    const PreprocessOptions& options, PreprocessCache* cache) {
    PreprocessResult result;
    CheckBudget budget(options.check);
    try {
        budget.Poll();
        if (mainName.size() > 4096 || mainText.size() > options.check.maxSourceBytes) budget.Fail(CheckFailure::SourceBytes);
        budget.ConsumeAst(options.defines.size() + options.forceIncludes.size() + options.systemIncludes.size() + options.userIncludes.size());
        Engine engine(options, cache != nullptr ? &cache->Get() : nullptr, result, budget);
        engine.Run(mainText, mainName, mainDirectory);
        budget.Poll();
    } catch (const detail::CheckAborted& aborted) {
        budget.MarkFailure(aborted.Cause());
        result.completed = false;
    } catch (const std::bad_alloc&) {
        budget.MarkFailure(CheckFailure::Allocation);
        result.completed = false;
    } catch (...) {
        budget.MarkFailure(CheckFailure::Worker);
        result.completed = false;
    }
    result.tokens = budget.Tokens();
    result.expansionNodes = budget.AstNodes();
    if (!result.completed) {
        result.text.clear();
        result.sourceMap = {};
        result.lexicalView = {};
        if (budget.Failure() != CheckFailure::None) {
            auto lexical = budget.TakeLexicalDiagnostics();
            for (auto& diagnostic : lexical) result.diagnostics.push_back(std::move(diagnostic));
            const auto cause = budget.Failure();
            const std::string code = cause == CheckFailure::Cancelled ? "JASS_CHECK_CANCELLED"
                : cause == CheckFailure::Deadline ? "JASS_CHECK_DEADLINE" : "PP_RESOURCE_BUDGET";
            result.diagnostics.push_back({DiagnosticSeverity::Error, "Jass.Preprocess", code,
                "预处理未完成，硬预算/取消/内部失败类型=" + std::to_string(static_cast<unsigned>(cause)), std::nullopt});
        }
    }
    return result;
}

} // namespace War3Map::Jass
