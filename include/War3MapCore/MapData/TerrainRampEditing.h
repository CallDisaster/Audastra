#pragma once

#include "War3MapCore/MapData/TerrainEditing.h"

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

// 基于原始层级与 ramp 位计算中点高度偏移。无可识别的插值时返回空。
// 渲染抬升与原始地形采样共用；已经抬升的显示副本不能再次累加。
[[nodiscard]] std::optional<float> TerrainRampHeightOffset(
    const TerrainGridSnapshot& terrain, std::uint32_t x, std::uint32_t y);

} // namespace War3Map::MapData
