#include "War3MapCore/MapData/TerrainStartLocationsJournal.h"

#include "TerrainJournalDelta.h"
#include "War3MapCore/MapData/TerrainClassicUnits.h"
#include "War3MapCore/MapData/TerrainStartLocations.h"

namespace War3Map::MapData {
namespace {
bool Identity(const TerrainPlacementJournalDelta& edit) {
    return edit.beforeBytes >= 12 && edit.beforeBytes <= TerrainStartSettingsMaximumBytes
        && edit.beforeBytes == edit.afterBytes && edit.prefixBytes == edit.beforeBytes && !edit.removedBytes
        && edit.inserted.empty() && edit.beforeSha256 == edit.afterSha256 && edit.beforeSha256.size() == 64
        && std::all_of(edit.beforeSha256.begin(), edit.beforeSha256.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}
}

Status ValidateTerrainStartLocationsJournalEdit(const TerrainStartLocationsJournalEdit& edit) {
    const auto units = ValidateTerrainUnitsJournalEdit(edit.units);
    if (!units.Succeeded()) return units;
    if (edit.settings.beforeBytes > TerrainStartSettingsMaximumBytes || edit.settings.afterBytes > TerrainStartSettingsMaximumBytes)
        return Status::Failure("Terrain.Journal", "TERRAIN_START_SETTINGS_BUDGET", "起始位置记录的完整W3I超过1MiB。");
    return Identity(edit.settings) ? Status::Ok()
        : Detail::ValidatePlacementDelta(edit.settings, 12, "TERRAIN_JOURNAL_START_SETTINGS_DELTA");
}

Result<TerrainStartLocationsJournalCost> MeasureTerrainStartLocationsJournalEdit(
    std::size_t unitsBefore, std::size_t unitsAfter, std::size_t settingsBefore, std::size_t settingsAfter,
    std::size_t maximumBytes, std::size_t maximumWork) {
    using Output = Result<TerrainStartLocationsJournalCost>;
    const auto units = MeasureTerrainUnitsJournalEdit(unitsBefore, unitsAfter, maximumBytes, maximumWork);
    if (!units.Succeeded() || !units.value) return {{}, units.diagnostics};
    if (settingsBefore > TerrainStartSettingsMaximumBytes || settingsAfter > TerrainStartSettingsMaximumBytes)
        return Output::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BUDGET", "完整起始位置W3I超过预算。");
    const auto sum = settingsBefore + settingsAfter;
    const auto bytes = units.value->workingBytes + 4096 + sum * 32;
    const auto work = units.value->work + 4096 + sum * 64;
    if (bytes > std::min(maximumBytes, Detail::JournalDeltaMaximumBytes) || work > maximumWork)
        return Output::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BUDGET", "起始位置双成员codec、完整比较或真实事务超过剩余累计预算。");
    return Output::Success({bytes, units.value->retainedBytes + 4096 + sum, work});
}

Result<TerrainStartLocationsJournalEdit> BuildTerrainStartLocationsJournalEdit(
    std::span<const std::byte> unitsBefore, std::span<const std::byte> unitsAfter,
    std::span<const std::byte> settingsBefore, std::span<const std::byte> settingsAfter,
    std::size_t maximumInsertedBytes) {
    using Output = Result<TerrainStartLocationsJournalEdit>;
    const auto cost = MeasureTerrainStartLocationsJournalEdit(unitsBefore.size(), unitsAfter.size(), settingsBefore.size(), settingsAfter.size(),
        Detail::JournalDeltaMaximumBytes, Detail::JournalDeltaMaximumBytes);
    if (!cost.Succeeded()) return {{}, cost.diagnostics};
    auto units = BuildTerrainUnitsJournalEdit(unitsBefore, unitsAfter, maximumInsertedBytes);
    if (!units.Succeeded() || !units.value) return {{}, std::move(units.diagnostics)};
    auto before = ReadTerrainClassicUnitsRoundtrip(unitsBefore), after = ReadTerrainClassicUnitsRoundtrip(unitsAfter);
    if (!before.Succeeded() || !before.value) return {{}, std::move(before.diagnostics)};
    if (!after.Succeeded() || !after.value) return {{}, std::move(after.diagnostics)};
    auto canonicalBefore = CanonicalTerrainStartSettings(settingsBefore), canonicalAfter = CanonicalTerrainStartSettings(settingsAfter);
    if (!canonicalBefore.Succeeded() || !canonicalBefore.value) return {{}, std::move(canonicalBefore.diagnostics)};
    if (!canonicalAfter.Succeeded() || !canonicalAfter.value) return {{}, std::move(canonicalAfter.diagnostics)};
    auto oldSettings = ReadTerrainClassicStartSettings(*canonicalBefore.value), newSettings = ReadTerrainClassicStartSettings(*canonicalAfter.value);
    if (!oldSettings.Succeeded() || !oldSettings.value) return {{}, std::move(oldSettings.diagnostics)};
    if (!newSettings.Succeeded() || !newSettings.value) return {{}, std::move(newSettings.diagnostics)};
    const auto linked = VerifyTerrainStartLocationsChange(*before.value, *after.value, *oldSettings.value, *newSettings.value);
    if (!linked.Succeeded()) return {{}, linked.diagnostics};
    if (*canonicalBefore.value == *canonicalAfter.value) {
        const auto sha = Sha256(*canonicalBefore.value);
        if (!sha.Succeeded() || !sha.value) return {{}, sha.diagnostics};
        TerrainPlacementJournalDelta identity;
        identity.beforeSha256 = identity.afterSha256 = *sha.value;
        identity.beforeBytes = identity.afterBytes = identity.prefixBytes = static_cast<std::uint32_t>(canonicalBefore.value->size());
        return Output::Success({std::move(*units.value), std::move(identity)});
    }
    auto settings = Detail::BuildPlacementDelta<TerrainPlacementJournalDelta>(*canonicalBefore.value, *canonicalAfter.value,
        maximumInsertedBytes - units.value->inserted.size(), 12, "TERRAIN_JOURNAL_START_SETTINGS_DELTA");
    if (!settings.Succeeded() || !settings.value) return {{}, std::move(settings.diagnostics)};
    return Output::Success({std::move(*units.value), std::move(*settings.value)});
}

Result<TerrainStartLocationsJournalOutput> ApplyTerrainStartLocationsJournalEdit(
    std::span<const std::byte> units, std::span<const std::byte> settings, const TerrainStartLocationsJournalEdit& edit) {
    using Output = Result<TerrainStartLocationsJournalOutput>;
    const auto valid = ValidateTerrainStartLocationsJournalEdit(edit);
    if (!valid.Succeeded()) return {{}, valid.diagnostics};
    const auto cost = MeasureTerrainStartLocationsJournalEdit(units.size(), edit.units.afterBytes, settings.size(), edit.settings.afterBytes,
        Detail::JournalDeltaMaximumBytes, Detail::JournalDeltaMaximumBytes);
    if (!cost.Succeeded()) return {{}, cost.diagnostics};
    auto after = ApplyTerrainUnitsJournalEdit(units, edit.units);
    if (!after.Succeeded() || !after.value) return {{}, std::move(after.diagnostics)};
    auto canonical = CanonicalTerrainStartSettings(settings);
    if (!canonical.Succeeded() || !canonical.value) return {{}, std::move(canonical.diagnostics)};
    Result<std::vector<std::byte>> afterSettings;
    if (Identity(edit.settings)) {
        const auto sha = Sha256(*canonical.value);
        if (!sha.Succeeded() || !sha.value) return {{}, sha.diagnostics};
        if (canonical.value->size() != edit.settings.beforeBytes || *sha.value != edit.settings.beforeSha256)
            return Output::Failure("Terrain.Journal", "TERRAIN_JOURNAL_START_SETTINGS_STALE", "起点记录的完整设置身份已变。");
        afterSettings = Result<std::vector<std::byte>>::Success(std::move(*canonical.value));
    } else afterSettings = Detail::ApplyPlacementDelta(*canonical.value, edit.settings, 12, "TERRAIN_JOURNAL_START_SETTINGS_DELTA",
        "TERRAIN_JOURNAL_START_SETTINGS_STALE", "TERRAIN_JOURNAL_START_SETTINGS_OUTPUT");
    if (!afterSettings.Succeeded() || !afterSettings.value) return {{}, std::move(afterSettings.diagnostics)};
    auto before = ReadTerrainClassicUnitsRoundtrip(units);
    if (!before.Succeeded() || !before.value) return {{}, std::move(before.diagnostics)};
    auto oldCanonical = CanonicalTerrainStartSettings(settings);
    if (!oldCanonical.Succeeded() || !oldCanonical.value) return {{}, std::move(oldCanonical.diagnostics)};
    auto a = ReadTerrainClassicStartSettings(*oldCanonical.value), b = ReadTerrainClassicStartSettings(*afterSettings.value);
    if (!a.Succeeded() || !a.value) return {{}, std::move(a.diagnostics)};
    if (!b.Succeeded() || !b.value) return {{}, std::move(b.diagnostics)};
    const auto linked = VerifyTerrainStartLocationsChange(*before.value, *after.value, *a.value, *b.value);
    if (!linked.Succeeded()) return {{}, linked.diagnostics};
    return Output::Success({std::move(*after.value), std::move(*afterSettings.value)});
}

} // namespace War3Map::MapData
