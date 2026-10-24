#include "TerrainUnitPlacementRealTests.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapMinimapMarkers.h"
#include "War3MapCore/Script/MapScriptBuild.h"
#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace War3Map::TerrainSaveTests {
namespace {
using namespace MapData;
using namespace Services;
constexpr std::size_t kBytes = 64u * 1024u * 1024u;
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    if (result.Succeeded() && result.value) return std::move(*result.value);
    std::string message;
    for (const auto& item : result.diagnostics) message += item.code + ": " + item.message + "\n";
    throw std::runtime_error(message.empty() ? "missing real units test result" : message);
}
std::string ScriptPlacement(const UnitPlacement& unit) {
    std::ostringstream text; text.imbue(std::locale::classic()); text << std::fixed << std::setprecision(1);
    text << (unit.rawcode == "ratf" ? "CreateItem( '" : "CreateUnit( p, '") << unit.rawcode
        << "', " << unit.position[0] << ", " << unit.position[1];
    return text.str();
}
} // namespace

void CheckRealTerrainUnitPlacementSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game) {
    const auto terrainBytes = Take(original.ReadMember("war3map.w3e"));
    const auto before = *Take(ParseMapTerrain(terrainBytes)).Snapshot();
    Require(before.header.width == 65 && before.header.height == 65, "units regression uses an original 64-cell map");
    const auto rules = Take(TerrainPathingSave::CaptureNative(original, before, before, game, kBytes));
    const auto native = Take(rules.GenerateNative(before, kBytes));
    auto expectedWpm = Take(original.ReadMember("war3map.wpm"));
    Require(expectedWpm.size() == native.size() + 16, "source WPM has complete original dimensions");
    for (std::size_t i = 0; i < native.size(); ++i)
        expectedWpm[16 + i] = (expectedWpm[16 + i] & std::byte{0x11}) | native[i];

    UnitPlacements baseline;
    UnitPlacement footman;
    footman.rawcode = "hfoo"; footman.position = {-1024, -256, 0}; footman.scale = {1, 1, 1};
    footman.owner = 0; footman.creationId = 7; footman.hitPoints = 73; footman.mana = 29;
    footman.unknownOwnerBytes = {17, 23}; baseline.units.push_back(footman);
    auto building = footman; building.rawcode = "ngol"; building.creationId = 19;
    building.position = {768, 640, 0}; building.hitPoints = -1; building.mana = -1;
    building.owner = 15; building.gold = 12500;
    baseline.units.push_back(building);
    auto item = building; item.rawcode = "ratf"; item.creationId = 23; item.owner = 15;
    item.position = {512, -768, 0}; baseline.units.push_back(item);
    const auto unitsBytes = Take(baseline.Serialize());

    TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.shape = TerrainBrushShape::Square;
    brush.radius = 0; brush.strength = 8;
    const TerrainStrokePoint center{before.header.offset[0] + 32 * 128, before.header.offset[1] + 32 * 128};
    const auto editedTerrain = Take(PrepareTerrainStroke(before, brush, std::span(&center, 1))).terrain;
    const auto expectedTerrain = Take(SerializeMapTerrain(editedTerrain));
    Require(expectedTerrain != terrainBytes, "real unit regression has an actual terrain edit");
    // 干地仅升raw地高：不改层级、水、边界、材质或对象DOO，故静态WPM全文应保原。
    // 单位、建筑和物品均不贡献原版静态WPM；不调用新WPM生成器计算expected。
    for (std::size_t i = 0; i < before.vertices.size(); ++i) {
        const auto& a = before.vertices[i]; const auto& b = editedTerrain.vertices[i];
        Require(a.CliffLevel() == b.CliffLevel() && !a.HasWater() && !b.HasWater()
            && a.IsBoundary() == b.IsBoundary() && a.IsMapEdge() == b.IsMapEdge()
            && a.GroundTextureIndex() == b.GroundTextureIndex() && a.IsBlight() == b.IsBlight()
            && !a.IsRamp() && !b.IsRamp(), "unit-only native contribution and dry raise preserve the complete pathing mask");
    }
    constexpr std::array<const char*, 4> names{"rotate", "copy", "create", "delete"};
    for (std::size_t operation = 0; operation < names.size(); ++operation) {
        const auto scenario = directory / (std::string("playable-terrain-units-") + names[operation]);
        Require(std::filesystem::create_directory(scenario), "exclusive units and terrain scenario");
        const std::array<MemberReplacement, 2> seeded{{{"war3mapUnits.doo", unitsBytes}, {"war3map.wpm", expectedWpm}}};
        const auto source = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "source.w3x", seeded)).path));
        const auto map = MapDocument::FromArchive(source);
        Require(map.Units() && map.Units()->units.size() == 3, "source contains all three classic placed kinds");
        MapEditPlacementSource selected{PlacementFile::Units, 1,
            Take(MapFieldRowKey(map, MapDataKind::Units, 1)), Take(Sha256(unitsBytes))};
        MapEditSaveRequest request;
        request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / "saved.w3x";
        request.toolRoot = tool; request.gameRoot = game;
        request.expectedSourceSha256 = source.SourceSha256(); request.expectedTerrainSha256 = Take(Sha256(terrainBytes));
        MapEditTerrainStroke stroke; stroke.brush = brush;
        stroke.pointSpace = MapEditPointSpace::TerrainVertex; stroke.points = {{32, 32}};
        request.terrainStrokes = {stroke};
        auto expectedUnits = baseline;
        if (operation == 0) {
            request.placementEdit = MapEditPlacementRotation{selected, 1.25f};
            expectedUnits.units[1].rotation = 1.25f;
        } else if (operation == 1) {
            request.placementEdit = MapEditPlacementCopy{{selected}, {256, 128, 0}};
            auto clone = building; clone.creationId = 24; clone.position = {1024, 768, 0};
            expectedUnits.units.push_back(clone);
        } else if (operation == 2) {
            auto added = building; added.position = {-256, 768, 0}; added.creationId = 997;
            request.placementEdit = MapEditPlacementCreate{selected.expectedMemberSha256, std::vector<UnitPlacement>{added}};
            added.creationId = 24; expectedUnits.units.push_back(added);
        } else {
            request.placementEdit = MapEditPlacementDelete{{selected}};
            expectedUnits.units.erase(expectedUnits.units.begin() + 1);
        }
        const auto expectedUnitBytes = Take(expectedUnits.Serialize());
        const std::array<MemberReplacement, 2> finalInputs{{
            {"war3map.w3e", expectedTerrain}, {"war3mapUnits.doo", expectedUnitBytes}}};
        const auto markerOwner = Take(TerrainMinimapMarkers::Capture(source, game, finalInputs));
        const auto originalMarkers = Take(TerrainMinimapMarkers::Capture(source, game));
        Require(originalMarkers->MarkerCount() == 1 && markerOwner->MarkerCount() == (operation == 0 ? 1 : operation == 3 ? 0 : 2),
            "the actual gold-mine type supplies a nonempty and independently counted marker fixture");
        Require((markerOwner->Bytes() == originalMarkers->Bytes()) == (operation == 0),
            "copy, create and delete change the final MMP; rotation alone preserves its marker");
        const auto saved = Take(SaveMapEditCandidate(request));
        const auto readback = Take(MapArchiveSession::Open(saved.candidate));
        Require(!saved.scriptPreserved && saved.appliedStrokeCount == 1 && saved.appliedPlacementCount == 1,
            "combined public save rebuilt and compiled the actual final placed-object script");
        Require(Take(readback.ReadMember("war3map.w3e")) == expectedTerrain
            && Take(readback.ReadMember("war3mapUnits.doo")) == expectedUnitBytes
            && Take(readback.ReadMember("war3map.wpm")) == expectedWpm,
            "combined public save reads back the complete expected terrain, Units and independently unchanged WPM");
        Require(Take(readback.ReadMember("war3map.mmp")) == markerOwner->Bytes()
            && markerOwner->VerifyReadback(readback).Succeeded(), "MMP uses the final complete Units source");
        const auto scriptBytes = Take(readback.ReadMember(Script::MapScriptMemberName(readback)));
        const std::string script(reinterpret_cast<const char*>(scriptBytes.data()), scriptBytes.size());
        for (const auto& unit : expectedUnits.units)
            Require(script.find(ScriptPlacement(unit)) != std::string::npos, "compiled script creates every final object at its final XY");
        if (operation == 3)
            Require(script.find(ScriptPlacement(building)) == std::string::npos, "compiled placed-object script omits the deleted unit");
        if (operation == 0)
            Require(script.find("'ngol', 768.0, 640.0, 71.620") != std::string::npos,
                "compiled script contains the final F32 radians-to-degrees rotation");
        for (const auto& member : source.Members()) {
            const auto name = Text::LowerAscii(member.name);
            if (name == "war3map.w3e" || name == "war3mapunits.doo" || name == "war3map.mmp"
                || name == "war3mapmap.blp" || name == "war3map.j" || name == "scripts/war3map.j") continue;
            Require(Take(readback.ReadMember(member.name)) == member.data, "combined save preserves every unrelated complete member");
        }
        const auto reject = [&](MapEditSaveRequest blocked, const char* suffix, const char* code) {
            blocked.targetCandidate = scenario / (std::string("refused-") + suffix + ".w3x");
            const auto result = SaveMapEditCandidate(blocked);
            Require(!result.Succeeded() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                [&](const auto& diagnostic) { return diagnostic.code == code; }), "combined save rejects the precise stale or mixed-domain cause");
            Require(!std::filesystem::exists(blocked.targetCandidate), "refused combination never writes a candidate");
            Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "refused combination preserves source SHA");
        };
        if (operation == 0) {
            auto bad = request; bad.expectedSourceSha256.assign(64, '0'); reject(bad, "source", "SAVE_SOURCE_STALE");
            bad = request; auto changed = selected; changed.expectedMemberSha256.assign(64, '0');
            bad.placementEdit = MapEditPlacementRotation{changed, 1.25f}; reject(bad, "member", "SAVE_PLACEMENT_MEMBER_STALE");
            bad = request; changed = selected; changed.expectedRowKey += "stale";
            bad.placementEdit = MapEditPlacementRotation{changed, 1.25f}; reject(bad, "row", "SAVE_PLACEMENT_ROW_STALE");
            bad = request; bad.terrainStrokes.front().followDoodads = true;
            reject(bad, "follow", "SAVE_TERRAIN_PLACEMENT_FOLLOW_CONFLICT");
            bad = request; changed = selected; changed.file = PlacementFile::Doodads;
            bad.placementEdit = MapEditPlacementRotation{changed, 1.25f}; reject(bad, "doo", "SAVE_OPERATION_SCOPE_INVALID");
        }
        Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "combined save leaves its original source untouched");
        std::cout << "terrain_units_save_passed operation=" << names[operation] << " source_sha256=" << source.SourceSha256()
            << " candidate_sha256=" << saved.candidateSha256 << " units_sha256=" << Take(Sha256(expectedUnitBytes))
            << " wpm_sha256=" << Take(Sha256(expectedWpm))
            << " real_script_compiled=true native_editor_verified=false game_verified=false\n";
    }
}
} // namespace War3Map::TerrainSaveTests
