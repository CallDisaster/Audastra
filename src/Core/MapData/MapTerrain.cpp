#include "War3MapCore/MapData/MapTerrain.h"
#include "MapTerrainDecode.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

// 按公开格式独立实现，不复制第三方解析器：
// https://github.com/stijnherfst/HiveWE/wiki/war3map.w3e-Terrain
// https://github.com/flowtsohg/mdx-m3-viewer/blob/master/src/parsers/w3x/w3e/corner.ts
// https://github.com/Drake53/War3Net/blob/master/src/War3Net.Build.Core/Serialization/Binary/Environment/TerrainTile.cs
// 前两者采用经典 variation 5+3 分位。ground 的 int16/uint16 解释在后两者中不同，
// 因此保留 uint16 原始字段，仅为 bit15 未置位的共同域提供高度，不猜异常高位数据。
namespace War3Map::MapData {
using namespace TerrainDecode;

Result<MapTerrain> ParseMapTerrain(std::span<const std::byte> bytes, const TerrainReadLimits& limits) {
    Reader reader(bytes);
    try {
        if (!ValidLimits(limits)) reader.Fail("TERRAIN_LIMITS", "调用方只能收紧地形硬预算，坐标预算须为非负有限数");
        if (bytes.size() > limits.maxBytes) reader.Fail("TERRAIN_BYTE_LIMIT", "地形成员超过读取字节预算");
        std::size_t decodedBytes = 0;
        SpendBytes(reader, decodedBytes, bytes.size(), 1, limits.maxDecodedBytes);
        SpendBytes(reader, decodedBytes, 1, sizeof(TerrainGridSnapshot), limits.maxDecodedBytes);
        auto layout = ReadLayout(reader, limits, decodedBytes);
        auto grid = std::make_shared<TerrainGridSnapshot>();
        grid->header = std::move(layout.header);
        const auto& header = grid->header;
        const auto columns = static_cast<std::size_t>(header.width), rows = static_cast<std::size_t>(header.height);
        const auto vertexCount = layout.vertexCount, tileCount = layout.tileCount;
        const auto recordBytes = header.VertexRecordBytes();
        SpendBytes(reader, decodedBytes, vertexCount, sizeof(TerrainVertex), limits.maxDecodedBytes);
        SpendBytes(reader, decodedBytes, tileCount, sizeof(TerrainTile), limits.maxDecodedBytes);
        grid->vertices.reserve(vertexCount);
        grid->tiles.reserve(tileCount);
        std::size_t unknownWaterCount = 0;
        std::size_t firstUnknownWater = 0, firstGroundMiss = 0, firstCliffMiss = 0;
        std::size_t unknownTextureCount = 0, firstUnknownTexture = 0;
        for (std::size_t y = 0; y < rows; ++y) {
            for (std::size_t x = 0; x < columns; ++x) {
                const auto recordOffset = reader.Offset();
                const auto vertex = ReadVertex(reader, header, x, y, limits);
                if (grid->vertices.empty()) grid->minimum = grid->maximum = vertex.position;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    grid->minimum[axis] = std::min(grid->minimum[axis], vertex.position[axis]);
                    grid->maximum[axis] = std::max(grid->maximum[axis], vertex.position[axis]);
                }
                if (vertex.GroundTextureIndex() >= header.groundTilesets.size()) {
                    if (grid->unresolvedGroundTextures++ == 0) firstGroundMiss = recordOffset;
                }
                if (vertex.CliffTextureIndex() != 15 && vertex.CliffTextureIndex() >= header.cliffTilesets.size()) {
                    if (grid->unresolvedCliffTextures++ == 0) firstCliffMiss = recordOffset;
                }
                if (vertex.UnknownWaterFlags() != 0 && unknownWaterCount++ == 0) firstUnknownWater = recordOffset;
                if (vertex.UnknownTextureFlags() != 0 && unknownTextureCount++ == 0) firstUnknownTexture = recordOffset;
                grid->vertices.push_back(vertex);
            }
        }
        for (std::uint32_t y = 0; y + 1 < header.height; ++y) {
            for (std::uint32_t x = 0; x + 1 < header.width; ++x) {
                const auto sw = y * header.width + x;
                grid->tiles.push_back({{sw, sw + 1, sw + header.width, sw + header.width + 1}});
            }
        }
        MapTerrain terrain;
        terrain.original_.assign(bytes.begin(), bytes.end());
        terrain.snapshot_ = std::move(grid);
        auto result = Result<MapTerrain>::Success(std::move(terrain));
        const auto& snapshot = *result.value->Snapshot();
        if (snapshot.unresolvedGroundTextures != 0) AddWarning(result.diagnostics, "TERRAIN_GROUND_TEXTURE_UNRESOLVED",
            std::to_string(snapshot.unresolvedGroundTextures) + " 个顶点的地面纹理索引不在表中；保留原索引", firstGroundMiss, recordBytes);
        if (snapshot.unresolvedCliffTextures != 0) AddWarning(result.diagnostics, "TERRAIN_CLIFF_TEXTURE_UNRESOLVED",
            std::to_string(snapshot.unresolvedCliffTextures) + " 个顶点的悬崖纹理索引未解析；不猜测材质", firstCliffMiss, recordBytes);
        if (unknownWaterCount != 0) AddWarning(result.diagnostics, "TERRAIN_WATER_FLAG_UNKNOWN",
            std::to_string(unknownWaterCount) + " 个顶点保留 water bit15；该位不计入水位高度", firstUnknownWater, recordBytes);
        if (unknownTextureCount != 0) AddWarning(result.diagnostics, "TERRAIN_TEXTURE_FLAG_UNKNOWN",
            std::to_string(unknownTextureCount) + " 个顶点保留 v12 byte B 高六位；不解释其用途", firstUnknownTexture, recordBytes);
        return result;
    } catch (const TerrainError& error) {
        Result<MapTerrain> result;
        result.diagnostics.push_back(error.diagnostic);
        return result;
    } catch (const std::bad_alloc&) {
        return Result<MapTerrain>::Failure("MapTerrain", "TERRAIN_ALLOCATION_FAILED", "无法分配地形只读快照内存");
    }
}

} // namespace War3Map::MapData
