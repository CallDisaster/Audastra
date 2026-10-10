#include "War3MapCore/Jass/IdentifierRewrite.h"

#include "JassParser.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <map>
#include <new>
#include <set>
#include <utility>

namespace War3Map::Jass {
namespace {

using namespace detail;

struct RewriteFailure { std::vector<Diagnostic> diagnostics; };

bool NonAscii(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x80; });
}

bool Candidate(std::string_view name) { return name.starts_with("udg_") && NonAscii(name); }

void Append32(std::string& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) output.push_back(static_cast<char>((value >> shift) & 0xFF));
}

std::string Hash(std::string_view text) {
    auto result = Sha256Text(text);
    if (!result.Succeeded()) throw RewriteFailure{std::move(result.diagnostics)};
    return std::move(*result.value);
}

class Rewriter {
public:
    explicit Rewriter(const IdentifierRewriteRequest& request) : request_(request) {}

    Result<IdentifierRewriteResult> Run() {
        CheckRequest();
        output_.inputSha256 = Hash(request_.text);
        std::string expected = request_.expectedInputSha256;
        for (char& c : expected) if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        if (expected != output_.inputSha256) Fail("IDENTIFIER_REWRITE_STALE", "阶段文本SHA与冻结输入不一致。", 0, 0);
        output_.ruleVersion = kUdgIdentifierRule;
        auto tokens = Tokens();
        const bool candidate = std::any_of(tokens.begin(), tokens.end(), [](const Token& token) {
            return token.kind == TokenKind::Identifier && Candidate(token.text);
        });
        if (!candidate) {
            if (request_.text.size() > request_.limits.maxOutputBytes) Budget("原文超过输出预算。", 0);
            output_.text = request_.text;
            if (request_.lexicalView) output_.lexicalView = *request_.lexicalView;
            output_.outputSha256 = output_.inputSha256;
            return Finish("未发现中文udg候选；未审定无关中文标识符、旧别名或方言语义。");
        }
        CheckSyntaxBudget(tokens);
        program_ = ParseProgram(std::move(tokens));
        CheckErrors(program_.errors);
        Declare();
        BindGlobals();
        for (const auto& function : program_.functions) BindFunction(function);
        // 只有声明与可达变量表达式能登记替换。函数调用、类型和漏绑定不得靠同名蒙混通过。
        for (const auto& token : program_.tokens) {
            if (token.kind == TokenKind::Identifier && NonAscii(token.text) && !replacements_.contains(token.offset)) {
                Fail("IDENTIFIER_REWRITE_UNBOUND", "中文名称未绑定到本单元支持的根全局udg声明/引用。", token.offset, token.text.size());
            }
        }
        Assemble();
        output_.coverage = IdentifierRewriteCoverage::RootGlobalJass;
        return Finish("仅完成根级JASS中文udg声明与变量表达式改名；未授予vJass扩展、类型检查或保存资格。");
    }

private:
    OriginSpan Origin(std::size_t offset, std::size_t length) const {
        return {sourcePath_, offset, length, output_.inputSha256};
    }

    [[noreturn]] void Fail(std::string code, std::string message, std::size_t offset, std::size_t length) const {
        throw RewriteFailure{{{DiagnosticSeverity::Error, "IdentifierRewrite", std::move(code), std::move(message), Origin(offset, length)}}};
    }

    [[noreturn]] void Budget(std::string message, std::size_t offset) const {
        Fail("IDENTIFIER_REWRITE_BUDGET", std::move(message), offset, 0);
    }

