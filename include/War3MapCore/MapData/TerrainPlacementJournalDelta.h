#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace War3Map::MapData {

// 仅为两种封闭摆放记录共用的wire字段；没有任意成员的公开应用入口。
// 实际读写必须经过各自完整经典codec、双SHA和事务校验。
struct TerrainPlacementJournalDelta {
    std::string beforeSha256, afterSha256;
    std::uint32_t beforeBytes = 0, afterBytes = 0;
    std::uint32_t prefixBytes = 0, removedBytes = 0;
    std::vector<std::byte> inserted;
};

} // namespace War3Map::MapData
