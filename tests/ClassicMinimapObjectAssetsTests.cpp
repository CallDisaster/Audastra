#include "War3MapCore/Render/ClassicMinimapObjectAssets.h"
#include "War3MapCore/MapData/MapSettings.h"

#include <iostream>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace War3Map;
using namespace War3Map::Render;

static_assert(std::is_move_constructible_v<ClassicMinimapObjectAssets>);
static_assert(std::is_move_assignable_v<ClassicMinimapObjectAssets>);
static_assert(!std::is_copy_constructible_v<ClassicMinimapObjectAssets>);
static_assert(!std::is_copy_assignable_v<ClassicMinimapObjectAssets>);
static_assert(!std::is_default_constructible_v<ClassicMinimapObjectAssets>);
static_assert(std::is_nothrow_move_constructible_v<ClassicMinimapObjectAssets>);
static_assert(std::is_nothrow_move_assignable_v<ClassicMinimapObjectAssets>);
constexpr auto kScratch = ClassicMinimapObjectAssetFacts::snapshotScratchBytesPerWireByte;
// 与classic reader的最短wire逐类约束ABI，而不是钉死某个历史乘数。
static_assert(sizeof(MapData::UnitPlacement) <= 91 * kScratch);
static_assert(sizeof(MapData::DoodadPlacement) <= 42 * kScratch);
static_assert(sizeof(MapData::SpecialDoodadPlacement) <= 16 * kScratch);
static_assert(sizeof(MapData::PlacementItemChoice) <= 8 * kScratch);
static_assert(sizeof(MapData::PlacementItemSet) <= 4 * kScratch);
static_assert(sizeof(MapData::PlacementInventoryItem) <= 8 * kScratch);
static_assert(sizeof(MapData::PlacementAbility) <= 12 * kScratch);
static_assert(sizeof(MapData::MapPlayerSettings) <= 33 * kScratch);
static_assert(sizeof(MapData::MapForceSettings) <= 9 * kScratch);
static_assert(sizeof(MapData::MapRandomUnitTable) <= 13 * kScratch);
static_assert(sizeof(MapData::MapRandomUnitRow) <= 4 * kScratch);
static_assert(sizeof(MapData::MapRandomItemTable) <= 9 * kScratch);
static_assert(sizeof(std::vector<MapData::MapRandomItem>) <= 4 * kScratch);
using TypedCandidateBuilder = Result<ClassicMinimapCandidate> (*)(
    const ClassicMinimapObjectAssets&, const TerrainMinimapPostprocessSettings&, const ClassicMinimapCandidateLimits&);
[[maybe_unused]] constexpr TypedCandidateBuilder kTypedCandidateBuilder = &BuildClassicMinimapCandidate;

std::vector<std::byte> U32(std::initializer_list<std::uint32_t> values) {
    std::vector<std::byte> result;
    for (const auto value : values) for (unsigned shift = 0; shift < 32; shift += 8)
        result.push_back(static_cast<std::byte>((value >> shift) & 255));
    return result;
}

void ParserZeroRecordsContract() {
    std::vector<std::byte> units;
    for (const auto c : std::string_view{"W3do"}) units.push_back(static_cast<std::byte>(c));
    const auto unitsHeader = U32({8, 11, 0}); units.insert(units.end(), unitsHeader.begin(), unitsHeader.end());
    MapData::PlacementReadLimits limits; limits.maxPlacements = 0; limits.maxNestedRecords = 0;
    const auto parsedUnits = MapData::ParseUnitPlacements(units, limits);
    if (!parsedUnits.Succeeded() || !parsedUnits.value || !parsedUnits.value->units.empty()
        || parsedUnits.value->recordLayout != MapData::PlacementRecordLayout::Classic)
        throw std::runtime_error("classic empty units must parse with zero placements");
    std::vector<std::byte> doodads;
    for (const auto c : std::string_view{"W3do"}) doodads.push_back(static_cast<std::byte>(c));
    const auto doodadHeader = U32({8, 11, 0, 0, 0}); doodads.insert(doodads.end(), doodadHeader.begin(), doodadHeader.end());
    const auto parsedDoodads = MapData::ParseDoodadPlacements(doodads, limits);
    if (!parsedDoodads.Succeeded() || !parsedDoodads.value || !parsedDoodads.value->doodads.empty()
        || !parsedDoodads.value->specialDoodads.empty() || parsedDoodads.value->specialVersion != 0
        || parsedDoodads.value->recordLayout != MapData::PlacementRecordLayout::Classic)
        throw std::runtime_error("classic empty doodads must parse with zero ordinary/special placements");
    units[12] = std::byte{1};
    if (MapData::ParseUnitPlacements(units, limits).Succeeded())
        throw std::runtime_error("maxPlacements=0 must reject a nonzero unit count before reserve");
    doodads[12] = std::byte{1};
    if (MapData::ParseDoodadPlacements(doodads, limits).Succeeded())
        throw std::runtime_error("maxPlacements=0 must reject a nonzero doodad count before reserve");
}

int main() {
    // 真实archive/edit session才能构造sealed对象。本目标只含ownership/API与parser fixture，
    // 不覆盖实际source/current捕获闭环；构建与执行统一由root串行负责。
    ParserZeroRecordsContract();
    std::cout << "Classic minimap object assets ownership and parser contracts declared\n";
    return 0;
}
