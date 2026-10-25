#include "War3MapCore/Script/MapRuntimeBoot.h"
#include "../Jass/JassLexer.h"
#include "../Lua/LuaLexer.h"
#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace War3Map::Script {
namespace {
using namespace Jass::detail;
using Tokens = std::vector<Token>;
constexpr std::string_view kBootstrap = "warvk_loader_raw.ai";
constexpr std::string_view kCall = "call StartCampaignAI(Player(PLAYER_NEUTRAL_AGGRESSIVE), \"warvk_loader_raw.ai\")";
struct Layout {
    Tokens tokens;
    std::size_t first = 0, mainEnd = 0, count = 0, boot = 0, bootEnd = 0;
    bool competingAi = false;
};
Result<Tokens> Lex(std::string_view script) {
    if (script.size() > 16 * 1024 * 1024) return Result<Tokens>::Failure("MapRuntime", "RUNTIME_BOOT_INVALID", "主脚本超过16MiB运行库结构检查预算。");
    auto result = LexJassBounded(script, {}, 1024 * 1024);
    if (!result.Succeeded() || !result.value) return result;
    if (std::any_of(result.value->begin(), result.value->end(), [](const auto& token) { return token.kind == TokenKind::Invalid; }))
        return Result<Tokens>::Failure("MapRuntime", "RUNTIME_BOOT_INVALID", "主脚本包含无效词法，不能安装运行库启动。");
    return result;
}
bool Digest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool RuntimeFunction(std::string_view name) {
    return name.starts_with("AudastraUI") || name.starts_with("AudastraJapi_")
        || name.starts_with("AjCall") || name == "AjInitialize" || name == "AjRequest"
        || name == "AjModuleLoad" || name == "AjModuleUnload" || name == "AjPump"
        || name == "AjOn" || name == "AjOff" || name == "AjDispatch" || name == "AjBindEvents";
}
bool SeparateAiPlayer(const Tokens& tokens, std::size_t call) {
    if (call + 6 >= tokens.size() || tokens[call + 2].text != "Player" || !tokens[call + 3].IsOperator("(")
        || !tokens[call + 5].IsOperator(")") || !tokens[call + 6].IsOperator(",")) return false;
    const auto& token = tokens[call + 4];
    if (token.text == "PLAYER_NEUTRAL_PASSIVE" || token.text == "PLAYER_NEUTRAL_VICTIM"
        || token.text == "PLAYER_NEUTRAL_EXTRA") return true;
    if (token.kind != TokenKind::Integer || token.text.empty() || (token.text.size() > 1 && token.text.front() == '0')) return false;
    unsigned index = 0;
    const auto parsed = std::from_chars(token.text.data(), token.text.data() + token.text.size(), index);
    return parsed.ec == std::errc{} && parsed.ptr == token.text.data() + token.text.size() && index < 16 && index != 12;
}
Result<Token> IdentityBinding(std::string_view script) {
    auto lexed = Lex(script);
    if (!lexed.Succeeded()) { Result<Token> result; result.diagnostics = lexed.diagnostics; return result; }
    const auto& tokens = *lexed.value;
    std::optional<Token> binding;
    bool globals = false;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].Is(Keyword::Globals)) globals = true;
        if (tokens[i].Is(Keyword::EndGlobals)) globals = false;
        if (tokens[i].text != "AjExpectedRuntimeIdentity" || !i) continue;
        if (tokens[i - 1].Is(Keyword::Set))
            return Result<Token>::Failure("MapRuntime", "RUNTIME_IDENTITY_BINDING_INVALID", "运行库预期身份不能在执行期被脚本重新赋值。");
        if (tokens[i - 1].text != "string") continue;
        if (!globals || binding || i + 3 >= tokens.size() || !tokens[i + 1].IsOperator("=")
            || tokens[i + 2].kind != TokenKind::String || !tokens[i + 3].EndsLine())
            return Result<Token>::Failure("MapRuntime", "RUNTIME_IDENTITY_BINDING_INVALID", "运行库身份必须为唯一的全局字符串字面量，不能被局部声明遮蔽。");
        binding = tokens[i + 2];
    }
    if (!binding) return Result<Token>::Failure("MapRuntime", "RUNTIME_IDENTITY_BINDING_MISSING", "主脚本缺少唯一的SDK运行库身份绑定，不能接受旧宿主握手。");
    return Result<Token>::Success(*binding);
}
Result<Layout> Inspect(std::string_view script) {
    using Output = Result<Layout>;
    auto lexed = Lex(script);
    if (!lexed.Succeeded()) { Output result; result.diagnostics = std::move(lexed.diagnostics); return result; }
    Layout result; result.tokens = std::move(*lexed.value);
    const auto& t = result.tokens;
    bool main = false, header = false, body = false, mainFound = false;
    for (std::size_t i = 0; i + 1 < t.size(); ++i) {
        if (i + 2 < t.size() && t[i].Is(Keyword::Function) && t[i + 1].text == "main" && t[i + 2].Is(Keyword::Takes)) {
            if (mainFound) return Output::Failure("MapRuntime", "RUNTIME_BOOT_INVALID", "主脚本包含多个main。");
            main = mainFound = header = true;
        }
        if (main && header && t[i].EndsLine()) { header = false; body = true; continue; }
        if (main && body && t[i].kind != TokenKind::Newline && t[i].kind != TokenKind::Pragma) {
            if (t[i].Is(Keyword::Local)) {
                while (i + 1 < t.size() && !t[i].EndsLine()) {
                    if (t[i].IsOperator("=")) return Output::Failure("MapRuntime", "RUNTIME_BOOT_INVALID", "main局部变量含初始化表达式，不能无证据重排早期启动。");
                    ++i;
                }
                continue;
            }
            result.first = i; body = false;
        }
        if (main && t[i].Is(Keyword::EndFunction)) { result.mainEnd = i; main = false; }
        if ((t[i].text != "StartCampaignAI" && t[i].text != "StartMeleeAI") || !t[i + 1].IsOperator("(")) continue;
        int depth = 0; bool owned = false; std::size_t end = i + 1;
        for (; end < t.size(); ++end) {
            if (t[end].IsOperator("(")) ++depth;
            if (t[end].kind == TokenKind::String && StringLiteralValue(t[end].text) == kBootstrap) owned = true;
            if (t[end].IsOperator(")") && --depth == 0) break;
            if (t[end].kind == TokenKind::End) break;
        }
        if (!owned) { result.competingAi = result.competingAi || !SeparateAiPlayer(t, i); continue; }
        if (i == 0 || !t[i - 1].Is(Keyword::Call) || end >= t.size())
            return Output::Failure("MapRuntime", "RUNTIME_BOOT_INVALID", "运行库启动不是可核验的独立call语句。");
        constexpr std::string_view expected[]{"call","StartCampaignAI","(","Player","(","PLAYER_NEUTRAL_AGGRESSIVE",")",",","\"warvk_loader_raw.ai\"",")"};
        const auto start = i - 1;
        if (end - start + 1 != std::size(expected)) return Output::Failure("MapRuntime", "RUNTIME_BOOT_INVALID", "运行库启动的玩家或参数结构不符。");
        for (std::size_t n = 0; n < std::size(expected); ++n)
            if (t[start + n].text != expected[n]) return Output::Failure("MapRuntime", "RUNTIME_BOOT_INVALID", "运行库启动须使用固定玩家和精确引导成员名。");
        ++result.count; result.boot = start; result.bootEnd = end;
    }
    if (!mainFound || !result.first || !result.mainEnd) return Output::Failure("MapRuntime", "RUNTIME_BOOT_INVALID", "缺少完整main，不能安装启动。");
    if (result.count > 1) return Output::Failure("MapRuntime", "RUNTIME_BOOT_INVALID", "本运行库存在重复启动，未删除作者语句。");
    return Output::Success(std::move(result));
}
}

