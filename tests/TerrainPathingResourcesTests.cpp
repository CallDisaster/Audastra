#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Mpq/MpqArchive.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "operation failed";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
void Check(Status status) {
    if (!status.Succeeded()) {
        std::string message = "check failed";
        for (const auto& diagnostic : status.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
}
bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) { return diagnostic.code == code; });
}
Bytes Text(std::string_view value) { return Bytes(reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data() + value.size())); }

class Directory {
public:
    Directory() {
        static std::atomic<unsigned> sequence{0};
        path = std::filesystem::temp_directory_path() / ("Audastra-terrain-pathing-resources-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
        Require(std::filesystem::create_directory(path), "private directory");
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
    std::filesystem::path path;
};

Bytes GroundTable(std::string_view secondWalk = "1", std::string_view secondBuild = "1") {
    return Text("ID;PWXL;N;E\nB;X4;Y4;D0\n"
        "C;X1;Y1;K\"tileID\"\nC;X2;K\"walkable\"\nC;X3;K\"flyable\"\nC;X4;K\"buildable\"\n"
        "C;X1;Y2;K\"Tg01\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\n"
        "C;X1;Y3;K\"Tg02\"\nC;X2;K" + std::string(secondWalk) + "\nC;X3;K1\nC;X4;K" + std::string(secondBuild) + "\n"
        "C;X1;Y4;K\"Tg03\"\nC;X2;K0\nC;X3;K1\nC;X4;K0\nE\n");
}

Bytes CliffTable(std::string_view first = "Cl01", std::string_view second = "Cl02") {
    return Text("ID;PWXL;N;E\nB;X1;Y3;D0\nC;X1;Y1;K\"cliffID\"\n"
        "C;X1;Y2;K\"" + std::string(first) + "\"\nC;X1;Y3;K\"" + std::string(second) + "\"\nE\n");
}

struct Fixture {
    TerrainGridSnapshot grid;
    Bytes terrain, pathing;
    std::vector<MapArchiveMember> members;
    Fixture(bool wet = false, float surface = 64.0f) {
        grid.header.width = grid.header.height = 9;
        grid.header.mainTileset = 'L'; grid.header.groundTilesets = {"Tg01", "Tg02", "Tg03"};
        grid.header.cliffTilesets = {"Cl01"};
        grid.vertices.resize(81);
        for (auto& vertex : grid.vertices) {
            vertex.groundHeightRaw = 8192;
            vertex.waterHeightAndFlagsRaw = static_cast<std::uint16_t>(8192 + surface * 4);
            vertex.textureAndFlagsRaw = wet ? 0x40 : 0;
            vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(grid, 0, 0, 9, 9);
        terrain = Take(SerializeMapTerrain(grid));
        pathing = Text("MP3W");
        for (const auto value : {0u, 32u, 32u}) for (unsigned shift = 0; shift < 32; shift += 8)
            pathing.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        pathing.resize(16 + 32 * 32);
        for (std::size_t i = 16; i < pathing.size(); ++i) pathing[i] = static_cast<std::byte>((i * 73 + 0x91) & 0xff);
        members = {{"war3map.w3e", terrain}, {"war3map.wpm", pathing},
            {"war3map.shd", Text("author-shadow")}, {"war3map.mmp", Text("author-markers")},
            {"war3mapMap.tga", Text("author-minimap")}, {"TerrainArt/Terrain.slk", GroundTable()}};
        if (wet) {
            members.push_back({"TerrainArt/Water.slk", Text("ID;PWXL;N;E\nB;X2;Y2;D0\nC;X1;Y1;K\"waterID\"\nC;X2;K\"height\"\nC;X1;Y2;K\"LSha\"\nC;X2;K0\nE\n")});
            members.push_back({"UI/MiscData.txt", Text("[Water]\nDeepLevelPathing=52\n")});
            members.push_back({"UI/WorldEditData.txt", Text("[Other]\nValue=1\n")});
        }
    }
    MapArchiveSession Write(const std::filesystem::path& path) const {
        return Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(path, members)).path));
    }
    void Palette(std::vector<std::string> ids) {
        grid.header.groundTilesets = std::move(ids);
        terrain = Take(SerializeMapTerrain(grid)); members[0].data = terrain;
    }
    void Attach(MapDataEditSession& edits) const { Check(edits.AttachTerrain(terrain)); Check(edits.AttachPathing(pathing)); }
    void Paint(MapDataEditSession& edits, std::string texture = "Tg02") const {
        TerrainBrush brush; brush.kind = TerrainBrushKind::Texture; brush.radius = 0; brush.groundTexture = std::move(texture);
        const TerrainStrokePoint point{512, 512}; Take(edits.EditTerrain(brush, std::span(&point, 1), false));
    }
    void Water(MapDataEditSession& edits, float surface) const {
        TerrainBrush brush; brush.kind = TerrainBrushKind::WaterHeight; brush.radius = 0; brush.waterHeight = surface;
        const TerrainStrokePoint point{512, 512}; Take(edits.EditTerrain(brush, std::span(&point, 1), false));
    }
};

void EqualMaterialsSaveRealArchiveWithoutChangingPathing() {
    Directory directory; Fixture fixture;
    const auto source = fixture.Write(directory.path / "source.w3x");
    MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits); fixture.Paint(edits);
    Require(!edits.VerifyTerrainPathingForSave().Succeeded(), "resource-free gate cannot invent material attributes");
    const auto plan = Take(TerrainSavePlan::PrepareForSave(source, edits, {}));
    Check(plan.SaveGate()); Check(plan.VerifyForSave(edits, source.SourceSha256(), edits.Replacements()));
    Require(plan.Replacements().size() == 1 && plan.Members()[1].action == TerrainSaveMemberAction::Preserve, "only W3E changes");
    const auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(directory.path / "saved.w3x", plan.Replacements())).path));
    Check(plan.VerifyReadback(saved));
    std::size_t memberBytes = 0;
    for (const auto& member : plan.Members()) memberBytes += member.outputBytes.value_or(0);
    Require(!plan.VerifyReadback(saved, {memberBytes}).Succeeded(), "readback cannot hide input reads outside the caller budget");
    Require(Take(saved.ReadMember("war3map.wpm")) == fixture.pathing, "every unknown, static and author pathing bit survives");
    Require(Take(saved.ReadMember("war3map.shd")) == Text("author-shadow") && Take(saved.ReadMember("war3map.mmp")) == Text("author-markers"), "shadow and marker contents survive");
    Require(Take(source.ReadMember("war3map.w3e")) == fixture.terrain, "original terrain remains unchanged");
    Require(edits.Undo(), "undo texture"); Check(edits.VerifyTerrainPathingForSave()); Require(edits.Redo(), "redo texture");
    Require(!plan.VerifyForSave(edits, source.SourceSha256(), edits.Replacements()).Succeeded(), "undo/redo invalidates old revision");
}

void PaletteReplacementAppendAndVariation() {
    Directory directory; Fixture fixture;
    const auto source = fixture.Write(directory.path / "palette.w3x");
    for (const bool append : {false, true}) {
        auto members = fixture.members;
        auto original = fixture.grid; original.header.groundTilesets = {"Tg01"};
        const auto terrain = Take(SerializeMapTerrain(original)); members[0].data = terrain;
        const auto map = Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(directory.path / (append ? "append.w3x" : "replace.w3x"), members)).path));
        MapDataEditSession edits(MapDocument::FromMembers({})); Check(edits.AttachTerrain(terrain)); Check(edits.AttachPathing(fixture.pathing));
        if (append) fixture.Paint(edits); else Take(edits.EditTerrainTextureSlot(TerrainTextureTable::Ground, 0, "Tg02"));
        const auto plan = Take(TerrainSavePlan::PrepareForSave(map, edits, {})); Check(plan.SaveGate());
        Check(plan.VerifyForSave(edits, map.SourceSha256(), edits.Replacements()));
    }
    MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits);
    TerrainBrush brush; brush.groundTexture = "Tg01"; brush.radius = 0; brush.variation = std::uint8_t{17};
    const TerrainStrokePoint point{512, 512}; Take(edits.EditTerrain(brush, std::span(&point, 1), false));
    Check(edits.VerifyTerrainPathingForSave());
    const auto plan = Take(TerrainSavePlan::Prepare(source, edits)); Check(plan.SaveGate());
    auto output = edits.Replacements(); output.push_back({"war3map.w3b", Text("changed-definition")});
    Require(!plan.VerifyFrozenOutput(source.SourceSha256(), output).Succeeded(), "variation cannot smuggle changed static definitions");
}

