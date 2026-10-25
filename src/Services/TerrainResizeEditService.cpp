#include "War3MapCore/Services/TerrainResizeEditService.h"

#include "War3MapCore/MapData/TerrainPathingResources.h"

namespace War3Map::Services {

Result<MapData::TerrainResizeResources> TerrainResizeEditService::Capture(const MapArchiveSession& source,
    const MapData::MapDataEditSession& session, const std::filesystem::path& gameRoot,
    const MapData::TerrainResizeLimits& limits) {
    return MapData::TerrainResizeResources::CaptureInputs(
        [&](auto names, const auto& readLimits) { return source.CaptureExactMembers(names, readLimits); },
        session, gameRoot, limits);
}

Result<MapData::TerrainResizeResources> TerrainResizeEditService::Capture(const WorkspaceSnapshot& source,
    const MapData::MapDataEditSession& session, const std::filesystem::path& gameRoot,
    const MapData::TerrainResizeLimits& limits) {
    return MapData::TerrainResizeResources::CaptureInputs(
        [&](auto names, const auto& readLimits) {
            return MapData::TerrainPathingResources::ReadDirectoryMembers(source, names, readLimits);
        }, session, gameRoot, limits);
}

Result<PreparedTerrainResizeEdit> PrepareTerrainResizeEdit(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected,
    const MapData::TerrainResizeResources& resources, const MapData::TerrainResizeIntent& intent,
    const MapData::TerrainResizeLimits& limits) {
    const auto checked = ValidateTerrainEditBinding(session, document, expected);
    if (!checked.Succeeded()) return {{}, checked.diagnostics};
    auto plan = session.PrepareTerrainResizeEdit(resources, intent, limits);
    if (!plan.Succeeded() || !plan.value) return {{}, std::move(plan.diagnostics)};
    return Result<PreparedTerrainResizeEdit>::Success(PreparedTerrainResizeEdit(expected, std::move(*plan.value)));
}

Status ApplyTerrainResizeEdit(MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const PreparedTerrainResizeEdit& prepared,
    const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>& prepublish) {
    const auto checked = ValidateTerrainEditBinding(session, document, prepared.Binding());
    if (!checked.Succeeded()) return checked;
    return session.ApplyPreparedTerrainResizeEdit(prepared.Plan(), prepublish);
}

} // namespace War3Map::Services
