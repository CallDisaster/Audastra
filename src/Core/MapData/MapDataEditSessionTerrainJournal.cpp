#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainStartLocations.h"

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
    Command command;
    auto& changes = command.changes;
    const auto prepared = PrepareUnits(*after.value, changes, false, &command);
    if (!prepared.Succeeded()) return prepared;
    if (changes.size() != 1 || changes.front().kind != MapDataKind::Units)
        return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_UNITS_LINKED_SETTINGS",
            "Units恢复不能省略或顺带改写起始位置联动的地图设置，保留完整raw草稿。");
    return CommitCommand(std::move(command));
}

Status MapDataEditSession::ReplayTerrainJournalStartLocations(const TerrainStartLocationsJournalEdit& edit,
    std::size_t maximumBytes, std::size_t maximumWork) {
    const auto valid = ValidateTerrainStartLocationsJournalEdit(edit);
    if (!valid.Succeeded()) return valid;
    const auto& units = current_[Index(MapDataKind::Units)];
    const auto& settings = current_[Index(MapDataKind::Settings)];
    if (!units || !settings || units->size() != edit.units.beforeBytes)
        return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_START_STALE", "起点恢复需要唯一完整的Units和W3I来源。");
    // W3I delta不含四说明；实际前后完整W3I依然按原大小预收codec预算。
    if (settings->size() < edit.settings.beforeBytes)
        return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_START_STALE", "起点设置布局或说明长度已经改变。");
    const auto afterSize = settings->size() - edit.settings.beforeBytes + edit.settings.afterBytes;
    const auto cost = MeasureTerrainStartLocationsJournalEdit(units->size(), edit.units.afterBytes,
        settings->size(), afterSize, maximumBytes, maximumWork);
    if (!cost.Succeeded() || !cost.value) return {cost.diagnostics};
    auto output = ApplyTerrainStartLocationsJournalEdit(*units, *settings, edit);
    if (!output.Succeeded() || !output.value) return {output.diagnostics};
    Command command;
    const auto prepared = PrepareUnits(output.value->units, command.changes, true, &command);
    if (!prepared.Succeeded()) return prepared;
    if (!command.startsBefore || !command.startsAfter || command.changes.size() != 2
        || command.changes[0].kind != MapDataKind::Units || command.changes[1].kind != MapDataKind::Settings
        || !command.changes[1].after)
        return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_START_NOT_LINKED", "记录未重建真实PrepareUnits起点复合命令，未提交任何成员。");
    const auto actual = CanonicalTerrainStartSettings(*command.changes[1].after);
    if (!actual.Succeeded() || !actual.value || *actual.value != output.value->settings)
        return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_START_OUTPUT", "真实联动生成的完整W3I与记录不一致。");
    return CommitCommand(std::move(command));
}

} // namespace War3Map::MapData