Result<MapRuntimeUsage> AnalyzeMapRuntimeUsage(std::string_view script, MapRuntimeScriptLanguage language) {
    if (language == MapRuntimeScriptLanguage::Lua) {
        try {
            if (script.size() > 16 * 1024 * 1024) throw std::length_error("source budget");
            auto tokens = Lua::Detail::Tokenize(script, true, 1024 * 1024);
            for (const auto& token : tokens) if (token.unterminated) throw std::runtime_error("incomplete Lua token");
            std::erase_if(tokens, [](const auto& token) { return token.kind == Lua::Detail::TokenKind::Comment; });
            MapRuntimeUsage usage;
            for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
                const auto& token = tokens[i];
                if (token.kind != Lua::Detail::TokenKind::Name) continue;
                const auto name = token.Text(script);
                if (RuntimeFunction(name) && !(i && tokens[i - 1].Is(script, "function"))) usage.required = true;
                if (name == "ExecuteFunc" && tokens[i + 1].Is(script, "(")) {
                    if (i + 3 < tokens.size() && tokens[i + 2].kind == Lua::Detail::TokenKind::String && tokens[i + 3].Is(script, ")")) {
                        if (tokens[i + 2].Text(script).find('\\') != std::string_view::npos) usage.unresolvedDynamicCalls = true;
                        if (RuntimeFunction(Lua::Detail::StringValue(tokens[i + 2].Text(script)))) usage.required = true;
                    } else usage.unresolvedDynamicCalls = true;
                }
                if ((name == "_G" || name == "jass") && tokens[i + 1].Is(script, "[")) {
                    if (i + 3 < tokens.size() && tokens[i + 2].kind == Lua::Detail::TokenKind::String && tokens[i + 3].Is(script, "]")) {
                        if (tokens[i + 2].Text(script).find('\\') != std::string_view::npos) usage.unresolvedDynamicCalls = true;
                        if (RuntimeFunction(Lua::Detail::StringValue(tokens[i + 2].Text(script)))) usage.required = true;
                    } else usage.unresolvedDynamicCalls = true;
                }
                if (name == "load" || name == "loadfile" || name == "dofile" || name == "require" || name == "rawget")
                    usage.unresolvedDynamicCalls = true;
            }
            return Result<MapRuntimeUsage>::Success(usage);
        } catch (const std::exception&) {
            return Result<MapRuntimeUsage>::Failure("MapRuntime", "RUNTIME_USAGE_UNCERTAIN", "Lua运行库依赖词法捕获不完整或超过预算，未返回确定未使用。");
        }
    }
    if (language != MapRuntimeScriptLanguage::Jass)
        return Result<MapRuntimeUsage>::Failure("MapRuntime", "RUNTIME_USAGE_UNCERTAIN", "未知脚本语言不能作为无依赖证明。");
    auto tokens = Lex(script);
    if (!tokens.Succeeded()) { Result<MapRuntimeUsage> result; result.diagnostics = std::move(tokens.diagnostics); return result; }
    MapRuntimeUsage usage;
    const auto& t = *tokens.value;
    for (std::size_t i = 0; i + 1 < t.size(); ++i) {
        if (t[i].kind != TokenKind::Identifier) continue;
        const auto name = t[i].text;
        if (name == "ExecuteFunc" && t[i + 1].IsOperator("(")) {
            if (i + 3 < t.size() && t[i + 2].kind == TokenKind::String && t[i + 3].IsOperator(")")) {
                const auto target = StringLiteralValue(t[i + 2].text);
                if (target && RuntimeFunction(*target)) usage.required = true;
            } else usage.unresolvedDynamicCalls = true;
        }
        if (!RuntimeFunction(name)) continue;
        if (t[i + 1].IsOperator("(") || (i && t[i - 1].Is(Keyword::Function) && !t[i + 1].Is(Keyword::Takes)))
            usage.required = true;
    }
    return Result<MapRuntimeUsage>::Success(usage);
}
Result<bool> UsesMapRuntime(std::string_view script) {
    const auto usage = AnalyzeMapRuntimeUsage(script);
    if (!usage.Succeeded()) { Result<bool> failed; failed.diagnostics = usage.diagnostics; return failed; }
    return Result<bool>::Success(usage.value->required);
}

