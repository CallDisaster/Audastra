#pragma once

#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"
#include "War3MapCore/MapData/TerrainBoundsEditing.h"

namespace War3Map::MapData {

// 只描述既有槽的替换；追加、删除和表重排不在工程重放域内。
struct TerrainPaletteEditIntent {
    TerrainTextureTable table = TerrainTextureTable::Ground;
    std::uint32_t slot = 0;
    std::string expectedRawcode;
    std::string replacementRawcode;
    std::uint8_t algorithm = 1;
};

struct TerrainStrokeIntent {
    TerrainBrush brush;
    std::vector<TerrainStrokePoint> points;
    bool followDoodads = false;
    // 仅选项；恢复时从本笔当前地形的第一点重新捕获锚点。
    std::optional<TerrainWaterStrokeOptions> waterBrush;
    // 独立范围命令变体：brush必须默认、points为空，不能伪装成地形笔刷。
    std::optional<TerrainBoundsEditIntent> bounds;
    // 独立无采样点命令；实际来源摘要由捕获资源owner生成。
    std::optional<TerrainPaletteEditIntent> palette;
};

} // namespace War3Map::MapData
