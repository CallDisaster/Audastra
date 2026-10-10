#include "War3MapCore/Render/TerrainPreviewMesh.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace War3Map::Render {
namespace {

constexpr std::uint64_t kMaximumVertices = 262144;
constexpr std::uint64_t kMaximumIndices = 1572864;

std::vector<std::uint32_t> Samples(std::uint32_t dimension, std::uint32_t stride) {
    std::vector<std::uint32_t> result;
    for (std::uint32_t index = 0; index < dimension - 1; index += stride) result.push_back(index);
    result.push_back(dimension - 1);
    return result;
}

} // namespace

Result<TerrainPreviewMesh> BuildTerrainPreviewMesh(const MapData::TerrainGridSnapshot& terrain) {
    const auto width = terrain.header.width, height = terrain.header.height;
    if (width < 2 || height < 2 || width > 2049 || height > 2049 ||
        static_cast<std::uint64_t>(width) * height != terrain.vertices.size()) {
        return Result<TerrainPreviewMesh>::Failure("World", "WORLD_GRID_INVALID", "地形顶点尺寸或数量无效。");
    }
    TerrainPreviewMesh output;
    output.sourceWidth = width; output.sourceHeight = height;
    const auto sampleCount = [](std::uint32_t dimension, std::uint32_t stride) {
        return (static_cast<std::uint64_t>(dimension) - 2) / stride + 2;
    };
    while (sampleCount(width, output.stride) * sampleCount(height, output.stride) > kMaximumVertices ||
        (sampleCount(width, output.stride) - 1) * (sampleCount(height, output.stride) - 1) * 6 > kMaximumIndices) ++output.stride;
    const auto xs = Samples(width, output.stride), ys = Samples(height, output.stride);
    output.vertices.reserve(xs.size() * ys.size());
    output.indices.reserve((xs.size() - 1) * (ys.size() - 1) * 6);
    const float low = terrain.minimum[2], high = terrain.maximum[2];
    if (!std::isfinite(low) || !std::isfinite(high) || high < low)
        return Result<TerrainPreviewMesh>::Failure("World", "WORLD_GRID_INVALID", "地形高度边界无效。");
    for (const auto y : ys) for (const auto x : xs) {
        const auto& vertex = terrain.vertices[static_cast<std::size_t>(y) * width + x];
        const auto& position = vertex.position;
        if (std::any_of(position.begin(), position.end(), [](float value) { return !std::isfinite(value) || std::abs(value) > 10000000.0f; }))
            return Result<TerrainPreviewMesh>::Failure("World", "WORLD_COORDINATE_UNSUPPORTED", "地形坐标超过当前渲染范围。");
        // 色彩只表达高度，不能冒充原版地表纹理或水面。
        const float heightRatio = high > low ? std::clamp((position[2] - low) / (high - low), 0.0f, 1.0f) : 0.5f;
        output.vertices.push_back({position[0], position[1], position[2],
            0.18f + heightRatio * 0.38f, 0.30f + heightRatio * 0.38f, 0.23f + heightRatio * 0.31f});
    }
    const auto columns = static_cast<std::uint32_t>(xs.size());
    for (std::uint32_t y = 0; y + 1 < ys.size(); ++y) for (std::uint32_t x = 0; x + 1 < xs.size(); ++x) {
        const auto sw = y * columns + x, se = sw + 1, nw = sw + columns, ne = nw + 1;
        output.indices.insert(output.indices.end(), {sw, se, nw, se, ne, nw});
    }
    return Result<TerrainPreviewMesh>::Success(std::move(output));
}

} // namespace War3Map::Render
