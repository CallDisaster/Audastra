#include "War3MapCore/MapData/TerrainRampEditing.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"
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

TerrainGridSnapshot Corner(int dx = 1, int dy = 1) {
    TerrainGridSnapshot terrain;
    terrain.header.version = 11; terrain.header.mainTileset = 'L';
    terrain.header.width = terrain.header.height = 17;
    terrain.header.groundTilesets = {"Tg01"}; terrain.header.cliffTilesets = {"Cl01"};
    terrain.vertices.resize(17 * 17);
    for (int y = 0; y < 17; ++y) for (int x = 0; x < 17; ++x) {
        auto& vertex = terrain.vertices[std::size_t(y) * 17 + x];
        vertex.groundHeightRaw = vertex.waterHeightAndFlagsRaw = 8192;
        vertex.cliffAndLayerRaw = (x - 8) * dx <= 0 && (y - 8) * dy <= 0 ? 3 : 2;
    }
    RefreshTerrainDerived(terrain, 0, 0, 17, 17); return terrain;
}

TerrainWaterLoadedRules CornerRules() {
    return {90, -256, 1536, 0, 128, 14, true, {{"Tg01", true}}, {"Cl01"}};
}

std::vector<std::byte> CornerWpmLiteral(bool after, int dx, int dy) {
    // 实施前手推的完整16x16地格矩阵；A=40，B=CA，每格展开为4x4。
    // 镜像的是地格SW索引15-x，不能误用顶点的16-x。
    constexpr std::array<std::string_view, 16> before{
        "AAAAAAAABAAAAAAA", "AAAAAAAABAAAAAAA", "AAAAAAAABAAAAAAA", "AAAAAAAABAAAAAAA",
        "AAAAAAAABAAAAAAA", "AAAAAAAABAAAAAAA", "AAAAAAAABAAAAAAA", "AAAAAAAABAAAAAAA",
        "BBBBBBBBBAAAAAAA", "AAAAAAAAAAAAAAAA", "AAAAAAAAAAAAAAAA", "AAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAA", "AAAAAAAAAAAAAAAA", "AAAAAAAAAAAAAAAA", "AAAAAAAAAAAAAAAA"};
    constexpr std::array<std::string_view, 16> created{
        "AAAAAAAABAAAAAAA", "AAAAAAAABAAAAAAA", "AAAAAAAABAAAAAAA", "AAAAAAAABAAAAAAA",
        "AAAAAAAABAAAAAAA", "AAAAAAAABAAAAAAA", "AAAAAAAABAAAAAAA", "AAAAAAAABBAAAAAA",
        "BBBBBBBBAAAAAAAA", "AAAAAAABAAAAAAAA", "AAAAAAAAAAAAAAAA", "AAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAA", "AAAAAAAAAAAAAAAA", "AAAAAAAAAAAAAAAA", "AAAAAAAAAAAAAAAA"};
    std::vector<std::byte> bytes(64 * 64);
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
        const auto cx = dx > 0 ? x / 4 : 15 - x / 4, cy = dy > 0 ? y / 4 : 15 - y / 4;
        bytes[y * 64 + x] = (after ? created : before)[cy][cx] == 'A' ? std::byte{0x40} : std::byte{0xca};
    }
    return bytes;
}

