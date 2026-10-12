#include "War3MapCore/MapData/MapDataPlacementEditing.h"
#include "War3MapCore/Hash.h"
#include "MapDataFixtures.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.empty() ? "missing result" : result.diagnostics.front().message);
    return std::move(*result.value);
}
template<class T> bool HasCode(const T& result, const char* code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& item) { return item.code == code; });
}
UnitPlacement Unit(std::string rawcode = "hfoo") {
    UnitPlacement value; value.rawcode = std::move(rawcode); value.position = {32, 64, 16}; value.scale = {1, 2, 3};
    value.flags = 2; value.owner = 1; value.creationId = 7; value.heroLevel = 6;
    value.itemSets = {{{{"I001", 70}, {"I002", 30}}}};
    value.inventory = {{0, "I003"}}; value.abilities = {{"A001", true, 2}};
    value.random = RandomCustomTable{{{"hfoo", 75}, {"hrif", 25}}};
    return value;
}
DoodadPlacement Doodad() {
    DoodadPlacement value; value.rawcode = "LTlt"; value.position = {-64, 64, 8}; value.scale = {1, 1, 1};
    value.flags = 2; value.lifePercent = 75; value.creationId = 11; value.itemSets = {{{{"I001", 100}}}};
    return value;
}
MapDocument Original(bool empty = false) {
    UnitPlacements units; DoodadPlacements doodads;
    if (!empty) { units.units.push_back(Unit()); doodads.doodads.push_back(Doodad()); }
    doodads.specialDoodads.push_back({"LTbx", 2, {1, 2}});
    return MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", Take(units.Serialize())}, {"war3map.doo", Take(doodads.Serialize())}});
}
std::string Sha(const MapDocument& map, MapDataKind kind) { return Take(Sha256(map.Member(kind).sources.front().data)); }
MapDataPlacementSource Source(const MapDocument& map, PlacementFile file, std::uint32_t row) {
    const auto kind = file == PlacementFile::Units ? MapDataKind::Units : MapDataKind::Doodads;
    const auto id = file == PlacementFile::Units ? MakePlacementId(file, row, map.Units()->units[row])
        : MakePlacementId(file, row, map.Doodads()->doodads[row]);
    return {id, Take(MapFieldRowKey(map, kind, row)), Sha(map, kind)};
}
void SameBytes(const MapDocument& left, const MapDocument& right) {
    for (std::size_t i = 0; i < left.Members().size(); ++i) {
        const auto& a = left.Members()[i]; const auto& b = right.Members()[i];
        Require(a.state == b.state && a.memberName == b.memberName && a.sources.size() == b.sources.size(), "member state changed");
        for (std::size_t j = 0; j < a.sources.size(); ++j)
            Require(a.sources[j].name == b.sources[j].name && a.sources[j].data == b.sources[j].data, "native bytes changed");
    }
}
void CreationPreservesRecordsAndUndo() {
    for (const bool empty : {false, true}) for (const bool unit : {false, true}) {
        const auto original = Original(empty); const auto kind = unit ? MapDataKind::Units : MapDataKind::Doodads;
        MapDataPlacementCreate create{Sha(original, kind), std::vector<UnitPlacement>{Unit("H001"), Unit("I001")}};
        if (!unit) create.records = std::vector<DoodadPlacement>{Doodad(), Doodad()};
        auto plan = Take(PrepareMapDataPlacement(original, original, create));
        Require(plan.NewIds().size() == 2 && plan.OriginalReplayIntent(), "create proof and new ids missing");
        const auto firstId = empty ? 0 : unit ? 8 : 12;
        Require(plan.NewIds()[0].creationId == firstId && plan.NewIds()[1].creationId == firstId + 1
            && plan.NewIds()[0].index == (empty ? 0U : 1U), "creation ids must be allocated from original maximum");
        if (unit) {
            auto expected = *original.Units(); auto records = std::get<std::vector<UnitPlacement>>(create.records);
            for (std::size_t i = 0; i < records.size(); ++i) { records[i].creationId = firstId + static_cast<int>(i); expected.units.push_back(records[i]); }
            Require(Take(expected.Serialize()) == plan.After().Member(kind).sources.front().data, "complete unit fields were not retained");
        } else {
            auto expected = *original.Doodads(); auto records = std::get<std::vector<DoodadPlacement>>(create.records);
            for (std::size_t i = 0; i < records.size(); ++i) { records[i].creationId = firstId + static_cast<int>(i); expected.doodads.push_back(records[i]); }
            Require(Take(expected.Serialize()) == plan.After().Member(kind).sources.front().data, "complete doodad or special tail fields changed");
        }
        const auto freeze = Take(FreezeOriginalMapDataPlacement(original, plan.After(), *plan.OriginalReplayIntent()));
        Require(freeze.Members().size() == 1 && freeze.HasChanges(), "create should freeze exactly one native member");
        MapDataEditSession session(original); Require(session.ApplyPreparedPlacement(plan).Succeeded(), "create apply failed");
        SameBytes(plan.After(), session.Snapshot()); Require(session.Revision() == 1, "batch create must be one transaction");
        Require(session.Undo(), "create undo missing"); SameBytes(original, session.Snapshot());
        Require(session.Redo(), "create redo missing"); SameBytes(plan.After(), session.Snapshot());
        Require(HasCode(session.ApplyPreparedPlacement(plan), "MAP_PLACEMENT_PLAN_STALE"), "stale create plan accepted");
        MapDataEditSession legacy(original); const auto added = Take(AddPlacements(legacy, create.records));
        Require(added == std::vector<PlacementId>(plan.NewIds().begin(), plan.NewIds().end()), "old UI and plan ids differ");
        SameBytes(legacy.Snapshot(), plan.After());
    }
}
void DeleteProofEmptyTablesAndProtectedStarts() {
    const auto original = Original();
    for (const bool unit : {false, true}) {
        const auto file = unit ? PlacementFile::Units : PlacementFile::Doodads;
        const MapDataPlacementDelete remove{{Source(original, file, 0)}};
        const auto plan = Take(PrepareMapDataPlacement(original, original, remove));
        Require(plan.NewIds().empty() && plan.Lineage().size() == 1 && !plan.Lineage()[0].beforeToAfter[0], "delete lineage invalid");
        Require(unit ? plan.After().Units()->units.empty() : plan.After().Doodads()->doodads.empty(), "last placement not removed");
        if (!unit) Require(plan.After().Doodads()->specialDoodads.size() == 1, "special tail removed with ordinary row");
        Require(FreezeOriginalMapDataPlacement(original, plan.After(), *plan.OriginalReplayIntent()).Succeeded(), "empty classic table lost replay proof");
        MapDataEditSession session(original); Require(session.ApplyPreparedPlacement(plan).Succeeded(), "delete apply failed");
        Require(session.Undo(), "delete undo missing"); SameBytes(original, session.Snapshot());
        Require(session.Redo(), "delete redo missing"); SameBytes(plan.After(), session.Snapshot());
        const auto id = remove.sources.front().id;
        MapDataEditSession legacy(original); Require(DeletePlacements(legacy, std::span(&id, 1)).Succeeded(), "original delete API failed");
        SameBytes(legacy.Snapshot(), plan.After());
    }
    auto units = *original.Units(); units.units.front().rawcode = "sloc";
    const auto start = Take(ProjectMapData(original, std::vector<MapArchiveMember>{{"war3mapUnits.doo", Take(units.Serialize())}}));
    Require(HasCode(PrepareMapDataPlacement(start, start, MapDataPlacementDelete{{Source(start, PlacementFile::Units, 0)}}),
        "MAP_START_REMOVE_UNSUPPORTED"), "sealed delete bypassed protected start rule");
    auto doodads = *original.Doodads(); doodads.doodads.front().rawcode = "sloc";
    const auto sameRawcode = Take(ProjectMapData(original, std::vector<MapArchiveMember>{{"war3map.doo", Take(doodads.Serialize())}}));
    Require(PrepareMapDataPlacement(sameRawcode, sameRawcode,
        MapDataPlacementDelete{{Source(sameRawcode, PlacementFile::Doodads, 0)}}).Succeeded(), "doodad rawcode confused with unit start location");
    Require(PrepareMapDataPlacement(original, original,
        MapDataPlacementCreate{Sha(original, MapDataKind::Doodads), doodads.doodads}).Succeeded(), "same doodad rawcode rejected during creation");
}
void RejectionsPreserveSourceAndCompleteReplay() {
    const auto original = Original(); const auto originalSha = Sha(original, MapDataKind::Units);
    MapDataPlacementCreate create{originalSha, std::vector<UnitPlacement>{Unit()}};
    auto invalid = create; invalid.expectedMemberSha256.assign(64, '0');
    Require(HasCode(PrepareMapDataPlacement(original, original, invalid), "MAP_PLACEMENT_MEMBER_STALE"), "stale create sha accepted");
    invalid = create; std::get<std::vector<UnitPlacement>>(invalid.records).front().rawcode = "sloc";
    Require(HasCode(PrepareMapDataPlacement(original, original, invalid), "MAP_PLACEMENT_START_CREATE_UNSUPPORTED"), "plain create admitted start location");
    invalid = create; std::get<std::vector<UnitPlacement>>(invalid.records).resize(33, Unit());
    Require(HasCode(PrepareMapDataPlacement(original, original, invalid), "MAP_PLACEMENT_LIMIT"), "oversized create batch accepted");
    invalid = create; std::get<std::vector<UnitPlacement>>(invalid.records).front().skinId = "hfoo";
    Require(!PrepareMapDataPlacement(original, original, invalid).Succeeded(), "skin record admitted by classic create");
    const auto measure = Take(MeasureClassicPlacementAppend(*original.Units(), std::get<std::vector<UnitPlacement>>(create.records)));
    PlacementCommandLimits tight; tight.maximumWireBytes = measure.byteLength - 1;
    Require(HasCode(PrepareMapDataPlacement(original, original, create, tight), "MAP_PLACEMENT_WIRE_BUDGET"), "create copied before wire budget rejection");
    PlacementReadLimits nested; nested.maxNestedRecords = measure.nestedRecordCount - 1;
    Require(HasCode(MeasureClassicPlacementAppend(*original.Units(), std::get<std::vector<UnitPlacement>>(create.records), nested),
        "PLACEMENT_RECORD_LIMIT"), "new nested records escaped global meter");
    const auto source = Source(original, PlacementFile::Units, 0);
    Require(HasCode(PrepareMapDataPlacement(original, original, MapDataPlacementDelete{{source, source}}), "PLACEMENT_DUPLICATE"), "duplicate delete accepted");
    auto stale = source; ++stale.id.creationId;
    Require(!PrepareMapDataPlacement(original, original, MapDataPlacementDelete{{source, stale}}).Succeeded(), "late stale delete accepted");
    const auto plan = Take(PrepareMapDataPlacement(original, original, create));
    MapDataEditSession edited(plan.After()); auto moved = plan.NewIds().front();
    const PlacementMove move{moved, {1, 2, 3}, {}};
    Require(MovePlacements(edited, std::span(&move, 1)).Succeeded(), "tamper fixture move failed");
    Require(HasCode(FreezeOriginalMapDataPlacement(original, edited.Snapshot(), *plan.OriginalReplayIntent()),
        "MAP_PLACEMENT_REPLAY_MISMATCH"), "forged extra change passed original replay");
    Require(Sha(original, MapDataKind::Units) == originalSha, "original source changed after rejection");
}
}
int main() {
    try { CreationPreservesRecordsAndUndo(); DeleteProofEmptyTablesAndProtectedStarts(); RejectionsPreserveSourceAndCompleteReplay(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "placement create/delete tests passed\n"; return 0;
}
