#pragma once

#include "War3MapCore/MapData/TerrainWaterBrush.h"

namespace War3Map::MapData {

// 普通悬崖笔划：固定起点层级，使用真实地形规则及与水刷共用的原版后处理。
// 不建立水面、不执行水刷的扩大原始地高平台。
struct TerrainCliffBrush {
    TerrainBrushShape shape = TerrainBrushShape::Circle;
    std::uint32_t size = 1;
    std::uint8_t cliffTexture = 0;
    std::uint32_t randomSeed = 0;
    TerrainStrokeAnchor anchor;
    int levelDelta = 0;
};

[[nodiscard]] Result<TerrainWaterStrokeCandidate> PrepareTerrainCliffStroke(
    const TerrainGridSnapshot& source, const TerrainWaterLoadedRules& rules, const TerrainCliffBrush& brush,
    std::span<const TerrainStrokePoint> points, const DoodadPlacements& doodads,
    std::span<const TerrainWaterSpecialDoodadShape> specialShapes = {}, const TerrainWaterBrushLimits& limits = {});

} // namespace War3Map::MapData
