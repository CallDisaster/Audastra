#include "War3MapCore/MapData/MapDataEditSession.h"

#include <cmath>

namespace War3Map::MapData {

Result<TerrainPointSample> MapDataEditSession::InspectTerrainPoint(const TerrainStrokePoint& point) const {
    if (!terrain_)
        return Result<TerrainPointSample>::Failure("MapData.Edit", "MAP_TERRAIN_UNAVAILABLE", "当前会话没有地形。");
    const auto sampled = SampleGroundHeight(*terrain_, point.x, point.y, TerrainHeightSource::RawWithRamps);
    if (!sampled)
        return Result<TerrainPointSample>::Failure("MapData.Edit", "TERRAIN_QUERY_COORDINATE", "查询坐标非有限数或不在地形范围内。");
    const auto u = (point.x - terrain_->header.offset[0]) / TerrainGridSnapshot::tileSize;
    const auto v = (point.y - terrain_->header.offset[1]) / TerrainGridSnapshot::tileSize;
    TerrainPointSample output;
    output.point = point; output.groundHeight = *sampled;
    output.vertexX = static_cast<std::uint32_t>(std::round(u));
    output.vertexY = static_cast<std::uint32_t>(std::round(v));
    output.vertex = terrain_->vertices[static_cast<std::size_t>(output.vertexY) * terrain_->header.width + output.vertexX];
    const auto ground = output.vertex.GroundTextureIndex(), cliff = output.vertex.CliffTextureIndex();
    if (ground < terrain_->header.groundTilesets.size()) output.groundTexture = terrain_->header.groundTilesets[ground];
    if (cliff != 15 && cliff < terrain_->header.cliffTilesets.size()) output.cliffTexture = terrain_->header.cliffTilesets[cliff];
    if (pathing_) {
        const auto width = (terrain_->header.width - 1) * 4;
        const auto height = (terrain_->header.height - 1) * 4;
        const auto x = std::floor(u * 4), y = std::floor(v * 4);
        if (x < 0 || y < 0 || x >= width || y >= height) {
            output.pathingState = TerrainPathingState::Outside;
        } else {
            // AttachPathing/SetPathing 已验证 MP3W 头与 4×tile 关系；逐点读取不再整图复制解析。
            output.pathingX = static_cast<std::uint32_t>(x); output.pathingY = static_cast<std::uint32_t>(y);
            const auto index = 16 + static_cast<std::size_t>(output.pathingY) * width + output.pathingX;
            if (index >= pathing_->size())
                return Result<TerrainPointSample>::Failure("MapData.Edit", "MAP_PATHING_SHAPE_CHANGED", "当前 WPM 长度与地形不一致。");
            output.pathingState = TerrainPathingState::Available;
            output.pathingFlags = std::to_integer<std::uint8_t>((*pathing_)[index]);
            if (pathingOriginal_ && index < pathingOriginal_->size()) {
                output.originalPathingFlags = std::to_integer<std::uint8_t>((*pathingOriginal_)[index]);
                output.addedPathingRestrictions = static_cast<std::uint8_t>(
                    *output.pathingFlags & ~*output.originalPathingFlags & kPathingRestrictionBits);
            }
        }
    }
    return Result<TerrainPointSample>::Success(std::move(output));
}

} // namespace War3Map::MapData
