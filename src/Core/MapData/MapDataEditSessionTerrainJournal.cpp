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

Status MapDataEditSession::ReplayTerrainJournalUnits(const TerrainUnitsJournalEdit& edit,
    std::size_t maximumBytes, std::size_t maximumWork) {
    const auto valid = ValidateTerrainUnitsJournalEdit(edit);
    if (!valid.Succeeded()) return valid;
    const auto& current = current_[Index(MapDataKind::Units)];
    if (!current || current->size() != edit.beforeBytes)
        return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_UNITS_STALE", "Units记录需要当前会话唯一且完整的原生成员。");
    const auto cost = MeasureTerrainUnitsJournalEdit(current->size(), edit.afterBytes, maximumBytes, maximumWork);
    if (!cost.Succeeded() || !cost.value) return {cost.diagnostics};
    auto after = ApplyTerrainUnitsJournalEdit(*current, edit);
    if (!after.Succeeded() || !after.value) return {after.diagnostics};
    std::vector<MemberChange> changes;
    const auto prepared = PrepareUnits(*after.value, changes);
    if (!prepared.Succeeded()) return prepared;
    if (changes.size() != 1 || changes.front().kind != MapDataKind::Units)
        return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_UNITS_LINKED_SETTINGS",
            "Units恢复不能省略或顺带改写起始位置联动的地图设置，保留完整raw草稿。");
    return Commit(std::move(changes));
}

} // namespace War3Map::MapData
