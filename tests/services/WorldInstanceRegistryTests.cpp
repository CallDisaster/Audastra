#include "War3MapCore/Services/WorldInstanceRegistry.h"
#include "../MapDataFixtures.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
MapDocument Source() {
    auto units = ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    auto doodads = ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11());
    UnitPlacement unit; unit.rawcode = "hfoo"; unit.creationId = 5; unit.scale = {1,1,1}; units.value->units.push_back(unit);
    DoodadPlacement tree; tree.rawcode = "LTlt"; tree.creationId = 5; tree.scale = {1,1,1}; doodads.value->doodads.push_back(tree);
    tree.creationId = 6; doodads.value->doodads.push_back(tree); // 同rawcode，且跨文件重复创建编号5。
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo",*units.value->Serialize().value},{"war3map.doo",*doodads.value->Serialize().value}};
    return MapDocument::FromMembers(members);
}
auto Capture(const MapDocument& document) {
    std::pair<std::vector<Render::ModelInstanceRequest>,std::vector<PlacementId>> result;
    const auto add = [&](PlacementId row, const PlacementTransform& transform) {
        Render::ModelInstanceRequest model; model.modelPath = "Fixture.mdx"; model.position = transform.position;
        model.angle = transform.rotation; model.scale = transform.scale; result.first.push_back(model); result.second.push_back(std::move(row));
    };
    for (std::uint32_t i = 0; i < document.Units()->units.size(); ++i) add(MakePlacementId(PlacementFile::Units,i,document.Units()->units[i]),document.Units()->units[i]);
    for (std::uint32_t i = 0; i < document.Doodads()->doodads.size(); ++i) add(MakePlacementId(PlacementFile::Doodads,i,document.Doodads()->doodads[i]),document.Doodads()->doodads[i]);
    return result;
}
auto Create(const MapDocument& document, const MapDataEditSession& edits, WorldInstanceHistoryLimits limits = {}) {
    const auto capture = Capture(document);
    auto registry = WorldInstanceRegistry::Create(document,{7,1,edits.SessionIdentity(),edits.Revision(),"source-SHA"},capture.first,capture.second,{},limits);
    Require(registry.Succeeded(),"registry source must be bound"); return *registry.value;
}
WorldInstanceRegistryChange Advance(const WorldInstanceRegistry& registry, const MapDataEditSession& edits) {
    const auto document = edits.Snapshot(); const auto capture = Capture(document);
    auto change = registry.Prepare(edits.LastAppliedEdit(),registry.Binding(),registry.Binding().mapRevision + 1,document,capture.first,capture.second);
    Require(change.Succeeded(),"committed lineage must prepare"); return std::move(*change.value);
}
void DeleteMoveCopyAndReplayKeepExactIdentities() {
    const auto document = Source(); MapDataEditSession edits(document); auto registry = Create(document,edits);
    const auto unit = MakePlacementId(PlacementFile::Units,0,document.Units()->units[0]);
    const auto first = MakePlacementId(PlacementFile::Doodads,0,document.Doodads()->doodads[0]);
    const auto second = MakePlacementId(PlacementFile::Doodads,1,document.Doodads()->doodads[1]);
    const auto unitId = *registry->Find(unit), firstId = *registry->Find(first), secondId = *registry->Find(second);
    Require(unitId != firstId && firstId != secondId,"file/row identity cannot collapse repeated names or creation IDs");
    Require(DeletePlacements(edits,std::span(&first,1)).Succeeded(),"delete first row"); const auto command = edits.LastEdit();
    Require(command.HasPlacementLineage() && !command.PlacementLineage()[0].beforeToAfter[0] && command.PlacementLineage()[0].beforeToAfter[1] == 0u,"delete lineage must explicitly shift row1 to row0");
    auto change = Advance(*registry,edits); const auto shifted = MakePlacementId(PlacementFile::Doodads,0,edits.Snapshot().Doodads()->doodads[0]);
    Require(change.delta.removals.size() == 1 && *change.registry->Find(shifted) == secondId,"survivor shifts without borrowing deleted identity");
    Require(change.delta.rows.size() == 1 && change.delta.rows.front().identity == secondId,
        "only shifted host rows need a remap, unchanged unit binding must survive");
    Require(change.registry->CapturedSpecs().size() == 2 && registry->CapturedSpecs().size() == 3
        && change.registry->Find(unit) == unitId, "candidate capture must preserve untouched file without modifying old snapshot");
    Require(registry->Find(first) == firstId,"Prepare cannot mutate current binding"); registry = change.registry;
    const auto composed = Create(document, edits)->DeltaTo(*registry);
    // 新建全局registry不能借用另一注册器的实例身份；这里只核对明确差集是身份相异的全替换。
    Require(composed.Succeeded() && !composed.value->additions.empty() && !composed.value->removals.empty(),
        "independent full build cannot masquerade as resident delta identity");
    Require(edits.Replay(command,false).Succeeded(),"undo through original immutable receipt"); change = Advance(*registry,edits); registry = change.registry;
    Require(edits.LastAppliedEdit().CommandIdentity() == command.CommandIdentity() && !edits.LastAppliedEdit().Forward() && edits.LastAppliedEdit().Restoring(),"Replay must keep command identity and inverse direction");
    Require(*registry->Find(first) == firstId && *registry->Find(second) == secondId,"undo restores original objects only");
    Require(edits.Replay(command,true).Succeeded(),"redo original delete"); registry = Advance(*registry,edits).registry;
    Require(*registry->Find(shifted) == secondId,"redo keeps survivor identity");
    Require(edits.Replay(command,false).Succeeded(),"restore before moving"); registry = Advance(*registry,edits).registry;
    const std::array<PlacementMove,2> moves{{{unit,{20,30,40},{}},{second,{50,60,70},{}}}};
    Require(MovePlacements(edits,moves).Succeeded(),"move both files"); change = Advance(*registry,edits); registry = change.registry;
    Require(change.delta.edits.size() == 2 && change.delta.additions.empty() && *registry->Find(unit) == unitId,"transform edits retain identities and clocks");
    Require(change.delta.rows.empty(), "transform-only edits must not manufacture full-table host remaps");
    const std::array<PlacementId,2> copies{unit,second}; const auto copied = CopyPlacements(edits,copies,{10,20,30}); Require(copied.Succeeded(),"copy across files");
    change = Advance(*registry,edits); registry = change.registry; Require(change.delta.additions.size() == 2,"copy creates two genuine new instances");
    const auto copiedUnitId = *registry->Find(copied.value->front()); Require(copiedUnitId != unitId,"copy must not clone identity");
    Require(edits.Undo(),"native Undo copy"); registry = Advance(*registry,edits).registry;
    Require(!registry->Find(copied.value->front()) && edits.Redo(),"native Redo copy"); registry = Advance(*registry,edits).registry;
    Require(*registry->Find(copied.value->front()) == copiedUnitId,"native Redo restores same added identity");
}
void ReusedCreationNumbersAndUntrustedTablesCannotReuseIdentity() {
    const auto document = Source(); MapDataEditSession edits(document); auto registry = Create(document,edits);
    auto unit = document.Units()->units[0]; unit.position[0] = 100;
    auto added = AddUnitPlacement(edits,unit); Require(added.Succeeded(),"first append"); const auto original = edits.LastEdit();
    registry = Advance(*registry,edits).registry; const auto firstId = *registry->Find(*added.value);
    Require(edits.Replay(original,false).Succeeded(),"undo append"); registry = Advance(*registry,edits).registry;
    unit.position[0] = 200; const auto other = AddUnitPlacement(edits,unit); Require(other.Succeeded(),"different append after undo");
    Require(other.value->creationId == added.value->creationId,"fixture must actually reuse deleted creation number");
    registry = Advance(*registry,edits).registry; Require(*registry->Find(*other.value) != firstId,"unrelated object cannot get undo record's identity");
    const auto snapshot = edits.Snapshot(); const auto capture = Capture(snapshot);
    Require(!registry->Prepare(edits.LastAppliedEdit(),registry->Binding(),registry->Binding().mapRevision + 1,snapshot,capture.first,capture.second).Succeeded(),"already-applied first redo cannot apply twice");
    auto table = *snapshot.Units(); table.units[0].position[0] = 999; Require(edits.EditUnits(table).Succeeded(),"generic table edit stays supported");
    const auto next = edits.Snapshot(); const auto newCapture = Capture(next);
    auto unsupported = registry->Prepare(edits.LastAppliedEdit(),registry->Binding(),registry->Binding().mapRevision + 1,next,newCapture.first,newCapture.second);
    Require(!unsupported.Succeeded() && unsupported.diagnostics.front().code == "WORLD_INSTANCE_LINEAGE_UNSUPPORTED","unknown table edits require full rebuild");
    MapDataEditSession boundedEdits(document); auto bounded = Create(document,boundedEdits,{0,0});
    Require(AddUnitPlacement(boundedEdits,unit).Succeeded(),"zero undo memory never limits map additions"); bounded = Advance(*bounded,boundedEdits).registry;
    Require(boundedEdits.Undo(),"undo with evicted registry history"); const auto restored = boundedEdits.Snapshot(); const auto restoredCapture = Capture(restored);
    Require(!bounded->Prepare(boundedEdits.LastAppliedEdit(),bounded->Binding(),bounded->Binding().mapRevision + 1,restored,restoredCapture.first,restoredCapture.second).Succeeded(),"missing identity history cannot guess undo restoration");
    auto rebuilt = Create(restored,boundedEdits); // fullbuild没有旧撤销身份，Redo不能伪装首次新增。
    Require(boundedEdits.Redo(),"redo after rebuild"); const auto redone = boundedEdits.Snapshot(); const auto redoCapture = Capture(redone);
    Require(!rebuilt->Prepare(boundedEdits.LastAppliedEdit(),rebuilt->Binding(),rebuilt->Binding().mapRevision + 1,redone,redoCapture.first,redoCapture.second).Succeeded(),"evicted forward restore must not allocate replacement identity");
}
void PreparedDifferencesRemainOwnedAndCoalesced() {
    const auto document = Source(); MapDataEditSession edits(document); const auto initial = Create(document,edits);
    auto addedUnit = document.Units()->units[0]; addedUnit.position[0] = 10;
    const auto added = AddUnitPlacement(edits,addedUnit); Require(added.Succeeded(),"append before reusing a difference");
    auto first = Advance(*initial,edits);
    const auto identity = *first.registry->Find(*added.value);
    auto difference = initial->DeltaTo(*first.registry);
    Require(difference.Succeeded() && difference.value->additions.size() == 1
        && difference.value->additions[0].identity == identity,"prepared difference retains exact new identity");
    difference.value->additions.clear(); first.delta.additions[0].model.modelPath = "caller-only.mdx";
    const auto repeated = initial->DeltaTo(*first.registry);
    Require(repeated.Succeeded() && repeated.value->additions.size() == 1
        && repeated.value->additions[0].model.modelPath == "Fixture.mdx", "caller cannot mutate the retained prepared difference");
    const PlacementMove move{*added.value,{20,30,40},{}};
    Require(MovePlacements(edits,std::span(&move,1)).Succeeded(),"move between coalesced snapshots");
    const auto second = Advance(*first.registry,edits);
    const auto adjacent = first.registry->DeltaTo(*second.registry), coalesced = initial->DeltaTo(*second.registry);
    Require(adjacent.Succeeded() && adjacent.value->additions.empty() && adjacent.value->edits.size() == 1
        && adjacent.value->edits[0].identity == identity, "adjacent move reuses its own prepared facts");
    Require(coalesced.Succeeded() && coalesced.value->additions.size() == 1 && coalesced.value->edits.empty()
        && coalesced.value->additions[0].identity == identity
        && coalesced.value->additions[0].model.position == std::array<float,3>{20,30,40},
        "skipped revision computes a complete fresh difference instead of borrowing the last command");
    Require(!second.registry->DeltaTo(*first.registry).Succeeded(),"cached adjacent difference cannot authorize reversed revisions");
    MapDataEditSession otherEdits(document); const auto other = Create(document,otherEdits);
    Require(!other->DeltaTo(*first.registry).Succeeded(),"prepared difference cannot authorize another edit session");
}
void TailAndReorderedCapturesKeepExactDifferences() {
    const auto document = Source(); MapDataEditSession edits(document);
    const auto capture = [](const MapDocument& source) {
        auto output = Capture(source);
        const auto count = source.Units()->units.size();
        std::rotate(output.first.begin(), output.first.begin() + count, output.first.end());
        std::rotate(output.second.begin(), output.second.begin() + count, output.second.end());
        return output; // 与世界实际捕获一致：装饰物在前，单位尾部追加。
    };
    auto inputs = capture(document);
    const auto original = WorldInstanceRegistry::Create(document, {7,1,edits.SessionIdentity(),edits.Revision(),"source-SHA"}, inputs.first, inputs.second);
    Require(original.Succeeded(), "doodads-first source");
    auto registry = *original.value;
    const auto advance = [&] {
        const auto next = edits.Snapshot(); const auto rows = capture(next);
        return registry->Prepare(edits.LastAppliedEdit(), registry->Binding(), registry->Binding().mapRevision + 1,
            next, rows.first, rows.second);
    };
    auto unit = document.Units()->units[0]; unit.position = {4,5,6};
    const auto added = AddUnitPlacement(edits, unit); Require(added.Succeeded(), "tail unit added");
    const auto receipt = edits.LastEdit();
    auto change = advance(); Require(change.Succeeded(), "tail capture prepares");
    const auto identity = *change.value->registry->Find(*added.value);
    Require(change.value->delta.additions.size() == 1 && change.value->delta.additions[0].identity == identity
        && change.value->delta.removals.empty() && change.value->delta.edits.empty() && change.value->delta.rows.empty(),
        "same-prefix append changes only its real new identity");
    registry = change.value->registry;
    Require(edits.Replay(receipt, false).Succeeded(), "tail undo");
    change = advance(); Require(change.Succeeded() && change.value->delta.removals.size() == 1
        && change.value->delta.removals[0] == identity && change.value->delta.additions.empty(), "tail undo removes exact identity");
    registry = change.value->registry;
    Require(edits.Replay(receipt, true).Succeeded(), "tail redo");
    change = advance(); Require(change.Succeeded() && change.value->delta.additions.size() == 1
        && change.value->delta.additions[0].identity == identity, "tail redo restores original identity");
    registry = change.value->registry;
    const PlacementMove move{*added.value, {14,15,16}, {}};
    Require(MovePlacements(edits, std::span(&move, 1)).Succeeded(), "move tail unit");
    const auto moved = edits.Snapshot(); auto movedRows = capture(moved);
    movedRows.first[0].modelPath = "ChangedAsset.mdx";
    const auto invalidAsset = registry->Prepare(edits.LastAppliedEdit(), registry->Binding(), registry->Binding().mapRevision + 1,
        moved, movedRows.first, movedRows.second);
    Require(!invalidAsset.Succeeded() && invalidAsset.diagnostics.front().code == "WORLD_INSTANCE_ASSET_REBIND_UNSUPPORTED",
        "same-prefix identities cannot bypass asset-change rejection");
    change = advance(); Require(change.Succeeded() && change.value->delta.edits.size() == 1
        && change.value->delta.edits[0].identity == identity && change.value->delta.edits[0].transform
        && change.value->delta.rows.empty(), "same-prefix movement still compares actual transforms");
    registry = change.value->registry;
    const PlacementMove movedAgain{*added.value, {24,25,26}, {}};
    Require(MovePlacements(edits, std::span(&movedAgain, 1)).Succeeded(), "move before reordered capture");
    const auto next = edits.Snapshot(); auto reordered = capture(next);
    std::reverse(reordered.first.begin(), reordered.first.end()); std::reverse(reordered.second.begin(), reordered.second.end());
    const auto orderChange = registry->Prepare(edits.LastAppliedEdit(), registry->Binding(), registry->Binding().mapRevision + 1,
        next, reordered.first, reordered.second);
    Require(orderChange.Succeeded() && orderChange.value->delta.edits.size() == 1
        && orderChange.value->delta.edits[0].identity == identity && orderChange.value->delta.additions.empty()
        && orderChange.value->delta.removals.empty() && orderChange.value->delta.rows.empty(),
        "reordered capture uses complete identity matching without manufacturing removals");
    auto altered = *next.Units(); altered.units[0].position[0] = 999;
    const std::vector<MapArchiveMember> wrongMembers{{"war3mapUnits.doo",*altered.Serialize().value},
        {"war3map.doo",*next.Doodads()->Serialize().value}};
    const auto wrongDocument = MapDocument::FromMembers(wrongMembers);
    const auto wrongSource = registry->Prepare(edits.LastAppliedEdit(), registry->Binding(), registry->Binding().mapRevision + 1,
        wrongDocument, reordered.first, reordered.second);
    Require(!wrongSource.Succeeded() && wrongSource.diagnostics.front().code == "WORLD_INSTANCE_MEMBER_STALE",
        "receipt proof still rejects an unrecorded member change");
    const WorldInstanceBinding binding{7,100,edits.SessionIdentity(),edits.Revision(),"source-SHA"};
    auto duplicate = reordered.second; duplicate[1] = duplicate[0];
    const auto duplicateCapture = WorldInstanceRegistry::Create(next, binding, reordered.first, duplicate);
    Require(!duplicateCapture.Succeeded() && duplicateCapture.diagnostics.front().code == "WORLD_INSTANCE_CAPTURE_STALE",
        "dense row flags still reject duplicate typed rows");
    duplicate = reordered.second; duplicate[0].file = static_cast<PlacementFile>(99);
    const auto invalidFile = WorldInstanceRegistry::Create(next, binding, reordered.first, duplicate);
    Require(!invalidFile.Succeeded() && invalidFile.diagnostics.front().code == "WORLD_INSTANCE_CAPTURE_STALE",
        "invalid file fails before indexing dense row flags");
}
}
int main() {
    try { DeleteMoveCopyAndReplayKeepExactIdentities(); ReusedCreationNumbersAndUntrustedTablesCannotReuseIdentity(); PreparedDifferencesRemainOwnedAndCoalesced(); TailAndReorderedCapturesKeepExactDifferences(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "world instance registry tests passed\n"; return 0;
}
