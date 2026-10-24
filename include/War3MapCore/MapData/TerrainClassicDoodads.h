#pragma once

#include "War3MapCore/MapData/MapPlacements.h"

namespace War3Map::MapData {

// 调用方须先预收整对codec树、原始wire副本、writer临时峰值和散列容量。
// 普通DOO保存证明与工程重放共用此计量及同一经典codec。
[[nodiscard]] Result<std::size_t> TerrainClassicDoodadsWorkingBytes(
    std::size_t beforeBytes, std::size_t afterBytes, std::size_t maximumBytes);
[[nodiscard]] Result<DoodadPlacements> ReadTerrainClassicDoodadsRoundtrip(std::span<const std::byte> wire);

// 仅比较经典DOO头和EOF的完整特殊尾；相同尾段不创建特殊模型来源。
[[nodiscard]] Status VerifyTerrainOrdinaryDoodadsTransition(
    const DoodadPlacements& before, const DoodadPlacements& after);

} // namespace War3Map::MapData
