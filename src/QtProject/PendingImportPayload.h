#pragma once

#include "War3MapCore/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace War3Map { class ProjectSaveControl; }

namespace War3Map::QtProject {
struct PendingImport;

// 工程指纹、恢复和候选保存共用内存/磁盘载荷资格；不以磁盘路径为空代替真实内存来源。
[[nodiscard]] Result<std::string> FingerprintPendingImport(const PendingImport& entry,
    const ProjectSaveControl* control = nullptr);
[[nodiscard]] Result<std::vector<std::byte>> ReadFrozenPendingImport(
    const PendingImport& entry, std::string_view expectedSha256, const ProjectSaveControl* control = nullptr);
} // namespace War3Map::QtProject