Status VerifyMapRuntimeBoot(std::string_view script, bool enabled) {
    auto result = Inspect(script);
    if (!result.Succeeded()) return {std::move(result.diagnostics)};
    const auto& layout = *result.value;
    if (enabled && layout.competingAi)
        return Status::Failure("MapRuntime", "RUNTIME_COEXISTENCE_UNVERIFIED",
            "发现同一neutral aggressive玩家的其他AI启动，或玩家表达式不能确定；尚无已验收的共存启动序列，未抢占、改玩家或删除原初始化。");
    if ((enabled && (layout.count != 1 || layout.boot != layout.first)) || (!enabled && layout.count))
        return Status::Failure("MapRuntime", "RUNTIME_BOOT_INVALID", "最终主脚本的运行库启动缺失、重复或不在早期首句。");
    return Status::Ok();
}
Status VerifyMapRuntimeIdentity(std::string_view script, std::string_view payloadSha256) {
    if (!Digest(payloadSha256)) return Status::Failure("MapRuntime", "RUNTIME_IDENTITY_INVALID", "运行库内容身份不是小写SHA-256。");
    const auto binding = IdentityBinding(script);
    if (!binding.Succeeded()) return {binding.diagnostics};
    if (StringLiteralValue(binding.value->text) != payloadSha256)
        return Status::Failure("MapRuntime", "RUNTIME_IDENTITY_MISMATCH", "最终主脚本的预期运行库身份与本图锁定载荷不符；未沿用旧握手成功状态。");
    return Status::Ok();
}

