#include "War3MapCore/MapData/MapDataPlacementPatchCodec.h"
#include "MapDataPatchSource.h"

#include <type_traits>

namespace War3Map::MapData {

Result<MapDataPatch> MapDataPlacementPatchCodec::Seal(const MapDocument& original,
    const MapDocument& current, const WorkspaceSnapshot& source, const MapDataPlacementIntent& originalIntent) {
    const auto frozen = FreezeOriginalMapDataPlacement(original, current, originalIntent);
    if (!frozen.Succeeded()) return {{}, frozen.diagnostics};
    const auto file = std::visit([](const auto& operation) {
        using Operation = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Operation, MapDataPlacementRotation>) return operation.source.id.file;
        else return operation.sources.front().id.file;
    }, frozen.value->Intent());
    const std::array kinds{file == PlacementFile::Units ? MapDataKind::Units : MapDataKind::Doodads};
    auto files = detail::BindMapDataPatchSources(original, current, source, kinds, "MCP 共用原生预放事务");
    if (!files.Succeeded()) return {{}, files.diagnostics};
    return Result<MapDataPatch>::Success(MapDataPatch(std::move(*files.value)));
}

} // namespace War3Map::MapData
