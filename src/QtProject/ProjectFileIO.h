#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace War3Map::QtProject {

// 保存、导入回读和恢复包发布共用完整读取边界；上限与 Core 会话的 512 MiB 总预算一致。
[[nodiscard]] std::optional<std::vector<std::byte>> ReadProjectFileBytes(
    const std::filesystem::path& path);

} // namespace War3Map::QtProject