void UnusedPaletteEntriesRequireValidMaterials() {
    Directory directory;
    for (const bool append : {false, true}) for (const bool invalidProperty : {false, true}) {
        Fixture fixture;
        fixture.Palette(append ? std::vector<std::string>{"Tg01"} : std::vector<std::string>{"Tg01", "Tg03"});
        if (invalidProperty) fixture.members.back().data = GroundTable("2");
        const auto name = std::string(append ? "append-" : "replace-") + (invalidProperty ? "invalid" : "missing");
        const auto source = fixture.Write(directory.path / (name + ".w3x"));
        MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits);
        Take(edits.EditTerrainTextureSlot(TerrainTextureTable::Ground, 1, invalidProperty ? "Tg02" : "ZZZZ"));
        const auto plan = TerrainSavePlan::PrepareForSave(source, edits, {});
        Require(!plan.Succeeded() && HasCode(plan.diagnostics, invalidProperty
            ? "TERRAIN_PATHING_PROPERTY_INVALID" : "TERRAIN_PATHING_PROPERTY_MISSING"),
            "unused new slots require real, valid material properties");
        Require(Take(source.ReadMember("war3map.w3e")) == fixture.terrain, "rejected palette leaves source unchanged");
    }
}

void UnusedKnownPaletteEntriesSaveWithoutChangingPathing() {
    Directory directory;
    const std::vector<std::vector<std::string>> palettes{{"Tg01"}, {"Tg01", "Tg02"}, {"Tg01", "ZZZZ"}};
    unsigned index = 0;
    for (const auto& palette : palettes) {
        Fixture fixture; fixture.Palette(palette);
        const auto name = std::to_string(index++);
        const auto source = fixture.Write(directory.path / (name + ".w3x"));
        MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits);
        Take(edits.EditTerrainTextureSlot(TerrainTextureTable::Ground, 1, "Tg03"));
        const auto plan = Take(TerrainSavePlan::PrepareForSave(source, edits, {})); Check(plan.SaveGate());
        Check(plan.VerifyForSave(edits, source.SourceSha256(), edits.Replacements()));
        const auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
            directory.path / (name + "-saved.w3x"), plan.Replacements())).path));
        Check(plan.VerifyReadback(saved));
        Require(Take(saved.ReadMember("war3map.w3e")) == Take(SerializeMapTerrain(*edits.Terrain())),
            "unused palette change reaches the saved terrain");
        Require(Take(saved.ReadMember("war3map.wpm")) == fixture.pathing,
            "different properties in an unused slot preserve every original pathing byte");
        Require(Take(source.ReadMember("war3map.w3e")) == fixture.terrain, "palette save leaves source unchanged");
    }
}

void UnusedPaletteDependenciesRemainBound() {
    Directory directory;
    for (const bool fromGame : {false, true}) {
        Fixture fixture; fixture.Palette({"Tg01", "Tg03"});
        const auto gameRoot = directory.path / "game";
        if (fromGame) {
            fixture.members.pop_back();
            Require(std::filesystem::create_directory(gameRoot), "private palette game directory");
            Take(WriteVerifiedArchive(gameRoot / "war3.mpq", std::vector<MapArchiveMember>{{"TerrainArt/Terrain.slk", GroundTable()}}));
        }
        const auto name = std::string(fromGame ? "game" : "map");
        const auto source = fixture.Write(directory.path / (name + ".w3x"));
        MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits);
        Take(edits.EditTerrainTextureSlot(TerrainTextureTable::Ground, 1, "Tg02"));
        const auto plan = Take(TerrainSavePlan::PrepareForSave(source, edits, fromGame ? gameRoot : std::filesystem::path{}));
        Check(plan.SaveGate()); Check(plan.VerifyForSave(edits, source.SourceSha256(), edits.Replacements()));
        const auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
            directory.path / (name + "-saved.w3x"), plan.Replacements())).path));
        Check(plan.VerifyReadback(saved));
        if (fromGame) {
            Take(WriteVerifiedArchive(gameRoot / "war3patch.mpq", std::vector<MapArchiveMember>{{"TerrainArt/Terrain.slk", GroundTable()}}));
            const auto frozen = plan.VerifyForSave(edits, source.SourceSha256(), edits.Replacements());
            const auto readback = plan.VerifyReadback(saved);
            Require(!frozen.Succeeded() && HasCode(frozen.diagnostics, "TERRAIN_PATHING_RESOURCE_CHANGED")
                && !readback.Succeeded() && HasCode(readback.diagnostics, "TERRAIN_PATHING_RESOURCE_CHANGED"),
                "same-byte winner change invalidates unused palette capture before save and at readback");
        } else {
            std::vector<MemberReplacement> altered(plan.Replacements().begin(), plan.Replacements().end());
            altered.push_back({"TerrainArt\\Terrain.slk", GroundTable("0")});
            const auto replaced = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
                directory.path / "table-replaced.w3x", altered)).path));
            const auto replacementCheck = plan.VerifyReadback(replaced);
            Require(!replacementCheck.Succeeded() && HasCode(replacementCheck.diagnostics, "TERRAIN_PATHING_RESOURCE_CHANGED"),
                "independent readback detects changed unused palette material table");
            const std::vector<std::string> removed{"TerrainArt/Terrain.slk"};
            const auto missing = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
                directory.path / "table-removed.w3x", plan.Replacements(), removed)).path));
            const auto removalCheck = plan.VerifyReadback(missing);
            Require(!removalCheck.Succeeded() && HasCode(removalCheck.diagnostics, "TERRAIN_PATHING_RESOURCE_CHANGED"),
                "independent readback detects removed unused palette material table");
        }
    }
}

void UnresolvedGroundIndicesCannotChangeThroughPaletteEdits() {
    Directory directory; Fixture fixture;
    fixture.grid.vertices[40].textureAndFlagsRaw = 14;
    fixture.Palette({"Tg01"});
    const auto source = fixture.Write(directory.path / "unresolved.w3x");
    for (const bool changeIndex : {false, true}) {
        MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits);
        Take(edits.EditTerrainTextureSlot(TerrainTextureTable::Ground, 1, "Tg02"));
        if (changeIndex) {
            auto current = *edits.Terrain(); current.vertices[40].textureAndFlagsRaw = 15;
            Check(edits.SetTerrain(Take(SerializeMapTerrain(current))));
        }
        const auto plan = Take(TerrainSavePlan::PrepareForSave(source, edits, {}));
        Require(plan.SaveGate().Succeeded() != changeIndex,
            "an unchanged historical unresolved index survives; changing one unresolved index to another must refuse");
        if (!changeIndex) {
            Check(plan.VerifyForSave(edits, source.SourceSha256(), edits.Replacements()));
            const auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
                directory.path / "unresolved-preserved.w3x", plan.Replacements())).path));
            Check(plan.VerifyReadback(saved));
            Require(Take(saved.ReadMember("war3map.wpm")) == fixture.pathing, "unknown historical index does not alter pathing");
        }
    }
}

