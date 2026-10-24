#include "War3MapCore/Services/TerrainBoundsEditService.h"

namespace War3Map::Services {
namespace {
Status CheckExpected(const MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const TerrainEditBinding& expected) {
    const auto state = CaptureTerrainEditState(session, document);
    if (!state.Succeeded() || !state.value) return {state.diagnostics};
    return state.value->binding == expected ? Status::Ok()
        : Status::Failure("Terrain.Edit", "TERRAIN_EDIT_STALE", "文档或地形修订已经变化，请重新查询地图范围。");
}
}

Result<PreparedTerrainBoundsEdit> PrepareTerrainBoundsEdit(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected,
    const MapData::TerrainPathingResources& resources, const MapData::TerrainBoundsEditIntent& intent,
    std::size_t maximumBytes, std::size_t maximumWork) {
    const auto checked = CheckExpected(session, document, expected);
    if (!checked.Succeeded()) return {{}, checked.diagnostics};
    auto plan = session.PrepareTerrainBoundsEdit(resources, intent, maximumBytes, maximumWork);
    if (!plan.Succeeded() || !plan.value) return {{}, std::move(plan.diagnostics)};
    return Result<PreparedTerrainBoundsEdit>::Success(PreparedTerrainBoundsEdit(expected, std::move(*plan.value)));
}

Status ApplyTerrainBoundsEdit(MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const PreparedTerrainBoundsEdit& prepared,
    const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>& prepublish) {
    const auto checked = CheckExpected(session, document, prepared.Binding());
    if (!checked.Succeeded()) return checked;
    return session.ApplyPreparedTerrainBoundsEdit(prepared.Plan(), prepublish);
}

} // namespace War3Map::Services
