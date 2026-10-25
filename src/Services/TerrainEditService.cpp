#include "War3MapCore/Services/TerrainEditService.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"

#include <algorithm>
#include <limits>

namespace War3Map::Services {
namespace {

Status CheckDocument(const MapDataQueryBinding& document) {
    if (document.documentId.empty() || document.documentId.size() > 128
        || document.documentId.find('\0') != std::string::npos || document.generation == 0
        || document.sourceSha256.size() != 64
        || !std::all_of(document.sourceSha256.begin(), document.sourceSha256.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        })) return Status::Failure("Terrain.Edit", "TERRAIN_DOCUMENT_IDENTITY",
            "地形请求需要明确的文档身份、代次和来源 SHA-256。");
    return Status::Ok();
}

TerrainEditBinding Binding(const MapData::MapDataEditSession& session, const MapDataQueryBinding& document) {
    return {document, session.SessionIdentity(), session.Revision(), session.TerrainSourceRevision()};
}

Status CheckExpected(const MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const TerrainEditBinding& expected) {
    if (const auto checked = CheckDocument(document); !checked.Succeeded()) return checked;
    if (session.TerrainSourceRevision() == std::numeric_limits<std::uint64_t>::max())
        return Status::Failure("Terrain.Edit", "TERRAIN_SOURCE_REVISION_EXHAUSTED", "地形来源修订已用尽，请重新打开地图。");
    if (Binding(session, document) != expected)
        return Status::Failure("Terrain.Edit", "TERRAIN_EDIT_STALE", "文档或地形修订已经变化，请重新查询后再操作。");
    if (!session.Terrain())
        return Status::Failure("Terrain.Edit", "TERRAIN_EDIT_UNAVAILABLE", "当前会话没有可查询的地形。");
    return Status::Ok();
}

} // namespace

Status ValidateTerrainEditBinding(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected) {
    return CheckExpected(session, document, expected);
}

Result<TerrainEditState> CaptureTerrainEditState(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document) {
    if (const auto checked = CheckDocument(document); !checked.Succeeded()) return {{}, checked.diagnostics};
    if (session.TerrainSourceRevision() == std::numeric_limits<std::uint64_t>::max())
        return Result<TerrainEditState>::Failure("Terrain.Edit", "TERRAIN_SOURCE_REVISION_EXHAUSTED", "地形来源修订已用尽，请重新打开地图。");
    const auto terrain = session.Terrain();
    if (!terrain) return Result<TerrainEditState>::Failure("Terrain.Edit", "TERRAIN_EDIT_UNAVAILABLE", "当前会话没有可查询的地形。");
    return Result<TerrainEditState>::Success({Binding(session, document), terrain->header, terrain->vertices.size(),
        session.HasTerrainPathing(), session.VerifyTerrainPathingForSave()});
}

Result<std::vector<MapData::TerrainPointSample>> QueryTerrainPoints(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected,
    std::span<const MapData::TerrainStrokePoint> points) {
    using Output = std::vector<MapData::TerrainPointSample>;
    if (const auto checked = CheckExpected(session, document, expected); !checked.Succeeded()) return {{}, checked.diagnostics};
    if (points.empty() || points.size() > TerrainQueryMaximumPoints)
        return Result<Output>::Failure("Terrain.Edit", "TERRAIN_QUERY_POINT_BUDGET", "单次地形查询需要 1 到 256 个坐标。");
    Output output;
    std::vector<Diagnostic> diagnostics;
    output.reserve(points.size());
    for (const auto& point : points) {
        auto sampled = session.InspectTerrainPoint(point);
        if (!sampled.Succeeded() || !sampled.value) return {{}, std::move(sampled.diagnostics)};
        if (sampled.value->groundHeightApproximate && diagnostics.empty()) diagnostics = sampled.diagnostics;
        output.push_back(std::move(*sampled.value));
    }
    return {std::move(output), std::move(diagnostics)};
}

