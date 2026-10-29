#pragma once
#include "War3MapCore/Trigger/TriggerValueSafety.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"

namespace War3Map::Script {
// 宿主已选择的工具根/目录/地图目录；不会接受Agent声明的支持布尔值。
[[nodiscard]] Result<std::shared_ptr<const Trigger::TriggerStorageCapabilities>> CaptureMapTriggerStorageCapabilities(
    const Trigger::UiCatalog& catalog, const std::filesystem::path& toolRoot,
    const std::filesystem::path& uiRoot, const std::filesystem::path& mapDirectory,
    std::span<const std::string> storageTypes);
}
