#pragma once

#include "War3MapCore/MapData/MapPlacements.h"

namespace War3Map::MapData {

// 复用唯一摆放codec；调用方必须在任何解析或写出分配前预收完整峰值。
[[nodiscard]] Result<std::size_t> TerrainClassicUnitsWorkingBytes(
    std::size_t beforeBytes, std::size_t afterBytes, std::size_t maximumBytes);
[[nodiscard]] Result<UnitPlacements> ReadTerrainClassicUnitsRoundtrip(std::span<const std::byte> wire);

} // namespace War3Map::MapData
