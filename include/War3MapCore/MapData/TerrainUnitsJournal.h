#pragma once

#include "War3MapCore/MapData/TerrainPlacementJournalDelta.h"
#include "War3MapCore/MapData/MapPlacements.h"

#include <memory>

namespace War3Map::MapData {

// FTJ9独立Units变体，只恢复完整经典Units，不能承载W3I或其它成员。
struct TerrainUnitsJournalEdit : TerrainPlacementJournalDelta {};

class MapDataEditSession;
class FrozenTerrainUnitsJournalEdit {
public:
    [[nodiscard]] std::span<const std::byte> Before() const noexcept {
        return before_ ? std::span<const std::byte>(*before_) : std::span<const std::byte>{};
    }
    [[nodiscard]] std::span<const std::byte> After() const noexcept {
        return after_ ? std::span<const std::byte>(*after_) : std::span<const std::byte>{};
    }
    [[nodiscard]] std::uint64_t SessionIdentity() const noexcept { return sessionIdentity_; }
    [[nodiscard]] std::uint64_t LogicalIdentity() const noexcept { return logicalIdentity_; }
    [[nodiscard]] std::size_t RetainedBytes() const noexcept { return retainedBytes_; }
private:
    friend class MapDataEditSession;
    FrozenTerrainUnitsJournalEdit() = default;
    std::shared_ptr<const std::vector<std::byte>> before_, after_;
    std::uint64_t sessionIdentity_ = 0, logicalIdentity_ = 0;
    std::size_t retainedBytes_ = 0;
};

struct TerrainUnitsJournalCost {
    std::size_t workingBytes = 0, retainedBytes = 0, work = 0;
};
[[nodiscard]] Status ValidateTerrainUnitsJournalEdit(const TerrainUnitsJournalEdit& edit);
[[nodiscard]] Result<TerrainUnitsJournalCost> MeasureTerrainUnitsJournalEdit(
    std::size_t beforeBytes, std::size_t afterBytes, std::size_t maximumBytes, std::size_t maximumWork);
[[nodiscard]] Result<TerrainUnitsJournalEdit> BuildTerrainUnitsJournalEdit(
    std::span<const std::byte> before, std::span<const std::byte> after,
    std::size_t maximumInsertedBytes = 64u * 1024u * 1024u);
[[nodiscard]] Result<UnitPlacements> ApplyTerrainUnitsJournalEdit(
    std::span<const std::byte> current, const TerrainUnitsJournalEdit& edit);

} // namespace War3Map::MapData
