#pragma once

#include "War3MapCore/MapData/MapTerrain.h"

#include <cstdint>
#include <optional>

namespace War3Map::Render {

// 1.27a 的小地图取色按顶点查询相邻四个地格。返回 nullopt 表示该顶点没有
// 可绘制的普通悬崖；坡道不会被误当成悬崖墙面。
[[nodiscard]] Result<std::optional<std::uint8_t>> ClassicMinimapCliffTextureAt(
    const MapData::TerrainGridSnapshot& terrain, std::uint32_t x, std::uint32_t y);

// 返回原始顶点地高与原版坡道抬升：直坡64、已匹配S形坡32/64/96。
// 原版字符!、S细分-1及高端按原版零附加高度分支保留原高度。
// 非法网格、坐标或非有限地高仍失败；输入须未额外抬升且没有运行时变形对象。
[[nodiscard]] Result<double> ClassicMinimapGroundHeightAt(
    const MapData::TerrainGridSnapshot& terrain, std::uint32_t x, std::uint32_t y);

} // namespace War3Map::Render
