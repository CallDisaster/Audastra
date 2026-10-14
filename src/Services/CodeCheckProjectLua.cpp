#include "CodeCheckProjectLua.h"
#include "War3MapCore/Runtime/RuntimeLuaApiInventory.h"
#include "War3MapCore/Services/MapTestService.h"

#include <algorithm>

namespace War3Map::Services::Detail {
void CaptureLuaCheckContext(const std::shared_ptr<const CodeCheckProjectSnapshot>& snapshot,
    std::optional<Trigger::CodeBlockTarget> target, CodeCheckRequest& request, std::stop_token cancel) {
    auto& context = request.context.luaSemantic;
    if (!snapshot) {
        context.runtime.reason = "当前工程尚未冻结F9运行库配置，未验证 jass.* 接口存在性。";
        return;
    }
    if (!snapshot->luaRuntimeRoot.empty()) {
        context.runtime.reason = "F9已显式选择Flat扩展运行时目录；本轮静态接口解析只支持YDWE Tree布局，未回退其他运行库。";
        return;
    }
    const auto selected = MapTestYdweInstallRoot(snapshot->luaRuntimeYdweRoot);
    if (selected.empty()) {
        context.runtime.reason = "F9运行时目录留空，且未配置本机YDWE来源，未验证 jass.* 接口存在性。";
        return;
    }
    if (!selected.is_absolute()) {
        context.runtime.reason = "F9配置的本机YDWE来源不是绝对路径，未按进程工作目录猜测运行库，未验证 jass.* 接口存在性。";
        return;
    }
    // 每次后台检查重新复核原文件SHA；捕获服务只缓存同SHA的静态解析结果。
    const auto inventory = Runtime::CaptureRuntimeLuaApiInventory(selected, {}, cancel, request.lua.deadline);
    if (!inventory || !inventory->trusted) {
        context.runtime.reason = inventory ? inventory->reason : "本机运行库接口清单未能读取。";
        return;
    }
    context.runtime.verified = true;
    context.runtime.binding = inventory->binding;
    context.runtime.reason = "来源：F9配置的本机YDWE Tree（mapTest/runtimeRoot留空时使用ui/dataRoot）。" + inventory->reason;
    context.runtime.moduleRegistryComplete = inventory->modulesComplete;
    for (const auto& module : inventory->modules) {
        context.runtime.presentModules.emplace(module.name);
        if (module.name == "jass.common" && module.commonLookup) {
            context.runtime.commonLookupVerified = true;
            for (const auto& field : module.fields)
                if (field.native && field.argumentCount) context.runtime.commonArguments.emplace(field.name, *field.argumentCount);
        }
    }
    if (!snapshot->triggers) return;
    const auto add = [&](Trigger::CodeBlockTarget blockTarget, const Trigger::RawText& block) {
        if (cancel.stop_requested()) return;
        // 当前草稿由主检查同次编译；其它作者块只读取冻结快照，不借用实时工程。
        if (target && blockTarget.kind == target->kind && blockTarget.triggerId == target->triggerId) return;
        const auto text = Trigger::TriggerCodeEditor::NormalizedText(block);
        if (text.empty()) return;
        const auto definitions = Lua::CollectGlobalDefinitions(text, CodeCheckTargetSourceName(blockTarget, CodeLanguage::Lua53), request.lua);
        if (definitions.syntax.Succeeded()) context.mapGlobals.insert(definitions.declaredGlobals.begin(), definitions.declaredGlobals.end());
    };
    add({Trigger::CodeBlockKind::GlobalHeader, 0}, snapshot->triggers->wct.customCode);
    for (const auto& trigger : snapshot->triggers->triggers)
        if (trigger.type == 0 && trigger.enabled != 0 && trigger.customTextFlag != 0)
            add({Trigger::CodeBlockKind::CustomTrigger, trigger.id}, trigger.customText);
    // GUI初始化、导入运行期脚本与动态模块仍可定义全局；没有全图Lua来源seal时不能说清单完整。
    context.mapGlobalsComplete = false;
}
} // namespace War3Map::Services::Detail

namespace War3Map::Services {
CodeCheckResult CheckProjectLuaSource(const std::shared_ptr<const CodeCheckProjectSnapshot>& snapshot,
    CodeCheckRequest request, std::stop_token cancel, CodeCheckLuaSources lua) {
    request.language = CodeLanguage::Lua53;
    request.context.binding = snapshot ? snapshot->binding : "unbound";
    request.context.luaSemantic = {};
    std::stop_source combined;
    std::stop_callback external(cancel, [&] { combined.request_stop(); });
    std::stop_callback configured(request.lua.cancel, [&] { combined.request_stop(); });
    request.lua.cancel = combined.get_token();
    request.lua.deadline = (std::min)(request.lua.deadline, std::chrono::steady_clock::now()
        + std::clamp(request.lua.maxDuration, std::chrono::milliseconds::zero(), std::chrono::milliseconds(30000)));
    try {
        Detail::CaptureLuaCheckContext(snapshot, {}, request, combined.get_token());
    } catch (...) {
        request.context.luaSemantic = {};
        request.context.luaSemantic.runtime.reason = "本机运行库来源未能完成捕获，未形成语义资格。";
    }
    request.context.luaCatalog = std::move(lua.catalog);
    request.context.luaModules = std::move(lua.modules);
    return CheckCode(request, combined.get_token());
}
} // namespace War3Map::Services
