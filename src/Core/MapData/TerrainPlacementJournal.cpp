#include "War3MapCore/MapData/TerrainStrokeJournal.h"

namespace War3Map::MapData {

Status MaterializeTerrainJournalPlacements(TerrainStrokeJournal& journal) {
    constexpr auto maximum = TerrainJournalMaximumBytes;
    const auto fail = [] { return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BUDGET",
        "摆放记录的冻结快照、完整codec、累计扫描或中段载荷超过工程预算，raw草稿仍可保留。"); };
    if (journal.strokes.size() > 128 || journal.baselinePathingLayers.size() > maximum - 128u * 1024u) return fail();
    std::size_t available = maximum - 128u * 1024u - journal.baselinePathingLayers.size();
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
            + unsigned(stroke.frozenUnits.has_value()) + unsigned(stroke.units.has_value());
        if (!variants) continue;
        if (variants != 1 || !IsTerrainDoodadsJournalPlaceholder(stroke.intent)
            || !stroke.rulesSha256.empty() || stroke.cliffEnforceWaterLimits) return fail();
        if (stroke.frozenDoodads && !chargeFrozen(*stroke.frozenDoodads, MeasureTerrainDoodadsJournalEdit)) return fail();
        if (stroke.frozenUnits && !chargeFrozen(*stroke.frozenUnits, MeasureTerrainUnitsJournalEdit)) return fail();
        if (stroke.ordinaryDoodads && !chargeDelta(*stroke.ordinaryDoodads, ValidateTerrainDoodadsJournalEdit)) return fail();
        if (stroke.units && !chargeDelta(*stroke.units, ValidateTerrainUnitsJournalEdit)) return fail();
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
        }
    }
    return Status::Ok();
}

} // namespace War3Map::MapData