void DifferentOrUnavailablePropertiesRefuse() {
    Directory directory;
    for (const auto value : {"0", "2", "", "not-a-boolean"}) {
        Fixture fixture; fixture.members.back().data = GroundTable(value);
        const auto source = fixture.Write(directory.path / (std::to_string(std::string_view(value).size()) + "-" + (value[0] ? std::string(value) : "empty") + ".w3x"));
        MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits); fixture.Paint(edits);
        const auto plan = TerrainSavePlan::PrepareForSave(source, edits, {});
        Require(!plan.Succeeded() || !plan.value->SaveGate().Succeeded(), "different or unknown material properties must refuse");
    }
    Fixture fixture;
    const auto source = fixture.Write(directory.path / "different.w3x");
    MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits); fixture.Paint(edits, "Tg03");
    Require(!Take(TerrainSavePlan::PrepareForSave(source, edits, {})).SaveGate().Succeeded(), "buildability and walkability change requires real WPM rebuild");
    fixture.members.pop_back();
    const auto missing = fixture.Write(directory.path / "missing.w3x");
    Require(!TerrainSavePlan::PrepareForSave(missing, edits, {}).Succeeded(), "no table and no game does not synthesize attributes");
}

void WaterThresholdsUseDepthAndStrictCoverage() {
    Directory directory;
    struct Case { float before, after; bool allowed; };
    const Case cases[]{{64, 80, true}, {64, 52, true}, {64, 51.75f, false},
        {10, 20, true}, {10, 0, false}, {0, 0.25f, false}, {0, -0.25f, false}};
    unsigned count = 0;
    for (const auto item : cases) {
        Fixture fixture(true, item.before); const auto source = fixture.Write(directory.path / (std::to_string(count++) + ".w3x"));
        MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits); fixture.Water(edits, item.after);
        const auto plan = Take(TerrainSavePlan::PrepareForSave(source, edits, {}));
        Require(plan.SaveGate().Succeeded() == item.allowed, "water classification and strict coverage boundary");
        if (item.allowed) {
            Check(plan.VerifyForSave(edits, source.SourceSha256(), edits.Replacements()));
            const auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(directory.path / ("saved-" + std::to_string(count) + ".w3x"), plan.Replacements())).path));
            Check(plan.VerifyReadback(saved));
            Require(Take(saved.ReadMember("war3map.wpm")) == fixture.pathing, "equivalent water keeps all pathing bytes");
        }
    }
    Fixture fixture(true); fixture.members.back().data = Text("[Water]\nDeepLevelPathing=100\n");
    const auto overridden = fixture.Write(directory.path / "override.w3x");
    MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits); fixture.Water(edits, 110);
    const auto ambiguous = TerrainSavePlan::PrepareForSave(overridden, edits, {});
    Require(!ambiguous.Succeeded() && HasCode(ambiguous.diagnostics, "TERRAIN_PATHING_PROPERTY_AMBIGUOUS"),
        "conflicting profile definitions cannot silently use the last value");
    fixture.members[7].data = Text("[Other]\nValue=1\n");
    fixture.members.back().data = Text("[Water]\nDeepLevelPathing=nan\n");
    const auto invalid = fixture.Write(directory.path / "invalid.w3x");
    Require(!TerrainSavePlan::PrepareForSave(invalid, edits, {}).Succeeded(), "invalid water threshold is not defaulted");

    Fixture offset(true);
    offset.members[6].data = Text("ID;PWXL;N;E\nB;X2;Y2;D0\nC;X1;Y1;K\"waterID\"\nC;X2;K\"height\"\nC;X1;Y2;K\"LSha\"\nC;X2;K-0.5\nE\n");
    const auto offsetSource = offset.Write(directory.path / "offset.w3x");
    MapDataEditSession offsetEdits(MapDocument::FromMembers({})); offset.Attach(offsetEdits); offset.Water(offsetEdits, 80);
    Require(!Take(TerrainSavePlan::PrepareForSave(offsetSource, offsetEdits, {})).SaveGate().Succeeded(),
        "actual Water.slk height offset changes strict-coverage classification");
    for (const bool crossing : {false, true}) {
        Fixture height(true); const auto heightSource = height.Write(directory.path / (crossing ? "height-crossing.w3x" : "height-equivalent.w3x"));
        MapDataEditSession heightEdits(MapDocument::FromMembers({})); height.Attach(heightEdits);
        TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = crossing ? 16.0f : 4.0f;
        const TerrainStrokePoint point{512, 512}; Take(heightEdits.EditTerrain(brush, std::span(&point, 1), false));
        Require(Take(TerrainSavePlan::PrepareForSave(heightSource, heightEdits, {})).SaveGate().Succeeded() != crossing,
            "wet ground height uses the same native depth boundary");
    }
}

void WaterProfileDuplicatesCannotHideDifferentNativeDepths() {
    Directory directory;
    struct Case { std::string_view misc, editor, error; };
    const Case cases[]{
        {"[Water]\nDeepLevelPathing=64\nDeepLevelPathing=128\n", "", "TERRAIN_PATHING_PROPERTY_AMBIGUOUS"},
        {"[Water]\nDeepLevelPathing=64\n[Other]\nValue=1\n[ water ]\n deepLEVELpathing = 128\n", "", "TERRAIN_PATHING_PROPERTY_AMBIGUOUS"},
        {"[Water]\nDeepLevelPathing=64\n", "[Water]\nDeepLevelPathing=128\n", "TERRAIN_PATHING_PROPERTY_AMBIGUOUS"},
        {"[Water]\nDeepLevelPathing=64,128\nDeepLevelPathing=128\n", "", "TERRAIN_PATHING_PROPERTY_INVALID"},
        {"[Water]\nDeepLevelPathing=64,128\n", "[Water]\nDeepLevelPathing=128\n", "TERRAIN_PATHING_PROPERTY_INVALID"},
        {"[Water]\nDeepLevelPathing=nan\nDeepLevelPathing=128\n", "", "TERRAIN_PATHING_PROPERTY_INVALID"},
        {"[Water]\nDeepLevelPathing=-1\nDeepLevelPathing=128\n", "", "TERRAIN_PATHING_PROPERTY_INVALID"},
        {"[Water]\nDeepLevelPathing=128\n", "[Water]\nDeepLevelPathing=128.0\n", ""},
        {"\xef\xbb\xbf[ WATER ]\r\n DeepLevelPathing = 128 \r\n;DeepLevelPathing=64\n//DeepLevelPathing=64\n", "[Other]\nDeepLevelPathing=64\n", ""},
        {"[Other]\nDeepLevelPathing=64\n", "[Water]\nDeepLevelPathing=128\n", ""},
    };
    unsigned count = 0;
    for (const auto& item : cases) {
        Fixture fixture(true, 48);
        fixture.members[7].data = Text(item.misc); fixture.members[8].data = Text(item.editor);
        const auto name = std::to_string(count++);
        const auto source = fixture.Write(directory.path / (name + ".w3x"));
        MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits); fixture.Water(edits, 80);
        const auto plan = TerrainSavePlan::PrepareForSave(source, edits, {});
        if (!item.error.empty()) {
            Require(!plan.Succeeded() && HasCode(plan.diagnostics, item.error),
                "all raw definitions are checked before a later scalar can hide an unsafe water threshold");
        } else {
            Require(plan.Succeeded(), "equal scalar duplicates and unrelated keys remain supported");
            Check(plan.value->SaveGate());
            const auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
                directory.path / (name + "-saved.w3x"), plan.value->Replacements())).path));
            Check(plan.value->VerifyReadback(saved));
            Require(Take(saved.ReadMember("war3map.wpm")) == fixture.pathing, "valid scalar-only water saves keep original pathing");
        }
        MapDataEditSession textureOnly(MapDocument::FromMembers({})); fixture.Attach(textureOnly); fixture.Paint(textureOnly);
        Check(Take(TerrainSavePlan::PrepareForSave(source, textureOnly, {})).SaveGate());
        Require(Take(source.ReadMember("war3map.w3e")) == fixture.terrain
            && Take(source.ReadMember("war3map.wpm")) == fixture.pathing, "profile checks never change the source map");
    }
    for (const bool miscFromGame : {false, true}) {
        Fixture fixture(true, 48);
        fixture.members[7].data = Text("[Water]\nDeepLevelPathing=64\n");
        fixture.members[8].data = Text("[Water]\nDeepLevelPathing=128\n");
        const auto member = fixture.members[miscFromGame ? 7 : 8];
        fixture.members.erase(fixture.members.begin() + (miscFromGame ? 7 : 8));
        const auto name = miscFromGame ? "misc-from-game" : "editor-from-game";
        const auto gameRoot = directory.path / name;
        Require(std::filesystem::create_directory(gameRoot), "private profile fixture directory");
        Take(WriteVerifiedArchive(gameRoot / "war3.mpq", std::vector<MapArchiveMember>{member}));
        const auto source = fixture.Write(directory.path / (std::string(name) + ".w3x"));
        MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits); fixture.Water(edits, 80);
        const auto failed = TerrainSavePlan::PrepareForSave(source, edits, gameRoot);
        Require(!failed.Succeeded() && HasCode(failed.diagnostics, "TERRAIN_PATHING_PROPERTY_AMBIGUOUS"),
            "conflicting actual map and game definitions cannot produce false water equivalence");
    }
    Fixture firstValue(true, 48); firstValue.members[7].data = Text("[Water]\nDeepLevelPathing=64\n");
    const auto source = firstValue.Write(directory.path / "first-value.w3x");
    MapDataEditSession edits(MapDocument::FromMembers({})); firstValue.Attach(edits); firstValue.Water(edits, 80);
    Require(!Take(TerrainSavePlan::PrepareForSave(source, edits, {})).SaveGate().Succeeded(),
        "the native first scalar classifies 48 to 80 as a shallow-to-deep transition");
}

