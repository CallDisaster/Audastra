#include "War3MapCore/MapData/TerrainRampEditing.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"

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

bool IsCanonicalTerrainCornerRampBrush(const TerrainBrush& brush) noexcept {
    const TerrainBrush neutral;
    return brush.kind == TerrainBrushKind::CornerRamp && brush.shape == neutral.shape && brush.radius == neutral.radius
        && brush.groundTexture.empty() && brush.cliffTexture == neutral.cliffTexture && brush.levelDelta == 0
        && brush.strength == neutral.strength && brush.seed == neutral.seed && !brush.anchor && !brush.size
        && !brush.variation && !brush.randomizeVariation && brush.waterHeight == neutral.waterHeight
        && brush.pathingRestrictions == neutral.pathingRestrictions;
}

std::optional<TerrainStrokePoint> SnapTerrainCornerRampPoint(
    const TerrainGridSnapshot& terrain, const TerrainStrokePoint& point) {
    if (!Consistent(terrain) || terrain.header.version != 11) return {};
    const double u = (point.x - terrain.header.offset[0]) / 128.0;
    const double v = (point.y - terrain.header.offset[1]) / 128.0;
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || v < 0
        || u >= terrain.header.width - 1 || v >= terrain.header.height - 1) return {};
    const auto snap = [](double value) { const auto cell = std::floor(value); return cell + (value - cell < 0.5 ? 0.25 : 0.75); };
    return TerrainStrokePoint{terrain.header.offset[0] + 128.0 * snap(u), terrain.header.offset[1] + 128.0 * snap(v)};
}

Result<TerrainCornerRampPlan> PlanTerrainCornerRamp(const TerrainGridSnapshot& terrain, const TerrainStrokePoint& point) {
    const auto fail = [](const char* code, const char* text) {
        return Result<TerrainCornerRampPlan>::Failure("MapTerrain.Edit", code, text);
    };
    if (!Consistent(terrain) || terrain.header.version != 11 || terrain.header.width > 2049 || terrain.header.height > 2049)
        return fail("TERRAIN_CORNER_RAMP_LAYOUT", "拐角坡道需要完整的经典W3E v11网格。");
    const auto& header = terrain.header;
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const double first = header.offset[axis], last = first + 128.0 * ((axis ? header.height : header.width) - 1);
        if (!std::isfinite(first) || std::fmod(first, 128.0) != 0 || first < -32768.0 || last > 32640.0)
            return fail("TERRAIN_CORNER_RAMP_COORDINATE", "拐角坡道只接受128对齐且在原版九位坐标范围内的地形。");
    }
    const auto snapped = SnapTerrainCornerRampPoint(terrain, point);
    if (!snapped || snapped->x != point.x || snapped->y != point.y
        || double(static_cast<float>(point.x)) != point.x || double(static_cast<float>(point.y)) != point.y)
        return fail("TERRAIN_CORNER_RAMP_CANONICAL_POINT", "拐角坡道需要每轴地格内32或96世界单位的单个精确落点。");
    const double u = (point.x - header.offset[0]) / 128.0, v = (point.y - header.offset[1]) / 128.0;
    const int cellX = int(std::floor(u)), cellY = int(std::floor(v));
    const int dx = u - cellX == 0.25 ? 1 : -1, dy = v - cellY == 0.25 ? 1 : -1;
    const int hx = cellX + (dx < 0), hy = cellY + (dy < 0);
    const int lx = hx + 2 * dx, ly = hy + 2 * dy;
    if (std::min(hx, lx) < 0 || std::min(hy, ly) < 0 || std::max(hx, lx) >= int(header.width)
        || std::max(hy, ly) >= int(header.height))
        return fail("TERRAIN_CORNER_RAMP_NEIGHBORHOOD", "拐角坡道九点必须完整位于地图内。");
    const auto& high = terrain.vertices[std::size_t(hy) * header.width + hx];
    if (high.CliffLevel() == 0 || high.CliffLevel() > 14)
        return fail("TERRAIN_CORNER_RAMP_STEP", "当前只支持一级干地高端外拐角。");
    TerrainCornerRampPlan plan; plan.highX = std::uint32_t(hx); plan.highY = std::uint32_t(hy);
    plan.directionX = dx; plan.directionY = dy;
    unsigned rampCount = 0;
    for (int x = 0; x < 3; ++x) for (int y = 0; y < 3; ++y) {
        const auto index = std::uint32_t((hy + y * dy) * int(header.width) + hx + x * dx);
        const auto& vertex = terrain.vertices[index];
        if (vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw || vertex.UnknownWaterFlags()
            || vertex.HasWater() || vertex.IsBoundary() || vertex.IsMapEdge() || vertex.groundHeightRaw > 0x3fff
            || vertex.groundHeightRaw != high.groundHeightRaw
            || vertex.CliffLevel() != high.CliffLevel() - ((x || y) ? 1 : 0))
            return fail("TERRAIN_CORNER_RAMP_STEP", "拐角坡道需要等原始地高的一级干地外角，不能含水、边界或未知位。");
        plan.vertexIndices[std::size_t(x * 3 + y)] = index; rampCount += vertex.IsRamp();
    }
    if (rampCount != 0 && rampCount != 9)
        return fail("TERRAIN_CORNER_RAMP_EXISTING", "已有九点拐角坡道不完整，整笔未修改。");
    plan.footprint = {header.offset[0] + (hx + dx) * 128.0, header.offset[1] + (hy + dy) * 128.0, 192.0,
        std::uint32_t(std::min(hx, lx)), std::uint32_t(std::min(hy, ly)),
        std::uint32_t(std::max(hx, lx) + 1), std::uint32_t(std::max(hy, ly) + 1)};
    return Result<TerrainCornerRampPlan>::Success(plan);
}

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

std::optional<float> TerrainRampHeightOffset(const TerrainGridSnapshot& terrain, std::uint32_t x,
    std::uint32_t y, std::uint8_t maximumCliffLevel) {
    if (!Consistent(terrain) || x >= terrain.header.width || y >= terrain.header.height
        || !terrain.vertices[std::size_t(y) * terrain.header.width + x].IsRamp()) return std::nullopt;
    const auto state = QueryNativeTerrainRamp(terrain, maximumCliffLevel, x, y);
    return state.Succeeded() && state.value ? std::optional<float>{state.value->heightOffset} : std::nullopt;
}

} // namespace War3Map::MapData
