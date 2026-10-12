#pragma once

#include "War3MapCore/Services/ObjectResourceQuery.h"
#include "War3MapCore/Services/ImportInventoryQuery.h"

namespace War3Map::Services {
// UI/stdio共用目录来源读取。仅使用快照的literal路径；改名/删除沿既有导入覆盖。
// 未冻结的新导入明确拒绝，不回退游戏资源。读取前后核验完整目录来源。
[[nodiscard]] Result<ObjectResourceInfo> InspectDirectoryObjectResource(const DirectoryWorkspace& directory,
    const std::filesystem::path& gameRoot, const ImportInventoryBinding& binding,
    const ImportInventoryOverlay& overlay, std::string_view value, ObjectResourceKind kind);
}