Result<PreparedTerrainEdit> PrepareTerrainEdit(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected, const MapData::TerrainBrush& brush,
    std::span<const MapData::TerrainStrokePoint> points, bool followDoodads, std::size_t maximumLevelPropagationWork) {
    if (const auto checked = CheckExpected(session, document, expected); !checked.Succeeded()) return {{}, checked.diagnostics};
    if (points.empty() || points.size() > TerrainEditMaximumPoints)
        return Result<PreparedTerrainEdit>::Failure("Terrain.Edit", "TERRAIN_EDIT_POINT_BUDGET", "单次地形编辑需要 1 到 4096 个采样点。");
    // UI 拖动可以掠过视口外；Agent 的显式坐标不能被静默忽略或裁成另一笔。
    for (const auto& point : points) {
        const auto checked = session.InspectTerrainPoint(point);
        if (!checked.Succeeded() || !checked.value) return {{}, checked.diagnostics};
    }
    auto plan = session.PrepareTerrainEdit(brush, points, followDoodads, maximumLevelPropagationWork);
    if (!plan.Succeeded() || !plan.value) return {{}, std::move(plan.diagnostics)};
    return Result<PreparedTerrainEdit>::Success(PreparedTerrainEdit(expected, std::move(*plan.value)));
}

Result<PreparedTerrainEdit> PrepareTerrainWaterEdit(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected,
    const MapData::TerrainPathingResources& resources, const MapData::TerrainWaterStrokeOptions& options,
    std::span<const MapData::TerrainStrokePoint> points, bool followDoodads,
    const MapData::TerrainWaterBrushLimits& limits) {
    if (const auto checked = CheckExpected(session, document, expected); !checked.Succeeded()) return {{}, checked.diagnostics};
    if (points.empty() || points.size() > TerrainEditMaximumPoints)
        return Result<PreparedTerrainEdit>::Failure("Terrain.Edit", "TERRAIN_EDIT_POINT_BUDGET", "单次地形编辑需要 1 到 4096 个采样点。");
    if ((options.kind != MapData::TerrainWaterBrushKind::Shallow && options.kind != MapData::TerrainWaterBrushKind::Deep)
        || (options.shape != MapData::TerrainBrushShape::Circle && options.shape != MapData::TerrainBrushShape::Square)
        || options.size < 1 || options.size > 8 || options.cliffTexture >= 15
        || (options.algorithm != MapData::TerrainWaterAlgorithm::LegacyFlat
            && options.algorithm != MapData::TerrainWaterAlgorithm::NativeRamps))
        return Result<PreparedTerrainEdit>::Failure("Terrain.Edit", "TERRAIN_WATER_BRUSH", "水刷种类、形状、经典尺寸或悬崖槽位非法。");
    if (!resources.WaterBrushRules() || resources.WaterBrushRules()->enforceWaterLimits != options.enforceWaterLimits)
        return Result<PreparedTerrainEdit>::Failure("Terrain.Edit", "TERRAIN_WATER_RULE_OPTIONS", "水刷选项与实际捕获的规则不一致。");
    for (const auto& point : points) {
        const auto checked = session.InspectTerrainPoint(point);
        if (!checked.Succeeded() || !checked.value) return {{}, checked.diagnostics};
    }
    auto anchor = options.algorithm == MapData::TerrainWaterAlgorithm::NativeRamps
        ? MapData::CaptureTerrainWaterStrokeAnchor(*session.Terrain(), points.front(),
            resources.WaterBrushRules()->maximumCliffLevel, limits.maximumWork)
        : MapData::CaptureTerrainWaterStrokeAnchor(*session.Terrain(), points.front());
    if (!anchor.Succeeded() || !anchor.value) return {{}, std::move(anchor.diagnostics)};
    const MapData::TerrainWaterBrush brush{options.kind, options.shape, options.size, options.cliffTexture,
        options.randomSeed, *anchor.value, options.algorithm};
    auto plan = session.PrepareTerrainWaterEdit(resources, brush, points, followDoodads, limits);
    if (!plan.Succeeded() || !plan.value) return {{}, std::move(plan.diagnostics)};
    PreparedTerrainEdit prepared(expected, std::move(*plan.value));
    prepared.waterOptions_ = options;
    return Result<PreparedTerrainEdit>::Success(std::move(prepared));
}