void RestrictionAndTerrainChangesUseIndependentSaveChecks() {
    Directory directory;
    enum class Edit { Height, EqualTexture, Water, CrossingWater, DifferentTexture };
    unsigned count = 0;
    for (const auto edit : {Edit::Height, Edit::EqualTexture, Edit::Water, Edit::CrossingWater, Edit::DifferentTexture})
        for (const bool restrictionFirst : {false, true}) {
            const bool wet = edit == Edit::Water || edit == Edit::CrossingWater;
            const bool allowed = edit != Edit::CrossingWater && edit != Edit::DifferentTexture;
            Fixture fixture(wet);
            const auto name = std::to_string(count++);
            const auto source = fixture.Write(directory.path / (name + ".w3x"));
            MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits);
            const TerrainStrokePoint point{512, 512};
            const auto paintRestriction = [&] {
                TerrainBrush brush; brush.kind = TerrainBrushKind::PathingRestriction;
                brush.size = 1; brush.pathingRestrictions = 0x0e; brush.value = true;
                Take(edits.EditTerrain(brush, std::span(&point, 1), false));
            };
            if (restrictionFirst) paintRestriction();
            if (edit == Edit::Height) {
                TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 4;
                Take(edits.EditTerrain(brush, std::span(&point, 1), false));
            } else if (edit == Edit::EqualTexture || edit == Edit::DifferentTexture) {
                fixture.Paint(edits, edit == Edit::EqualTexture ? "Tg02" : "Tg03");
            } else {
                fixture.Water(edits, edit == Edit::Water ? 80.0f : 51.75f);
            }
            if (!restrictionFirst) paintRestriction();
            const auto plan = Take(TerrainSavePlan::PrepareForSave(source, edits, {}));
            Require(plan.SaveGate().Succeeded() == allowed,
                "declared restrictions compose with proven terrain changes but cannot authorize changed terrain pathing");
            Require(Take(source.ReadMember("war3map.w3e")) == fixture.terrain
                && Take(source.ReadMember("war3map.wpm")) == fixture.pathing, "combined edits keep original source bytes");
            if (!allowed) {
                Require(!plan.VerifyForSave(edits, source.SourceSha256(), edits.Replacements()).Succeeded(),
                    "restriction declaration does not waive changed water depth or material properties");
                continue;
            }
            Check(plan.VerifyForSave(edits, source.SourceSha256(), edits.Replacements()));
            Require(plan.Replacements().size() == 2, "one normal-save plan includes W3E and WPM");
            const auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
                directory.path / (name + "-saved.w3x"), plan.Replacements())).path));
            Check(plan.VerifyReadback(saved));
            auto expected = fixture.pathing;
            expected[16 + 16 * 32 + 16] |= std::byte{0x0e};
            Require(Take(saved.ReadMember("war3map.wpm")) == expected,
                "combined save changes only declared restrictions and keeps all protected and unknown bits");
            Require(Take(saved.ReadMember("war3map.w3e")) == Take(SerializeMapTerrain(*edits.Terrain()))
                && Take(saved.ReadMember("war3map.w3e")) != fixture.terrain, "combined save writes the actual edited terrain");
            Require(Take(saved.ReadMember("war3map.shd")) == Text("author-shadow")
                && Take(saved.ReadMember("war3map.mmp")) == Text("author-markers"), "combined save preserves derived members");
            auto changedOutput = edits.Replacements();
            changedOutput.push_back({"TerrainArt/Terrain.slk", GroundTable("0")});
            Require(!plan.VerifyFrozenOutput(source.SourceSha256(), changedOutput).Succeeded(),
                "combined save still rejects a changed final material source");
        }
}

void FinalResourcesAndGameWinnersAreRechecked() {
    Directory directory; Fixture fixture;
    const auto source = fixture.Write(directory.path / "source.w3x");
    MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits); fixture.Paint(edits);
    const auto plan = Take(TerrainSavePlan::PrepareForSave(source, edits, directory.path / "unread-lower-game")); Check(plan.SaveGate());
    auto output = edits.Replacements(); output.push_back({"TerrainArt\\Terrain.slk", GroundTable("0")});
    Require(!plan.VerifyFrozenOutput(source.SourceSha256(), output).Succeeded(), "changed final table cannot use old equivalence");
    const auto altered = Take(source.SaveCandidate(directory.path / "altered.w3x", output));
    Require(!plan.VerifyReadback(Take(MapArchiveSession::Open(altered.path))).Succeeded(), "independent readback checks table identity");
    fixture.members.pop_back(); const auto gameSource = fixture.Write(directory.path / "game-source.w3x");
    const auto gameRoot = directory.path / "game"; Require(std::filesystem::create_directory(gameRoot), "game directory");
    Take(WriteVerifiedArchive(gameRoot / "war3.mpq", std::vector<MapArchiveMember>{{"TerrainArt/Terrain.slk", GroundTable()}}));
    const auto gamePlan = Take(TerrainSavePlan::PrepareForSave(gameSource, edits, gameRoot)); Check(gamePlan.SaveGate());
    Check(gamePlan.VerifyFrozenOutput(gameSource.SourceSha256(), edits.Replacements()));
    Take(WriteVerifiedArchive(gameRoot / "war3patch.mpq", std::vector<MapArchiveMember>{{"TerrainArt/Terrain.slk", GroundTable()}}));
    Require(!gamePlan.VerifyFrozenOutput(gameSource.SourceSha256(), edits.Replacements()).Succeeded(), "same bytes in a changed winning game archive invalidate the input");
}

