#include "War3MapCore/MapData/TerrainUnitsJournal.h"

#include "TerrainJournalDelta.h"
#include "War3MapCore/MapData/TerrainClassicUnits.h"

namespace War3Map::MapData {
namespace {
Status SameHeader(const UnitPlacements& a, const UnitPlacements& b) {
    if (a.version != b.version || a.subversion != b.subversion)
        return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_UNITS_LAYOUT", "Units记录不能更换经典头版本。");
    return Status::Ok();
}
}

Status ValidateTerrainUnitsJournalEdit(const TerrainUnitsJournalEdit& edit) {
    return Detail::ValidatePlacementDelta(edit, 16, "TERRAIN_JOURNAL_UNITS_DELTA");
}

Result<TerrainUnitsJournalCost> MeasureTerrainUnitsJournalEdit(
    std::size_t beforeBytes, std::size_t afterBytes, std::size_t maximumBytes, std::size_t maximumWork) {
    using Output = Result<TerrainUnitsJournalCost>;
    const auto codec = TerrainClassicUnitsWorkingBytes(beforeBytes, afterBytes,
        std::min(maximumBytes, Detail::JournalDeltaMaximumBytes));
    if (!codec.Succeeded() || !codec.value) return {{}, codec.diagnostics};
    if (maximumWork > Detail::JournalDeltaMaximumBytes || maximumWork < 4096
        || beforeBytes > (maximumWork - 4096) / 32 || afterBytes > (maximumWork - 4096) / 32 - beforeBytes)
        return Output::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BUDGET", "完整Units codec、散列和事务超过剩余累计预算。");
    return Output::Success({*codec.value, 4096 + beforeBytes + afterBytes, 4096 + (beforeBytes + afterBytes) * 32});
}

Result<TerrainUnitsJournalEdit> BuildTerrainUnitsJournalEdit(std::span<const std::byte> before,
    std::span<const std::byte> after, std::size_t maximumInsertedBytes) {
    if (before.size() > Detail::JournalDeltaMaximumBytes || after.size() > Detail::JournalDeltaMaximumBytes)
        return Result<TerrainUnitsJournalEdit>::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BUDGET", "Units记录超过单成员预算。");
    auto a = ReadTerrainClassicUnitsRoundtrip(before); if (!a.Succeeded() || !a.value) return {{}, std::move(a.diagnostics)};
    auto b = ReadTerrainClassicUnitsRoundtrip(after); if (!b.Succeeded() || !b.value) return {{}, std::move(b.diagnostics)};
    const auto same = SameHeader(*a.value, *b.value); if (!same.Succeeded()) return {{}, same.diagnostics};
    return Detail::BuildPlacementDelta<TerrainUnitsJournalEdit>(before, after, maximumInsertedBytes, 16,
        "TERRAIN_JOURNAL_UNITS_DELTA");
}

Result<UnitPlacements> ApplyTerrainUnitsJournalEdit(std::span<const std::byte> current, const TerrainUnitsJournalEdit& edit) {
    auto after = Detail::ApplyPlacementDelta(current, edit, 16, "TERRAIN_JOURNAL_UNITS_DELTA",
        "TERRAIN_JOURNAL_UNITS_STALE", "TERRAIN_JOURNAL_UNITS_OUTPUT");
    if (!after.Succeeded() || !after.value) return {{}, std::move(after.diagnostics)};
    auto a = ReadTerrainClassicUnitsRoundtrip(current); if (!a.Succeeded() || !a.value) return a;
    auto b = ReadTerrainClassicUnitsRoundtrip(*after.value); if (!b.Succeeded() || !b.value) return b;
    const auto same = SameHeader(*a.value, *b.value); if (!same.Succeeded()) return {{}, same.diagnostics};
    return b;
}

} // namespace War3Map::MapData
