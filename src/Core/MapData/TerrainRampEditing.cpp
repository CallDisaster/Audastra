#include "War3MapCore/MapData/TerrainRampEditing.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace War3Map::MapData {
namespace {

bool Consistent(const TerrainGridSnapshot& terrain) {
    return terrain.header.width >= 2 && terrain.header.height >= 2
        && static_cast<std::uint64_t>(terrain.header.width) * terrain.header.height == terrain.vertices.size()
        && terrain.vertices.size() <= std::numeric_limits<std::uint32_t>::max();
}

Result<TerrainRampPlan> Rejected(const char* code, const char* text) {
    return Result<TerrainRampPlan>::Failure("MapTerrain.Edit", code, text);
}

} // namespace

Result<TerrainRampPlan> PlanTerrainRamp(const TerrainGridSnapshot& terrain, const TerrainStrokePoint& point) {
    if (!Consistent(terrain) || terrain.header.version != 11)
        return Rejected("TERRAIN_RAMP_LAYOUT", "坡道编辑需要完整的经典 W3E v11 地形");
    const auto& header = terrain.header;
    const double u = (point.x - header.offset[0]) / TerrainGridSnapshot::tileSize;
    const double v = (point.y - header.offset[1]) / TerrainGridSnapshot::tileSize;
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || v < 0 || u >= header.width - 1 || v >= header.height - 1)
        return Rejected("TERRAIN_RAMP_OUTSIDE", "坡道落点必须位于地图内的悬崖地格");
    const auto column = static_cast<std::int64_t>(std::floor(u)), row = static_cast<std::int64_t>(std::floor(v));
    const auto at = [&](std::int64_t x, std::int64_t y) -> const TerrainVertex& {
        return terrain.vertices[static_cast<std::size_t>(y) * header.width + static_cast<std::size_t>(x)];
    };
    const int a = at(column, row).CliffLevel(), b = at(column + 1, row).CliffLevel();
    const int c = at(column, row + 1).CliffLevel(), d = at(column + 1, row + 1).CliffLevel();
    int dx = 0, dy = 0, high = 0, low = 0;
    std::int64_t highX = 0, highY = 0;
    if (a == c && b == d && std::abs(a - b) == 1) {
        dx = a > b ? 1 : -1;
        highX = column + (a > b ? 0 : 1); highY = static_cast<std::int64_t>(std::round(v));
        high = std::max(a, b); low = std::min(a, b);
    } else if (a == b && c == d && std::abs(a - c) == 1) {
        dy = a > c ? 1 : -1;
        highX = static_cast<std::int64_t>(std::round(u)); highY = row + (a > c ? 0 : 1);
        high = std::max(a, c); low = std::min(a, c);
    } else {
        return Rejected("TERRAIN_RAMP_STRAIGHT_STEP", "当前坡道工具只支持一级直线台阶；平地、拐角和多级悬崖不能直接转为坡道");
    }
    if (high > 14) return Rejected("TERRAIN_RAMP_LEVEL", "坡道邻域含未支持的悬崖层级");
    const auto rawHeight = at(highX, highY).groundHeightRaw;
    // 三点之外保留一圈完整直台阶，避免把拐角或另一条坡道的邻接形态改坏。
    for (int tangent = -1; tangent <= 1; ++tangent) {
        int rampVertices = 0;
        for (int step = -1; step <= 3; ++step) {
            const auto x = highX + step * dx - tangent * dy, y = highY + step * dy + tangent * dx;
            if (x < 0 || y < 0 || x >= header.width || y >= header.height)
                return Rejected("TERRAIN_RAMP_NEIGHBORHOOD", "坡道缺少完整邻域，不能贴近地图外缘创建");
            const auto& vertex = at(x, y);
            if (vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw != 0 || vertex.HasWater()
                || vertex.IsBoundary() || vertex.IsMapEdge() || vertex.UnknownWaterFlags() != 0
                || vertex.groundHeightRaw > 0x3fff || vertex.groundHeightRaw != rawHeight
                || vertex.CliffLevel() != (step <= 0 ? high : low))
                return Rejected("TERRAIN_RAMP_NEIGHBORHOOD", "坡道需要等原始地高的干地直台阶，邻域不能包含水、边界或不同方向的层级变化");
            if (vertex.IsRamp()) {
                if (step < 0 || step > 2)
                    return Rejected("TERRAIN_RAMP_EXISTING", "相邻坡道超出本次直坡形态，未修改地形");
                ++rampVertices;
            }
        }
        if (rampVertices != 0 && rampVertices != 3)
            return Rejected("TERRAIN_RAMP_EXISTING", "相邻坡道的三点不完整，不能继续修改");
    }
    TerrainRampPlan plan;
    const auto lowX = highX + 2 * dx, lowY = highY + 2 * dy;
    for (int step = 0; step < 3; ++step)
        plan.vertexIndices[static_cast<std::size_t>(step)] = static_cast<std::uint32_t>(
            (highY + step * dy) * header.width + highX + step * dx);
    auto& footprint = plan.footprint;
    footprint.x0 = static_cast<std::uint32_t>(std::min(highX, lowX));
    footprint.y0 = static_cast<std::uint32_t>(std::min(highY, lowY));
    footprint.x1 = static_cast<std::uint32_t>(std::max(highX, lowX) + 1);
    footprint.y1 = static_cast<std::uint32_t>(std::max(highY, lowY) + 1);
    footprint.centerX = header.offset[0] + (highX + dx) * double(TerrainGridSnapshot::tileSize);
    footprint.centerY = header.offset[1] + (highY + dy) * double(TerrainGridSnapshot::tileSize);
    footprint.radius = 1.5 * TerrainGridSnapshot::tileSize;
    return Result<TerrainRampPlan>::Success(plan);
}