void StoredAndDecodedBudgetsShareTheCallerAllowance() {
    const std::vector<std::string> names{"TerrainArt/Terrain.slk", "TerrainArt/Water.slk",
        "UI/MiscData.txt", "UI/WorldEditData.txt"};
    for (const int mapCount : {4, 0, 2}) {
        Directory directory; Fixture fixture(true);
        const auto resources = std::vector<MapArchiveMember>(fixture.members.begin() + 5, fixture.members.end());
        fixture.members.resize(5);
        // 混合例中前两项由游戏提供，后两项在地图中，覆盖整批地图先读入的计费顺序。
        fixture.members.insert(fixture.members.end(), resources.end() - mapCount, resources.end());
        const auto source = fixture.Write(directory.path / "source.w3x");
        const auto gameRoot = directory.path / "game";
        Require(std::filesystem::create_directory(gameRoot), "private game directory");
        const auto gamePath = Take(WriteVerifiedArchive(gameRoot / "war3.mpq", resources)).path;
        const auto map = Take(Mpq::MpqArchive::OpenReadOnly(source.SourcePath()));
        const auto game = Take(Mpq::MpqArchive::OpenReadOnly(gamePath));
        const auto tiny = game.GetFileInfo(names.back());
        Require(tiny && tiny->compressedSize > tiny->fileSize, "fixture must include sector-table overhead");
        std::size_t decoded = 0, minimumBudget = 0;
        const auto account = [&](const Mpq::MpqFileInfo& file) {
            minimumBudget = std::max(minimumBudget, decoded + std::max(file.fileSize, file.compressedSize));
            decoded += file.fileSize;
        };
        for (const auto& name : names) if (const auto file = map.GetFileInfo(name)) account(*file);
        for (const auto& name : names) if (!map.HasFile(name)) account(*game.GetFileInfo(name));
        MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits);
        fixture.Paint(edits); fixture.Water(edits, 80);
        const auto captured = Take(TerrainPathingResources::Capture(source, fixture.grid, *edits.Terrain(), gameRoot, minimumBudget));
        Require(captured.SourceBytes() == decoded, "decoded accounting must not include stored overhead or double-count map data");
        Check(captured.VerifyFrozenOutput({}, {}));
        Check(captured.VerifyReadback(source, minimumBudget));
        Require(!TerrainPathingResources::Capture(source, fixture.grid, *edits.Terrain(), gameRoot, minimumBudget - 1).Succeeded(),
            "one byte below the actual stored/decoded envelope must refuse");
        Require(!captured.VerifyReadback(source, minimumBudget - 1).Succeeded(),
            "readback must use the caller remaining budget across map and game");
        Require(!captured.VerifyReadback(source, decoded - 1).Succeeded(), "cumulative decoded budget remains bounded");
        if (mapCount == 0) {
            Require(!captured.VerifyFrozenOutput(std::vector<MemberReplacement>{resources.front()}, {}).Succeeded(),
                "new map override cannot reuse game-source equivalence even with identical bytes");
            Take(WriteVerifiedArchive(gameRoot / "war3patch.mpq", resources));
            Require(!captured.VerifyReadback(source, minimumBudget).Succeeded(), "readback must detect changed game winners");
        } else if (mapCount == 4) {
            const std::vector<MemberReplacement> duplicate{resources.front(), resources.front()};
            Require(!captured.VerifyFrozenOutput(duplicate, {}).Succeeded(), "duplicate resource replacement must refuse");
            Require(!captured.VerifyFrozenOutput({}, std::vector<std::string>{names.front()}).Succeeded(),
                "removing a winning map dependency must refuse");
        }
    }

    Directory directory; Fixture fixture(true);
    auto invalidGround = GroundTable(); std::fill(invalidGround.begin(), invalidGround.end(), std::byte{'x'});
    fixture.members.erase(fixture.members.begin() + 5);
    const auto source = fixture.Write(directory.path / "mixed.w3x");
    const auto gameRoot = directory.path / "game";
    Require(std::filesystem::create_directory(gameRoot), "private mixed game directory");
    Take(WriteVerifiedArchive(gameRoot / "war3.mpq", std::vector<MapArchiveMember>{{names.front(), invalidGround}}));
    MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits);
    fixture.Paint(edits); fixture.Water(edits, 80);
    std::size_t mapBytes = 0;
    for (std::size_t i = 5; i < fixture.members.size(); ++i) mapBytes += fixture.members[i].data.size();
    const auto failed = TerrainPathingResources::Capture(source, fixture.grid, *edits.Terrain(), gameRoot,
        mapBytes + invalidGround.size() - 1);
    Require(!failed.Succeeded() && std::any_of(failed.diagnostics.begin(), failed.diagnostics.end(), [](const auto& d) {
        return d.code == "FILE_READ_BUDGET_EXCEEDED";
    }), "all already captured map bytes must be charged before reading or parsing the first game fallback");
}

MapSettings NativeSettings() {
    MapSettings settings;
    settings.formatVersion = 25; settings.custom.emplace(); settings.flags = 0x400;
    settings.tileset = 'L'; settings.playableWidth = settings.playableHeight = 8;
    return settings;
}
void NativeSources(Fixture& fixture, const MapSettings& settings, const DoodadPlacements& doodads = {}) {
    fixture.members.push_back({"war3map.w3i", Take(settings.Serialize())});
    fixture.members.push_back({"war3map.doo", Take(doodads.Serialize())});
    if (fixture.grid.vertices.front().HasWater()) for (auto& member : fixture.members)
        if (member.name == "TerrainArt/Water.slk") member.data = Text(
            "ID;PWXL;N;E\nB;X3;Y2;D0\nC;X1;Y1;K\"waterID\"\nC;X2;K\"height\"\nC;X3;K\"impassable\"\n"
            "C;X1;Y2;K\"LSha\"\nC;X2;K0\nC;X3;K0\nE\n");
    const auto camera = Text("[CameraMargins]\nleft=0\nright=0\nbottom=0\ntop=0\n");
    const auto misc = std::find_if(fixture.members.begin(), fixture.members.end(), [](const auto& member) { return member.name == "UI/MiscData.txt"; });
    if (misc != fixture.members.end()) misc->data.insert(misc->data.end(), camera.begin(), camera.end());
    else fixture.members.push_back({"UI/MiscData.txt", camera});
    if (std::none_of(fixture.members.begin(), fixture.members.end(), [](const auto& member) { return member.name == "UI/WorldEditData.txt"; }))
        fixture.members.push_back({"UI/WorldEditData.txt", Text("[Other]\nValue=1\n")});
}

void NativeCaptureBindsFullRulesAndBothTerrainRevisions() {
    Directory directory; Fixture fixture;
    NativeSources(fixture, NativeSettings());
    // 非空Units不写保存selector2；资源捕获不能以Units是否为空代替原版层规则。
    UnitPlacements units; UnitPlacement unit; unit.rawcode = "hfoo"; unit.scale = {1, 1, 1};
    units.units.push_back(unit); fixture.members.push_back({"war3mapUnits.doo", Take(units.Serialize())});
    const auto source = fixture.Write(directory.path / "native.w3x");
    auto after = fixture.grid; after.vertices[40].variationRaw = 1;
    const auto captured = Take(TerrainPathingResources::CaptureNative(source, fixture.grid, after, {}, 1024 * 1024));
    Require(!captured.VerifyWaterBrushInput(fixture.grid, after).Succeeded(),
        "native save resources do not grant editing rule input identity");
    const auto beforeGrid = Take(captured.GenerateNative(fixture.grid, 1024 * 1024));
    Require(beforeGrid.size() == 1024 && std::all_of(beforeGrid.begin(), beforeGrid.end(), [](auto v) { return v == std::byte{0x40}; }),
        "unchanged full dry ground uses actual source properties, not random source WPM");
    Require(Take(captured.GenerateNative(after, 1024 * 1024)) == beforeGrid, "both captured W3E revisions are permitted");
    auto unrelated = after; unrelated.vertices[41].variationRaw = 2;
    Require(!captured.GenerateNative(unrelated, 1024 * 1024).Succeeded(), "arbitrary third terrain revision must refuse");
    Require(!captured.GenerateNative(fixture.grid, 100).Succeeded(), "native generation cannot exceed caller budget");
    const std::vector<MemberReplacement> replacements{{"war3map.w3e", Take(SerializeMapTerrain(after))}};
    Check(captured.VerifyFrozenOutput(replacements, {}));
    Require(!captured.VerifyFrozenOutput({}, {}).Succeeded(), "changed captured W3E cannot be omitted from output");
    Require(!captured.VerifyFrozenOutput(replacements, std::vector<std::string>{"war3map.w3i"}).Succeeded(), "native W3I source cannot disappear");
    const auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(directory.path / "native-saved.w3x", replacements)).path));
    Check(captured.VerifyReadback(saved, 1024 * 1024));
    Require(!captured.VerifyReadback(saved, captured.SourceBytes() - 1).Succeeded(), "native metadata consumes readback budget");
    const auto preserveOnly = Take(TerrainPathingResources::Capture(source, fixture.grid, fixture.grid, {}, 1024 * 1024));
    Require(!preserveOnly.GenerateNative(fixture.grid, 1024 * 1024).Succeeded(), "equivalence capture does not grant full generation");
    auto wrongSource = fixture.grid; wrongSource.vertices[0].variationRaw = 1;
    const auto mismatch = TerrainPathingResources::CaptureNative(source, wrongSource, after, {}, 1024 * 1024);
    Require(!mismatch.Succeeded() && HasCode(mismatch.diagnostics, "TERRAIN_NATIVE_SOURCE_IDENTITY"), "before must be actual source W3E");
}

