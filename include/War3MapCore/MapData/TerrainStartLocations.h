#pragma once

#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"

namespace War3Map::MapData {

inline constexpr std::size_t TerrainStartSettingsMaximumBytes = 1024u * 1024u;

// 纯codec/语义检查不授予来源；会话只在真实PrepareUnits事务中保存私有状态。
[[nodiscard]] Result<MapSettings> ReadTerrainClassicStartSettings(std::span<const std::byte> bytes);
// 四说明由最终raw草稿单独合并，复合记录仍比较其余完整W3I。
[[nodiscard]] Result<std::vector<std::byte>> CanonicalTerrainStartSettings(std::span<const std::byte> bytes);
[[nodiscard]] Status VerifyTerrainStartLocationsChange(const UnitPlacements& beforeUnits,
    const UnitPlacements& afterUnits, const MapSettings& beforeSettings, const MapSettings& afterSettings);

} // namespace War3Map::MapData
