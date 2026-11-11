#include "War3MapCore/MapData/TerrainStrokeJournal.h"
#include "TerrainJournalDelta.h"

namespace War3Map::MapData {

Status ValidateMapMemberJournalEdit(const MapMemberJournalEdit& edit) {
    switch (edit.kind) {
    case MapDataKind::Settings: case MapDataKind::Regions: case MapDataKind::Cameras:
    case MapDataKind::Sounds: case MapDataKind::Strings: break;
    default:
        return Status::Failure("MapData.History", "MAP_HISTORY_MEMBER_KIND", "普通地图数据历史不允许写入这种成员。");
    }
    if (!edit.beforePresent && edit.delta.beforeBytes)
        return Status::Failure("MapData.History", "MAP_HISTORY_MEMBER_DELTA", "缺席的旧成员不能携带长度。");
    return Detail::ValidatePlacementDelta(edit.delta, 0, "MAP_HISTORY_MEMBER_DELTA");
}

Status MaterializeTerrainJournalPlacements(TerrainStrokeJournal& journal) {
    constexpr auto maximum = TerrainJournalMaximumBytes;
    const auto fail = [] { return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BUDGET",
        "摆放记录的冻结快照、完整codec、累计扫描或中段载荷超过工程预算，raw草稿仍可保留。"); };
    const auto history = MaterializeTerrainJournalHistory(journal);
    if (!history.Succeeded()) return history;
    if (journal.strokes.size() > 128 || journal.baselinePathingLayers.size() > maximum - 128u * 1024u) return fail();
    std::size_t available = maximum - 128u * 1024u - journal.baselinePathingLayers.size();
    if (journal.history) {
        if (journal.history->baselinePathingLayers.size() > available) return fail();
        available -= journal.history->baselinePathingLayers.size();
        if (journal.history->baselineShadow) {
            if (journal.history->baselineShadow->bytes.size() > available) return fail();
            available -= journal.history->baselineShadow->bytes.size();
        }
        for (const auto& member : journal.history->baselineMembers) {
            if (member.bytes.size() > available) return fail();
            available -= member.bytes.size();
        }
    }
    std::size_t frozenBytes = 0, remainingWork = maximum;
    // 所有尺寸和累计work先收齐；两类记录共用同一个原有总额度，不加倍预算。
    const auto chargeFrozen = [&](const auto& frozen, const auto& measure) {
        if (!frozen.RetainedBytes() || frozen.Before().empty() || frozen.After().empty()
            || frozen.RetainedBytes() > maximum - frozenBytes) return false;
        frozenBytes += frozen.RetainedBytes();
        const auto cost = measure(frozen.Before().size(), frozen.After().size(), maximum, remainingWork);
        if (!cost.Succeeded() || !cost.value) return false;
        remainingWork -= cost.value->work; return true;
    };
    const auto chargeDelta = [&](const auto& edit, const auto& validate) {
        if (!validate(edit).Succeeded() || edit.inserted.size() > available) return false;
        available -= edit.inserted.size(); return true;
    };
    for (const auto& stroke : journal.strokes) {
        const auto variants = unsigned(stroke.frozenDoodads.has_value()) + unsigned(stroke.ordinaryDoodads.has_value())
            + unsigned(stroke.frozenUnits.has_value()) + unsigned(stroke.units.has_value())
            + unsigned(stroke.frozenStartLocations.has_value()) + unsigned(stroke.startLocations.has_value())
            + unsigned(!stroke.ordinaryMembers.empty()) + unsigned(!stroke.frozenOrdinaryMembers.empty());
        if (!variants) continue;
        if (variants != 1 || !IsTerrainDoodadsJournalPlaceholder(stroke.intent)
            || !stroke.rulesSha256.empty() || stroke.cliffEnforceWaterLimits) return fail();
        if (stroke.frozenDoodads && !chargeFrozen(*stroke.frozenDoodads, MeasureTerrainDoodadsJournalEdit)) return fail();
        if (stroke.frozenUnits && !chargeFrozen(*stroke.frozenUnits, MeasureTerrainUnitsJournalEdit)) return fail();
        if (stroke.ordinaryDoodads && !chargeDelta(*stroke.ordinaryDoodads, ValidateTerrainDoodadsJournalEdit)) return fail();
        if (stroke.units && !chargeDelta(*stroke.units, ValidateTerrainUnitsJournalEdit)) return fail();
        if (stroke.frozenStartLocations) {
            const auto& frozen = *stroke.frozenStartLocations;
            if (!frozen.RetainedBytes() || frozen.UnitsBefore().empty() || frozen.UnitsAfter().empty()
                || frozen.SettingsBefore().empty() || frozen.SettingsAfter().empty()
                || frozen.RetainedBytes() > maximum - frozenBytes) return fail();
            frozenBytes += frozen.RetainedBytes();
            const auto cost = MeasureTerrainStartLocationsJournalEdit(frozen.UnitsBefore().size(), frozen.UnitsAfter().size(),
                frozen.SettingsBefore().size(), frozen.SettingsAfter().size(), maximum, remainingWork);
            if (!cost.Succeeded() || !cost.value) return fail();
            remainingWork -= cost.value->work;
        }
        if (stroke.startLocations) {
            const auto& edit = *stroke.startLocations;
            if (!ValidateTerrainStartLocationsJournalEdit(edit).Succeeded() || edit.units.inserted.size() > available) return fail();
            available -= edit.units.inserted.size();
            if (edit.settings.inserted.size() > available) return fail();
            available -= edit.settings.inserted.size();
        }
        if (stroke.ordinaryMembers.size() > 5 || stroke.frozenOrdinaryMembers.size() > 5) return fail();
        for (const auto& edit : stroke.ordinaryMembers)
            if (!ValidateMapMemberJournalEdit(edit).Succeeded() || edit.delta.inserted.size() > available) return fail();
            else available -= edit.delta.inserted.size();
        for (const auto& edit : stroke.frozenOrdinaryMembers) {
            if (!edit.after) return fail();
            for (const auto& bytes : {edit.before, edit.after}) if (bytes) {
                if (bytes->capacity() > maximum - frozenBytes || bytes->size() > remainingWork / 4) return fail();
                frozenBytes += bytes->capacity();
                remainingWork -= bytes->size() * 4;
            }
        }
    }
    // 冻结Command连同谱系最多64MiB、codec峰值64MiB、最终wire/baseline各64MiB，
    // 与原来的512MiB工程候选帽一致。逐项释放别名，不再回UI读取活动历史。
    for (auto& stroke : journal.strokes) {
        if (stroke.frozenDoodads) {
            const auto& frozen = *stroke.frozenDoodads;
            auto edit = BuildTerrainDoodadsJournalEdit(frozen.Before(), frozen.After(), available);
            if (!edit.Succeeded() || !edit.value) return {edit.diagnostics};
            available -= edit.value->inserted.size();
            stroke.ordinaryDoodads = std::move(*edit.value); stroke.frozenDoodads.reset();
        } else if (stroke.frozenUnits) {
            const auto& frozen = *stroke.frozenUnits;
            auto edit = BuildTerrainUnitsJournalEdit(frozen.Before(), frozen.After(), available);
            if (!edit.Succeeded() || !edit.value) return {edit.diagnostics};
            available -= edit.value->inserted.size();
            stroke.units = std::move(*edit.value); stroke.frozenUnits.reset();
        } else if (stroke.frozenStartLocations) {
            const auto& frozen = *stroke.frozenStartLocations;
            auto edit = BuildTerrainStartLocationsJournalEdit(frozen.UnitsBefore(), frozen.UnitsAfter(),
                frozen.SettingsBefore(), frozen.SettingsAfter(), available);
            if (!edit.Succeeded() || !edit.value) return {edit.diagnostics};
            available -= edit.value->units.inserted.size() + edit.value->settings.inserted.size();
            stroke.startLocations = std::move(*edit.value); stroke.frozenStartLocations.reset();
        } else if (!stroke.frozenOrdinaryMembers.empty()) {
            for (const auto& frozen : stroke.frozenOrdinaryMembers) {
                auto delta = Detail::BuildPlacementDelta<TerrainPlacementJournalDelta>(
                    frozen.before ? std::span<const std::byte>(*frozen.before) : std::span<const std::byte>{},
                    *frozen.after, available, 0, "MAP_HISTORY_MEMBER_DELTA");
                if (!delta.Succeeded() || !delta.value) return {delta.diagnostics};
                available -= delta.value->inserted.size();
                stroke.ordinaryMembers.push_back({frozen.kind, bool(frozen.before), std::move(*delta.value)});
            }
            stroke.frozenOrdinaryMembers.clear();
        }
    }
    return Status::Ok();
}

} // namespace War3Map::MapData
