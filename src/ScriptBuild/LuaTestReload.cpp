#include "War3MapCore/Script/LuaTestReload.h"
#include "War3MapCore/Hash.h"
#include "LuaTriggerEmitter.h"

#include <algorithm>
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
        LuaTestReloadPatch patch;
        patch.baselineScriptSha256 = baseline.scriptSha256; patch.nextScriptSha256 = current.scriptSha256;
        std::string definitions, operations;
        const auto append = [](std::string& output, std::string_view text) {
            if (text.size() > kMailboxBytes - output.size()) throw std::length_error("reload source budget");
            output.append(text); output.push_back('\n');
            if (output.size() > kMailboxBytes) throw std::length_error("reload source budget");
        };
        for (const auto& chunk : current.testGlobals) if (!oldGlobals.contains(chunk.name)) {
            append(definitions, chunk.declaration); append(definitions, chunk.initialization);
            patch.newGlobals.push_back(chunk.variableId);
        }
        for (const auto& next : current.testTriggers) {
            const auto previous = oldTriggers.find(next.symbol.triggerId);
            const auto* old = previous == oldTriggers.end() ? nullptr : previous->second;
            if (old && old->functions == next.functions && old->initializer == next.initializer
                && old->initialization == next.initialization) continue;
            if (next.customCode || (old && old->customCode)) {
                reject("LUA_RELOAD_CUSTOM_UNSUPPORTED", "自定义 Lua 块尚不能证明只重定义函数，不能重复执行其顶层副作用。"); return result;
            }
            const bool recreate = !old || old->initializer != next.initializer || old->initialization != next.initialization
                || old->symbol.variable != next.symbol.variable || old->symbol.initializer != next.symbol.initializer;
            if (recreate && (next.initialization || (old && old->initialization))) {
                reject("LUA_RELOAD_INITIALIZATION", "初始化触发器的新增或事件变化不能在当前这局重跑。"); return result;
            }
            append(definitions, next.functions);
            if (recreate) {
                append(definitions, next.initializer);
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
            append(patch.source, definitions); append(patch.source, operations);
            auto limited = syntax; limited.maxSourceBytes = (std::min)(limited.maxSourceBytes, kMailboxBytes);
            const auto checked = Lua::CheckSyntax(patch.source, "@Audastra/test-reload.lua", limited);
            if (!checked.Succeeded()) {
                for (const auto& diagnostic : checked.diagnostics)
                    result.diagnostics.push_back(Error(diagnostic.code, "热重载未发送，第 "
                        + std::to_string(diagnostic.line) + " 行：" + diagnostic.message));
                if (result.diagnostics.empty()) result.diagnostics.push_back(Error("LUA_RELOAD_CHECK_INCOMPLETE", "热重载语法检查未完成，未发送。"));
                return result;
            }
        }
        result.value = std::move(patch);
    } catch (const std::length_error&) { reject("LUA_RELOAD_SOURCE_BUDGET", "差异超过 1 MiB 邮箱预算。"); }
    catch (const std::bad_alloc&) { reject("LUA_RELOAD_ALLOCATION", "准备差异时内存不足。"); }
    return result;
}
} // namespace War3Map::Script