    void CheckRequest() {
        const IdentifierRewriteLimits ceiling;
        const auto& limit = request_.limits;
        if (limit.maxInputBytes > ceiling.maxInputBytes || limit.maxOutputBytes > ceiling.maxOutputBytes
            || limit.maxTokens > ceiling.maxTokens || limit.maxMappings > ceiling.maxMappings
            || limit.maxReferences > ceiling.maxReferences || limit.maxSyntaxDepth > ceiling.maxSyntaxDepth
            || limit.maxTokens == 0 || limit.maxSyntaxDepth == 0 || request_.text.size() > limit.maxInputBytes) {
            Budget("输入或请求预算超出有界服务上限。", 0);
        }
        if (request_.sourceName.empty() || request_.sourceName.size() > 4096
            || request_.globalScopeIdentity.empty() || request_.globalScopeIdentity.size() > 1024) {
            Fail("IDENTIFIER_REWRITE_IDENTITY", "必须给出有界来源名称与稳定语义作用域身份。", 0, 0);
        }
        if (!request_.encoding || (*request_.encoding != Text::SourceEncoding::Utf8 && *request_.encoding != Text::SourceEncoding::Gbk)) {
            Fail("IDENTIFIER_REWRITE_ENCODING", "必须显式声明UTF-8或CP936来源编码。", 0, 0);
        }
        for (std::uint32_t at = 0; at < request_.sourceName.size();) {
            const auto scalar = Text::ReadSourceScalar(request_.sourceName, at, Text::SourceEncoding::Utf8);
            if (!scalar.Succeeded() || scalar.value->codepoint == 0) {
                Fail("IDENTIFIER_REWRITE_IDENTITY", "报告来源名称必须是无NUL的有效UTF-8。", 0, 0);
            }
            at += scalar.value->bytes;
        }
        sourcePath_ = std::filesystem::path(std::u8string(request_.sourceName.begin(), request_.sourceName.end()));
        for (std::uint32_t at = 0; at < request_.globalScopeIdentity.size();) {
            const auto scalar = Text::ReadSourceScalar(request_.globalScopeIdentity, at, Text::SourceEncoding::Utf8);
            if (!scalar.Succeeded() || scalar.value->codepoint < 32 || scalar.value->codepoint == 127
                || scalar.value->codepoint == 0xFEFF) {
                Fail("IDENTIFIER_REWRITE_IDENTITY", "语义作用域身份必须是无控制字符的有效UTF-8。", 0, 0);
            }
            at += scalar.value->bytes;
        }
    }

    std::vector<Token> Tokens() {
        if (request_.lexicalView) {
            auto valid = ValidateSourceLexicalView(request_.text, *request_.lexicalView);
            if (!valid.Succeeded()) throw RewriteFailure{std::move(valid.diagnostics)};
            auto result = LexJass(request_.lexicalView->text, Text::SourceEncoding::Utf8,
                {.blockComments = true, .directiveComments = true});
            if (!result.Succeeded()) throw RewriteFailure{std::move(result.diagnostics)};
            if (result.value->size() > request_.limits.maxTokens) Budget("词法记号超过预算。", 0);
            for (auto& token : *result.value) token.text = request_.text.substr(token.offset, token.text.size());
            return std::move(*result.value);
        }
        std::size_t bom = 0;
        if (request_.text.starts_with("\xEF\xBB\xBF")) {
            if (*request_.encoding != Text::SourceEncoding::Utf8) {
                Fail("IDENTIFIER_REWRITE_BOM", "UTF-8 BOM与声明编码不一致。", 0, 3);
            }
            bom = 3;
            diagnostics_.push_back({DiagnosticSeverity::Info, "IdentifierRewrite", "IDENTIFIER_REWRITE_BOM",
                "开头UTF-8 BOM仅作为标记解释，输出保留原字节。", Origin(0, 3)});
        }
        auto result = LexJass(request_.text.substr(bom), *request_.encoding,
            {.blockComments = true, .directiveComments = true});
        if (!result.Succeeded()) {
            for (auto& diagnostic : result.diagnostics) {
                const auto offset = diagnostic.origin ? diagnostic.origin->offset : 0;
                const auto length = diagnostic.origin ? diagnostic.origin->length : 0;
                diagnostic.origin = Origin(offset + bom, length);
            }
            throw RewriteFailure{std::move(result.diagnostics)};
        }
        if (result.value->size() > request_.limits.maxTokens) Budget("词法记号超过预算。", 0);
        for (auto& token : *result.value) token.offset += static_cast<std::uint32_t>(bom);
        return std::move(*result.value);
    }

