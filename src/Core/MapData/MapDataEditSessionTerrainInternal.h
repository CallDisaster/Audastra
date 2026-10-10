#pragma once

#include "War3MapCore/MapData/MapTerrain.h"

#include <cstdint>
#include <vector>

namespace War3Map::MapData::detail {

// 地形事务和共享撤销路径共用同一原始 7 字节顶点区块写回实现。
void WriteChunk(TerrainGridSnapshot& terrain, std::uint32_t x0, std::uint32_t y0,
    std::uint32_t x1, std::uint32_t y1, const std::vector<std::uint8_t>& raw);

} // namespace War3Map::MapData::detail
