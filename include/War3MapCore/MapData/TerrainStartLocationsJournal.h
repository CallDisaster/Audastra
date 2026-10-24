#pragma once

#include "War3MapCore/MapData/TerrainUnitsJournal.h"

namespace War3Map::MapData {

struct TerrainStartLocationsJournalEdit {
    TerrainUnitsJournalEdit units;
    TerrainPlacementJournalDelta settings;
};
class FrozenTerrainStartLocationsJournalEdit {
public:
    [[nodiscard]] std::span<const std::byte> UnitsBefore() const noexcept { return View(unitsBefore_); }
    [[nodiscard]] std::span<const std::byte> UnitsAfter() const noexcept { return View(unitsAfter_); }
    [[nodiscard]] std::span<const std::byte> SettingsBefore() const noexcept { return View(settingsBefore_); }
    [[nodiscard]] std::span<const std::byte> SettingsAfter() const noexcept { return View(settingsAfter_); }
    [[nodiscard]] std::uint64_t SessionIdentity() const noexcept { return sessionIdentity_; }
    [[nodiscard]] std::uint64_t LogicalIdentity() const noexcept { return logicalIdentity_; }
    [[nodiscard]] std::size_t RetainedBytes() const noexcept { return retainedBytes_; }
private:
    friend class MapDataEditSession;
    FrozenTerrainStartLocationsJournalEdit() = default;
    static std::span<const std::byte> View(const std::shared_ptr<const std::vector<std::byte>>& value) noexcept {
        return value ? std::span<const std::byte>(*value) : std::span<const std::byte>{};
    }
    std::shared_ptr<const std::vector<std::byte>> unitsBefore_, unitsAfter_, settingsBefore_, settingsAfter_;
    std::uint64_t sessionIdentity_ = 0, logicalIdentity_ = 0;
    std::size_t retainedBytes_ = 0;
};
struct TerrainStartLocationsJournalCost {
    std::size_t workingBytes = 0, retainedBytes = 0, work = 0;
};
struct TerrainStartLocationsJournalOutput {
    UnitPlacements units;
    std::vector<std::byte> settings;
};
[[nodiscard]] Status ValidateTerrainStartLocationsJournalEdit(const TerrainStartLocationsJournalEdit& edit);
[[nodiscard]] Result<TerrainStartLocationsJournalCost> MeasureTerrainStartLocationsJournalEdit(
    std::size_t unitsBefore, std::size_t unitsAfter, std::size_t settingsBefore, std::size_t settingsAfter,
    std::size_t maximumBytes, std::size_t maximumWork);
[[nodiscard]] Result<TerrainStartLocationsJournalEdit> BuildTerrainStartLocationsJournalEdit(
    std::span<const std::byte> unitsBefore, std::span<const std::byte> unitsAfter,
    std::span<const std::byte> settingsBefore, std::span<const std::byte> settingsAfter,
    std::size_t maximumInsertedBytes = 64u * 1024u * 1024u);
[[nodiscard]] Result<TerrainStartLocationsJournalOutput> ApplyTerrainStartLocationsJournalEdit(
    std::span<const std::byte> units, std::span<const std::byte> settings, const TerrainStartLocationsJournalEdit& edit);

} // namespace War3Map::MapData