Result<TerrainStrokeResult> ApplyTerrainRampStroke(
    TerrainGridSnapshot& terrain, bool enabled, std::span<const TerrainStrokePoint> points) {
    const auto fail = [](const char* code, const char* message) {
        return Result<TerrainStrokeResult>::Failure("MapTerrain.Edit", code, message);
    };
    if (!Consistent(terrain) || terrain.header.version != 11
        || std::any_of(terrain.vertices.begin(), terrain.vertices.end(), [](const auto& vertex) {
            return vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw != 0;
        })) return fail("TERRAIN_RAMP_LAYOUT", "坡道编辑只支持完整的经典 W3E v11 地形");
    if (points.empty() || points.size() > 65536)
        return fail("TERRAIN_STROKE_POINTS", "笔刷点数量必须在 1 到 65536 之间");
    std::vector<std::uint32_t> planned;
    planned.reserve(points.size() * 3);
    for (const auto& point : points) {
        auto plan = PlanTerrainRamp(terrain, point);
        if (!plan.Succeeded() || !plan.value) return {std::nullopt, std::move(plan.diagnostics)};
        planned.insert(planned.end(), plan.value->vertexIndices.begin(), plan.value->vertexIndices.end());
    }
    std::sort(planned.begin(), planned.end());
    planned.erase(std::unique(planned.begin(), planned.end()), planned.end());
    TerrainStrokeResult result;
    bool changed = false;
    for (const auto index : planned) {
        auto& vertex = terrain.vertices[index];
        if (vertex.IsRamp() == enabled) continue;
        vertex.textureAndFlagsRaw = static_cast<std::uint8_t>(enabled
            ? vertex.textureAndFlagsRaw | 0x10 : vertex.textureAndFlagsRaw & ~0x10);
        const auto x = index % terrain.header.width, y = index / terrain.header.width;
        if (!changed) { result = {x, y, x + 1, y + 1, false}; changed = true; }
        else {
            result.x0 = std::min(result.x0, x); result.y0 = std::min(result.y0, y);
            result.x1 = std::max(result.x1, x + 1); result.y1 = std::max(result.y1, y + 1);
        }
    }
    if (changed) RefreshTerrainDerived(terrain, result.x0, result.y0, result.x1, result.y1);
    return Result<TerrainStrokeResult>::Success(result);
}

std::optional<float> TerrainRampHeightOffset(const TerrainGridSnapshot& terrain, std::uint32_t x, std::uint32_t y) {
    if (!Consistent(terrain) || x >= terrain.header.width || y >= terrain.header.height) return std::nullopt;
    const auto at = [&](std::uint32_t xx, std::uint32_t yy) -> const TerrainVertex& {
        return terrain.vertices[static_cast<std::size_t>(yy) * terrain.header.width + xx];
    };
    const auto& vertex = at(x, y);
    if (!vertex.IsRamp()) return std::nullopt;
    const auto interpolate = [&](const TerrainVertex& first, const TerrainVertex& last) -> std::optional<float> {
        if (!first.IsRamp() || !last.IsRamp()) return std::nullopt;
        const int a = first.CliffLevel(), b = last.CliffLevel(), level = vertex.CliffLevel();
        if (a == b || level < std::min(a, b) || level > std::max(a, b)) return std::nullopt;
        return 64.0f * static_cast<float>(a + b - 2 * level);
    };
    if (x > 0 && x + 1 < terrain.header.width)
        if (auto height = interpolate(at(x - 1, y), at(x + 1, y))) return height;
    if (y > 0 && y + 1 < terrain.header.height)
        return interpolate(at(x, y - 1), at(x, y + 1));
    return std::nullopt;
}

} // namespace War3Map::MapData
