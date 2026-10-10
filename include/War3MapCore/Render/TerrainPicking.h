#pragma once

#include "War3MapCore/MapData/MapTerrain.h"

namespace War3Map::Render {

struct TerrainRay {
    std::array<double, 3> origin{};
    std::array<double, 3> direction{0, 0, -1};
};

struct TerrainPickLimits {
    // 默认值也是硬上限，只能收紧。距离为世界单位；零预算允许确定的 AABB 未命中。
    double maxDistance = 1000000000.0;
    std::size_t maxVisitedTiles = 8192;
};

struct TerrainPickHit {
    std::size_t tileIndex = 0; // 原始 grid.tiles 的 row-major 索引
    std::uint32_t tileX = 0;
    std::uint32_t tileY = 0;
    std::uint32_t triangle = 0; // 0=(SW,SE,NW)，1=(SE,NE,NW)，对角线 NW→SE
    std::array<double, 3> position{};
    double distance = 0; // direction 在函数内归一化，因此不是输入 direction 的参数 t
    std::array<double, 3> barycentric{}; // 与上面的三角顶点顺序一致
};

struct TerrainPickResult {
    std::optional<TerrainPickHit> hit;
    std::size_t visitedTiles = 0; // 实际检查的不同 tile 数，不按全图大小计费
};

// 只读拾取 ParseMapTerrain 产生的完整经典高度快照，不借用可变地图状态。
// XY 规则格 DDA + AABB 裁剪，复杂度与射线穿越格子数相关，不扫描整张地图。
// 输入须有限，origin 各分量绝对值 <= 1e9；direction 长度 <= 1e-12 明确拒绝。
// 顶点/格线均包含；1e-6 世界单位以内的同距离命中按 tileIndex、triangle 稳定裁决。
// 不返回负距离。平行且共面相交没有唯一拾取点，明确报 TERRAIN_PICK_COPLANAR。
// 成功且 hit 为空表示确定未命中；输入错误、访问到坏/退化 tile、预算耗尽均失败且无部分命中。
// 此函数只检查快照的整体布局及访问到的格子，要求调用方保留解析器的不可变快照约束。
// 只代表完整原始高度网格，不代表 UI 降采样、原版坡道/悬崖模型、水面或单位碰撞。
[[nodiscard]] Result<TerrainPickResult> PickTerrainHeightPreview(
    const MapData::TerrainGridSnapshot& terrain,
    const TerrainRay& ray,
    const TerrainPickLimits& limits = {});

// 世界 XY 处的地表高度（同拾取的三角划分：对角线 NW→SE）；落在网格外或数据无效时为空。
// 供摆放贴地、区域/镜头标记等编辑叠加共用。
[[nodiscard]] std::optional<float> SampleTerrainHeight(const MapData::TerrainGridSnapshot& terrain, double x, double y);

} // namespace War3Map::Render