void NativeCornerFourDirectionsAndLiteralCompleteOutput() {
    static_assert(static_cast<unsigned>(TerrainBrushKind::CornerRamp) == 13);
    const std::array ground{TerrainNativeGroundRule{"Tg01", true, true, true}};
    const auto doodads = *ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11()).value;
    for (const int dx : {1, -1}) for (const int dy : {1, -1}) {
        const auto source = Corner(dx, dy); const auto bytes = *SerializeMapTerrain(source).value;
        const TerrainStrokePoint point{1024.0 + 32 * dx, 1024.0 + 32 * dy};
        TerrainBrush brush; brush.kind = TerrainBrushKind::CornerRamp;
        const auto plan = PlanTerrainCornerRamp(source, point);
        Require(plan.Succeeded() && plan.value->highX == 8 && plan.value->highY == 8
            && plan.value->directionX == dx && plan.value->directionY == dy, "canonical corner selects the high vertex in all directions");
        const auto input = [&](const TerrainGridSnapshot& terrain) {
            TerrainNativePathingInput value; value.terrain = &terrain; value.groundRules = ground;
            value.playableBounds = {0, 0, 2048, 2048}; value.maximumCliffLevel = 14; return value;
        };
        Require(*GenerateTerrainNativePathing(input(source)).value == CornerWpmLiteral(false, dx, dy), "complete original corner WPM matches independent literal");
        const auto candidate = PrepareTerrainCornerRampStroke(source, CornerRules(), brush, std::span(&point, 1), doodads);
        Require(candidate.Succeeded() && candidate.value && candidate.value->result.changedVertices == 9, "owned corner writes exactly nine R bits");
        auto expected = source;
        for (int x = 0; x < 3; ++x) for (int y = 0; y < 3; ++y)
            expected.vertices[std::size_t(8 + y * dy) * 17 + 8 + x * dx].textureAndFlagsRaw = 0x10;
        Require(*SerializeMapTerrain(candidate.value->terrain).value == *SerializeMapTerrain(expected).value, "all W3E bytes equal the independent nine-bit expected map");
        Require(*SerializeMapTerrain(source).value == bytes, "pure candidate preserves source bytes");
        Require(*GenerateTerrainNativePathing(input(candidate.value->terrain)).value == CornerWpmLiteral(true, dx, dy), "complete created WPM includes both new shoulder blocks");
        const auto labels = ClassifyNativeDryRampCorners(candidate.value->terrain, 14);
        constexpr std::array<std::string_view, 5> expectedLabels{"ABAAA", "BHRLA", "ARSLA", "ALLLA", "AAAAA"};
        Require(labels.Succeeded(), "complete corner labels classify");
        for (int y = -1; y <= 3; ++y) for (int x = -1; x <= 3; ++x)
            Require((*labels.value)[std::size_t(8 + y * dy) * 17 + 8 + x * dx] == expectedLabels[y + 1][x + 1], "live corner labels match pre-implementation literal");
        Require(QueryNativeTerrainRamp(candidate.value->terrain, 14, 8 + dx, 8 + dy).value->heightOffset == 32,
            "corner S height is32 rather than the straight-ramp64");
        Require(PrepareTerrainCornerRampStroke(candidate.value->terrain, CornerRules(), brush, std::span(&point, 1), doodads).value->result.changed.Empty(), "complete repeated creation is a no-op");
        brush.value = false;
        const auto removed = PrepareTerrainCornerRampStroke(candidate.value->terrain, CornerRules(), brush, std::span(&point, 1), doodads);
        Require(removed.Succeeded() && *SerializeMapTerrain(removed.value->terrain).value == bytes, "complete removal restores every W3E byte");
        Require(*GenerateTerrainNativePathing(input(removed.value->terrain)).value == CornerWpmLiteral(false, dx, dy), "removal restores full WPM literal");
    }
}

