#include "War3MapCore/Jass/IdentifierRewrite.h"
#include "War3MapCore/Hash.h"
#include "JassParser.h"
#include <algorithm>
#include <map>
#include <set>

namespace War3Map::Jass {
Result<ExactIdentifierResult> RewriteExactIdentifiers(const IdentifierRewriteRequest& request,
    std::span<const ExactIdentifierMapping> mappings, bool statements) {
    using R = Result<ExactIdentifierResult>;
    using namespace detail;
    const auto fail = [&](std::string code, std::string message, std::uint32_t offset = 0) {
        R result = R::Failure("Jass.Identifier", "TRIGGER_CODE_" + code, std::move(message));
        result.diagnostics.front().origin = {request.sourceName, offset, 0, request.expectedInputSha256};
        return result;
    };
    const IdentifierRewriteLimits ceiling;
    if (!request.encoding || (*request.encoding != Text::SourceEncoding::Utf8 && *request.encoding != Text::SourceEncoding::Gbk))
        return fail("ENCODING_UNSUPPORTED", "必须明确指定UTF-8或CP936来源编码。");
    if (request.text.size() > std::min(request.limits.maxInputBytes, ceiling.maxInputBytes)
        || !request.limits.maxTokens || request.limits.maxTokens > ceiling.maxTokens
        || mappings.size() > std::min(request.limits.maxMappings, ceiling.maxMappings))
        return fail("BUDGET_EXCEEDED", "标识符改写输入超过预算。");
    const auto hash = Sha256Text(request.text);
    if (!hash.Succeeded() || *hash.value != request.expectedInputSha256)
        return fail("SOURCE_STALE", "代码原字节SHA已变化。");
    std::map<std::string, std::string> names;
    const auto identifier = [](std::string_view s) {
        return !s.empty() && !(s.front() >= '0' && s.front() <= '9') && std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
    };
    std::set<std::string> destinations;
    for (const auto& mapping : mappings) {
        if (!identifier(mapping.before) || !identifier(mapping.after) || !names.emplace(mapping.before, mapping.after).second
            || !destinations.insert(mapping.after).second)
            return fail("BINDING_UNSUPPORTED", "改写映射必须为唯一的完整ASCII标识符。");
    }
    // 片段只在分析时补函数边界；输出与定位仍基于原始片段。
    const std::string prefix = statements ? "function forge_identity_probe takes nothing returns nothing\n" : "";
    const std::string source = prefix + std::string(request.text) + (statements ? "\nendfunction\n" : "");
    auto lexed = LexJassBounded(source, *request.encoding, {true, true}, request.limits.maxTokens);
    if (!lexed.Succeeded()) return fail("BINDING_UNSUPPORTED", "代码词法不完整，未返回部分改写。");
    for (const auto& token : *lexed.value) {
        if (token.kind == TokenKind::Pragma)
            return fail("COVERAGE_INCOMPLETE", "代码含未展开指令，无法证明标识符绑定。", token.offset > prefix.size() ? token.offset - static_cast<std::uint32_t>(prefix.size()) : 0);
        if (token.kind == TokenKind::Identifier && token.text == "ExecuteFunc")
            return fail("COVERAGE_INCOMPLETE", "代码含动态函数分发，字符串不参与改写，无法证明引用完整。", token.offset > prefix.size() ? token.offset - static_cast<std::uint32_t>(prefix.size()) : 0);
    }
    auto program = ParseProgram(std::move(*lexed.value));
    const auto errorOffset = [&](std::uint32_t line) {
        const auto found = std::find_if(program.tokens.begin(), program.tokens.end(), [&](const auto& token) { return token.line >= line; });
        return found == program.tokens.end() || found->offset < prefix.size() ? 0U : found->offset - static_cast<std::uint32_t>(prefix.size());
    };
    if (!program.errors.empty()) return fail("BINDING_UNSUPPORTED", "仅支持完整根级JASS或完整语句片段；方言、作用域或语法尚不能证明：" + program.errors.front().message, errorOffset(program.errors.front().line));
    ExactIdentifierResult result;
    const auto add = [&](std::string_view name, std::uint32_t line, bool function, bool local, bool initializer = false) {
        const auto offset = static_cast<std::size_t>(name.data() - source.data());
        if (offset < prefix.size() || offset >= prefix.size() + request.text.size()) return;
        result.declarations.push_back({std::string(name), static_cast<std::uint32_t>(offset - prefix.size()), line - (statements ? 1 : 0), function, local, initializer});
    };
    for (const auto& global : program.globals) add(global.name, global.line, false, false);
    for (const auto& native : program.natives) add(native.name, native.line, true, false);
    for (const auto& type : program.types) {
        add(type.name, type.line, false, false);
        if (names.contains(std::string(type.name)) || destinations.contains(std::string(type.name)))
            return fail("BINDING_UNSUPPORTED", "目标标识符被用作类型声明，不能按触发器符号改写。");
    }
    for (const auto& function : program.functions) {
        add(function.name, function.line, true, false, function.params.empty() && function.returns.empty());
        for (const auto& parameter : function.params) add(parameter.name, function.line, false, true);
        auto body = ParseFunctionBody(program.tokens, function);
        if (!body.errors.empty()) return fail("BINDING_UNSUPPORTED", "函数体语法不完整：" + body.errors.front().message, errorOffset(body.errors.front().line));
        for (const auto& local : body.locals) add(local.name, local.line, false, true);
    }
    for (const auto& declaration : result.declarations) {
        if (declaration.local && (names.contains(declaration.name) || destinations.contains(declaration.name)))
            return fail("BINDING_UNSUPPORTED", "目标名称被参数或局部变量遮蔽：" + declaration.name, declaration.offset);
        if (!declaration.local && destinations.contains(declaration.name) && !names.contains(declaration.name))
            return fail("SYMBOL_CONFLICT", "目标符号已有声明：" + declaration.name, declaration.offset);
    }
    std::size_t begin = 0;
    for (const auto& token : program.tokens) {
        if (token.kind != TokenKind::Identifier || token.offset < prefix.size() || token.offset >= prefix.size() + request.text.size()) continue;
        const auto found = names.find(std::string(token.text));
        if (found == names.end() || found->first == found->second) continue;
        const auto offset = token.offset - prefix.size();
        if (++result.replacements > std::min(request.limits.maxReferences, ceiling.maxReferences))
            return fail("BUDGET_EXCEEDED", "代码引用数量超过改写预算。");
        result.text.append(request.text.substr(begin, offset - begin)); result.text += found->second;
        begin = offset + token.text.size();
        if (result.text.size() > std::min(request.limits.maxOutputBytes, ceiling.maxOutputBytes))
            return fail("BUDGET_EXCEEDED", "改写输出超过预算。");
    }
    result.text.append(request.text.substr(begin));
    if (result.text.size() > std::min(request.limits.maxOutputBytes, ceiling.maxOutputBytes))
        return fail("BUDGET_EXCEEDED", "改写输出超过预算。");
    return R::Success(std::move(result));
}
}