Result<std::string> ApplyMapRuntimeBoot(std::string_view script, const MapRuntimeBootPlan& plan) {
    if (plan.onlyIfUsed) {
        const auto used = AnalyzeMapRuntimeUsage(script);
        if (!used.Succeeded()) { Result<std::string> failed; failed.diagnostics = used.diagnostics; return failed; }
        if (!used.value->required) {
            if (!plan.ownsExisting) return Result<std::string>::Success(std::string(script));
            if (used.value->unresolvedDynamicCalls)
                return Result<std::string>::Failure("MapRuntime", "RUNTIME_USAGE_UNCERTAIN", "仍有动态执行入口，未把静态未检出当作移除已有运行库的证明；请明确启用或先调整依赖。");
            auto removal = plan; removal.onlyIfUsed = false; removal.enabled = false;
            return ApplyMapRuntimeBoot(script, removal);
        }
    }
    auto identity = plan.payloadSha256;
    if (plan.enabled && plan.resolvePayloadIdentity) {
        auto resolved = plan.resolvePayloadIdentity();
        if (!resolved.Succeeded()) { Result<std::string> failed; failed.diagnostics = resolved.diagnostics; return failed; }
        identity = std::move(*resolved.value);
    }
    if (plan.enabled && plan.verifyScriptInputs) {
        const auto checked = plan.verifyScriptInputs(plan.injectedSourceSha256);
        if (!checked.Succeeded()) { Result<std::string> failed; failed.diagnostics = checked.diagnostics; return failed; }
    }
    std::string identityScript;
    if (plan.enabled && !identity.empty()) {
        if (!Digest(identity))
            return Result<std::string>::Failure("MapRuntime", "RUNTIME_IDENTITY_INVALID", "运行库内容身份不是小写SHA-256。");
        const auto binding = IdentityBinding(script);
        if (!binding.Succeeded()) { Result<std::string> failed; failed.diagnostics = binding.diagnostics; return failed; }
        identityScript = std::string(script);
        identityScript.replace(binding.value->offset, binding.value->text.size(), "\"" + identity + "\"");
        script = identityScript;
    }
    auto inspected = Inspect(script);
    if (!inspected.Succeeded()) { Result<std::string> result; result.diagnostics = std::move(inspected.diagnostics); return result; }
    const auto& layout = *inspected.value;
    const auto& tokens = layout.tokens;
    if (layout.count && (!plan.ownsExisting || layout.boot != layout.first))
        return Result<std::string>::Failure("MapRuntime", "RUNTIME_BOOT_INVALID", "发现手工启动或受管启动被移动；未接管、重排或删除作者代码。");
    std::string result(script);
    if (plan.enabled && !layout.count) {
        auto begin = static_cast<std::size_t>(tokens[layout.first].offset);
        while (begin && script[begin - 1] != '\r' && script[begin - 1] != '\n') --begin;
        const auto indent = script.substr(begin, tokens[layout.first].offset - begin);
        if (indent.find_first_not_of(" \t") != std::string_view::npos)
            return Result<std::string>::Failure("MapRuntime", "RUNTIME_BOOT_INVALID", "main语句不在独立行，拒绝改写。");
        result.insert(begin, std::string(indent) + std::string(kCall) + "\r\n");
    } else if (!plan.enabled && layout.count) {
        const auto begin = tokens[layout.boot].offset;
        const auto end = tokens[layout.bootEnd].offset + tokens[layout.bootEnd].text.size();
        result.erase(begin, end - begin);
    }
    const auto verified = VerifyMapRuntimeBoot(result, plan.enabled);
    if (!verified.Succeeded()) { Result<std::string> failed; failed.diagnostics = verified.diagnostics; return failed; }
    return Result<std::string>::Success(std::move(result));
}
} // namespace War3Map::Script
