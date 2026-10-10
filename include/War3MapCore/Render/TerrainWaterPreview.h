#pragma once

#include "War3MapCore/Render/TerrainGroundMesh.h"
#include "War3MapCore/Render/TerrainMaterials.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace War3Map::Render {

struct TerrainWaterPreviewLimits {
    // 只扫描借入的 W3E 快照，不分配逐格输出；上限与解析器一致，调用方只能收紧。
    std::size_t maxTiles = 2048 * 2048;
    std::size_t maxVertices = 2049 * 2049;
    TerrainMaterialLimits materials;
};

struct TerrainWaterPreviewCoverage {
    std::size_t totalTiles = 0;
    std::size_t waterFlaggedVertices = 0;
    std::size_t waterFlaggedTiles = 0; // 四角任一带水，与 BuildTerrainGroundMesh 一致
    std::size_t mixedWaterFlagTiles = 0; // 四角中仅有一至三个带水标记
};

struct TerrainWaterPreview {
    TerrainWaterPreviewCoverage coverage;
    // 无带水原格时不读 Water.slk 或帧，style 为空；这并不表示选中帧已显示。
    std::optional<TerrainWaterStyle> style;
    std::string waterId; // 有水时为 W3E 主 tileset 字母 + "Sha"
    Resources::ResourceLocation waterTableSource; // Water.slk 命中层；贴图来源在 style 内
    int waterTableRow = 0;
    Resources::ResourceLocation frameSource; // 所选帧实际命中的层及成员；SHA 在 style 内
};

// 只读材质样式构建：帧必须由调用方显式指定，不根据 Water.slk texRate 推测播放时钟。
// 有水格时，缺失/非法 waterID、帧越界、贴图缺失/损坏或预算超限均失败且无部分样式；
// 无水格时不读无关的 SLK/BLP，成功结果的 style 为空。成功警告放在 Result.diagnostics。
// 此结果只证明样式与帧资源身份，不证明原版水面几何、岸线、UV 或动画等价。
[[nodiscard]] Result<TerrainWaterPreview> BuildTerrainWaterPreview(
    const MapData::TerrainGridSnapshot& terrain, const Resources::ResourceResolver& resolver,
    std::uint32_t explicitFrameIndex, const TerrainWaterPreviewLimits& limits = {});

// 已有材质目录的入口，供共享场景构建复用同一次 SLK 读取。
[[nodiscard]] Result<TerrainWaterPreview> BuildTerrainWaterPreview(
    const MapData::TerrainGridSnapshot& terrain, const TerrainMaterialCatalog& catalog,
    const Resources::ResourceResolver& resolver, std::uint32_t explicitFrameIndex,
    const TerrainWaterPreviewLimits& limits = {});

} // namespace War3Map::Render
