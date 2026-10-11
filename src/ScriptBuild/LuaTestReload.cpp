#include "War3MapCore/Script/LuaTestReload.h"
#include "War3MapCore/Hash.h"
#include "LuaTriggerEmitter.h"
#include "LuaCustomReloadAnalysis.h"
#include "LuaDeclarationScan.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <new>
#include <stdexcept>

namespace War3Map::Script {
namespace lua { extern const std::string_view kLuaTriggerRuntimeSource; }
namespace {
constexpr std::size_t kMailboxBytes = 1024 * 1024;
Diagnostic Error(std::string code, std::string message) {
    return {DiagnosticSeverity::Error, "Script.LuaReload", std::move(code), std::move(message), {}};
}
bool Identity(const LuaTriggerUnit& unit) {
    if (!unit.Succeeded() || !unit.testHotReload || unit.text.size() > 8 * 1024 * 1024
        || unit.testTriggers.size() != unit.symbols.size() || unit.testGlobals.size() > 8192) return false;
    const auto hash = Sha256Text(unit.text);
    return hash.Succeeded() && hash.value && *hash.value == unit.scriptSha256;
}
bool Slice(const LuaTriggerUnit& unit, std::size_t offset, std::string_view text) {
    return offset <= unit.text.size() && text.size() <= unit.text.size() - offset
        && std::string_view(unit.text).substr(offset, text.size()) == text;
}
Result<lua::CustomReloadContext> AuthorContext(const LuaTriggerUnit& unit, const Lua::SyntaxOptions& syntax) {
    std::vector<lua::CustomReloadSource> sources;
    for (const auto& chunk : unit.testTriggers) if (chunk.customCode)
        sources.push_back({chunk.authorSource, chunk.symbol.initializer});
    auto context = lua::CaptureCustomReloadContext(unit.testSharedSource, sources, syntax);
    if (!context.Succeeded()) return context;
    for (const auto& chunk : unit.testTriggers) if (chunk.customCode) {
        const auto proof = lua::AnalyzeCustomReload({chunk.authorSource, chunk.symbol.initializer}, *context.value, syntax);
        if (!proof.Succeeded()) return {std::nullopt, proof.diagnostics};
        if (proof.value->text != chunk.functions)
            return Result<lua::CustomReloadContext>::Failure("Script.LuaReload", "LUA_RELOAD_CUSTOM_MANIFEST",
                "作者原文不能重新生成本局实际转发代码，不能授予热重载资格。");
    }
    return context;
}
Result<bool> CapturesChangedGuiFunction(const LuaTriggerUnit& baseline,
    const std::map<std::string_view, bool>& changedNames, const Lua::SyntaxOptions& requested) {
    try {
        auto syntax = requested;
        syntax.deadline = (std::min)(syntax.deadline, std::chrono::steady_clock::now()
            + (std::clamp)(syntax.maxDuration, std::chrono::milliseconds::zero(), std::chrono::milliseconds{30000}));
        std::size_t bytes = 0;
        const auto captures = [&](std::string_view text) {
            if (text.size() > 8 * 1024 * 1024 - bytes) throw std::length_error("author reference scan budget");
            bytes += text.size();
            const auto tokens = lua::ScanDeclarationTokens(text, syntax);
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                const auto name = tokens[i].text;
                const auto found = changedNames.find(name);
                if (found == changedNames.end()) continue;
                // 稳定全局名称的直接调用会取得新实现；保存、传参、字段、重定义和改名后的旧调用都不授予资格。
                const bool direct = i + 1 < tokens.size() && tokens[i + 1].text == "("
                    && (!i || (tokens[i - 1].text != "." && tokens[i - 1].text != ":" && tokens[i - 1].text != "function"));
                if (!found->second || !direct) return true;
            }
            return false;
        };
        if (captures(baseline.testSharedSource)) return Result<bool>::Success(true);
        // 使用已核对属于实际脚本的片段，不能用可篡改的 authorSource 元数据掩盖旧函数引用。
        for (const auto& chunk : baseline.testTriggers)
            if (chunk.customCode && captures(chunk.functions)) return Result<bool>::Success(true);
        return Result<bool>::Success(false);
    } catch (const Diagnostic& diagnostic) { return {std::nullopt, {diagnostic}}; }
    catch (const std::length_error&) {
        return Result<bool>::Failure("Script.LuaReload", "LUA_RELOAD_AUTHOR_REFERENCE_BUDGET", "作者代码引用扫描超过 8 MiB 预算。");
    }
}
}

