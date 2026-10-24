#pragma once

#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"
#include "War3MapCore/MapData/TerrainBoundsEditing.h"

namespace War3Map::MapData {

// algorithm1替换既有槽；algorithm2仅追加Ground表尾，旧rawcode为空且slot等于旧表长。
// 不描述删除、重排、主tileset改变或悬崖槽追加；两种算法共用FTJ6的显式版本字段。
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
    // 仅CornerRamp可取2；其余命令保持1。不是来源授权，仍须实际规则owner。
    TerrainCornerRampAlgorithm cornerRampAlgorithm = TerrainCornerRampAlgorithm::DryOnly;
};

} // namespace War3Map::MapData
