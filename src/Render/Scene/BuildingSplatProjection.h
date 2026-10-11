#pragma once

#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Services/BuildingSplatService.h"

namespace War3Map::Render::Detail {
struct SplatProjection {
    std::vector<War3RenderModelVertexV2> vertices;
    std::vector<std::uint16_t> indices;
};

// 每片几何只属于一个地形三角面；裁切沿原面的边插值，不能跨过地形折线。
// 预算在增加 owned 顶点前检查，临时裁切多边形只用固定栈空间。
[[nodiscard]] Result<SplatProjection> ProjectBuildingSplat(
    const MapData::TerrainGridSnapshot& terrain,
    const Services::BuildingSplatFootprint& footprint, float maximumCellSize,
    std::size_t maximumVertices, std::size_t maximumIndices);
} // namespace War3Map::Render::Detail
