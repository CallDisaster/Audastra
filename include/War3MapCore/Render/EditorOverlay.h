#pragma once

#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/RenderPluginV2.h"

#include <array>
#include <vector>

namespace War3Map::Render {

// 世界视图的编辑叠加线（选择框、区域、镜头标记），供渲染插件 setOverlayLines 使用。
// 只生成几何，不读地图或 Qt；地形为空时贴 z=0。
using OverlayLines = std::vector<War3RenderOverlayLineV2>;
using OverlayColor = std::array<float, 3>;

// 8 个角点（下层 4 点后接上层同序，见 ModelInstanceBox）的 12 条棱。
void AppendBoxLines(OverlayLines& lines, const std::array<std::array<float, 3>, 8>& corners, const OverlayColor& color);

// 贴地矩形轮廓：沿边按不超过 64 单位分段采样地表高度并抬高 lift；无效矩形不输出。
void AppendGroundRectangle(OverlayLines& lines, const MapData::TerrainGridSnapshot* terrain,
    float left, float bottom, float right, float top, const OverlayColor& color, float lift = 4.0f);

// 贴地圆形轮廓（笔刷落点等）：按周长不超过 32 单位分段，至少 24 段；半径须为正。
void AppendGroundCircle(OverlayLines& lines, const MapData::TerrainGridSnapshot* terrain,
    double centerX, double centerY, double radius, const OverlayColor& color, float lift = 4.0f);

// 镜头标记：目标点十字、视线、以及位于镜头处的视锥（按 FOV 与 4:3 画幅，长度取距离的 15%）。
// 目标高度 = 地表高度 + zOffset；角度按原版以度保存（攻击角 304 表示向下 56°）。
void AppendCameraMarker(OverlayLines& lines, const MapData::TerrainGridSnapshot* terrain,
    const MapData::CameraRecord& camera, const OverlayColor& color);

// 区域颜色（文件按 BGR 保存）转为 0..1。
[[nodiscard]] OverlayColor RegionOverlayColor(const MapData::RegionRecord& region);

} // namespace War3Map::Render
