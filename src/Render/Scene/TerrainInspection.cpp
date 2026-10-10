#include "War3MapCore/Render/TerrainInspection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <string_view>

namespace War3Map::Render {
namespace {

using InspectionResult = Result<TerrainInspectionBlock>;
constexpr std::size_t MaximumTiles = 4096;
constexpr std::size_t MaximumVertices = 8192;
constexpr std::size_t MaximumOutputBytes = 2 * 1024 * 1024;

bool ValidSha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool ValidBinding(const TerrainInspectionBinding& binding) {
    return !binding.documentId.empty() && binding.documentId.size() <= 128
        && binding.documentId.find('\0') == std::string::npos
        && ValidSha(binding.sourceSha256) && ValidSha(binding.terrainSha256);
}

bool ValidTable(const std::vector<std::string>& table) {
    return table.size() <= 256 && std::all_of(table.begin(), table.end(), [](const std::string& id) {
        return id.size() == 4;
    });
}

InspectionResult Failure(const char* code, const char* message) {
    return InspectionResult::Failure("terrain-inspection", code, message);
}

InspectionResult Query(const MapData::MapTerrain& terrain, const TerrainInspectionBinding& currentBinding,
    const TerrainInspectionRequest& request, const TerrainInspectionLimits& limits) {
    if (!ValidBinding(currentBinding) || !ValidBinding(request.expectedBinding)) {
        return Failure("TERRAIN_INSPECT_BINDING_INVALID", "地形查询需要有界文档 ID 与完整的小写 SHA-256 身份。");
    }
    if (currentBinding != request.expectedBinding) {
        return Failure("TERRAIN_INSPECT_STALE", "请求的文档、修订或地形来源已改变，请重新获取快照身份。");
    }
    if (limits.maxTiles == 0 || limits.maxTiles > MaximumTiles
        || limits.maxVertices == 0 || limits.maxVertices > MaximumVertices
        || limits.maxOutputBytes == 0 || limits.maxOutputBytes > MaximumOutputBytes) {
        return Failure("TERRAIN_INSPECT_LIMITS_INVALID", "地形查询预算必须为正且不能超过硬上限。");
    }
    const auto grid = terrain.Snapshot();
    if (!grid) return Failure("TERRAIN_INSPECT_UNAVAILABLE", "尚未取得完整的地形文档。");
    const auto& header = grid->header;
    if (header.VertexRecordBytes() == 0 || header.width < 2 || header.width > 2049
        || header.height < 2 || header.height > 2049
        || grid->vertices.size() != static_cast<std::size_t>(header.width) * header.height
        || grid->tiles.size() != static_cast<std::size_t>(header.width - 1) * (header.height - 1)
        || !ValidTable(header.groundTilesets) || !ValidTable(header.cliffTilesets)) {
        return Failure("TERRAIN_INSPECT_GRID_INVALID", "地形文档必须包含完整的已知版本 W3E 网格和原始材质表。");
    }
    const auto rectangle = request.tiles;
    const auto tileWidth = header.width - 1, tileHeight = header.height - 1;
    // 先比较起点再做减法；恶意 UINT32_MAX 范围不能通过加法溢出变成小区块。
    if (rectangle.width == 0 || rectangle.height == 0
        || rectangle.x >= tileWidth || rectangle.y >= tileHeight
        || rectangle.width > tileWidth - rectangle.x || rectangle.height > tileHeight - rectangle.y) {
        return Failure("TERRAIN_INSPECT_BOUNDS", "请求必须是完全位于原始 tile 范围内的非空矩形。");
    }
    const std::size_t cellCount = static_cast<std::size_t>(rectangle.width) * rectangle.height;
    const std::size_t vertexWidth = static_cast<std::size_t>(rectangle.width) + 1;
    const std::size_t vertexCount = vertexWidth * (static_cast<std::size_t>(rectangle.height) + 1);
    if (cellCount > limits.maxTiles || vertexCount > limits.maxVertices) {
        return Failure("TERRAIN_INSPECT_BUDGET", "区块超过格子或顶点预算，请缩小查询范围。");
    }
    // 所有乘数已被上面的硬上限限制。完整计入 header 表的 string 对象和字符。
    const std::size_t outputBytes = sizeof(TerrainInspectionBlock)
        + currentBinding.documentId.size() + 1 + currentBinding.sourceSha256.size() + 1
        + currentBinding.terrainSha256.size() + 1
        + (header.groundTilesets.size() + header.cliffTilesets.size()) * (sizeof(std::string) + 5)
        + cellCount * sizeof(TerrainInspectionTile) + vertexCount * sizeof(TerrainInspectionVertex);
    if (outputBytes > limits.maxOutputBytes) {
        return Failure("TERRAIN_INSPECT_BUDGET", "区块结果超过字节预算，请缩小查询范围。");
    }

    TerrainInspectionBlock block;
    block.binding = currentBinding;
    block.sourceHeader = header;
    block.tiles = rectangle;
    block.outputBytes = outputBytes;
    block.vertices.reserve(vertexCount);
    block.cells.reserve(cellCount);
    block.minimum.fill(std::numeric_limits<float>::infinity());
    block.maximum.fill(-std::numeric_limits<float>::infinity());
    for (std::uint32_t localY = 0; localY <= rectangle.height; ++localY) {
        for (std::uint32_t localX = 0; localX <= rectangle.width; ++localX) {
            const auto x = rectangle.x + localX, y = rectangle.y + localY;
            const auto sourceIndex = y * header.width + x;
            const auto& vertex = grid->vertices[sourceIndex];
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(vertex.position[axis])) {
                    return Failure("TERRAIN_INSPECT_GRID_INVALID", "地形区块含非有限世界坐标。");
                }
                block.minimum[axis] = std::min(block.minimum[axis], vertex.position[axis]);
                block.maximum[axis] = std::max(block.maximum[axis], vertex.position[axis]);
            }
            if (!std::isfinite(vertex.waterBaseHeight)) {
                return Failure("TERRAIN_INSPECT_GRID_INVALID", "地形区块含非有限基础水位。");
            }
            if (vertex.UnknownWaterFlags() != 0) ++block.unknownWaterFlagVertices;
            if (vertex.UnknownTextureFlags() != 0) ++block.unknownTextureFlagVertices;
            if (vertex.GroundTextureIndex() >= header.groundTilesets.size()) ++block.unresolvedGroundTextureVertices;
            if (vertex.CliffTextureIndex() != 15 && vertex.CliffTextureIndex() >= header.cliffTilesets.size()) {
                ++block.unresolvedCliffTextureVertices;
            }
            block.vertices.push_back({x, y, sourceIndex, vertex});
        }
    }
    for (std::uint32_t localY = 0; localY < rectangle.height; ++localY) {
        for (std::uint32_t localX = 0; localX < rectangle.width; ++localX) {
            const auto x = rectangle.x + localX, y = rectangle.y + localY;
            const auto sourceIndex = y * tileWidth + x;
            const auto& source = grid->tiles[sourceIndex];
            const auto sw = y * header.width + x;
            const std::array<std::uint32_t, 4> corners{sw, sw + 1, sw + header.width, sw + header.width + 1};
            if (source.vertices != corners) {
                return Failure("TERRAIN_INSPECT_GRID_INVALID", "地形区块的原始 tile 索引与规则网格不一致。");
            }
            const auto localSw = localY * (rectangle.width + 1) + localX;
            block.cells.push_back({x, y, sourceIndex, source,
                {localSw, localSw + 1, localSw + rectangle.width + 1, localSw + rectangle.width + 2}});
        }
    }
    return InspectionResult::Success(std::move(block));
}

} // namespace

Result<TerrainInspectionBlock> QueryTerrainBlock(const MapData::MapTerrain& terrain,
    const TerrainInspectionBinding& currentBinding, const TerrainInspectionRequest& request,
    const TerrainInspectionLimits& limits) {
    try {
        return Query(terrain, currentBinding, request, limits);
    } catch (const std::bad_alloc&) {
        return Failure("TERRAIN_INSPECT_ALLOCATION", "无法分配有界地形查询结果。");
    }
}

} // namespace War3Map::Render
