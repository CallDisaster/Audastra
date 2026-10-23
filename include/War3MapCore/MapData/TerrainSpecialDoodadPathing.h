#pragma once

#include "War3MapCore/MapData/TerrainDoodadFootprint.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"

namespace War3Map::MapData {

// 顺序必须与完整special DOO段相同。无footprint仅表示实际pathTex getter明确禁用。
// 像素借用来源捕获者的原字节，本函数不提供文件来源或保存资格。
struct TerrainSpecialDoodadPathingInput {
    const SpecialDoodadPlacement* placement = nullptr;
    std::optional<TerrainDoodadFootprint> footprint;
};

// 与通行层共用同一alpha解码和资格检查，供真实水刷owner建立逐记录形状。
[[nodiscard]] Result<std::vector<TerrainWaterSpecialDoodadShape>> DescribeTerrainSpecialDoodads(
    const TerrainGridSnapshot& terrain, std::span<const TerrainSpecialDoodadPathingInput> inputs,
    std::uint8_t maximumCliffLevel, std::size_t maximumWorkingBytes);

// 完整WPM大小的特殊层：bit0表示覆盖，0xCE是原版通道==255的禁止位。
// 保留登记顺序的首个覆盖；旧/新地形的层级图案都必须与alpha一致，
// 不能保留原版会删除的特殊模型。最大层级来自实际Terrain.MaxHeight。
[[nodiscard]] Result<std::vector<std::uint8_t>> BuildTerrainSpecialDoodadPathing(
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    std::span<const TerrainSpecialDoodadPathingInput> inputs, std::uint8_t maximumCliffLevel,
    std::size_t maximumWorkingBytes);

} // namespace War3Map::MapData
