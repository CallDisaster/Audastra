#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"

#include <algorithm>
#include <cmath>

namespace War3Map::MapData {

Result<TerrainPointSample> MapDataEditSession::InspectTerrainPoint(const TerrainStrokePoint& point,
    std::optional<std::uint8_t> actualMaximumCliffLevel) const {
    if (!terrain_)
        return Result<TerrainPointSample>::Failure("MapData.Edit", "MAP_TERRAIN_UNAVAILABLE", "当前会话没有地形。");
    if (actualMaximumCliffLevel && *actualMaximumCliffLevel > 14)
        return Result<TerrainPointSample>::Failure("MapData.Edit", "TERRAIN_HEIGHT_RULE_UNAVAILABLE", "实际地形层级上限不在0..14支持域内。");
    const auto u = (point.x - terrain_->header.offset[0]) / TerrainGridSnapshot::tileSize;
    const auto v = (point.y - terrain_->header.offset[1]) / TerrainGridSnapshot::tileSize;
    const auto sampled = SampleGroundHeight(*terrain_, point.x, point.y, TerrainHeightSource::RawWithRamps, actualMaximumCliffLevel);
    if (!sampled) {
        // 有效坐标上的规则/邻域错误保留原诊断，不能误报为坐标越界。
        const auto width = terrain_->header.width, height = terrain_->header.height;
        if (actualMaximumCliffLevel && width >= 2 && height >= 2 && std::isfinite(u) && std::isfinite(v)
            && u >= 0 && v >= 0 && u <= width - 1 && v <= height - 1) {
            const auto x = std::min(static_cast<std::uint32_t>(u), width - 2);
            const auto y = std::min(static_cast<std::uint32_t>(v), height - 2);
            for (std::uint32_t dy = 0; dy < 2; ++dy) for (std::uint32_t dx = 0; dx < 2; ++dx) {
                const auto state = QueryNativeTerrainRamp(*terrain_, *actualMaximumCliffLevel, x + dx, y + dy);
                if (!state.Succeeded() || !state.value) return {{}, state.diagnostics};
            }
        }
        return Result<TerrainPointSample>::Failure("MapData.Edit", "TERRAIN_QUERY_COORDINATE", "查询坐标非有限数或不在地形范围内。");
    }
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
                output.addedPathingRestrictions = std::to_integer<std::uint8_t>((*pathingRestrictions_)[index - 16]);
            }
        }
    }
    auto result = Result<TerrainPointSample>::Success(std::move(output));
    if (!actualMaximumCliffLevel) {
        const auto x = std::min(static_cast<std::uint32_t>(u), terrain_->header.width - 2);
        const auto y = std::min(static_cast<std::uint32_t>(v), terrain_->header.height - 2);
        bool ramp = false;
        for (std::uint32_t dy = 0; dy < 2; ++dy) for (std::uint32_t dx = 0; dx < 2; ++dx)
            ramp |= terrain_->vertices[static_cast<std::size_t>(y + dy) * terrain_->header.width + x + dx].IsRamp();
        result.value->groundHeightApproximate = ramp;
        if (ramp) result.diagnostics.push_back({DiagnosticSeverity::Warning, "MapData.Edit", "TERRAIN_HEIGHT_APPROXIMATE",
            "未提供同地图实际Terrain.MaxHeight来源，此坡道点高度仅为旧直坡近似。", std::nullopt});
    }
    return result;
}

} // namespace War3Map::MapData