void NativeCornerUnsupportedDomainsRejectAtomically() {
    const auto doodads = *ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11()).value;
    TerrainBrush brush; brush.kind = TerrainBrushKind::CornerRamp;
    const TerrainStrokePoint point{1056, 1056};
    const auto rejects = [&](const TerrainGridSnapshot& terrain) {
        const auto before = *SerializeMapTerrain(terrain).value;
        Require(!PrepareTerrainCornerRampStroke(terrain, CornerRules(), brush, std::span(&point, 1), doodads).Succeeded(), "unsupported corner domain rejects");
        Require(*SerializeMapTerrain(terrain).value == before, "domain rejection preserves complete source");
    };
    auto terrain = Corner(); terrain.vertices[8 * 17 + 8].textureAndFlagsRaw |= 0x10; rejects(terrain);
    terrain = Corner(); terrain.vertices[4 * 17 + 4].textureAndFlagsRaw |= 0x40; rejects(terrain);
    terrain = Corner(); terrain.vertices[2 * 17 + 2].waterHeightAndFlagsRaw |= 0x8000; rejects(terrain);
    terrain = Corner(); terrain.vertices[4 * 17 + 4].textureAndFlagsRaw |= 0x80; rejects(terrain);
    terrain = Corner(); terrain.vertices[4 * 17 + 4].cliffAndLayerRaw = 2; rejects(terrain);
    terrain = Corner(); terrain.vertices[4 * 17 + 4].groundHeightRaw += 1; rejects(terrain);
    terrain = Corner(); terrain.vertices[4 * 17 + 4].textureAndFlagsRaw |= 0x10; rejects(terrain);
    const auto source = Corner();
    auto existing = PrepareTerrainCornerRampStroke(source, CornerRules(), brush, std::span(&point, 1), doodads).value->terrain;
    existing.vertices[4 * 17 + 4].textureAndFlagsRaw |= 0x10; rejects(existing); // no-op也核完整域。
    for (const TerrainStrokePoint invalid : {TerrainStrokePoint{1024, 1056}, TerrainStrokePoint{1088, 1056},
        TerrainStrokePoint{1056.5, 1056}, TerrainStrokePoint{std::numeric_limits<double>::infinity(), 1056}})
        Require(!PlanTerrainCornerRamp(source, invalid).Succeeded(), "canonical point rejects boundaries, fractional or nonfinite input");
    Require(SnapTerrainCornerRampPoint(source, {1050, 1060})->x == 1056, "UI snap is explicit quarter-cell center");
    const std::array points{point, point};
    Require(!PrepareTerrainCornerRampStroke(source, CornerRules(), brush, points, doodads).Succeeded(), "multiple points cannot silently split into corners");
    brush.radius = 0;
    Require(!PrepareTerrainCornerRampStroke(source, CornerRules(), brush, std::span(&point, 1), doodads).Succeeded(), "nondefault ordinary field rejects");
    brush = {}; brush.kind = TerrainBrushKind::CornerRamp;
    auto limits = TerrainWaterBrushLimits{}; limits.maximumWork = 1;
    Require(!PrepareTerrainCornerRampStroke(source, CornerRules(), brush, std::span(&point, 1), doodads, {}, limits).Succeeded(), "bounded work rejects before any source mutation");
    auto special = doodads; special.specialDoodads.push_back({"Doo0", 0, {8, 8}});
    const std::array shapes{TerrainWaterSpecialDoodadShape{0, "Doo0", 0, {8, 8}, 8, 8, 9, 9, {0, 0, 0, 0}}};
    Require(!PrepareTerrainCornerRampStroke(source, CornerRules(), brush, std::span(&point, 1), special, shapes).Succeeded(), "special intersection rejects rather than deleting it");
    auto edge = Corner();
    for (int y = 0; y < 17; ++y) for (int x = 0; x < 17; ++x) edge.vertices[y * 17 + x].cliffAndLayerRaw = x <= 2 && y <= 2 ? 3 : 2;
    const TerrainStrokePoint edgePoint{288, 288};
    Require(PlanTerrainCornerRamp(edge, edgePoint).Succeeded()
        && !PrepareTerrainCornerRampStroke(edge, CornerRules(), brush, std::span(&edgePoint, 1), doodads).Succeeded(), "nine points alone do not grant incomplete dirty/halo domain");
    auto ordinary = source;
    Require(!ApplyTerrainStroke(ordinary, brush, std::span(&point, 1)).Succeeded(), "unowned ordinary brush cannot bypass structural owner");
}

void NativeCornerBlightOnlyUsesRampFinisher() {
    auto source = Corner(); for (auto& vertex : source.vertices) vertex.textureAndFlagsRaw = 0x20;
    auto rules = CornerRules(); rules.groundRules.front().permitsBlight = false;
    const auto doodads = *ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11()).value;
    TerrainBrush brush; brush.kind = TerrainBrushKind::CornerRamp; const TerrainStrokePoint point{1056, 1056};
    auto expected = source;
    // 实际九点Gather出的四块顶点并集为[4,12]^2；禁荒芜地面逐点清除。
    for (int y = 4; y <= 12; ++y) for (int x = 4; x <= 12; ++x) expected.vertices[y * 17 + x].textureAndFlagsRaw = 0;
    for (int y = 8; y <= 10; ++y) for (int x = 8; x <= 10; ++x) expected.vertices[y * 17 + x].textureAndFlagsRaw |= 0x10;
    const auto candidate = PrepareTerrainCornerRampStroke(source, rules, brush, std::span(&point, 1), doodads);
    Require(candidate.Succeeded() && *SerializeMapTerrain(candidate.value->terrain).value == *SerializeMapTerrain(expected).value,
        "corner only adds nine R bits and applies ramp blight finisher to actual dirty blocks");
}

} // namespace

int main() {
    try {
        FourDirectionsAndExactBytes(); WholeStrokeRejectsBeforeMutation();
        AdjacentStripsAndSampling(); SharedSessionUndoAndDoodadFollow();
        NativeCornerFourDirectionsAndLiteralCompleteOutput(); NativeCornerUnsupportedDomainsRejectAtomically();
        NativeCornerBlightOnlyUsesRampFinisher();
        std::cout << "TerrainRampEditing: 7 scenario groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TerrainRampEditing: " << error.what() << '\n';
        return 1;
    }
}
