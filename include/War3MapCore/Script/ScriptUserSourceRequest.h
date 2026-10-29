#pragma once
#include "War3MapCore/Script/ScriptUserSourcePlan.h"
namespace War3Map::Script {
// 检查和存取探测共用启用层的显式来源选择，不自行增加目录资格。
[[nodiscard]] ScriptUserSourceRequest MakeModernUserSourceRequest(const ScriptToolchainLayout& layout,
    const Trigger::UiCatalog& catalog, const std::filesystem::path& uiRoot, const std::filesystem::path& mapDirectory);
}
