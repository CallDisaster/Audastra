#include "War3MapCore/MapData/MapDataEditSession.h"

namespace War3Map::MapData {

Status MapDataEditSession::ReplayTerrainJournalDoodads(const TerrainDoodadsJournalEdit& edit,
    std::size_t maximumBytes, std::size_t maximumWork) {
    const auto valid = ValidateTerrainDoodadsJournalEdit(edit);
    if (!valid.Succeeded()) return valid;
    const auto& current = current_[Index(MapDataKind::Doodads)];
    if (!current || current->size() != edit.beforeBytes)
        return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_DOODADS_STALE", "普通DOO记录需要当前会话唯一且完整的原生成员。");
    const auto cost = MeasureTerrainDoodadsJournalEdit(current->size(), edit.afterBytes, maximumBytes, maximumWork);
    if (!cost.Succeeded() || !cost.value) return {cost.diagnostics};
    auto after = ApplyTerrainDoodadsJournalEdit(*current, edit);
    if (!after.Succeeded() || !after.value) return {after.diagnostics};
    return EditDoodads(*after.value);
}

} // namespace War3Map::MapData