Result<LuaTestReloadPatch> PrepareLuaTestReload(const LuaTriggerUnit& baseline, const LuaTriggerUnit& current,
    std::string_view beforeBinding, std::string_view afterBinding, const Lua::SyntaxOptions& syntax) {
    Result<LuaTestReloadPatch> result;
    const auto reject = [&](std::string code, std::string message) {
        result.diagnostics.push_back(Error(std::move(code), "需要重新测试：" + std::move(message)));
    };
    try {
        if (!Identity(baseline) || !Identity(current)) {
            reject("LUA_RELOAD_IDENTITY", "本局或当前脚本不是完整的测试热重载生成产物。"); return result;
        }
        if (beforeBinding.empty() || beforeBinding != afterBinding) {
            reject("LUA_RELOAD_MAP_CHANGED", "物编、地形、资源或地图设置发生变化。"); return result;
        }
        if (baseline.targetIdentity != current.targetIdentity || baseline.targetSha256 != current.targetSha256
            || baseline.uiCatalogSha256 != current.uiCatalogSha256 || baseline.testSharedSource != current.testSharedSource) {
            reject("LUA_RELOAD_CONTEXT_CHANGED", "运行时、函数目录、共享代码或回调绑定发生变化。"); return result;
        }
        std::map<StableId, const LuaTestTriggerChunk*> oldTriggers, newTriggers;
        std::map<std::string, const LuaTestGlobalChunk*> oldGlobals, newGlobals;
        const auto triggers = [&](const LuaTriggerUnit& unit, auto& byId) {
            std::map<StableId, const LuaTriggerSymbol*> symbols;
            for (const auto& symbol : unit.symbols)
                if (!symbols.emplace(symbol.triggerId, &symbol).second) return false;
            for (const auto& chunk : unit.testTriggers) {
                const auto found = symbols.find(chunk.symbol.triggerId);
                if (!chunk.symbol.triggerId || found == symbols.end()
                    || found->second->variable != chunk.symbol.variable || found->second->initializer != chunk.symbol.initializer
                    || !byId.emplace(chunk.symbol.triggerId, &chunk).second)
                    return false;
                // 片段必须是实际生成文本的完整部分，不能把调用方伪造的 metadata 当作正文。
                if (!Slice(unit, chunk.functionsOffset, chunk.functions)
                    || !Slice(unit, chunk.initializerOffset, chunk.initializer)) return false;
            }
            return true;
        };
        const auto globals = [](const LuaTriggerUnit& unit, auto& byName) {
            for (const auto& chunk : unit.testGlobals)
                if (!chunk.variableId || !lua::Identifier(chunk.name) || !byName.emplace(chunk.name, &chunk).second
                    || !Slice(unit, chunk.declarationOffset, chunk.declaration)
                    || !Slice(unit, chunk.initializationOffset, chunk.initialization)) return false;
            return true;
        };
        if (!triggers(baseline, oldTriggers) || !triggers(current, newTriggers)
            || !globals(baseline, oldGlobals) || !globals(current, newGlobals)) {
            reject("LUA_RELOAD_MANIFEST", "生成片段身份重复、缺失或不属于实际脚本。"); return result;
        }
        for (const auto& [name, old] : oldGlobals) {
            const auto next = newGlobals.find(name);
            if (next == newGlobals.end() || next->second->shape != old->shape
                || next->second->variableId != old->variableId) {
                reject("LUA_RELOAD_GLOBAL_CHANGED", "已有变量 " + name + " 的名称、类型、范围或初始化值发生变化。"); return result;
            }
        }
        std::map<std::string_view, bool> changedGuiNames;
        for (const auto& next : current.testTriggers) {
            const auto found = oldTriggers.find(next.symbol.triggerId);
            if (found == oldTriggers.end() || next.customCode || found->second->customCode
                || found->second->functions == next.functions) continue;
            const auto& old = found->second->symbol;
            changedGuiNames.emplace(old.actions, old.actions == next.symbol.actions);
            changedGuiNames.emplace(old.conditions, old.conditions == next.symbol.conditions);
        }
        if (!changedGuiNames.empty()) {
            // 批量改动也只扫描一次作者上下文，避免每个 GUI 再扫描全部 WCT。
            const auto captured = CapturesChangedGuiFunction(baseline, changedGuiNames, syntax);
            if (!captured.Succeeded() || *captured.value) {
                reject("LUA_RELOAD_AUTHOR_GUI_REFERENCE", captured.Succeeded()
                    ? "作者代码保存或传递了本次被改 GUI 触发器的函数，旧回调无法自动替换。"
                    : "作者代码的 GUI 回调引用检查未完成。"); return result;
            }
        }
        LuaTestReloadPatch patch;
        patch.baselineScriptSha256 = baseline.scriptSha256; patch.nextScriptSha256 = current.scriptSha256;
        std::string definitions, operations;
        const auto append = [](std::string& output, std::string_view text) {
            if (text.size() > kMailboxBytes - output.size()) throw std::length_error("reload source budget");
            output.append(text); output.push_back('\n');
            if (output.size() > kMailboxBytes) throw std::length_error("reload source budget");
        };
        const auto appendDefinition = [&](const LuaTriggerUnit& unit, std::size_t offset, std::string_view text) {
            const auto start = definitions.size();
            append(definitions, text);
            for (const auto& span : unit.sourceSpans) {
                if (span.byteOffset > unit.text.size() || span.byteLength > unit.text.size() - span.byteOffset) continue;
                const auto begin = (std::max)(offset, span.byteOffset);
                const auto end = (std::min)(offset + text.size(), span.byteOffset + span.byteLength);
                if (begin >= end) continue;
                const bool author = span.kind == LuaSourceSpanKind::AuthorCustom || span.kind == LuaSourceSpanKind::AuthorHeader;
                const auto line = author ? 1 + std::count(unit.text.begin() + span.byteOffset, unit.text.begin() + begin, '\n') : 0;
                patch.sourceSpans.push_back({start + begin - offset, end - begin, span.triggerId, span.nodeId,
                    static_cast<std::size_t>(line)});
            }
        };
        for (const auto& chunk : current.testGlobals) if (!oldGlobals.contains(chunk.name)) {
            appendDefinition(current, chunk.declarationOffset, chunk.declaration);
            appendDefinition(current, chunk.initializationOffset, chunk.initialization);
            patch.newGlobals.push_back(chunk.variableId);
        }
        std::optional<Result<lua::CustomReloadContext>> oldAuthorContext, newAuthorContext;
        for (const auto& next : current.testTriggers) {
            const auto previous = oldTriggers.find(next.symbol.triggerId);
            const auto* old = previous == oldTriggers.end() ? nullptr : previous->second;
            if (old && old->functions == next.functions && old->initializer == next.initializer
                && old->initialization == next.initialization) continue;
            if (next.customCode || (old && old->customCode)) {
                if (!old || !old->customCode || !next.customCode || old->initialization != next.initialization
                    || old->symbol.variable != next.symbol.variable || old->symbol.initializer != next.symbol.initializer) {
                    reject("LUA_RELOAD_CUSTOM_STRUCTURE", "自定义 Lua 块的新增、类型转换或初始化身份变化尚不支持热重载。"); return result;
                }
                if (!oldAuthorContext) oldAuthorContext = AuthorContext(baseline, syntax);
                if (!newAuthorContext) newAuthorContext = AuthorContext(current, syntax);
                if (!oldAuthorContext->Succeeded() || !newAuthorContext->Succeeded()) {
                    const auto& failed = !oldAuthorContext->Succeeded() ? *oldAuthorContext : *newAuthorContext;
                    reject("LUA_RELOAD_CUSTOM_UNSUPPORTED", failed.diagnostics.empty()
                        ? "自定义 Lua 上下文未完成安全分析。" : failed.diagnostics.front().message); return result;
                }
                const auto before = lua::AnalyzeCustomReload({old->authorSource, old->symbol.initializer}, *oldAuthorContext->value, syntax);
                const auto after = lua::AnalyzeCustomReload({next.authorSource, next.symbol.initializer}, *newAuthorContext->value, syntax);
                if (!before.Succeeded() || !after.Succeeded() || before.value->shape != after.value->shape) {
                    reject("LUA_RELOAD_CUSTOM_STRUCTURE", "只支持具名函数体更新；函数声明、形参、顺序或 InitTrig 初始化正文发生变化。"); return result;
                }
                appendDefinition(current, next.functionsOffset, next.functions);
                patch.changes.push_back({next.symbol.triggerId, LuaTestReloadChangeKind::Functions});
                continue;
            }
            const bool recreate = !old || old->initializer != next.initializer || old->initialization != next.initialization
                || old->symbol.variable != next.symbol.variable || old->symbol.initializer != next.symbol.initializer;
            if (recreate && (next.initialization || (old && old->initialization))) {
                reject("LUA_RELOAD_INITIALIZATION", "初始化触发器的新增或事件变化不能在当前这局重跑。"); return result;
            }
            appendDefinition(current, next.functionsOffset, next.functions);
            if (recreate) {
                appendDefinition(current, next.initializerOffset, next.initializer);
                append(operations, "__forge_test_drop(" + lua::Quote(std::to_string(next.symbol.triggerId)) + ")");
                append(operations, next.symbol.initializer + "()");
            }
            patch.changes.push_back({next.symbol.triggerId, !old ? LuaTestReloadChangeKind::Add
                : recreate ? LuaTestReloadChangeKind::Recreate : LuaTestReloadChangeKind::Functions});
        }
        for (const auto& old : baseline.testTriggers) if (!newTriggers.contains(old.symbol.triggerId)) {
            if (old.customCode) {
                reject("LUA_RELOAD_CUSTOM_UNSUPPORTED", "自定义 Lua 触发器没有可靠的已登记句柄。"); return result;
            }
            append(operations, "__forge_test_drop(" + lua::Quote(std::to_string(old.symbol.triggerId)) + ")");
            append(operations, old.symbol.variable + " = nil");
            patch.changes.push_back({old.symbol.triggerId, LuaTestReloadChangeKind::Remove});
        }
        if (!patch.changes.empty() || !patch.newGlobals.empty()) {
            append(patch.source, lua::kLuaTriggerRuntimeSource);
            append(patch.source, "assert(type(__forge_test_triggers) == 'table' and type(__forge_test_drop) == 'function', 'LUA_RELOAD_SESSION_MISSING')");
            for (auto& span : patch.sourceSpans) span.byteOffset += patch.source.size();
            append(patch.source, definitions); append(patch.source, operations);
            auto limited = syntax; limited.maxSourceBytes = (std::min)(limited.maxSourceBytes, kMailboxBytes);
            const auto checked = Lua::CheckSyntax(patch.source, "@Audastra/test-reload.lua", limited);
            if (!checked.Succeeded()) {
                for (const auto& diagnostic : checked.diagnostics) {
                    const auto location = LocateLuaTestReload(patch, diagnostic.line);
                    const auto source = location ? "触发器 " + std::to_string(location->triggerId)
                        + "、节点 " + std::to_string(location->nodeId)
                        + (location->sourceLine ? "、原第 " + std::to_string(location->sourceLine) + " 行" : "")
                        : "差异脚本第 " + std::to_string(diagnostic.line) + " 行";
                    result.diagnostics.push_back(Error(diagnostic.code, "热重载未发送，" + source + "：" + diagnostic.message));
                }
                if (result.diagnostics.empty()) result.diagnostics.push_back(Error("LUA_RELOAD_CHECK_INCOMPLETE", "热重载语法检查未完成，未发送。"));
                return result;
            }
        }
        result.value = std::move(patch);
    } catch (const std::length_error&) { reject("LUA_RELOAD_SOURCE_BUDGET", "差异超过 1 MiB 邮箱预算。"); }
    catch (const std::bad_alloc&) { reject("LUA_RELOAD_ALLOCATION", "准备差异时内存不足。"); }
    return result;
}

std::optional<LuaTestReloadLocation> LocateLuaTestReload(const LuaTestReloadPatch& patch, std::size_t line) {
    if (!line) return {};
    std::size_t start = 0;
    for (std::size_t current = 1; current < line; ++current) {
        const auto newline = patch.source.find('\n', start);
        if (newline == std::string::npos) return {};
        start = newline + 1;
    }
    const auto newline = patch.source.find('\n', start);
    const auto end = newline == std::string::npos ? patch.source.size() : newline;
    const LuaTestReloadSourceSpan* best = nullptr;
    for (const auto& span : patch.sourceSpans) {
        if (span.byteOffset > patch.source.size() || span.byteLength > patch.source.size() - span.byteOffset
            || span.byteOffset >= end || span.byteOffset + span.byteLength <= start) continue;
        if (!best || span.byteLength < best->byteLength) best = &span;
    }
    if (!best) return {};
    const auto offset = (std::max)(start, best->byteOffset);
    return LuaTestReloadLocation{best->triggerId, best->nodeId, best->sourceLine ? best->sourceLine
        + static_cast<std::size_t>(std::count(patch.source.begin() + best->byteOffset, patch.source.begin() + offset, '\n')) : 0};
}
} // namespace War3Map::Script
