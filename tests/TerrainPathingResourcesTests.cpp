#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqArchive.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>

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
        CliffRefinementRequiresTheSameBlockingOutline();
        std::cout << "Terrain pathing resources tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