void NativeWaterAndFractionalWorldBoundsUseActualSources() {
    Directory directory; Fixture wet(true);
    NativeSources(wet, NativeSettings());
    const auto wetSource = wet.Write(directory.path / "wet.w3x");
    const auto water = Take(TerrainPathingResources::CaptureNative(wetSource, wet.grid, wet.grid, {}, 1024 * 1024));
    Require(Take(water.GenerateNative(wet.grid, 1024 * 1024)).front() == std::byte{0x0a}, "actual deep water blocks walk/build only");
    for (auto& member : wet.members) if (member.name == "TerrainArt/Water.slk") {
        auto text = std::string(reinterpret_cast<const char*>(member.data.data()), member.data.size());
        const auto at = text.find("C;X3;K0"); text.replace(at, 7, "C;X3;K1"); member.data = Text(text);
    }
    const auto impassableSource = wet.Write(directory.path / "impassable.w3x");
    const auto impassable = Take(TerrainPathingResources::CaptureNative(impassableSource, wet.grid, wet.grid, {}, 1024 * 1024));
    Require(Take(impassable.GenerateNative(wet.grid, 1024 * 1024)).front() == std::byte{0xca}, "impassable must be read from Water.slk");

    Fixture dry; auto settings = NativeSettings(); settings.flags = 0;
    settings.cameraBounds = {128.25f, 0, 1024, 1024, 0, 0, 0, 0};
    NativeSources(dry, settings);
    const auto source = dry.Write(directory.path / "fractional.w3x");
    const auto captured = Take(TerrainPathingResources::CaptureNative(source, dry.grid, dry.grid, {}, 1024 * 1024));
    const auto generated = Take(captured.GenerateNative(dry.grid, 1024 * 1024));
    Require(generated[4] == std::byte{0xce} && generated[8] == std::byte{0x40}, "128.25 world bound cannot become tile-rounded 128");
    Require(generated.back() == std::byte{0x40}, "right/top equality stays inside playable bounds");
    dry.members.back().data = Text("[CameraMargins]\nleft=1\n");
    const auto conflict = dry.Write(directory.path / "camera-conflict.w3x");
    Require(!TerrainPathingResources::CaptureNative(conflict, dry.grid, dry.grid, {}, 1024 * 1024).Succeeded(), "conflicting camera definitions cannot hide native first value");
}

void NativeChangedCliffSlotsBindActualRowsAndWinners() {
    Directory directory; Fixture fixture;
    fixture.grid.header.cliffTilesets.push_back("Cl02");
    fixture.terrain = Take(SerializeMapTerrain(fixture.grid)); fixture.members[0].data = fixture.terrain;
    NativeSources(fixture, NativeSettings());
    fixture.members.push_back({"TerrainArt/CliffTypes.slk", CliffTable()});
    const auto source = fixture.Write(directory.path / "cliff-slots.w3x");
    for (const std::uint8_t variation : {0, 1, 2}) {
        auto after = fixture.grid;
        after.vertices[40].cliffAndLayerRaw |= 0x10;
        after.vertices[40].variationRaw = static_cast<std::uint8_t>(variation << 5);
        const auto captured = Take(TerrainPathingResources::CaptureNative(source, fixture.grid, after, {}, 1024 * 1024));
        Require(Take(captured.GenerateNative(after, 1024 * 1024)) == Take(captured.GenerateNative(fixture.grid, 1024 * 1024)),
            "valid existing cliff slot and legal visual variation preserve G0/G1 when levels do not change");
        const std::vector<MemberReplacement> replacements{{"war3map.w3e", Take(SerializeMapTerrain(after))}};
        Check(captured.VerifyFrozenOutput(replacements, {}));
        const auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
            directory.path / ("cliff-saved-" + std::to_string(variation) + ".w3x"), replacements)).path));
        Check(captured.VerifyReadback(saved, 1024 * 1024));
        auto altered = replacements;
        auto table = CliffTable(); table.push_back(std::byte{'\n'});
        altered.push_back({"TerrainArt/CliffTypes.slk", table});
        Require(!captured.VerifyFrozenOutput(altered, {}).Succeeded(), "changed cliff table bytes invalidate frozen output");
        const auto changed = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
            directory.path / ("cliff-drift-" + std::to_string(variation) + ".w3x"), altered)).path));
        Require(!captured.VerifyReadback(changed, 1024 * 1024).Succeeded(), "independent readback binds the actual cliff table");
    }
    auto after = fixture.grid; after.vertices[40].cliffAndLayerRaw |= 0x10;
    for (const auto& pair : {std::pair{"ClXX", "Cl02"}, std::pair{"Cl01", "ClXX"}}) {
        fixture.members.back().data = CliffTable(pair.first, pair.second);
        const auto missing = fixture.Write(directory.path / (std::string(pair.first) + pair.second + ".w3x"));
        const auto refused = TerrainPathingResources::CaptureNative(missing, fixture.grid, after, {}, 1024 * 1024);
        Require(!refused.Succeeded() && HasCode(refused.diagnostics, "TERRAIN_PATHING_PROPERTY_MISSING"),
            "both changed old and new cliff rawcodes need actual rows");
    }
    fixture.members.pop_back();
    const auto without = fixture.Write(directory.path / "missing-cliff-table.w3x");
    Require(!TerrainPathingResources::CaptureNative(without, fixture.grid, after, {}, 1024 * 1024).Succeeded(),
        "missing actual cliff table has no compatibility fallback");
    const auto game = directory.path / "cliff-game"; Require(std::filesystem::create_directory(game), "private cliff game");
    const std::vector<MapArchiveMember> table{{"TerrainArt/CliffTypes.slk", CliffTable()}};
    Take(WriteVerifiedArchive(game / "war3.mpq", table));
    const auto gameOwner = Take(TerrainPathingResources::CaptureNative(without, fixture.grid, after, game, 1024 * 1024));
    Take(WriteVerifiedArchive(game / "war3patch.mpq", table));
    Require(!gameOwner.VerifyReadback(without, 1024 * 1024).Succeeded(),
        "same cliff bytes in a different game archive winner invalidate source identity");
}

