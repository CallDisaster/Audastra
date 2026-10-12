#include "War3MapCore/MapData/TerrainRampEditing.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"
#include "MapDataFixtures.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::MapData;

namespace {

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

TerrainGridSnapshot Step(bool horizontal = false, bool reversed = false) {
    TerrainGridSnapshot terrain;
    terrain.header.version = 11; terrain.header.mainTileset = 'L';
    terrain.header.width = 17; terrain.header.height = 17; terrain.header.offset = {-1024, -1024};
    terrain.header.groundTilesets = {"Ldrt"}; terrain.header.cliffTilesets = {"CLdi"};
    terrain.vertices.resize(17 * 17);
    for (std::uint32_t y = 0; y < 17; ++y) for (std::uint32_t x = 0; x < 17; ++x) {
        auto& vertex = terrain.vertices[y * 17 + x];
        const bool highSide = ((horizontal ? y : x) >= 8) != reversed;
        vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936;
        vertex.textureAndFlagsRaw = 0x20; vertex.variationRaw = 0xa3;
        vertex.cliffAndLayerRaw = static_cast<std::uint8_t>(highSide ? 3 : 2);
    }
    RefreshTerrainDerived(terrain, 0, 0, 17, 17);
    return terrain;
}

TerrainStrokePoint Cursor(bool horizontal = false) {
    return horizontal ? TerrainStrokePoint{32, -96} : TerrainStrokePoint{-96, 32};
}

void FourDirectionsAndExactBytes() {
    for (const bool horizontal : {false, true}) for (const bool reversed : {false, true}) {
        auto terrain = Step(horizontal, reversed);
        const auto original = *SerializeMapTerrain(terrain).value;
        const auto cursor = Cursor(horizontal);
        const auto plan = PlanTerrainRamp(terrain, cursor);
        Require(plan.Succeeded() && plan.value, "straight step produces a plan");
        Require(plan.value->footprint.x1 - plan.value->footprint.x0 == (horizontal ? 1u : 3u)
            && plan.value->footprint.y1 - plan.value->footprint.y0 == (horizontal ? 3u : 1u),
            "preview is the actual three-vertex strip in every direction");
        const auto applied = ApplyTerrainRampStroke(terrain, true, std::span(&cursor, 1));
        Require(applied.Succeeded() && applied.value && !applied.value->Empty(), "straight ramp applies");
        auto expected = Step(horizontal, reversed);
        const std::uint32_t first = reversed ? 7 : 6;
        for (std::uint32_t value = first; value < first + 3; ++value)
            expected.vertices[horizontal ? value * 17 + 8 : 8 * 17 + value].textureAndFlagsRaw |= 0x10;
        const auto written = *SerializeMapTerrain(terrain).value;
        Require(written == *SerializeMapTerrain(expected).value, "only the three selected ramp bits change");
        const auto parsed = ParseMapTerrain(written);
        Require(parsed.Succeeded() && *SerializeMapTerrain(*parsed.value->Snapshot()).value == written,
            "ramp record survives parsing and serialization byte for byte");
        Require(ApplyTerrainRampStroke(terrain, true, std::span(&cursor, 1)).value->Empty(), "same ramp is a no-op");
        Require(ApplyTerrainRampStroke(terrain, false, std::span(&cursor, 1)).Succeeded(), "whole ramp can be removed");
        Require(*SerializeMapTerrain(terrain).value == original, "removal restores every original byte");
    }
}

void WholeStrokeRejectsBeforeMutation() {
    auto terrain = Step();
    const auto original = *SerializeMapTerrain(terrain).value;
    const std::array points{Cursor(), TerrainStrokePoint{-640, -640}};
    Require(!ApplyTerrainRampStroke(terrain, true, points).Succeeded(), "flat later point rejects the whole stroke");
    Require(*SerializeMapTerrain(terrain).value == original, "rejected later point leaves no first ramp behind");
    const TerrainStrokePoint outside{std::numeric_limits<double>::infinity(), 0};
    Require(!PlanTerrainRamp(terrain, outside).Succeeded(), "non-finite cursor rejects");
    Require(!ApplyTerrainRampStroke(terrain, true, {}).Succeeded(), "empty ramp stroke rejects");
    terrain.vertices[8 * 17 + 6].textureAndFlagsRaw |= 0x40;
    const auto wet = *SerializeMapTerrain(terrain).value;
    const auto cursor = Cursor();
    Require(!ApplyTerrainRampStroke(terrain, true, std::span(&cursor, 1)).Succeeded(), "water in the ramp neighborhood rejects");
    Require(*SerializeMapTerrain(terrain).value == wet, "water rejection preserves the source");
    terrain = Step(); terrain.vertices[8 * 17 + 7].textureAndFlagsRaw |= 0x10;
    Require(!PlanTerrainRamp(terrain, cursor).Succeeded(), "incomplete imported ramp is not extended silently");
    terrain = Step(); terrain.vertices[7 * 17 + 8].cliffAndLayerRaw = 2;
    Require(!PlanTerrainRamp(terrain, cursor).Succeeded(), "corner or conflicting step in the neighbor row rejects");
    terrain = Step(); terrain.vertices[9 * 17 + 8].groundHeightRaw += 1;
    Require(!PlanTerrainRamp(terrain, cursor).Succeeded(), "uneven raw height remains unsupported");
    terrain = Step(); terrain.vertices[9 * 17 + 8].waterHeightAndFlagsRaw |= 0x8000;
    Require(!PlanTerrainRamp(terrain, cursor).Succeeded(), "unknown neighboring water flags reject");
}

void AdjacentStripsAndSampling() {
    auto terrain = Step();
    const std::array points{Cursor(), TerrainStrokePoint{-96, 160}, Cursor()};
    Require(ApplyTerrainRampStroke(terrain, true, points).Succeeded(), "adjacent strips form one stroke");
    Require(std::count_if(terrain.vertices.begin(), terrain.vertices.end(), [](const auto& vertex) { return vertex.IsRamp(); }) == 6,
        "overlap is deduplicated while adjacent strips remain distinct");
    Require(TerrainRampHeightOffset(terrain, 7, 8) == 64.0f && !TerrainRampHeightOffset(terrain, 6, 8),
        "only the midpoint between high and low ramp ends is raised");
    Require(SampleGroundHeight(terrain, -128, 0, TerrainHeightSource::RawWithRamps) == 64.0f,
        "raw terrain sampling includes the ramp midpoint");
    Require(SampleGroundHeight(terrain, -128, 0) == 0.0f, "stored-position sampling keeps its existing meaning");
    auto shown = terrain;
    for (std::uint32_t y = 0; y < 17; ++y) for (std::uint32_t x = 0; x < 17; ++x)
        shown.vertices[y * 17 + x].position[2] += TerrainRampHeightOffset(terrain, x, y).value_or(0);
    Require(SampleGroundHeight(shown, -128, 0) == 64.0f, "rendered surface picking does not add the ramp twice");
}

void SharedSessionUndoAndDoodadFollow() {
    auto doodads = ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11());
    Require(doodads.Succeeded(), "doodad fixture parses");
    DoodadPlacement tree;
    tree.rawcode = "LTlt"; tree.creationId = 1; tree.flags = 2; tree.position = {-128, 0, 10}; tree.scale = {1, 1, 1};
    doodads.value->doodads.push_back(tree);
    const auto source = MapDocument::FromMembers(std::vector<MapArchiveMember>{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}, {"war3map.doo", *doodads.value->Serialize().value}});
    MapDataEditSession edits(source);
    const auto original = *SerializeMapTerrain(Step()).value;
    Require(edits.AttachTerrain(original).Succeeded(), "session attaches step terrain");
    TerrainBrush brush; brush.kind = TerrainBrushKind::Ramp;
    const auto cursor = Cursor();
    Require(edits.EditTerrain(brush, std::span(&cursor, 1), true).Succeeded(), "ramp uses the shared session transaction");
    Require(edits.Snapshot().Doodads()->doodads.front().position[2] == 74.0f, "doodad follows the actual ramp lift");
    Require(!edits.VerifyTerrainPathingForSave().Succeeded(), "ramp does not bypass the derived-data save gate");
    const auto candidate = *SerializeMapTerrain(*edits.Terrain()).value;
    Require(edits.Undo() && *SerializeMapTerrain(*edits.Terrain()).value == original,
        "undo restores all ramp bytes");
    Require(edits.Snapshot().Doodads()->doodads.front().position[2] == 10.0f, "same undo restores doodad offset");
    Require(edits.Redo() && *SerializeMapTerrain(*edits.Terrain()).value == candidate,
        "redo restores the same ramp bytes");
    Require(edits.Snapshot().Doodads()->doodads.front().position[2] == 74.0f, "redo restores doodad follow");
    const auto revision = edits.Revision();
    Require(edits.EditTerrain(brush, std::span(&cursor, 1), true).Succeeded() && edits.Revision() == revision,
        "an unchanged ramp does not create another history entry");
    MapDataEditSession restored(source);
    Require(restored.AttachTerrain(original).Succeeded() && restored.SetTerrain(candidate).Succeeded(),
        "serialized project terrain overlay restores through the existing command");
    Require(*SerializeMapTerrain(*restored.Terrain()).value == candidate && restored.Undo() && !restored.IsDirty(),
        "restored overlay remains one exact undoable edit");
}

} // namespace

int main() {
    try {
        FourDirectionsAndExactBytes(); WholeStrokeRejectsBeforeMutation();
        AdjacentStripsAndSampling(); SharedSessionUndoAndDoodadFollow();
        std::cout << "TerrainRampEditing: 4 scenario groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TerrainRampEditing: " << error.what() << '\n';
        return 1;
    }
}
