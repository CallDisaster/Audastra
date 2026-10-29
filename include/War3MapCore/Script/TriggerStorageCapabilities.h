#pragma once
#include "War3MapCore/Trigger/TriggerValueSafety.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"

namespace War3Map::Script {
struct MapScriptResult;
// 宿主已选择的工具根/目录/地图目录；不会接受Agent声明的支持布尔值。
[[nodiscard]] Result<std::shared_ptr<const Trigger::TriggerStorageCapabilities>> CaptureMapTriggerStorageCapabilities(
    const Trigger::UiCatalog& catalog, const std::filesystem::path& toolRoot,
    const std::filesystem::path& uiRoot, const std::filesystem::path& mapDirectory,
    std::span<const std::string> storageTypes);
// 正常保存的最后兜底；只追加同一分析器的诊断，不修改作者图或脚本文本。
void AppendMapTriggerValueDiagnostics(MapScriptResult& generated, const Trigger::TriggerDocument& document,
    const Trigger::UiCatalog& catalog, const std::filesystem::path& toolRoot,
    const std::filesystem::path& uiRoot, const std::filesystem::path& mapDirectory);
}
