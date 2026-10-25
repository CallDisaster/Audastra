#pragma once

#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"
#include "War3MapCore/MapData/TerrainBoundsEditing.h"
#include "War3MapCore/MapData/TerrainBlockEditing.h"
#include "War3MapCore/MapData/TerrainResizeEditing.h"

namespace War3Map::MapData {

// algorithm1替换既有槽；algorithm2追加Ground表尾；algorithm3追加Cliff表尾。
// 追加时旧rawcode为空且slot等于旧表长；不描述删除、重排或主tileset改变，保持FTJ6布局。
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
    // 独立绝对高度块命令；仅携源矩形、目标和内容摘要，重放从当时before读取实际源块。
    std::optional<TerrainBlockPasteIntent> blockPaste;
    // FTJG独立外矩形扩裁；相对当笔before，不能夹带普通笔刷或raw网格。
    std::optional<TerrainResizeIntent> resize;
    // 仅CornerRamp可取2；其余命令保持1。不是来源授权，仍须实际规则owner。
    TerrainCornerRampAlgorithm cornerRampAlgorithm = TerrainCornerRampAlgorithm::DryOnly;
    // FTJB及旧请求固定独立三点；Connected须显式记入新wire，不能扩展旧历史接受域。
    TerrainStraightRampAlgorithm straightRampAlgorithm = TerrainStraightRampAlgorithm::Independent;
};

} // namespace War3Map::MapData
