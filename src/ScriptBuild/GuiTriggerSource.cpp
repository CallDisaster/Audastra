#include "War3MapCore/Script/MapScript.h"

#include "ScriptText.h"
#include "TriggerEmitter.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <exception>

namespace War3Map::Script {
Result<GuiTriggerSource> GenerateGuiTriggerSource(const MapScriptInputs& inputs, StableId triggerId) {
    try {
        if (!inputs.triggers || !inputs.catalog)
            return Result<GuiTriggerSource>::Failure("Script.Trigger", "CODE_CONVERSION_CONTEXT", "转换缺少当前触发器文档或函数库。");
        const Trigger::TriggerRecord* target = nullptr;
        for (const auto& trigger : inputs.triggers->triggers) {
            if (trigger.id != triggerId) continue;
            if (target) return Result<GuiTriggerSource>::Failure("Script.Trigger", "CODE_CONVERSION_AMBIGUOUS", "转换目标身份重复。");
            target = &trigger;
        }
        if (!target || target->type != 0 || target->customTextFlag != 0)
            return Result<GuiTriggerSource>::Failure("Script.Trigger", "CODE_CONVERSION_GUI_REQUIRED", "只能转换普通 GUI 触发器。");
        auto validation = Trigger::WtgWctCodec{}.ValidateTrigger(*inputs.triggers, *inputs.catalog, triggerId);
        if (!validation.Succeeded()) return {std::nullopt, std::move(validation.diagnostics)};
        ScriptText output;
        std::vector<Diagnostic> diagnostics;
        detail::TriggerEmitter emitter(*inputs.triggers, *inputs.catalog, output, diagnostics,
            inputs.debug, inputs.nullSafeHandleSet);
        GuiTriggerSource source;
        source.triggerVariable = emitter.TriggerVariable(*target);
        source.initFunction = emitter.InitTriggerName(*target);
        source.effectiveRunOnInit = target->runOnInit != 0 || emitter.HasMapInitEvent(*target);
        emitter.EmitGuiTrigger(*target);
        source.text = output.Take(); source.usesStorage = emitter.UsesStorage();
        auto result = Result<GuiTriggerSource>::Success(std::move(source));
        result.diagnostics = std::move(diagnostics);
        return result;
    } catch (const std::exception& error) {
        return Result<GuiTriggerSource>::Failure("Script.Trigger", "CODE_CONVERSION_FAILED", error.what());
    }
}
} // namespace War3Map::Script
