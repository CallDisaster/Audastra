#pragma once

#include "War3MapCore/MapData/TerrainWaterBrush.h"

namespace War3Map::MapData {

inline constexpr std::uint32_t TerrainBlockMaximumSide = 256;

// 半开地格矩形；块载荷包含[x0,x1]×[y0,y1]的完整顶点，不取整世界坐标。
struct TerrainCellRect {
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool operator==(const TerrainCellRect&) const = default;
};

// 同图、同有序palette、绝对原始高度；不搬边界、外缘、WPM或对象。
// 指纹绑定本笔before的源块，不是来源或保存资格。
struct TerrainBlockPasteIntent {
    std::uint8_t algorithm = 1;
    TerrainCellRect sourceCells;
    std::array<std::uint32_t, 2> destinationVertex{};
    std::string expectedBlockSha256;
    bool operator==(const TerrainBlockPasteIntent&) const = default;
};

// 仅结构检查；journal和宿主重放共用，不读取地形或授予来源资格。
[[nodiscard]] Status ValidateTerrainBlockPasteIntent(const TerrainBlockPasteIntent& intent);
[[nodiscard]] Result<std::string> CaptureTerrainBlockFingerprint(const TerrainGridSnapshot& terrain,
    const TerrainCellRect& cells, const TerrainWaterBrushLimits& limits = {});

struct TerrainBlockStrokeCandidate {
    TerrainGridSnapshot terrain;
    TerrainWaterBrushResult result;
    std::size_t peakBytes = 0;
};

// 纯owned候选。实际规则和special形状须由会话从本次真实owner取得；
// 源/目标完整二圈只接受known/no-ramp/no-boundary/no-edge的普通一级接缝。
[[nodiscard]] Result<TerrainBlockStrokeCandidate> PrepareTerrainBlockPasteStroke(
    const TerrainGridSnapshot& source, const TerrainWaterLoadedRules& rules,
    const TerrainBlockPasteIntent& intent, std::span<const TerrainWaterSpecialDoodadShape> specialShapes = {},
    const TerrainWaterBrushLimits& limits = {});

} // namespace War3Map::MapData
