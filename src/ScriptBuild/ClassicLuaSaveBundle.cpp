#include "War3MapCore/Script/ClassicLuaSaveBundle.h"
#include "War3MapCore/Script/ClassicLuaCompatibility.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Lua/LuaSyntax.h"
#include "LuaMapScriptInternal.h"
#include "MapSections.h"
#include "ScriptNames.h"

#include <algorithm>
#include <set>

namespace War3Map::Script {
namespace {
constexpr std::size_t kBundleBytes = 64 * 1024 * 1024;
bool Identifier(std::string_view value) {
    if (value.empty() || value.size() > 256) return false;
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    return letter(value.front()) && std::all_of(value.begin(), value.end(), [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
}
} // namespace

bool ClassicLuaSaveBundle::Succeeded() const noexcept {
    return !jassEntry.empty() && modules.size() == 5 && std::none_of(diagnostics.begin(), diagnostics.end(),
        [](const auto& d) { return d.severity == DiagnosticSeverity::Error; });
}

ClassicLuaSaveBundle BuildClassicLuaSaveBundle(const MapInitializationPlan& plan,
    const LuaMapScriptResult& generated, const LuaTargetProfile& target) {
    ClassicLuaSaveBundle result;
    const auto fail = [&](const char* code, std::string message) {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.ClassicLua", code, std::move(message), {}});
    };
    try {
        if (plan.settings.formatVersion != 25 || plan.settings.SourceFormatVersion() != 25
            || plan.playerSlotCount != 12 || target.nativeAuthorCode) {
            fail("CLASSIC_LUA_SOURCE_UNSUPPORTED", "经典Lua档案仅接受真实v25、12槽和纯T输入；不升级设置或猜测作者代码语言。");
            return result;
        }
        if (!generated.Succeeded() || generated.targetIdentity != target.identity
            || generated.classicObjectsContinuation != "AudastraMainObjects"
            || generated.classicMainContinuation != "AudastraMainAfterBlizzard") {
            fail("CLASSIC_LUA_PHASES_MISSING", "输入不是完整且明确分段的经典Lua生成结果。");
            return result;
        }
        const auto mapping = lua::ComputeLuaMapSourceMapSha256(generated);
        if (!mapping.Succeeded() || !mapping.value || *mapping.value != generated.sourceMapSha256) {
            fail("CLASSIC_LUA_GENERATED_CHANGED", "Lua正文、来源映射或阶段身份在生成后变化。");
            return result;
        }
        const auto compatibility = BuildClassicLuaCompatibility(target);
        result.diagnostics = compatibility.diagnostics;
        if (!compatibility.Succeeded()) return result;
        std::set<std::string> allowed;
        for (const auto* names : {&generated.requiredApis, &generated.requiredGlobals, &generated.requiredConstants}) {
            for (const auto& name : *names) {
                if (!Identifier(name)) {
                    fail("CLASSIC_LUA_BINDING_INVALID", "Lua目标包含无效的公开符号名。");
                    return result;
                }
                allowed.insert(name);
            }
        }
        // Lua配置不执行，不能将它的局部槽登记混入实际JASS大厅配置。
        for (const auto& api : generated.triggers.requiredApis) {
            if (api == "SetPlayerSlotAvailable" || api == "InitGenericPlayerSlots" || api == "InitBlizzard"
                || api == "SetAmbientDaySound" || api == "SetAmbientNightSound") {
                fail("CLASSIC_LUA_JASS_STATE_REQUIRED", "该T调用依赖未共享的JASS状态，经典Lua档案暂不支持：" + api);
                return result;
            }
        }
        std::string bootstrap = "local common = require('jass.common')\nlocal compat = require('audastra.compat')(common)\nlocal allowed = {\n";
        for (const auto& name : allowed) bootstrap += "    ['" + name + "'] = true,\n";
        bootstrap += "}\nassert(getmetatable(_G) == nil, 'CLASSIC_LUA_GLOBAL_ENVIRONMENT')\n"
            "setmetatable(_G, {__index = function(_, name)\n"
            "    if not allowed[name] then return nil end\n"
            "    local value = compat[name]\n"
            "    if value ~= nil then return value end\n"
            "    return common[name]\n"
            "end})\nrequire('audastra.map')\nmain()\n";
        std::vector<ClassicLuaModule> modules{
            {"audastra\\map.lua", generated.text, {}},
            {"audastra\\compat.lua", compatibility.moduleText, {}},
            {"audastra\\bootstrap.lua", std::move(bootstrap), {}},
            {"audastra\\objects.lua", "AudastraMainObjects()\n", {}},
            {"audastra\\continue.lua", "AudastraMainAfterBlizzard()\n", {}}};
        std::size_t total = 0;
        for (auto& module : modules) {
            if (module.text.size() > kBundleBytes - total) {
                fail("CLASSIC_LUA_BUNDLE_BUDGET", "经典Lua地图模块超出64MiB总预算。");
                return result;
            }
            total += module.text.size();
            Lua::SyntaxOptions options;
            options.maxSourceBytes = kBundleBytes;
            options.maxMemoryBytes = 256 * 1024 * 1024;
            const auto syntax = Lua::CheckSyntax(module.text, "@" + module.member, options);
            if (!syntax.Succeeded()) {
                for (const auto& d : syntax.diagnostics) fail("CLASSIC_LUA_MODULE_SYNTAX", module.member + ":" + std::to_string(d.line) + ": " + d.message);
                if (syntax.diagnostics.empty()) fail("CLASSIC_LUA_MODULE_SYNTAX", module.member + "：语法检查未完成。");
                return result;
            }
            const auto hash = Sha256Text(module.text);
            if (!hash.Succeeded() || !hash.value) {
                fail("CLASSIC_LUA_MODULE_HASH", "无法取得Lua模块身份。"); return result;
            }
            module.sha256 = *hash.value;
        }
        MapScriptInputs input;
        input.settings = &plan.settings;
        input.strings = plan.strings ? &*plan.strings : nullptr;
        ScriptText entry;
        entry.Line("// Audastra classic Lua entry; lobby configuration stays in JASS.");
        detail::MapContext context{input, entry, result.diagnostics, {}, {}};
        detail::WritePlayers(context);
        detail::WriteConfig(context);
        entry.Line("function main takes nothing returns nothing");
        entry.Begin();
        entry.Line("call Cheat(\"exec-lua: \\\"audastra.bootstrap\\\"\")");
        entry.Line("call SetAmbientDaySound(" + detail::QuoteJassString(plan.ambientSound + "Day") + ")");
        entry.Line("call SetAmbientNightSound(" + detail::QuoteJassString(plan.ambientSound + "Night") + ")");
        entry.Line("call SetMapMusic(\"Music\", true, 0)");
        entry.Line("call Cheat(\"exec-lua: \\\"audastra.objects\\\"\")");
        entry.Line("call InitBlizzard()");
        entry.Line("call Cheat(\"exec-lua: \\\"audastra.continue\\\"\")");
        entry.End(); entry.Line("endfunction");
        if (std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& d) { return d.severity == DiagnosticSeverity::Error; })) return result;
        const auto hash = Sha256Text(entry.Text());
        if (!hash.Succeeded() || !hash.value) { fail("CLASSIC_LUA_ENTRY_HASH", "无法取得JASS入口身份。"); return result; }
        result.jassEntry = entry.Take(); result.jassSha256 = *hash.value;
        result.generatedScriptSha256 = generated.scriptSha256;
        result.sourceMapSha256 = generated.sourceMapSha256;
        result.modules = std::move(modules);
        return result;
    } catch (const std::exception&) {
        fail("CLASSIC_LUA_BUNDLE_FAILED", "经典Lua模块包生成失败，未发布部分结果。");
        result.jassEntry.clear(); result.modules.clear();
        return result;
    }
}
} // namespace War3Map::Script