void NativeCliffSlotsDoNotExpandThePaletteDomain() {
    Directory directory; Fixture fixture;
    NativeSources(fixture, NativeSettings());
    const auto source = fixture.Write(directory.path / "slot-domain.w3x");
    for (const std::uint8_t slot : {1, 15}) {
        // 15号哨兵只新增完整内圈平地能力；边缘没有完整3×3，仍须拒绝。
        auto after = fixture.grid; after.vertices[slot == 15 ? 0 : 40].cliffAndLayerRaw = static_cast<std::uint8_t>((slot << 4) | 2);
        const auto result = TerrainPathingResources::CaptureNative(source, fixture.grid, after, {}, 1024 * 1024);
        Require(!result.Succeeded() && HasCode(result.diagnostics, "TERRAIN_NATIVE_CLIFF_SLOT"),
            "out-of-palette and incomplete-edge sentinel slots cannot acquire native save resources");
    }
    auto appended = fixture.grid; appended.header.cliffTilesets.push_back("Cl02");
    appended.vertices[40].cliffAndLayerRaw |= 0x10;
    const auto palette = TerrainPathingResources::CaptureNative(source, fixture.grid, appended, {}, 1024 * 1024);
    Require(!palette.Succeeded() && HasCode(palette.diagnostics, "TERRAIN_NATIVE_SOURCE_SHAPE"),
        "this save domain only permits existing cliff slots, not palette mutation");
    fixture.grid.vertices[0].cliffAndLayerRaw = 0xf2;
    fixture.members[0].data = Take(SerializeMapTerrain(fixture.grid));
    const auto sentinel = fixture.Write(directory.path / "old-sentinel.w3x");
    auto resolved = fixture.grid; resolved.vertices[0].cliffAndLayerRaw = 2;
    const auto oldInvalid = TerrainPathingResources::CaptureNative(sentinel, fixture.grid, resolved, {}, 1024 * 1024);
    Require(!oldInvalid.Succeeded() && HasCode(oldInvalid.diagnostics, "TERRAIN_NATIVE_CLIFF_SLOT"),
        "a new valid slot does not legitimize a sentinel with an incomplete edge neighborhood");
}

void NativeFlatSentinelTransitionsBindOnlyConcreteResources() {
    Directory directory;
    for (const bool reverse : {false, true}) for (const std::uint8_t level : {2, 3}) {
        const auto suffix = std::string(reverse ? "restore-" : "select-") + std::to_string(level);
        Fixture fixture; fixture.grid.header.cliffTilesets.push_back("Cl02");
        for (auto& vertex : fixture.grid.vertices) vertex.cliffAndLayerRaw = 0xf2;
        auto after = fixture.grid;
        (reverse ? fixture.grid : after).vertices[40].cliffAndLayerRaw = static_cast<std::uint8_t>(0x10 | level);
        fixture.terrain = Take(SerializeMapTerrain(fixture.grid)); fixture.members[0].data = fixture.terrain;
        NativeSources(fixture, NativeSettings());
        // 只有实际槽1的Cl02需要存在；15不是第16个材质，未用的Cl01不应被伪造或强求。
        fixture.members.push_back({"TerrainArt/CliffTypes.slk", CliffTable("ClXX", "Cl02")});
        const auto source = fixture.Write(directory.path / (suffix + ".w3x"));
        const auto captured = Take(TerrainPathingResources::CaptureNative(source, fixture.grid, after, {}, 1024 * 1024));
        Bytes flat(32 * 32, std::byte{0x40}), concrete = flat;
        if (level == 3) for (std::size_t y = 12; y < 20; ++y) for (std::size_t x = 12; x < 20; ++x)
            concrete[y * 32 + x] = std::byte{0xca};
        Require(Take(captured.GenerateNative(fixture.grid, 1024 * 1024)) == (reverse ? concrete : flat)
            && Take(captured.GenerateNative(after, 1024 * 1024)) == (reverse ? flat : concrete),
            "sentinel transition retains independently expected whole old/new grids, including changed cliff blocking");
        const std::vector<MemberReplacement> output{{"war3map.w3e", Take(SerializeMapTerrain(after))}};
        Check(captured.VerifyFrozenOutput(output, {}));
        Require(!captured.VerifyFrozenOutput({}, {}).Succeeded(), "sentinel output must carry the captured new W3E");
        const auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(directory.path / (suffix + "-saved.w3x"), output)).path));
        Check(captured.VerifyReadback(saved, 1024 * 1024));
        const auto falseSource = TerrainPathingResources::CaptureNative(source, after, fixture.grid, {}, 1024 * 1024);
        Require(!falseSource.Succeeded() && HasCode(falseSource.diagnostics, "TERRAIN_NATIVE_SOURCE_IDENTITY"),
            "symmetric sentinel handling cannot substitute the edited terrain for the actual old source");
        auto extra = after; extra.vertices[40].groundHeightRaw += 1;
        const auto outsidePair = captured.GenerateNative(extra, 1024 * 1024);
        Require(!outsidePair.Succeeded() && HasCode(outsidePair.diagnostics, "TERRAIN_NATIVE_SOURCE_IDENTITY"),
            "sentinel capture still binds both exact terrain revisions");

        auto altered = output;
        altered.push_back({"TerrainArt/CliffTypes.slk", CliffTable("ClXX", "ClYY")});
        Require(!captured.VerifyFrozenOutput(altered, {}).Succeeded(), "actual concrete cliff table remains a frozen dependency");
        const auto drift = Take(MapArchiveSession::Open(Take(source.SaveCandidate(directory.path / (suffix + "-drift.w3x"), altered)).path));
        Require(!captured.VerifyReadback(drift, 1024 * 1024).Succeeded(), "readback detects replaced concrete cliff resources");
        fixture.members.back().data = altered.back().data;
        const auto missing = fixture.Write(directory.path / (suffix + "-missing-row.w3x"));
        const auto refused = TerrainPathingResources::CaptureNative(missing, fixture.grid, after, {}, 1024 * 1024);
        Require(!refused.Succeeded() && HasCode(refused.diagnostics, "TERRAIN_PATHING_PROPERTY_MISSING"),
            "both sentinel directions still require the actual concrete-side CliffTypes row");

        // 同一输入的地图winner可胜过游戏中的缺行；移除地图表后必须采用实际游戏winner。
        const auto game = directory.path / (suffix + "-game"); Require(std::filesystem::create_directory(game), "private sentinel game fixture");
        const std::vector<MapArchiveMember> wrongGame{{"TerrainArt/CliffTypes.slk", CliffTable("ClXX", "ClYY")}};
        Take(WriteVerifiedArchive(game / "war3.mpq", wrongGame));
        const auto mapWinner = Take(TerrainPathingResources::CaptureNative(source, fixture.grid, after, game, 1024 * 1024));
        Check(mapWinner.VerifyFrozenOutput(output, {}));
        fixture.members.pop_back();
        const auto without = fixture.Write(directory.path / (suffix + "-without-table.w3x"));
        const auto wrongWinner = TerrainPathingResources::CaptureNative(without, fixture.grid, after, game, 1024 * 1024);
        Require(!wrongWinner.Succeeded() && HasCode(wrongWinner.diagnostics, "TERRAIN_PATHING_PROPERTY_MISSING"),
            "missing map table cannot borrow the previously captured map winner");
        const std::vector<MapArchiveMember> actualGame{{"TerrainArt/CliffTypes.slk", CliffTable("ClXX", "Cl02")}};
        Take(WriteVerifiedArchive(game / "war3patch.mpq", actualGame));
        const auto gameWinner = Take(TerrainPathingResources::CaptureNative(without, fixture.grid, after, game, 1024 * 1024));
        Check(gameWinner.VerifyFrozenOutput(output, {}));
        const auto gameSaved = Take(MapArchiveSession::Open(Take(without.SaveCandidate(
            directory.path / (suffix + "-game-saved.w3x"), output)).path));
        Check(gameWinner.VerifyReadback(gameSaved, 1024 * 1024));
    }
}

