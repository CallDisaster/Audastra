#pragma once

#include "War3MapCore/Render/TerrainGroundMesh.h"
#include "War3MapCore/Render/TerrainCliffPreview.h"
#include "War3MapCore/Render/TerrainMaterials.h"
#include "War3MapCore/Render/TerrainWaterPreview.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace War3Map::Render {

struct TerrainMaterialPreviewLimits {
    // 默认值也是本入口的硬上限；底层各项预算也只能收紧。
    std::size_t maxTiles = 256 * 256;
    std::size_t maxGroundAtlases = 16;
    std::size_t maxTotalAtlasRgbaBytes = 64 * 1024 * 1024;
    TerrainMaterialLimits materials;
    TerrainAtlasLimits atlas;
    TerrainGroundMeshLimits ground;
    TerrainCliffPreviewLimits cliffs;
};

struct TerrainMaterialPreviewCoverage {
    std::size_t totalTiles = 0;
    std::size_t texturedTiles = 0;
    std::size_t heightColorTiles = 0;
    std::size_t hiddenCliffTiles = 0; // 实际由悬崖/坡道模型覆盖、不再绘制地面的原格
    std::size_t referencedAtlasSlots = 0;
    std::size_t loadedAtlasSlots = 0;
    std::size_t unavailableAtlasSlots = 0;
    std::size_t waterFlaggedTiles = 0; // 四角任一带水的原格
    std::size_t waterRenderedTiles = 0; // 实际进入网格的水面原格；不能以帧读取代替此计数
    bool waterOmitted = false; // 有水而未绘制（未选帧或网格纹理预算不足）
    std::size_t blightApproximationTiles = 0; // 本入口不加载荒芜 atlas。
    TerrainGroundFallbackCounts fallback; // 原因可以重叠，不能求和当作回退格数。
};

struct TerrainMaterialPreview {
    TerrainGroundMesh ground;
    TerrainMaterialPreviewCoverage coverage;
    std::size_t rampAdjustedVertices = 0;
    std::optional<TerrainWaterPreview> water; // 显式选帧才填；含来源、SHA 与 W3E 水标记覆盖
    std::optional<TerrainCliffPreview> cliffs; // 显式启用才填；无实际模型覆盖时不可称为悬崖画面
};

// 输入为 ParseMapTerrain 得到的原始 W3E 快照。内部只复制并抬高坡道顶点，不修改输入。
// 成功时 Result::diagnostics 保留所有局部资源失败和近似的 Warning；调用方须同时公开
// coverage，不能把有高度色回退的结果标为完整材质场景。目录三表、资源来源冲突/读取错误、
// 任一预算错误或所有被引用 atlas 均不可用时失败且不返回网格。
// 默认只构造地面，保持旧的图像与资源身份；显式选择 waterFrameIndex 时才加载 Water.slk
// 对应帧并交给地表网格。帧/水材质失败则无场景，网格省略水面时在 coverage 和诊断中如实报告。
// includeCliffs 为 true 时复用同一材质目录读取悬崖模型，并从地面隐藏已由模型覆盖的格；
// 模型缺失/未知类型保留高度色和诊断。仍不加载荒芜 atlas 或原版昼夜光照；水面 quad、
// 悬崖模型摆放与贴图也未证明原版画面等价。
// 调用方负责固定并复核地图与资源来源身份。
[[nodiscard]] Result<TerrainMaterialPreview> BuildTerrainMaterialPreview(
    const MapData::TerrainGridSnapshot& terrain, const Resources::ResourceResolver& resolver,
    const TerrainMaterialPreviewLimits& limits = {}, const TerrainLighting& lighting = {},
    std::optional<std::uint32_t> waterFrameIndex = std::nullopt, bool includeCliffs = false);

} // namespace War3Map::Render
