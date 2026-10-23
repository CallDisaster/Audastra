#pragma once

#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"

namespace War3Map::MapData {

struct TerrainStrokeIntent {
    TerrainBrush brush;
    std::vector<TerrainStrokePoint> points;
    bool followDoodads = false;
    // 仅选项；恢复时从本笔当前地形的第一点重新捕获锚点。
    std::optional<TerrainWaterStrokeOptions> waterBrush;
};

} // namespace War3Map::MapData
