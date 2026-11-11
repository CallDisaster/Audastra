#pragma once

#include "War3MapCore/MapData/TerrainWaterBrush.h"

namespace War3Map::MapData {

inline constexpr std::uint32_t TerrainBlockMaximumSide = 256;

// 半开地格矩形；块载荷包含[x0,x1]×[y0,y1]的完整顶点，不取整世界坐标。
struct TerrainCellRect {
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool operator==(const TerrainCellRect&) const = default;
};

// 粘贴朝向（俯视，x向东、y向北）：先按mirrorX东西翻转源块，再绕块逆时针转quarterTurns个90度。
// 八种朝向各只有这一种写法；地格贴图图案本身不旋转，悬崖由四角实际层级重新决定。
struct TerrainBlockOrientation {
    std::uint8_t quarterTurns = 0;
    bool mirrorX = false;
    bool operator==(const TerrainBlockOrientation&) const = default;
    [[nodiscard]] constexpr bool Identity() const noexcept { return quarterTurns == 0 && !mirrorX; }
    [[nodiscard]] constexpr bool Valid() const noexcept { return quarterTurns < 4; }
    // 以下均为“在当前朝向之后再做一次”的俯视操作。
    [[nodiscard]] constexpr TerrainBlockOrientation CounterClockwise() const noexcept {
        return {std::uint8_t((quarterTurns + 1) % 4), mirrorX};
    }
    [[nodiscard]] constexpr TerrainBlockOrientation Clockwise() const noexcept {
        return {std::uint8_t((quarterTurns + 3) % 4), mirrorX};
    }
    // 东西翻转：M·R^k·M^m = R^-k·M^(m+1)。
    [[nodiscard]] constexpr TerrainBlockOrientation FlippedEastWest() const noexcept {
        return {std::uint8_t((4 - quarterTurns) % 4), !mirrorX};
    }
    // 南北翻转 = 东西翻转后转半圈：R^2·M·R^k·M^m = R^(2-k)·M^(m+1)。
    [[nodiscard]] constexpr TerrainBlockOrientation FlippedNorthSouth() const noexcept {
        return {std::uint8_t((6 - quarterTurns) % 4), !mirrorX};
    }
};

// 同图、同有序palette、绝对原始高度；不搬边界、外缘、WPM或对象。
// 指纹绑定本笔before的源块，不是来源或保存资格。
// algorithm 1：原朝向逐顶点复制；algorithm 2：非原朝向，按orientation重排顶点。
struct TerrainBlockPasteIntent {
    std::uint8_t algorithm = 1;
    TerrainCellRect sourceCells;
    std::array<std::uint32_t, 2> destinationVertex{};
    std::string expectedBlockSha256;
    TerrainBlockOrientation orientation;
    bool operator==(const TerrainBlockPasteIntent&) const = default;
};

// 按朝向选择唯一算法号；调用方不用自己维护“原朝向只能写算法1”的约定。
[[nodiscard]] TerrainBlockPasteIntent MakeTerrainBlockPasteIntent(const TerrainCellRect& sourceCells,
    std::array<std::uint32_t, 2> destinationVertex, std::string expectedBlockSha256,
    TerrainBlockOrientation orientation = {});
// 目标半开地格矩形；奇数个90度时宽高互换。未校验的意图也只做纯算术，不越界读取。
[[nodiscard]] TerrainCellRect TerrainBlockPasteTarget(const TerrainBlockPasteIntent& intent) noexcept;
// 源块内顶点(x,y)（相对源西南角，0..宽、0..高）落到目标块内的相对顶点。
[[nodiscard]] std::array<std::uint32_t, 2> TerrainBlockOrientVertex(std::uint32_t width, std::uint32_t height,
    TerrainBlockOrientation orientation, std::uint32_t x, std::uint32_t y) noexcept;

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
