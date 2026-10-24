#pragma once

#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"

namespace War3Map::MapData {

struct TerrainRampPlan {
    // 从高端向低端排列：一个高端、一个中点、一个低端。
    std::array<std::uint32_t, 3> vertexIndices{};
    TerrainBrushFootprint footprint;
};

// 当前只支持原始地高一致、干地、一级直线台阶。拐角和不规则邻接明确拒绝。
// 查询只读取落点附近的有限邻域，可供光标预览和编辑命令共用。
[[nodiscard]] Result<TerrainRampPlan> PlanTerrainRamp(
    const TerrainGridSnapshot& terrain, const TerrainStrokePoint& point);

// 整笔预检后统一修改 ramp 位；失败不留部分笔划，仍须经过会话的保存门。
[[nodiscard]] Result<TerrainStrokeResult> ApplyTerrainRampStroke(
    TerrainGridSnapshot& terrain, bool enabled, std::span<const TerrainStrokePoint> points);

struct TerrainCornerRampPlan {
    // x外/y内，从高端向两个低端方向各延伸两格。
    std::array<std::uint32_t, 9> vertexIndices{};
    std::uint32_t highX = 0, highY = 0;
    int directionX = 0, directionY = 0;
    TerrainBrushFootprint footprint;
};

// UI明确吸附到每格四分区中心；CLI/MCP直接传canonical点。该吸附不是
// 原版任意浮点坐标的取整模拟，不向半格边界或未知形态授予资格。
[[nodiscard]] std::optional<TerrainStrokePoint> SnapTerrainCornerRampPoint(
    const TerrainGridSnapshot& terrain, const TerrainStrokePoint& point);
// 只查询单点几何与九点原始形态；完整dirty block/halo、实际M、special及
// live合法性和补角闭包由下面的owned事务核对，预览不授予提交资格。
[[nodiscard]] Result<TerrainCornerRampPlan> PlanTerrainCornerRamp(
    const TerrainGridSnapshot& terrain, const TerrainStrokePoint& point,
    TerrainCornerRampAlgorithm algorithm = TerrainCornerRampAlgorithm::WetTerrain);
// 新模式只接受默认brush的kind/value替换，拒绝未取证的paint/size组合。
[[nodiscard]] bool IsCanonicalTerrainCornerRampBrush(const TerrainBrush& brush) noexcept;
[[nodiscard]] Result<TerrainWaterStrokeCandidate> PrepareTerrainCornerRampStroke(
    const TerrainGridSnapshot& source, const TerrainWaterLoadedRules& rules, const TerrainBrush& brush,
    std::span<const TerrainStrokePoint> points, const DoodadPlacements& doodads,
    std::span<const TerrainWaterSpecialDoodadShape> specialShapes = {}, const TerrainWaterBrushLimits& limits = {},
    TerrainCornerRampAlgorithm algorithm = TerrainCornerRampAlgorithm::WetTerrain);

// 基于原始层级与 ramp 位计算中点高度偏移。无可识别的插值时返回空。
// 渲染抬升与原始地形采样共用；已经抬升的显示副本不能再次累加。
[[nodiscard]] std::optional<float> TerrainRampHeightOffset(
    const TerrainGridSnapshot& terrain, std::uint32_t x, std::uint32_t y);

// 已捕获实际Terrain.MaxHeight规则时使用完整原版R/S分类；不猜默认最大层级。
// 非坡道或无法验证的邻域返回空；合法H/L/!等无偏移坡道返回0。
[[nodiscard]] std::optional<float> TerrainRampHeightOffset(
    const TerrainGridSnapshot& terrain, std::uint32_t x, std::uint32_t y, std::uint8_t maximumCliffLevel);

} // namespace War3Map::MapData