Result<PreparedTerrainEdit> PrepareTerrainCliffEdit(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected,
    const MapData::TerrainPathingResources& resources, const MapData::TerrainBrush& brush,
    std::span<const MapData::TerrainStrokePoint> points, bool followDoodads,
    const MapData::TerrainWaterBrushLimits& limits) {
    if (const auto checked = CheckExpected(session, document, expected); !checked.Succeeded()) return {{}, checked.diagnostics};
    if (points.empty() || points.size() > TerrainEditMaximumPoints)
        return Result<PreparedTerrainEdit>::Failure("Terrain.Edit", "TERRAIN_EDIT_POINT_BUDGET", "单次地形编辑需要 1 到 4096 个采样点。");
    for (const auto& point : points) {
        const auto checked = session.InspectTerrainPoint(point);
        if (!checked.Succeeded() || !checked.value) return {{}, checked.diagnostics};
    }
    auto plan = session.PrepareTerrainCliffEdit(resources, brush, points, followDoodads, limits);
    if (!plan.Succeeded() || !plan.value) return {{}, std::move(plan.diagnostics)};
    return Result<PreparedTerrainEdit>::Success(PreparedTerrainEdit(expected, std::move(*plan.value)));
}

Result<PreparedTerrainEdit> PrepareTerrainCornerRampEdit(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected,
    const MapData::TerrainPathingResources& resources, const MapData::TerrainBrush& brush,
    std::span<const MapData::TerrainStrokePoint> points, bool followDoodads,
    const MapData::TerrainWaterBrushLimits& limits, MapData::TerrainCornerRampAlgorithm algorithm) {
    if (const auto checked = CheckExpected(session, document, expected); !checked.Succeeded()) return {{}, checked.diagnostics};
    if (points.size() != 1)
        return Result<PreparedTerrainEdit>::Failure("Terrain.Edit", "TERRAIN_CORNER_RAMP_POINTS", "拐角坡道每笔必须恰好一个canonical采样点。");
    auto plan = session.PrepareTerrainCornerRampEdit(resources, brush, points, followDoodads, limits, algorithm);
    if (!plan.Succeeded() || !plan.value) return {{}, std::move(plan.diagnostics)};
    return Result<PreparedTerrainEdit>::Success(PreparedTerrainEdit(expected, std::move(*plan.value)));
}

Result<PreparedTerrainEdit> PrepareTerrainStraightRampEdit(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected,
    const MapData::TerrainPathingResources& resources, const MapData::TerrainBrush& brush,
    std::span<const MapData::TerrainStrokePoint> points, bool followDoodads,
    const MapData::TerrainWaterBrushLimits& limits, MapData::TerrainStraightRampAlgorithm algorithm) {
    if (const auto checked = CheckExpected(session, document, expected); !checked.Succeeded()) return {{}, checked.diagnostics};
    if (points.size() != 1)
        return Result<PreparedTerrainEdit>::Failure("Terrain.Edit", "TERRAIN_STRAIGHT_RAMP_POINTS", "一级直坡每笔必须恰好一个canonical采样点。");
    auto plan = session.PrepareTerrainStraightRampEdit(resources, brush, points, followDoodads, limits, algorithm);
    if (!plan.Succeeded() || !plan.value) return {{}, std::move(plan.diagnostics)};
    return Result<PreparedTerrainEdit>::Success(PreparedTerrainEdit(expected, std::move(*plan.value)));
}

Result<std::string> CaptureTerrainBlockFingerprint(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected,
    const MapData::TerrainCellRect& sourceCells, const MapData::TerrainWaterBrushLimits& limits) {
    if (const auto checked = CheckExpected(session, document, expected); !checked.Succeeded()) return {{}, checked.diagnostics};
    return MapData::CaptureTerrainBlockFingerprint(*session.Terrain(), sourceCells, limits);
}

Result<PreparedTerrainEdit> PrepareTerrainBlockPasteEdit(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected,
    const MapData::TerrainPathingResources& resources, const MapData::TerrainBlockPasteIntent& intent,
    bool followDoodads, const MapData::TerrainWaterBrushLimits& limits) {
    if (const auto checked = CheckExpected(session, document, expected); !checked.Succeeded()) return {{}, checked.diagnostics};
    auto plan = session.PrepareTerrainBlockPasteEdit(resources, intent, followDoodads, limits);
    if (!plan.Succeeded() || !plan.value) return {{}, std::move(plan.diagnostics)};
    return Result<PreparedTerrainEdit>::Success(PreparedTerrainEdit(expected, std::move(*plan.value)));
}

Result<MapData::TerrainStrokeResult> ApplyTerrainEdit(MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const PreparedTerrainEdit& prepared,
    const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>& prepublish) {
    if (const auto checked = CheckExpected(session, document, prepared.Binding()); !checked.Succeeded())
        return {{}, checked.diagnostics};
    return session.ApplyPreparedTerrainEdit(prepared.Plan(), prepublish);
}

} // namespace War3Map::Services
