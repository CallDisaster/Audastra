#include "War3MapCore/MapData/PlacementEditing.h"
#include "MapDataFixtures.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

using namespace War3Map;
using namespace War3Map::MapData;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 设置 + 两个单位（起始位置、步兵）+ 两个装饰物（树、石头）。
std::vector<MapArchiveMember> Members() {
    auto units = ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    Require(units.Succeeded() && units.value, "empty units fixture must parse");
    UnitPlacement unit;
    unit.rawcode = "sloc"; unit.creationId = 10; unit.owner = 0;
    unit.position = {100.0f, 200.0f, 64.0f}; unit.scale = {1, 1, 1};
    units.value->units.push_back(unit);
    unit.rawcode = "hfoo"; unit.creationId = 20; unit.owner = 1;
    unit.position = {300.0f, 400.0f, 0.0f};
    units.value->units.push_back(unit);
    auto doodads = ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11());
    Require(doodads.Succeeded() && doodads.value, "empty doodads fixture must parse");
    DoodadPlacement doodad;
    doodad.rawcode = "LTlt"; doodad.creationId = 5; doodad.flags = 2;
    doodad.position = {-50.0f, 60.0f, 8.0f}; doodad.scale = {1, 1, 1};
    doodads.value->doodads.push_back(doodad);
    doodad.rawcode = "LOrk"; doodad.creationId = 6; doodad.position = {70.0f, 80.0f, 0.0f};
    doodads.value->doodads.push_back(doodad);
    auto unitBytes = units.value->Serialize();
    auto doodadBytes = doodads.value->Serialize();
    Require(unitBytes.Succeeded() && doodadBytes.Succeeded(), "placement fixtures must serialize");
    return {{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", std::move(*unitBytes.value)}, {"war3map.doo", std::move(*doodadBytes.value)}};
}

void MoveAcrossFilesIsOneTransaction() {
    const auto members = Members();
    const auto source = MapDocument::FromMembers(members);
    MapDataEditSession edits(source);
    const auto footman = MakePlacementId(PlacementFile::Units, 1, source.Units()->units[1]);
    const auto tree = MakePlacementId(PlacementFile::Doodads, 0, source.Doodads()->doodads[0]);
    const std::vector<PlacementMove> moves{{footman, {512.0f, 640.0f, 16.0f}, 7.0f}, {tree, {1.0f, 2.0f, 3.0f}, std::nullopt}};
    Require(MovePlacements(edits, moves).Succeeded(), "moving across both files must succeed");
    Require(edits.Revision() == 1 && edits.Replacements().size() == 2, "both files must change in one revision");
    const auto moved = edits.Snapshot();
    const auto& unit = moved.Units()->units[1];
    Require(unit.position == std::array<float, 3>{512.0f, 640.0f, 16.0f}, "unit position must be written");
    Require(std::abs(unit.rotation - (7.0f - 6.28318530718f)) < 1e-5f, "rotation must be normalized to [0, 2pi)");
    Require(moved.Doodads()->doodads[0].position == std::array<float, 3>{1.0f, 2.0f, 3.0f}
        && moved.Doodads()->doodads[0].rotation == source.Doodads()->doodads[0].rotation,
        "doodad moves without rotation must keep orientation");
    Require(FindPlacement(moved, footman).Succeeded() && FindPlacement(moved, tree).value->position[0] == 1.0f,
        "ids stay valid after a move");
    Require(edits.Undo() && !edits.IsDirty(), "one undo restores both files");
}

void StartLocationMoveLinksPlayerSettings() {
    const auto members = Members();
    const auto source = MapDocument::FromMembers(members);
    MapDataEditSession edits(source);
    const PlacementMove move{MakePlacementId(PlacementFile::Units, 0, source.Units()->units[0]), {900.0f, 1000.0f, 64.0f}, {}};
    Require(MovePlacements(edits, std::span(&move, 1)).Succeeded(), "moving a start location must succeed");
    const auto moved = edits.Snapshot();
    Require(moved.Settings()->players[0].startX == 900.0f && moved.Settings()->players[0].startY == 1000.0f,
        "player start must follow the moved start location");
    const auto id = MakePlacementId(PlacementFile::Units, 0, moved.Units()->units[0]);
    Require(!DeletePlacements(edits, std::span(&id, 1)).Succeeded() && edits.Revision() == 1,
        "start locations cannot be deleted through placement editing");
}

void DeleteShiftsIndicesAndRejectsStaleIds() {
    const auto members = Members();
    const auto source = MapDocument::FromMembers(members);
    MapDataEditSession edits(source);
    const std::vector<PlacementId> ids{MakePlacementId(PlacementFile::Doodads, 0, source.Doodads()->doodads[0]),
        MakePlacementId(PlacementFile::Units, 1, source.Units()->units[1])};
    Require(DeletePlacements(edits, ids).Succeeded() && edits.Revision() == 1, "delete must be one transaction");
    const auto after = edits.Snapshot();
    Require(after.Doodads()->doodads.size() == 1 && after.Doodads()->doodads[0].rawcode == "LOrk"
        && after.Units()->units.size() == 1, "deleted placements must be removed");
    const auto stale = MakePlacementId(PlacementFile::Doodads, 1, source.Doodads()->doodads[1]);
    const PlacementMove move{stale, {0, 0, 0}, {}};
    Require(!MovePlacements(edits, std::span(&move, 1)).Succeeded(), "an id whose index shifted must be stale");
    Require(!FindPlacement(after, stale).Succeeded(), "find must reject stale ids");
    auto wrongCreation = MakePlacementId(PlacementFile::Doodads, 0, after.Doodads()->doodads[0]);
    wrongCreation.creationId = 99;
    Require(!FindPlacement(after, wrongCreation).Succeeded(), "creation number must be verified");
}

void InvalidInputChangesNothing() {
    const auto members = Members();
    const auto source = MapDocument::FromMembers(members);
    MapDataEditSession edits(source);
    const auto id = MakePlacementId(PlacementFile::Units, 1, source.Units()->units[1]);
    PlacementMove move{id, {std::numeric_limits<float>::quiet_NaN(), 0, 0}, {}};
    Require(!MovePlacements(edits, std::span(&move, 1)).Succeeded(), "NaN coordinates must be rejected");
    move.position = {2e6f, 0, 0};
    Require(!MovePlacements(edits, std::span(&move, 1)).Succeeded(), "out-of-range coordinates must be rejected");
    move.position = {1, 2, 3};
    const std::vector<PlacementMove> twice{move, move};
    Require(!MovePlacements(edits, twice).Succeeded(), "duplicate ids must be rejected");
    Require(edits.Revision() == 0 && !edits.IsDirty(), "rejected edits must not create history");
    const auto missing = MapDocument::FromMembers(std::vector<MapArchiveMember>{members[0]});
    MapDataEditSession empty(missing);
    Require(!MovePlacements(empty, std::span(&move, 1)).Succeeded(), "missing files must reject ids");
}

void CopyGroupPreservesFieldsAndOneUndo() {
    auto members = Members();
    auto units = ParseUnitPlacements(members[1].data);
    auto doodads = ParseDoodadPlacements(members[2].data);
    Require(units.Succeeded() && doodads.Succeeded(), "copy fixtures must parse");
    auto& originalUnit = units.value->units[1];
    originalUnit.flags = 7; originalUnit.hitPoints = 173; originalUnit.owner = 5;
    originalUnit.unknownOwnerBytes = {0x12, 0x34}; originalUnit.droppedItemTable = 9;
    originalUnit.itemSets.push_back({{{"I000", 85}}});
    originalUnit.inventory.push_back({2, "I001"}); originalUnit.abilities.push_back({"A000", true, 3});
    originalUnit.random = RandomAnyUnit{4, 2};
    auto& originalDoodad = doodads.value->doodads[0];
    originalDoodad.lifePercent = 37; originalDoodad.droppedItemTable = 8;
    originalDoodad.itemSets.push_back({{{"I002", 60}}});
    members[1].data = *units.value->Serialize().value;
    members[2].data = *doodads.value->Serialize().value;
    const auto source = MapDocument::FromMembers(members);
    MapDataEditSession edits(source);
    const std::vector<PlacementId> ids{
        MakePlacementId(PlacementFile::Doodads, 0, source.Doodads()->doodads[0]),
        MakePlacementId(PlacementFile::Units, 1, source.Units()->units[1])};
    const auto copied = CopyPlacements(edits, ids, {64, -32, 4});
    Require(copied.Succeeded() && copied.value->size() == 2 && edits.Revision() == 1 && edits.Replacements().size() == 2,
        "mixed group must copy in one edit transaction");
    const auto after = edits.Snapshot();
    const auto& newDoodad = after.Doodads()->doodads.back();
    const auto& newUnit = after.Units()->units.back();
    Require((*copied.value)[0].file == PlacementFile::Doodads && (*copied.value)[0].creationId == 7 &&
        (*copied.value)[1].file == PlacementFile::Units && (*copied.value)[1].creationId == 21,
        "returned identities must keep selection order and use fresh ids");
    Require(newDoodad.position == std::array<float, 3>{14, 28, 12} && newDoodad.lifePercent == 37 &&
        newDoodad.droppedItemTable == 8 && newDoodad.itemSets[0].items[0].rawcode == "I002" &&
        newUnit.position == std::array<float, 3>{364, 368, 4} && newUnit.owner == 5 && newUnit.flags == 7 &&
        newUnit.unknownOwnerBytes == std::array<std::uint8_t, 2>{0x12, 0x34} && newUnit.hitPoints == 173 &&
        newUnit.droppedItemTable == 9 && newUnit.itemSets[0].items[0].rawcode == "I000" &&
        newUnit.inventory[0].rawcode == "I001" && newUnit.abilities[0].level == 3 &&
        std::holds_alternative<RandomAnyUnit>(newUnit.random) && newUnit.rotation == originalUnit.rotation,
        "copy must preserve owner, drop, nested data, orientation and full source placement");
    Require(edits.Undo() && edits.Snapshot().Units()->units.size() == 2 &&
        edits.Snapshot().Doodads()->doodads.size() == 2, "one undo must restore both files");
    Require(edits.Redo() && FindPlacement(edits.Snapshot(), (*copied.value)[1]).Succeeded(),
        "one redo must restore the copied group");
    const auto revision = edits.Revision();
    const std::vector<PlacementId> starts{MakePlacementId(PlacementFile::Units, 0, source.Units()->units[0])};
    Require(!CopyPlacements(edits, starts, {10, 0, 0}).Succeeded(), "start location cannot be copied");
    Require(!CopyPlacements(edits, ids, {std::numeric_limits<float>::infinity(), 0, 0}).Succeeded(),
        "invalid group offset must be rejected");
    Require(!CopyPlacements(edits, {ids.data(), ids.size()}, {1e6f, 0, 0}).Succeeded(),
        "out-of-range destination must be rejected");
    const std::vector<PlacementId> duplicates{ids[0], ids[0]};
    Require(!CopyPlacements(edits, duplicates, {0, 0, 0}).Succeeded(),
        "duplicate source must fail as a whole");
    auto stale = ids[1]; stale.creationId = 999;
    Require(!CopyPlacements(edits, std::span(&stale, 1), {0, 0, 0}).Succeeded() && edits.Revision() == revision,
        "stale identity and other rejected copies must create no revision");
}

void AddRegionAndCameraEdits() {
    auto members = Members();
    // 一个区域与一个镜头（经典 v5 / v0 布局）。
    RegionsDocument regions{5, {{-10, -20, 30, 40, "Spawn", 1, {}, "", {0, 0, 255, 0}}}, {}};
    auto regionBytes = regions.Serialize();
    CamerasDocument cameras{0, {{100, 200, 0, 90, 304, 1650, 0, 70, 5000, 100, "Intro"}}, {}};
    auto cameraBytes = cameras.Serialize();
    Require(regionBytes.Succeeded() && cameraBytes.Succeeded(), "auxiliary fixtures must serialize");
    members.push_back({"war3map.w3r", std::move(*regionBytes.value)});
    members.push_back({"war3map.w3c", std::move(*cameraBytes.value)});
    const auto source = MapDocument::FromMembers(members);
    Require(source.Regions() && source.Cameras(), "auxiliary fixtures must parse");
    MapDataEditSession edits(source);

    UnitPlacement unit;
    unit.rawcode = "hpea"; unit.owner = 0; unit.position = {5, 6, 7}; unit.scale = {1, 1, 1};
    const auto added = AddUnitPlacement(edits, unit);
    Require(added.Succeeded() && added.value->index == 2 && added.value->creationId == 21, "new unit gets next creation id");
    DoodadPlacement doodad;
    doodad.rawcode = "LTlt"; doodad.flags = 2; doodad.position = {1, 1, 1}; doodad.scale = {1, 1, 1};
    const auto tree = AddDoodadPlacement(edits, doodad);
    Require(tree.Succeeded() && tree.value->index == 2 && tree.value->creationId == 7, "new doodad gets next creation id");
    Require(edits.Revision() == 2 && FindPlacement(edits.Snapshot(), *added.value).Succeeded(), "each add is one transaction");
    unit.rawcode = "bad";
    Require(!AddUnitPlacement(edits, unit).Succeeded() && edits.Revision() == 2, "invalid rawcode is rejected");

    const auto region = MakeRegionId(0, source.Regions()->entries[0]);
    Require(SetRegionBounds(edits, region, 0, 0, 64, 32).Succeeded(), "region bounds edit must succeed");
    const auto afterRegion = edits.Snapshot();
    const auto& moved = afterRegion.Regions()->entries[0];
    Require(moved.left == 0 && moved.top == 32 && moved.name == "Spawn", "region bounds written, name kept");
    Require(!SetRegionBounds(edits, region, 10, 0, 5, 32).Succeeded(), "inverted bounds rejected");
    auto staleRegion = region; staleRegion.name = "Other";
    Require(!SetRegionBounds(edits, staleRegion, 0, 0, 64, 64).Succeeded(), "stale region id rejected");
    const auto camera = MakeCameraId(0, source.Cameras()->entries[0]);
    Require(SetCameraTarget(edits, camera, -300, 400).Succeeded(), "camera target edit must succeed");
    const auto afterCamera = edits.Snapshot();
    const auto& target = afterCamera.Cameras()->entries[0];
    Require(target.targetX == -300 && target.targetY == 400 && target.targetDistance == 1650, "camera target only");
    Require(edits.Undo() && edits.Snapshot().Cameras()->entries[0].targetX == 100, "camera edit is undoable");

    auto nextRegion = source.Regions()->entries[0];
    nextRegion.name = "SpawnCopy"; nextRegion.creationNumber = 2;
    nextRegion.left = 64; nextRegion.right = 96;
    const auto beforeCreate = edits.Revision();
    const auto createdRegion = AddRegion(edits, nextRegion);
    Require(createdRegion.Succeeded() && createdRegion.value->index == 1 &&
        edits.Revision() == beforeCreate + 1, "explicit region record is one transaction");
    Require(edits.Snapshot().Regions()->entries[0].name == "Spawn" &&
        edits.Snapshot().Regions()->entries[1].ambientSound == nextRegion.ambientSound,
        "region creation preserves existing and explicit fields");
    Require(!AddRegion(edits, nextRegion).Succeeded() && edits.Revision() == beforeCreate + 1,
        "duplicate region identity creates no revision");
    auto nextCamera = source.Cameras()->entries[0];
    nextCamera.name = "IntroCopy"; nextCamera.targetX = 300;
    const auto createdCamera = AddCamera(edits, nextCamera);
    Require(createdCamera.Succeeded() && createdCamera.value->index == 1 &&
        edits.Revision() == beforeCreate + 2, "explicit camera record is one transaction");
    Require(!AddCamera(edits, nextCamera).Succeeded() && edits.Revision() == beforeCreate + 2,
        "duplicate camera identity creates no revision");
    Require(edits.Undo() && edits.Snapshot().Cameras()->entries.size() == 1 &&
        edits.Redo() && edits.Snapshot().Cameras()->entries[1].targetX == 300,
        "camera creation is undone and redone as one edit");
    nextRegion.left = std::numeric_limits<float>::infinity();
    Require(!AddRegion(edits, nextRegion).Succeeded(), "invalid explicit region is rejected");
}

} // namespace

int main() {
    try {
        MoveAcrossFilesIsOneTransaction();
        StartLocationMoveLinksPlayerSettings();
        DeleteShiftsIndicesAndRejectsStaleIds();
        InvalidInputChangesNothing();
        CopyGroupPreservesFieldsAndOneUndo();
        AddRegionAndCameraEdits();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "placement editing tests passed\n";
    return 0;
}