    void CheckSyntaxBudget(const std::vector<Token>& tokens) const {
        std::size_t groups = 0, blocks = 0, prefixes = 0;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            const auto& token = tokens[i];
            if (token.kind == TokenKind::Pragma && token.text.starts_with("//!")) {
                Fail("IDENTIFIER_REWRITE_UNEXPANDED", "改名之前必须完成//!指令、模板及宏展开。", token.offset, token.text.size());
            }
            if (token.IsOperator("(") || token.IsOperator("[")) ++groups;
            if ((token.IsOperator(")") || token.IsOperator("]")) && groups != 0) --groups;
            if (token.Is(Keyword::If) || token.Is(Keyword::Loop)) ++blocks;
            if ((token.Is(Keyword::EndIf) || token.Is(Keyword::EndLoop)) && blocks != 0) --blocks;
            prefixes = token.Is(Keyword::Not) || token.IsOperator("+") || token.IsOperator("-") ? prefixes + 1 : 0;
            if (groups > request_.limits.maxSyntaxDepth || blocks > request_.limits.maxSyntaxDepth
                || prefixes > request_.limits.maxSyntaxDepth) Budget("表达式或控制块嵌套超过预算。", token.offset);
            if (token.kind == TokenKind::Identifier && AtLineStart(tokens, i)) {
                constexpr std::string_view unsupported[] = {"library", "library_once", "scope", "struct", "module",
                    "interface", "private", "public", "textmacro", "runtextmacro", "static", "method"};
                if (std::find(std::begin(unsupported), std::end(unsupported), token.text) != std::end(unsupported)) {
                    Fail("IDENTIFIER_REWRITE_SCOPE_UNSUPPORTED", "本单元只绑定根级JASS全局；vJass名称域/扩展尚未支持。",
                        token.offset, token.text.size());
                }
            }
        }
    }

    void CheckErrors(const std::vector<SyntaxError>& errors) const {
        if (errors.empty()) return;
        const auto& error = errors.front();
        const auto found = std::find_if(program_.tokens.begin(), program_.tokens.end(), [&](const Token& token) {
            return token.line >= error.line;
        });
        Fail("IDENTIFIER_REWRITE_SYNTAX", "声明/引用结构无法确定：" + error.message,
            found == program_.tokens.end() ? request_.text.size() : found->offset,
            found == program_.tokens.end() ? 0 : found->text.size());
    }

    std::uint32_t Offset(std::string_view name) const {
        return static_cast<std::uint32_t>(name.data() - request_.text.data());
    }

    IdentifierOccurrence Occurrence(std::string_view name, std::uint32_t line) const {
        return {Origin(Offset(name), name.size()), line};
    }

    void Register(std::string_view name) {
        if (!declarations_.insert(name).second) {
            Fail("IDENTIFIER_REWRITE_DECLARATION_CONFLICT", "全局、函数、native或类型声明重名，拒绝不确定绑定。", Offset(name), name.size());
        }
    }

    void Declare() {
        std::set<std::string_view> occupied;
        for (const auto& token : program_.tokens) if (token.kind == TokenKind::Identifier) occupied.insert(token.text);
        for (const auto& type : program_.types) Register(type.name);
        for (const auto& function : program_.functions) Register(function.name);
        for (const auto& native : program_.natives) Register(native.name);
        for (const auto& global : program_.globals) Register(global.name);
        std::set<std::string> generated;
        for (const auto& global : program_.globals) {
            if (!Candidate(global.name)) continue;
            if (output_.mappings.size() >= request_.limits.maxMappings) Budget("中文全局映射超过预算。", Offset(global.name));
            IdentifierRename rename;
            rename.originalBytes = global.name;
            rename.scopeIdentity = request_.globalScopeIdentity;
            rename.declaration = Occurrence(global.name, global.line);
            for (std::uint32_t at = 0; at < global.name.size();) {
                auto scalar = Text::ReadSourceScalar(global.name, at, *request_.encoding);
                if (!scalar.Succeeded()) throw RewriteFailure{std::move(scalar.diagnostics)};
                rename.codepoints.push_back(static_cast<char32_t>(scalar.value->codepoint));
                at += scalar.value->bytes;
            }
            std::string identity;
            Append32(identity, static_cast<std::uint32_t>(kUdgIdentifierRule.size()));
            identity.append(kUdgIdentifierRule);
            Append32(identity, static_cast<std::uint32_t>(rename.scopeIdentity.size()));
            identity.append(rename.scopeIdentity);
            Append32(identity, static_cast<std::uint32_t>(rename.codepoints.size()));
            for (const auto value : rename.codepoints) Append32(identity, static_cast<std::uint32_t>(value));
            rename.generatedName = "forge_udg_" + Hash(identity).substr(0, 16);
            if (occupied.contains(rename.generatedName) || !generated.insert(rename.generatedName).second) {
                Fail("IDENTIFIER_REWRITE_NAME_CONFLICT", "稳定生成名已被代码占用或发生摘要碰撞；不追加枚举后缀。",
                    Offset(global.name), global.name.size());
            }
            const auto index = output_.mappings.size();
            mapping_.emplace(global.name, index);
            replacements_.emplace(Offset(global.name), index);
            output_.mappings.push_back(std::move(rename));
        }
    }

    void BindGlobals() {
        std::vector<std::int32_t> roots;
        for (const auto& global : program_.globals) if (global.init >= 0) roots.push_back(global.init);
        BindExpressions(program_.globalPool, std::move(roots), {});
    }

    void BindFunction(const FunctionDecl& function) {
        auto body = ParseFunctionBody(program_.tokens, function);
        CheckErrors(body.errors);
        std::set<std::string_view> locals;
        const auto local = [&](std::string_view name) {
            if (mapping_.contains(name)) {
                Fail("IDENTIFIER_REWRITE_SHADOW_UNSUPPORTED", "参数/local遮蔽中文udg全局；首批不改写局部名称，拒绝含糊输出。",
                    Offset(name), name.size());
            }
            locals.insert(name);
        };
        for (const auto& param : function.params) local(param.name);
        std::vector<std::int32_t> roots;
        for (const auto& decl : body.locals) {
            local(decl.name);
            if (decl.init >= 0) roots.push_back(decl.init);
        }
        // pool中每条嵌套语句只登记一次；不沿body再重复遍历，也不扫描不可达的孤立Name结点。
        for (const auto& statement : body.pool.statements) {
            if (statement.target >= 0) roots.push_back(statement.target);
            if (statement.value >= 0) roots.push_back(statement.value);
            for (const auto& branch : statement.branches) if (branch.condition >= 0) roots.push_back(branch.condition);
        }
        BindExpressions(body.pool, std::move(roots), locals);
    }

    void BindExpressions(const SyntaxPool& pool, std::vector<std::int32_t> pending,
        const std::set<std::string_view>& locals) {
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            const auto& expression = pool.At(index);
            if (expression.kind == ExprKind::Name) {
                const auto found = mapping_.find(expression.text);
                if (found != mapping_.end() && !locals.contains(expression.text)) {
                    if (++references_ > request_.limits.maxReferences) Budget("真实引用总数超过预算。", expression.begin);
                    output_.mappings[found->second].references.push_back(Occurrence(expression.text, expression.line));
                    replacements_.emplace(expression.begin, found->second);
                }
            }
            if ((expression.kind == ExprKind::Name || expression.kind == ExprKind::Call || expression.kind == ExprKind::FunctionRef)
                && expression.text.starts_with("xgdg_") && !locals.contains(expression.text) && !declarations_.contains(expression.text)) {
                Fail("IDENTIFIER_REWRITE_LEGACY_ALIAS_UNPROVEN", "未声明的旧生成名候选缺少可信映射；不能按名称或声明顺序猜测。",
                    Offset(expression.text), expression.text.size());
            }
            if (expression.left >= 0) pending.push_back(expression.left);
            if (expression.right >= 0) pending.push_back(expression.right);
            if (expression.kind == ExprKind::Call) {
                for (std::uint32_t i = 0; i < expression.argumentCount; ++i) pending.push_back(pool.Argument(expression, i));
            }
        }
    }

    void Assemble() {
        std::size_t bytes = request_.text.size();
        for (const auto& [offset, index] : replacements_) {
            const auto& rename = output_.mappings[index];
            bytes -= rename.originalBytes.size();
            if (rename.generatedName.size() > request_.limits.maxOutputBytes
                || bytes > request_.limits.maxOutputBytes - rename.generatedName.size()) Budget("改名输出超过预算。", offset);
            bytes += rename.generatedName.size();
        }
        if (bytes > request_.limits.maxOutputBytes) Budget("改名输出超过预算。", 0);
        output_.text.reserve(bytes);
        if (request_.lexicalView) output_.lexicalView.text.reserve(bytes);
        std::size_t begin = 0;
        for (const auto& [offset, index] : replacements_) {
            const auto& rename = output_.mappings[index];
            output_.text.append(request_.text.substr(begin, offset - begin));
            output_.text.append(rename.generatedName);
            if (request_.lexicalView) {
                output_.lexicalView.text.append(request_.lexicalView->text.substr(begin, offset - begin));
                output_.lexicalView.text.append(rename.generatedName);
            }
            begin = offset + rename.originalBytes.size();
        }
        output_.text.append(request_.text.substr(begin));
        if (request_.lexicalView) {
            output_.lexicalView.text.append(request_.lexicalView->text.substr(begin));
            auto replacement = replacements_.begin();
            std::int64_t adjustment = 0;
            for (auto span : request_.lexicalView->opaque) {
                while (replacement != replacements_.end() && replacement->first < span.offset) {
                    const auto& rename = output_.mappings[replacement->second];
                    adjustment += static_cast<std::int64_t>(rename.generatedName.size()) - static_cast<std::int64_t>(rename.originalBytes.size());
                    ++replacement;
                }
                span.offset = static_cast<std::uint32_t>(span.offset + adjustment);
                output_.lexicalView.opaque.push_back(std::move(span));
            }
        }
        for (auto& rename : output_.mappings) {
            std::sort(rename.references.begin(), rename.references.end(), [](const auto& a, const auto& b) {
                return a.origin.offset < b.origin.offset;
            });
        }
        output_.outputSha256 = Hash(output_.text);
    }

    Result<IdentifierRewriteResult> Finish(std::string coverage) {
        diagnostics_.push_back({DiagnosticSeverity::Info, "IdentifierRewrite", "IDENTIFIER_REWRITE_COVERAGE", std::move(coverage),
            Origin(0, request_.text.size())});
        return {std::move(output_), std::move(diagnostics_)};
    }

    const IdentifierRewriteRequest& request_;
    std::filesystem::path sourcePath_;
    IdentifierRewriteResult output_;
    std::vector<Diagnostic> diagnostics_;
    ProgramFile program_;
    std::set<std::string_view> declarations_;
    std::map<std::string_view, std::size_t> mapping_;
    std::map<std::uint32_t, std::size_t> replacements_;
    std::size_t references_ = 0;
};

} // namespace

Result<IdentifierRewriteResult> RewriteUdgIdentifiers(const IdentifierRewriteRequest& request) {
    try {
        return Rewriter(request).Run();
    } catch (RewriteFailure& error) {
        return {std::nullopt, std::move(error.diagnostics)};
    } catch (const std::bad_alloc&) {
        return Result<IdentifierRewriteResult>::Failure("IdentifierRewrite", "IDENTIFIER_REWRITE_MEMORY", "改名服务内存分配失败；未返回部分脚本。");
    }
}

} // namespace War3Map::Jass
