#pragma once
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Trigger/TriggerRemovalAudit.h"

namespace War3Map::Script {
struct RemovalTemplateInputs {
    TemplateContext context;
    std::string identitySha256;
    std::vector<std::string> libraries;
    // 整份执行后补入实际slk地图/默认数据身份；失败不能使用部分覆盖。
    std::function<Result<std::string>()> identityAfterEvaluation;
};
// 与保存共用生产输入、所选档案、owned包含捕获、真实预处理器及受限模板宿主；不执行外部程序。
// inputs中的非触发器数据由宿主冻结保活；候选触发器在每次调用时重新生成。
[[nodiscard]] Result<Trigger::RemovalPreprocessCoverage> CaptureRemovalPreprocessCoverage(
    const MapScriptInputs& inputs, const ScriptFrameworkInputs& framework,
    const Trigger::GraphRemove& removedTarget,
    std::string_view versionString = "1.32.13", bool scriptInjection = true,
    const RemovalTemplateInputs* templates = nullptr);
[[nodiscard]] Result<Trigger::RemovalPreprocessCoverage> CaptureRemovalPreprocessCoverage(
    const MapScriptInputs& inputs, const ScriptUserSourcePlan& entryPlan,
    const ScriptUserSourceRequest& currentSources, const ScriptInjectionSources& injection,
    const Trigger::GraphRemove& removedTarget,
    std::string_view versionString = "1.32.13", bool scriptInjection = true,
    const RemovalTemplateInputs* templates = nullptr);
}