void NativeSentinelTransitionsRequireSafeCompleteFlatNeighborhoods() {
    Directory directory;
    for (const bool reverse : {false, true}) for (const unsigned failure : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
        Fixture fixture; fixture.grid.header.cliffTilesets.push_back("Cl02");
        for (auto& vertex : fixture.grid.vertices) vertex.cliffAndLayerRaw = 0xf2;
        auto after = fixture.grid;
        auto& sentinel = reverse ? after : fixture.grid;
        auto& concrete = reverse ? fixture.grid : after;
        const std::size_t index = failure == 9 ? 0 : 40;
        concrete.vertices[index].cliffAndLayerRaw = 0x12;
        switch (failure) {
        case 0: sentinel.vertices[50].cliffAndLayerRaw = 0xf3; break; // 对角点使incident cell非平。
        case 1: fixture.grid.vertices[30].textureAndFlagsRaw |= 0x10; break;
        case 2: after.vertices[50].textureAndFlagsRaw |= 0x80; break;
        case 3: fixture.grid.vertices[39].waterHeightAndFlagsRaw |= 0x4000; break;
        case 4: after.vertices[41].waterHeightAndFlagsRaw |= 0x8000; break;
        case 5: fixture.grid.vertices[31].groundHeightRaw |= 0x4000; break;
        case 6: sentinel.vertices[49].variationRaw = 3u << 5; break;
        case 7: concrete.vertices[49].variationRaw = 3u << 5; break;
        case 8: sentinel.vertices[48].cliffAndLayerRaw = 0x22; break;
        default: break;
        }
        fixture.terrain = Take(SerializeMapTerrain(fixture.grid)); fixture.members[0].data = fixture.terrain;
        NativeSources(fixture, NativeSettings()); fixture.members.push_back({"TerrainArt/CliffTypes.slk", CliffTable()});
        const auto source = fixture.Write(directory.path / (std::string(reverse ? "reverse-" : "forward-") + std::to_string(failure) + ".w3x"));
        const auto result = TerrainPathingResources::CaptureNative(source, fixture.grid, after, {}, 1024 * 1024);
        Require(!result.Succeeded() && HasCode(result.diagnostics, "TERRAIN_NATIVE_CLIFF_SLOT"),
            "actual cliff rows cannot legitimize a nonflat, ramp, boundary, unknown or incomplete sentinel neighborhood");
    }
}

void NativeCoreRequiresObjectAdapterForNonemptyPlacements() {
    Directory directory;
    for (const auto mode : {0, 1, 2, 3}) {
        Fixture fixture; DoodadPlacements doodads; DoodadPlacement record;
        record.rawcode = "Dtr1"; record.scale = {1, 1, 1}; doodads.doodads.push_back(record);
        NativeSources(fixture, NativeSettings(), doodads);
        fixture.members.push_back({"Units/DestructableData.slk", Text("ID;PWXL;N;E\nB;X2;Y2;D0\n"
            "C;X1;Y1;K\"DestructableID\"\nC;X2;K\"walkable\"\nC;X1;Y2;K\""
            + std::string(mode == 2 ? "Dtr2" : "Dtr1") + "\"\nC;X2;K" + (mode == 1 ? "1" : "0") + "\nE\n")});
        if (mode == 3) fixture.members.push_back({"war3map.w3b", Text("unparsed-overrides")});
        const auto source = fixture.Write(directory.path / ("object-" + std::to_string(mode) + ".w3x"));
        auto captured = TerrainPathingResources::CaptureNative(source, fixture.grid, fixture.grid, {}, 1024 * 1024);
        Require(!captured.Succeeded() && HasCode(captured.diagnostics, mode == 3
                ? "TERRAIN_NATIVE_OBJECT_OVERRIDE_UNSUPPORTED" : "TERRAIN_NATIVE_OBJECT_ADAPTER_REQUIRED"),
            "a destructable row alone cannot prove a unique class without the shared strict adapter");
    }
    Fixture fixture; DoodadPlacements special; special.specialDoodads.push_back({"Dtr1", 0, {1, 1}});
    NativeSources(fixture, NativeSettings(), special);
    const auto source = fixture.Write(directory.path / "special.w3x");
    const auto result = TerrainPathingResources::CaptureNative(source, fixture.grid, fixture.grid, {}, 1024 * 1024);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "TERRAIN_NATIVE_SPECIAL_UNSUPPORTED"), "special TerrainModel cannot be silently ignored");
}

void NativeDirectoryCaptureUsesTheSameFrozenResources() {
    Directory directory; Fixture fixture; NativeSources(fixture, NativeSettings());
    const auto root = directory.path / "map"; Require(std::filesystem::create_directory(root), "native map directory");
    for (const auto& member : fixture.members) {
        const auto path = root / member.name;
        std::filesystem::create_directories(path.parent_path()); Check(IO::WriteFileBytes(path, member.data));
    }
    const auto workspace = Take(DirectoryWorkspace::Open(root));
    const auto captured = Take(TerrainPathingResources::CaptureNative(workspace.Snapshot(), fixture.grid, fixture.grid, {}, 1024 * 1024));
    Require(Take(captured.GenerateNative(fixture.grid, 1024 * 1024)).front() == std::byte{0x40}, "directory uses same actual ground rules");
    Check(captured.VerifyReadback(workspace.Snapshot(), 1024 * 1024));
    Check(IO::WriteFileBytes(root / "TerrainArt/Terrain.slk", GroundTable("0")));
    Require(!captured.VerifyReadback(workspace.Snapshot(), 1024 * 1024).Succeeded(), "directory source drift cannot reuse old identity");
}
} // namespace

void CliffRefinementRequiresTheSameBlockingOutline() {
    Directory directory;
    for (const bool existingCliffs : {false, true}) {
        Fixture fixture;
        if (existingCliffs) {
            for (std::uint32_t y = 0; y < 9; ++y) for (std::uint32_t x = 0; x < 9; ++x)
                fixture.grid.vertices[y * 9 + x].cliffAndLayerRaw = static_cast<std::uint8_t>(2 + (x % 2));
            fixture.terrain = Take(SerializeMapTerrain(fixture.grid)); fixture.members[0].data = fixture.terrain;
        }
        const auto source = fixture.Write(directory.path / (existingCliffs ? "existing-cliffs.w3x" : "flat.w3x"));
        MapDataEditSession edits(MapDocument::FromMembers({})); fixture.Attach(edits);
        TerrainBrush brush; brush.kind = TerrainBrushKind::CliffLevel; brush.radius = 0; brush.levelDelta = 1;
        const TerrainStrokePoint point{512, 512}; Take(edits.EditTerrain(brush, std::span(&point, 1), false));
        const auto plan = Take(TerrainSavePlan::PrepareForSave(source, edits, {}));
        Require(plan.SaveGate().Succeeded() == existingCliffs, "cliff outline change must refuse; unchanged native C may preserve pathing");
        if (existingCliffs) {
            Check(plan.VerifyForSave(edits, source.SourceSha256(), edits.Replacements()));
            const auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(directory.path / "cliffs-saved.w3x", plan.Replacements())).path));
            Check(plan.VerifyReadback(saved));
            Require(Take(saved.ReadMember("war3map.wpm")) == fixture.pathing, "cliff refinement preserves every author pathing byte");
        }
    }
}

int main() {
    try {
        EqualMaterialsSaveRealArchiveWithoutChangingPathing();
        PaletteReplacementAppendAndVariation();
        UnusedPaletteEntriesRequireValidMaterials();
        UnusedKnownPaletteEntriesSaveWithoutChangingPathing();
        UnusedPaletteDependenciesRemainBound();
        UnresolvedGroundIndicesCannotChangeThroughPaletteEdits();
        DifferentOrUnavailablePropertiesRefuse();
        WaterThresholdsUseDepthAndStrictCoverage();
        WaterProfileDuplicatesCannotHideDifferentNativeDepths();
        RestrictionAndTerrainChangesUseIndependentSaveChecks();
        FinalResourcesAndGameWinnersAreRechecked();
        StoredAndDecodedBudgetsShareTheCallerAllowance();
        NativeCaptureBindsFullRulesAndBothTerrainRevisions();
        NativeWaterAndFractionalWorldBoundsUseActualSources();
        NativeChangedCliffSlotsBindActualRowsAndWinners();
        NativeCliffSlotsDoNotExpandThePaletteDomain();
        NativeFlatSentinelTransitionsBindOnlyConcreteResources();
        NativeSentinelTransitionsRequireSafeCompleteFlatNeighborhoods();
        NativeCoreRequiresObjectAdapterForNonemptyPlacements();
        NativeDirectoryCaptureUsesTheSameFrozenResources();
        CliffRefinementRequiresTheSameBlockingOutline();
        std::cout << "Terrain pathing resources tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
