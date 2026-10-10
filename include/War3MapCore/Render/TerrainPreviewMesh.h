#pragma once

#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/RenderPluginV2.h"

#include <cstdint>
#include <vector>

namespace War3Map::Render {

// 高度色预览网格；不包含地面材质、悬崖、水面或摆放实体。
struct TerrainPreviewMesh {
    std::vector<War3RenderVertexV2> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::uint32_t stride = 1;
};

// 地形快照到渲染器网格的共用只读适配。大地图按固定步长抽样并保留末行/末列。
[[nodiscard]] Result<TerrainPreviewMesh> BuildTerrainPreviewMesh(const MapData::TerrainGridSnapshot& terrain);

} // namespace War3Map::Render
