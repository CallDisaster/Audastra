#pragma once

#include <algorithm>
#include <span>
#include <string>

namespace War3Map::MapData::TerrainPaletteEditDetail {

// 表长不同时只接受完整前缀的末尾增减；减项用于保存后Undo，不放行中段删槽或重排。
// 15是顶点的无悬崖哨兵，最多15条可用材质，实际旧/新行仍由资源owner分别核对。
inline bool SupportsCliffTableDelta(std::span<const std::string> before, std::span<const std::string> after) {
    if (before.empty() || after.empty() || before.size() > 15 || after.size() > 15) return false;
    const auto common = std::min(before.size(), after.size());
    return before.size() == after.size() || std::equal(before.begin(), before.begin() + common, after.begin());
}

} // namespace War3Map::MapData::TerrainPaletteEditDetail
