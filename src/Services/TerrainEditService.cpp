#include "War3MapCore/Services/TerrainEditService.h"

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
    output.reserve(points.size());
    for (const auto& point : points) {
        auto sampled = session.InspectTerrainPoint(point);
        if (!sampled.Succeeded() || !sampled.value) return {{}, std::move(sampled.diagnostics)};
        output.push_back(std::move(*sampled.value));
    }
    return Result<Output>::Success(std::move(output));
}

Result<PreparedTerrainEdit> PrepareTerrainEdit(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected, const MapData::TerrainBrush& brush,
    std::span<const MapData::TerrainStrokePoint> points, bool followDoodads) {
    if (const auto checked = CheckExpected(session, document, expected); !checked.Succeeded()) return {{}, checked.diagnostics};
    if (points.empty() || points.size() > TerrainEditMaximumPoints)
        return Result<PreparedTerrainEdit>::Failure("Terrain.Edit", "TERRAIN_EDIT_POINT_BUDGET", "单次地形编辑需要 1 到 4096 个采样点。");
    // UI 拖动可以掠过视口外；Agent 的显式坐标不能被静默忽略或裁成另一笔。
    for (const auto& point : points) {
        const auto checked = session.InspectTerrainPoint(point);
        if (!checked.Succeeded() || !checked.value) return {{}, checked.diagnostics};
    }
    auto plan = session.PrepareTerrainEdit(brush, points, followDoodads);
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
