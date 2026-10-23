#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Services/TerrainPathingSave.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::ObjectData;
using War3Map::Services::TerrainPathingSave;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kBudget = 8u * 1024u * 1024u;
constexpr std::string_view kUniformDoodadTexture = "PathTextures/synthetic-uniform.tga";

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Check(Status status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& diagnostic : status.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
    throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) { Check({result.diagnostics}); throw std::runtime_error("missing result"); }
    return std::move(*result.value);
}
template<class T> bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
Bytes Text(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}
Bytes Slk(const std::vector<std::string>& columns, std::initializer_list<std::vector<std::string>> rows) {
    std::string text = "ID;PWXL;N;E\n";
    const auto append = [&](const std::vector<std::string>& cells, std::size_t row) {
        for (std::size_t x = 0; x < cells.size(); ++x)
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(row) + ";K\"" + cells[x] + "\"\n";
    };
    append(columns, 1); std::size_t row = 2;
    for (const auto& cells : rows) append(cells, row++);
    return Text(text + "E\n");
}
Bytes ZeroOverride(std::string field = "p001", bool custom = false) {
    auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Destructable));
    if (custom) Check(document.AddCustomObject("New0", "Dst0"));
    else Check(document.AddOriginalObject("Dst0"));
    Check(document.AddModification({ObjectType::Destructable, custom ? "New0" : "Dst0"},
        {std::move(field)}, NativeValueType::Int32, std::int32_t{0}));
    return Take(NativeObjectCodec(ObjectType::Destructable).Serialize(document));
}

struct Fixture {
    std::filesystem::path root;
    std::filesystem::path gameRoot;
    TerrainGridSnapshot original;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> archive;
    std::unique_ptr<MapDataEditSession> edits;
    const TerrainStrokePoint point{512, 512};
    unsigned sourceSequence = 0;

    explicit Fixture(bool custom = false) {
        static std::atomic<unsigned> sequence{0};
        root = std::filesystem::temp_directory_path() / ("Audastra-native-object-save-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
        Require(std::filesystem::create_directory(root), "create private fixture directory");
        original.header.width = original.header.height = 9; original.header.mainTileset = 'L';
        original.header.groundTilesets = {"Tg01"}; original.header.cliffTilesets = {"Cl01"};
        original.vertices.resize(81);
        for (auto& vertex : original.vertices) {
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 8192 + 48 * 4;
            vertex.textureAndFlagsRaw = 0x40; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(original, 0, 0, 9, 9);
        Bytes wpm = Text("MP3W");
        for (const auto value : {0u, 32u, 32u}) for (unsigned shift = 0; shift < 32; shift += 8)
            wpm.push_back(static_cast<std::byte>((value >> shift) & 255));
        // 独立浅水预期08和作者保护11，不能用待测通行生成器制造原始WPM。
        wpm.resize(16 + 32 * 32, std::byte{0x19});
        MapSettings settings; settings.formatVersion = 25; settings.custom.emplace();
        settings.tileset = 'L'; settings.playableWidth = settings.playableHeight = 8; settings.flags |= 0x400;
        DoodadPlacements doodads;
        DoodadPlacement placed; placed.rawcode = custom ? "New0" : "Dst0";
        placed.position = {512, 512, 0}; placed.scale = {1, 1, 1}; placed.flags = 2; placed.creationId = 1;
        doodads.doodads.push_back(placed);
        members = {{"war3map.w3e", Take(SerializeMapTerrain(original))}, {"war3map.wpm", std::move(wpm)},
            {"war3map.w3i", Take(settings.Serialize())}, {"war3map.doo", Take(doodads.Serialize())},
            {"war3mapUnits.doo", Take(UnitPlacements{}.Serialize())}, {"war3map.w3b", ZeroOverride("p001", custom)},
            {"TerrainArt/Terrain.slk", Slk({"tileID", "walkable", "flyable", "buildable"}, {{"Tg01", "1", "1", "1"}})},
            {"TerrainArt/Water.slk", Slk({"waterID", "height", "impassable"}, {{"LSha", "0", "0"}})},
            {"UI/MiscData.txt", Text("[Water]\nDeepLevelPathing=64\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")},
            {"UI/WorldEditData.txt", Text("[Other]\nValue=1\n")},
            {"Units/DestructableData.slk", Slk({"destructableID", "code", "walkable"}, {{"Dst0", "Dst0", "1"}})},
            {"Doodads/Doodads.slk", Slk({"doodID", "code"}, {{"Doo0", "Doo0"}})},
            {"Units/DestructableMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "data", "index"},
                {{"p001", "walkable", "DestructableData", "bool", "0", "0", "-1"}})},
            {"war3map.j", Text("// original fixture script\n")}, {"war3mapMap.tga", Text("preserved fixture minimap")}};
        Open();
    }
    ~Fixture() {
        edits.reset(); archive.reset();
        std::error_code error; std::filesystem::remove_all(root, error);
    }
    Bytes& Member(std::string_view name) {
        const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
        Require(found != members.end(), "fixture member exists"); return found->data;
    }
    void Open() {
        const auto name = "source-" + std::to_string(sourceSequence++) + ".w3x";
        archive = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / name, members)).path)));
        edits = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(members));
        Check(edits->AttachTerrain(Member("war3map.w3e"))); Check(edits->AttachPathing(Member("war3map.wpm")));
    }
    TerrainBrush Water() const {
        TerrainBrush brush; brush.kind = TerrainBrushKind::WaterHeight; brush.shape = TerrainBrushShape::Square;
        brush.radius = 8; brush.waterHeight = 80; return brush;
    }
    void Paint() { Take(edits->EditTerrain(Water(), std::span(&point, 1), false)); }
    TerrainSavePlan Plan(const TerrainSavePlanLimits& limits = {}) const {
        return Take(TerrainPathingSave::PrepareForSave(*archive, *edits, gameRoot, limits));
    }
    Bytes ExpectedTerrain(bool deep) const {
        auto expected = original;
        if (deep) for (auto& vertex : expected.vertices) vertex.waterHeightAndFlagsRaw = 8192 + 80 * 4;
        // writer仅把明确raw字段编码为字节；预期不调用笔刷或通行算法。
        return Take(SerializeMapTerrain(expected));
    }
    Bytes ExpectedPathing(bool deep) {
        auto expected = Member("war3map.wpm");
        std::fill(expected.begin() + 16, expected.end(), deep ? std::byte{0x1b} : std::byte{0x19});
        return expected;
    }
    std::vector<MemberReplacement> Output(const TerrainSavePlan& plan) const {
        Check(plan.SaveGate()); auto output = edits->Replacements(); Check(plan.InstallPathingOutput(*edits, output));
        Check(plan.VerifyForSave(*edits, archive->SourceSha256(), output)); return output;
    }
    void SaveExpected(const char* name, const Bytes& terrain, const Bytes& pathing) {
        const auto sourcePath = archive->SourcePath();
        const auto sourceSha = Take(Sha256File(sourcePath));
        const auto plan = Plan(); Check(plan.SaveGate()); Require(plan.RebuiltPathing(), "native pathing rebuilt");
        const auto output = Output(plan);
        auto saved = Take(MapArchiveSession::Open(Take(archive->SaveCandidate(root / name, output)).path));
        Check(plan.VerifyReadback(saved));
        Require(Take(saved.ReadMember("war3map.w3e")) == terrain, "full independent W3E expected bytes");
        Require(Take(saved.ReadMember("war3map.wpm")) == pathing, "full independent WPM expected bytes");
        for (const auto& member : members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm")
            Require(Take(saved.ReadMember(member.name)) == member.data, "all unrelated members preserved exactly");
        Require(Take(Sha256File(sourcePath)) == sourceSha, "save does not rewrite source archive");
        Check(plan.CommitSaved(*edits, MapDocument::FromArchive(saved)));
        Require(!edits->IsDirty(), "verified saved source becomes the clean baseline");
        archive = std::make_unique<MapArchiveSession>(std::move(saved));
    }
    void Save(const char* name, bool deep) { SaveExpected(name, ExpectedTerrain(deep), ExpectedPathing(deep)); }
    std::filesystem::path Directory() const {
        const auto directory = root / "directory"; Require(std::filesystem::create_directory(directory), "private directory fixture");
        for (const auto& member : members) {
            const auto path = directory / member.name; std::filesystem::create_directories(path.parent_path());
            Check(IO::WriteFileBytes(path, member.data));
        }
        return directory;
    }
};

void StandardDoodad(Fixture& fixture, bool mixed = false) {
    auto placements = Take(ParseDoodadPlacements(fixture.Member("war3map.doo")));
    auto doodad = placements.doodads.front(); doodad.rawcode = "Doo0"; doodad.creationId = 2;
    if (!mixed) {
        placements.doodads.clear();
        std::erase_if(fixture.members, [](const auto& member) { return member.name == "war3map.w3b"; });
    }
    placements.doodads.push_back(std::move(doodad));
    fixture.Member("war3map.doo") = Take(placements.Serialize());
    fixture.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex"}, {{"Doo0", "Doo0", "_"}});
}

Bytes UniformDoodadTexture(std::uint16_t side = 4) {
    Bytes bytes(18 + std::size_t{side} * side * 3, std::byte{0});
    bytes[2] = std::byte{2}; bytes[16] = std::byte{24};
    bytes[12] = bytes[14] = static_cast<std::byte>(side & 255);
    bytes[13] = bytes[15] = static_cast<std::byte>(side >> 8);
    for (std::size_t at = 18; at < bytes.size(); at += 3) bytes[at] = bytes[at + 2] = std::byte{0xff};
    return bytes;
}
void UniformDoodadExpectedFootprint(Bytes& wpm, bool overlap = false) {
    // 手写原版预期：4x4/BGR ff00ff的内部35编码为ca；与作者11及浅/深水、干地均合成db。
    // 中心512、g=2的起点为14；第二中心576的起点为16。加0.5/0.25后仍为同一矩形。
    for (std::size_t y = 14; y < 18; ++y) for (std::size_t x = 14; x < (overlap ? 20u : 18u); ++x)
        wpm[16 + y * 32 + x] = std::byte{0xdb};
}
void UniformDoodad(Fixture& fixture, bool overlap = false, bool fractional = false) {
    StandardDoodad(fixture);
    fixture.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category"},
        {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), "E"}});
    fixture.members.push_back({std::string(kUniformDoodadTexture), UniformDoodadTexture()});
    if (overlap || fractional) {
        auto placements = Take(ParseDoodadPlacements(fixture.Member("war3map.doo")));
        if (overlap) {
            auto duplicate = placements.doodads.front(); duplicate.creationId = 3;
            placements.doodads.push_back(duplicate);
            duplicate.position[0] = 576; duplicate.creationId = 4; placements.doodads.push_back(duplicate);
        }
        if (fractional) for (auto& placement : placements.doodads) {
            placement.position[0] += 0.5f; placement.position[1] += 0.25f;
        }
        fixture.Member("war3map.doo") = Take(placements.Serialize());
    }
    UniformDoodadExpectedFootprint(fixture.Member("war3map.wpm"), overlap);
}

Bytes DoodadOverride(const char* path = nullptr, std::string field = "dpt0", bool custom = false) {
    auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
    if (custom) Check(document.AddCustomObject("NewD", "Doo0"));
    else Check(document.AddOriginalObject("Doo0"));
    if (path) Check(document.AddModification({ObjectType::Doodad, custom ? "NewD" : "Doo0"}, {std::move(field)},
        NativeValueType::String, std::string(path)));
    return Take(NativeObjectCodec(ObjectType::Doodad).Serialize(document));
}
Bytes DoodadMetadata(std::string field = "dpt0") {
    return Slk({"ID", "field", "slk", "type", "repeat", "index"},
        {{std::move(field), "pathTex", "DoodadData", "pathingTexture", "0", "0"}});
}
void AddDoodadOriginal(Fixture& fixture) {
    fixture.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex"}, {{"Doo0", "Doo0", "PathTextures\\synthetic.tga"}});
    fixture.members.push_back({"war3map.w3d", DoodadOverride("")});
    fixture.members.push_back({"Doodads/DoodadMetaData.slk", DoodadMetadata()});
}

void StandardDoodadsUseTheNormalSaveAndUndoChain() {
    Fixture f; StandardDoodad(f); f.Open(); f.Paint();
    const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget));
    const auto plan = f.Plan(); const auto output = f.Output(plan);
    Check(owner.VerifyFrozenOutput(output, {}));
    auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "doodad-correct.w3x", output)).path));
    Check(plan.VerifyReadback(saved));
    const std::array<MemberReplacement, 2> changes{{
        {"Doodads/Doodads.slk", Slk({"doodID", "code", "pathTex"}, {{"Doo0", "Doo0", "none"}})},
        {"war3map.w3d", DoodadOverride()}}};
    for (std::size_t i = 0; i < changes.size(); ++i) {
        auto altered = output; altered.push_back(changes[i]);
        Require(!owner.VerifyFrozenOutput(altered, {}).Succeeded(), "doodad fields and missing W3D remain source-bound even for semantically empty pathTex");
        const auto tampered = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(
            f.root / ("doodad-changed-" + std::to_string(i) + ".w3x"), std::span(&changes[i], 1))).path));
        Require(!owner.VerifyReadback(tampered, kBudget).Succeeded(), "doodad dependency changes must fail independent readback");
    }
    const auto directory = f.Directory();
    const auto workspace = Take(DirectoryWorkspace::Open(directory));
    const TerrainStrokeIntent intent{f.Water(), {f.point}, false};
    Take(TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.Member("war3map.w3e"), f.Member("war3map.wpm"),
        workspace.Snapshot(), *f.edits, std::span(&intent, 1)));
    Check(IO::WriteFileBytes(directory / "war3map.w3d", DoodadOverride()));
    Require(!TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.Member("war3map.w3e"), f.Member("war3map.wpm"),
        workspace.Snapshot(), *f.edits, std::span(&intent, 1)).Succeeded(), "directory recipe cannot add an unexamined doodad override");
    f.Save("doodad-deep.w3x", true);
    Require(f.edits->Undo(), "undo saved doodad water edit"); f.Save("doodad-shallow.w3x", false);
    Require(f.edits->Redo(), "redo saved doodad water edit"); f.Save("doodad-deep-again.w3x", true);

    Fixture mixed; StandardDoodad(mixed, true); mixed.Open(); mixed.Paint(); mixed.Save("mixed-deep.w3x", true);
    Fixture old; StandardDoodad(old); old.Member("war3map.wpm")[16] |= std::byte{0x04}; old.Open(); old.Paint();
    const auto unexplained = old.Plan();
    Require(!unexplained.SaveGate().Succeeded() && HasCode(unexplained.SaveGate(), "TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
        "standard zero-footprint doodad must not hide unexplained source WPM bits");
    Fixture overridden; StandardDoodad(overridden); overridden.members.push_back({"war3map.w3d", DoodadOverride()});
    overridden.Open(); overridden.Paint();
    const auto refused = overridden.Plan();
    Require(!refused.SaveGate().Succeeded() && HasCode(refused.SaveGate(), "TERRAIN_PATHING_RESOURCE_MISSING"),
        "Original without actual metadata and known fallback source must still refuse");
}

void StandardDoodadsAllowActualCliffAndGroundPathingChanges() {
    for (const bool cliff : {false, true}) for (const bool original : {false, true}) {
        Fixture f; StandardDoodad(f, true);
        if (original) AddDoodadOriginal(f);
        f.original.header.groundTilesets.push_back("Tg02");
        for (auto& vertex : f.original.vertices) vertex.textureAndFlagsRaw = 0;
        RefreshTerrainDerived(f.original, 0, 0, 9, 9);
        f.Member("war3map.w3e") = Take(SerializeMapTerrain(f.original));
        std::fill(f.Member("war3map.wpm").begin() + 16, f.Member("war3map.wpm").end(), std::byte{0x51});
        f.Member("TerrainArt/Terrain.slk") = Slk({"tileID", "walkable", "flyable", "buildable"},
            {{"Tg01", "1", "1", "1"}, {"Tg02", "0", "1", "1"}});
        f.Open();
        TerrainBrush brush; brush.radius = 0;
        if (cliff) { brush.kind = TerrainBrushKind::CliffLevel; brush.levelDelta = 1; }
        else { brush.kind = TerrainBrushKind::Texture; brush.groundTexture = "Tg02"; brush.variation = 0; }
        Take(f.edits->EditTerrain(brush, std::span(&f.point, 1), false));
        auto expectedTerrain = f.original;
        if (cliff) expectedTerrain.vertices[40].cliffAndLayerRaw = 3;
        else expectedTerrain.vertices[40].textureAndFlagsRaw = 1;
        auto expectedPathing = f.Member("war3map.wpm");
        // 独立预期：中心悬崖影响四个地格的8x8；地面属性只影响最近顶点的4x4。
        const std::size_t low = cliff ? 12 : 14, high = cliff ? 20 : 18;
        for (auto y = low; y < high; ++y) for (auto x = low; x < high; ++x)
            expectedPathing[16 + y * 32 + x] = cliff ? std::byte{0xdb} : std::byte{0x53};
        f.SaveExpected(cliff ? "doodad-cliff.w3x" : "doodad-ground.w3x", Take(SerializeMapTerrain(expectedTerrain)), expectedPathing);
        Require(f.edits->Undo(), "undo saved mixed-object cliff/material edit");
        f.SaveExpected("doodad-dry-restored.w3x", f.Member("war3map.w3e"), f.Member("war3map.wpm"));
        Require(f.edits->Redo(), "redo saved mixed-object cliff/material edit");
        f.SaveExpected("doodad-dry-redone.w3x", Take(SerializeMapTerrain(expectedTerrain)), expectedPathing);
    }
}

void DoodadOriginalUsesTheNormalSaveAndUndoChain() {
    Fixture f; StandardDoodad(f, true); AddDoodadOriginal(f); f.Open(); f.Paint();
    const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget));
    Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, owner.CaptureBudgetBytes()));
    const auto shortBudget = TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, owner.CaptureBudgetBytes() - 1);
    Require(!shortBudget.Succeeded() && HasCode(shortBudget, "TERRAIN_NATIVE_OBJECT_BUDGET"),
        "sixth source is subtracted before the complete second validation work reservation");
    const auto plan = f.Plan(); const auto output = f.Output(plan);
    auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "original-verified.w3x", output)).path));
    Check(plan.VerifyReadback(saved));
    unsigned sequence = 0;
    for (const auto* name : {"war3map.w3d", "Doodads/DoodadMetaData.slk"}) {
        auto changed = name == std::string_view("war3map.w3d") ? DoodadOverride("none") : f.Member(name);
        if (name != std::string_view("war3map.w3d")) changed.push_back(std::byte{'\n'});
        auto altered = output; altered.push_back({name, changed});
        Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), altered).Succeeded(), "even semantically equivalent sixth-source/W3D changes invalidate freeze");
        const std::array<MemberReplacement, 1> replacement{{{name, changed}}};
        auto tampered = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(
            f.root / ("original-changed-" + std::to_string(sequence++) + ".w3x"), replacement)).path));
        Require(!plan.VerifyReadback(tampered).Succeeded(), "independent readback binds sixth-source/W3D bytes");
        const std::array<std::string, 1> removed{name};
        Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output, removed).Succeeded(), "sixth-source/W3D removal invalidates freeze");
        auto deleted = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(
            f.root / ("original-removed-" + std::to_string(sequence++) + ".w3x"), {}, removed)).path));
        Require(!plan.VerifyReadback(deleted).Succeeded(), "sixth-source/W3D absence invalidates readback");
    }
    auto aliases = output;
    aliases.push_back({"Doodads/DoodadMetaData.slk", f.Member("Doodads/DoodadMetaData.slk")});
    aliases.push_back({"DOODADS/DOODADMETADATA.SLK", f.Member("Doodads/DoodadMetaData.slk")});
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), aliases).Succeeded(), "sixth-source aliases cannot duplicate the frozen dependency");
    const auto directory = f.Directory(); const auto workspace = Take(DirectoryWorkspace::Open(directory));
    const TerrainStrokeIntent intent{f.Water(), {f.point}, false};
    const auto seal = [&] { return TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.Member("war3map.w3e"),
        f.Member("war3map.wpm"), workspace.Snapshot(), *f.edits, std::span(&intent, 1)); };
    Take(seal());
    auto changed = f.Member("Doodads/DoodadMetaData.slk"); changed.push_back(std::byte{'\n'});
    Check(IO::WriteFileBytes(directory / "Doodads/DoodadMetaData.slk", changed));
    Require(!seal().Succeeded(), "directory recipe rejects changed sixth source");
    f.Save("original-deep.w3x", true);
    Require(f.edits->Undo(), "undo saved Original water edit"); f.Save("original-shallow.w3x", false);
    Require(f.edits->Redo(), "redo saved Original water edit"); f.Save("original-deep-again.w3x", true);

    Fixture fallback; StandardDoodad(fallback); fallback.members.push_back({"war3map.w3d", DoodadOverride()});
    fallback.members.push_back({"Doodads/DoodadMetaData.slk", DoodadMetadata()}); fallback.Open(); fallback.Paint();
    fallback.Save("empty-original.w3x", true);
    Fixture old; StandardDoodad(old); AddDoodadOriginal(old); old.Member("war3map.wpm")[16] |= std::byte{0x04}; old.Open(); old.Paint();
    const auto unexplained = old.Plan();
    Require(!unexplained.SaveGate().Succeeded() && HasCode(unexplained.SaveGate(), "TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
        "Original metadata does not bypass old WPM provenance checks");
}

void DoodadMetadataIsDemandLoadedAndWinnerBound() {
    Fixture old; StandardDoodad(old);
    auto unrelated = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad)); Check(unrelated.AddOriginalObject("Doo1"));
    old.members.push_back({"war3map.w3d", Take(NativeObjectCodec(ObjectType::Doodad).Serialize(unrelated))});
    old.Open(); old.Paint();
    const auto oldOwner = Take(TerrainPathingSave::CaptureNative(*old.archive, old.original, *old.edits->Terrain(), {}, kBudget));
    old.members.push_back({"Doodads/DoodadMetaData.slk", Text("not a valid table and must not be requested")});
    old.Open(); old.Paint();
    const auto sameOwner = Take(TerrainPathingSave::CaptureNative(*old.archive, old.original, *old.edits->Terrain(), {}, kBudget));
    Require(oldOwner.SourceBytes() == sameOwner.SourceBytes() && oldOwner.CaptureBudgetBytes() == sameOwner.CaptureBudgetBytes(),
        "unrelated W3D preserves exactly the five-source domain and budget with unknown game fallback");
    old.Save("unrelated-w3d.w3x", true);

    Fixture f; StandardDoodad(f); AddDoodadOriginal(f); f.Open(); f.Paint();
    const auto game = f.root / "game"; Require(std::filesystem::create_directory(game), "private doodad game fixture");
    const std::array<MapArchiveMember, 1> gameMembers{{{"Doodads/DoodadMetaData.slk", DoodadMetadata("alt0")}}};
    Take(WriteVerifiedArchive(game / "war3.mpq", gameMembers));
    auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), game, kBudget));
    Check(owner.VerifyFrozenOutput(f.edits->Replacements(), {}));
    std::erase_if(f.members, [](const auto& member) { return member.name == "Doodads/DoodadMetaData.slk"; });
    f.Open(); f.Paint();
    const auto mismatch = TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), game, kBudget);
    Require(!mismatch.Succeeded() && HasCode(mismatch, "TERRAIN_NATIVE_OBJECT_METADATA")
        && !HasCode(mismatch, "TERRAIN_NATIVE_OBJECT_DOODAD_METADATA_REQUIRED"), "actual game winner mismatch is terminal after one capture");
    f.Member("war3map.w3d") = DoodadOverride("", "alt0"); f.Open(); f.Paint();
    owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), game, kBudget));
    auto inserted = f.edits->Replacements();
    Check(owner.VerifyFrozenOutput(inserted, {}));
    const auto baseline = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "game-sixth.w3x", inserted)).path));
    Check(owner.VerifyReadback(baseline, kBudget));
    inserted.push_back({"Doodads/DoodadMetaData.slk", DoodadMetadata("alt0")});
    const auto metadataChanged = [](const Status& status) {
        return !status.Succeeded() && std::any_of(status.diagnostics.begin(), status.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "TERRAIN_PATHING_RESOURCE_CHANGED"
                && diagnostic.message.find("doodads/doodadmetadata.slk") != std::string::npos;
        });
    };
    Require(metadataChanged(owner.VerifyFrozenOutput(inserted, {})), "map import cannot replace a captured game metadata winner");
    const auto changed = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "inserted-sixth.w3x", inserted)).path));
    Require(metadataChanged(owner.VerifyReadback(changed, kBudget)), "readback rejects metadata insertion with the correct after W3E");
    const auto gameBytes = Take(IO::ReadFileBytes(game / "war3.mpq"));
    Check(IO::WriteFileBytes(game / "war3.mpq", Text("damaged synthetic archive")));
    Require(!owner.VerifyFrozenOutput(f.edits->Replacements(), {}).Succeeded(), "sixth-source game bytes remain bound until publication");
    Check(IO::WriteFileBytes(game / "war3.mpq", gameBytes));
    Check(owner.VerifyFrozenOutput(f.edits->Replacements(), {}));
}

void TypedOverrideUsesTheNormalSaveAndUndoChain() {
    for (const bool custom : {false, true}) {
        Fixture f(custom); f.Paint();
        const auto unsupportedCore = TerrainPathingResources::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget);
        Require(!unsupportedCore.Succeeded() && HasCode(unsupportedCore, "TERRAIN_NATIVE_OBJECT_OVERRIDE_UNSUPPORTED"),
            "Core without ObjectData cannot grant typed object qualification");
        const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget));
        Require(owner.CaptureBudgetBytes() > owner.SourceBytes(), "typed parser working bytes are accumulated with source bytes");
        f.Save("deep.w3x", true);
        Require(f.edits->Undo(), "undo saved water brush"); f.Save("shallow.w3x", false);
        Require(f.edits->Redo(), "redo saved water brush"); f.Save("deep-again.w3x", true);
    }
}

void MissingW3bStillUsesStrictClassificationAndTheSaveChain() {
    const auto standardOnly = [](Fixture& f) {
        std::erase_if(f.members, [](const auto& member) { return member.name == "war3map.w3b"; });
        f.Member("Units/DestructableData.slk") = Slk({"destructableID", "code", "walkable"}, {{"Dst0", "Dst0", "0"}});
    };
    Fixture f; standardOnly(f); f.Open(); f.Paint();
    const auto core = TerrainPathingResources::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget);
    Require(!core.Succeeded() && HasCode(core, "TERRAIN_NATIVE_OBJECT_ADAPTER_REQUIRED"),
        "Core must not infer the class from a destructable table without the shared object adapter");
    const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget));
    const auto plan = f.Plan();
    const auto output = f.Output(plan);
    Check(owner.VerifyFrozenOutput(output, {}));
    const auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "classified.w3x", output)).path));
    Check(owner.VerifyReadback(saved, kBudget));
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    placements.doodads.front().position[2] = 20;
    auto elevation = output; elevation.push_back({"war3map.doo", Take(placements.Serialize())});
    Check(owner.VerifyFrozenOutput(elevation, {}));
    placements.doodads.front().position[0] += 20;
    elevation.back().data = Take(placements.Serialize());
    Require(!owner.VerifyFrozenOutput(elevation, {}).Succeeded(),
        "a typed nonwalkable class cannot hide a changed XY position");
    for (const auto type : {ObjectType::Destructable, ObjectType::Doodad}) {
        const auto name = type == ObjectType::Destructable ? "war3map.w3b" : "war3map.w3d";
        const auto inserted = Take(NativeObjectCodec(type).Serialize(Take(NativeObjectDocument::CreateEmpty(type))));
        auto changed = output; changed.push_back({name, inserted});
        Require(!owner.VerifyFrozenOutput(changed, {}).Succeeded(),
            "both native object absences are frozen even without any W3B overrides");
        const std::array<MemberReplacement, 1> changes{{{name, inserted}}};
        const auto tampered = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root / (std::string(name) + ".w3x"), changes)).path));
        Require(!owner.VerifyReadback(tampered, kBudget).Succeeded(), "owner readback checks absent-to-present object dependencies");
    }
    const auto directory = f.Directory();
    const auto workspace = Take(DirectoryWorkspace::Open(directory));
    const TerrainStrokeIntent intent{f.Water(), {f.point}, false};
    Take(TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.Member("war3map.w3e"),
        f.Member("war3map.wpm"), workspace.Snapshot(), *f.edits, std::span(&intent, 1)));
    Check(IO::WriteFileBytes(directory / "Doodads/Doodads.slk", Slk({"doodID", "code"}, {{"Dst0", "Dst0"}})));
    Require(!TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.Member("war3map.w3e"),
        f.Member("war3map.wpm"), workspace.Snapshot(), *f.edits, std::span(&intent, 1)).Succeeded(),
        "no-W3B directory recipe still binds the actual doodad classification table");
    f.Save("standard-deep.w3x", true);
    Require(f.edits->Undo(), "undo standard-only saved water brush"); f.Save("standard-shallow.w3x", false);
    Require(f.edits->Redo(), "redo standard-only saved water brush"); f.Save("standard-deep-again.w3x", true);

    for (const auto mode : {0, 1, 2}) {
        Fixture conflict; standardOnly(conflict);
        if (mode == 0) {
            conflict.Member("Doodads/Doodads.slk") = Slk({"doodID", "code"}, {{"Doo0", "Doo0"}, {"Dst0", "Dst0"}});
        } else {
            auto doodads = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
            if (mode == 1) Check(doodads.AddOriginalObject("Dst0"));
            else Check(doodads.AddCustomObject("Dst0", "Doo0"));
            conflict.members.push_back({"war3map.w3d", Take(NativeObjectCodec(ObjectType::Doodad).Serialize(doodads))});
        }
        conflict.Open(); conflict.Paint();
        const auto refused = conflict.Plan();
        Require(!refused.SaveGate().Succeeded() && HasCode(refused.SaveGate(), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS"),
            "without W3B, standard and native doodad aliases must still refuse ambiguous class");
    }
}

void DependencyReplacementRemovalAndReadbackStayBound() {
    Fixture f; f.Paint(); const auto plan = f.Plan(); const auto output = f.Output(plan);
    for (const auto* name : {"war3map.w3b", "Units/DestructableData.slk", "Doodads/Doodads.slk", "Units/DestructableMetaData.slk"}) {
        auto replaced = output; auto changed = f.Member(name); changed.push_back(std::byte{0}); replaced.push_back({name, std::move(changed)});
        Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), replaced).Succeeded(), "dependency replacement rejected");
        const std::array<std::string, 1> removed{name};
        Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output, removed).Succeeded(), "dependency removal rejected");
        auto duplicate = output; duplicate.push_back({name, f.Member(name)}); duplicate.push_back({name, f.Member(name)});
        Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), duplicate).Succeeded(), "duplicate dependency rejected");
    }
    auto alias = output; alias.push_back({"WAR3MAP.W3B", f.Member("war3map.w3b")});
    alias.push_back({"war3map.w3b", f.Member("war3map.w3b")});
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), alias).Succeeded(), "canonical alias duplicate rejected");
    const auto emptyDoodads = Take(NativeObjectCodec(ObjectType::Doodad).Serialize(
        Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad))));
    auto inserted = output; inserted.push_back({"war3map.w3d", emptyDoodads});
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), inserted).Succeeded(), "missing W3D cannot become present in final output");
    auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "correct.w3x", output)).path));
    Check(plan.VerifyReadback(saved));
    unsigned sequence = 0;
    for (const auto* name : {"war3map.w3b", "war3map.w3d", "Units/DestructableMetaData.slk"}) {
        auto changed = name == std::string_view("war3map.w3d") ? emptyDoodads : f.Member(name);
        if (name != std::string_view("war3map.w3d")) changed.push_back(std::byte{0});
        const std::array<MemberReplacement, 1> changes{{{name, std::move(changed)}}};
        auto tampered = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(
            f.root / ("changed-" + std::to_string(sequence++) + ".w3x"), changes)).path));
        Require(!plan.VerifyReadback(tampered).Succeeded(), "independent readback detects dependency bytes and absent-to-present changes");
    }
    const std::array<std::string, 1> removed{"war3map.w3b"};
    auto deleted = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root / "deleted.w3x", {}, removed)).path));
    Require(!plan.VerifyReadback(deleted).Succeeded(), "readback detects missing source dependency");
}

void NativeObjectsDoNotWeakenSourceCasOrOldPathingChecks() {
    Fixture f; f.Paint(); const auto plan = f.Plan(); const auto output = f.Output(plan);
    Require(!plan.VerifyForSave(*f.edits, std::string(64, '0'), output).Succeeded(), "source SHA CAS remains required");
    TerrainBrush raise; raise.kind = TerrainBrushKind::Raise; raise.radius = 0; raise.strength = 1;
    Take(f.edits->EditTerrain(raise, std::span(&f.point, 1), false));
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output).Succeeded(), "later edit invalidates frozen plan");
    Fixture old;
    old.Member("war3map.wpm")[16] |= std::byte{0x04}; old.Open(); old.Paint();
    const auto unexplained = old.Plan();
    Require(!unexplained.SaveGate().Succeeded() && HasCode(unexplained.SaveGate(), "TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
        "typed zero walkable cannot hide an unexplained source WPM bit");
    Fixture special;
    auto placements = Take(ParseDoodadPlacements(special.Member("war3map.doo")));
    placements.specialDoodads.push_back({"Spc0", 0, {1, 1}});
    special.Member("war3map.doo") = Take(placements.Serialize()); special.Open(); special.Paint();
    const auto refused = special.Plan();
    Require(!refused.SaveGate().Succeeded() && HasCode(refused.SaveGate(), "TERRAIN_NATIVE_OBJECT_INHERITANCE"),
        "unknown special rawcode never inherits normal typed object qualification");
    Fixture limited; limited.Paint();
    const auto owner = Take(TerrainPathingSave::CaptureNative(*limited.archive, limited.original, *limited.edits->Terrain(), {}, kBudget));
    const auto budget = TerrainPathingSave::CaptureNative(*limited.archive, limited.original, *limited.edits->Terrain(), {}, owner.SourceBytes());
    Require(!budget.Succeeded(), "reading source bytes alone does not pay the parser working budget");
    const TerrainSavePlanLimits limits{owner.SourceBytes() + 32768};
    const auto limitedPlan = TerrainPathingSave::PrepareForSave(*limited.archive, *limited.edits, {}, limits);
    Require(!limitedPlan.Succeeded() || !limitedPlan.value || !limitedPlan.value->SaveGate().Succeeded(),
        "whole save plan includes typed parser working bytes in cumulative budget");
}

void MapAndGameMetadataWinnersRemainBound() {
    Fixture f; const auto game = f.root / "game"; Require(std::filesystem::create_directory(game), "private game fixture");
    const auto gameMetadata = Slk({"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"q902", "walkable", "DestructableData", "bool", "0", "0", "-1"}});
    const std::array<MapArchiveMember, 1> gameMembers{{{"Units/DestructableMetaData.slk", gameMetadata}}};
    Take(WriteVerifiedArchive(game / "war3.mpq", gameMembers));
    f.Paint();
    auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), game, kBudget));
    Check(owner.VerifyFrozenOutput(f.edits->Replacements(), {}));
    // 地图中的p001必须胜过本机包中的q902，不能混用两处metadata。
    std::erase_if(f.members, [](const auto& member) { return member.name == "Units/DestructableMetaData.slk"; });
    f.Open(); f.Paint();
    const auto mismatched = TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), game, kBudget);
    Require(!mismatched.Succeeded() && HasCode(mismatched, "TERRAIN_NATIVE_OBJECT_METADATA"),
        "missing map metadata uses the actual game winner instead of a cached map definition");
    f.Member("war3map.w3b") = ZeroOverride("q902"); f.Open(); f.Paint();
    owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), game, kBudget));
    Check(owner.VerifyFrozenOutput(f.edits->Replacements(), {}));
    const auto previousArchive = Take(IO::ReadFileBytes(game / "war3.mpq"));
    Check(IO::WriteFileBytes(game / "war3.mpq", Text("damaged private fixture archive")));
    Require(!owner.VerifyFrozenOutput(f.edits->Replacements(), {}).Succeeded(), "game-winning dependency must still be readable before publication");
    Check(IO::WriteFileBytes(game / "war3.mpq", previousArchive));
    Check(owner.VerifyFrozenOutput(f.edits->Replacements(), {}));
}

void DirectoryRecipePreservesReplayAndSourceChecks() {
    Fixture f; const auto directory = f.Directory(); const auto workspace = Take(DirectoryWorkspace::Open(directory)); f.Paint();
    const TerrainStrokeIntent intent{f.Water(), {f.point}, false};
    const auto seal = [&] {
        return TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.Member("war3map.w3e"),
            f.Member("war3map.wpm"), workspace.Snapshot(), *f.edits, std::span(&intent, 1));
    };
    const auto patch = Take(seal());
    for (const auto* name : {"war3map.w3e", "war3map.wpm"}) {
        const auto file = std::find_if(patch.Files().begin(), patch.Files().end(),
            [&](const auto& value) { return value.relativePath.filename() == name; });
        Require(file != patch.Files().end(), "directory patch contains both terrain outputs");
        const auto expected = name == std::string_view("war3map.w3e") ? f.ExpectedTerrain(true) : f.ExpectedPathing(true);
        Require(file->replacement == expected, "directory output matches complete independent expected bytes");
        Require(Take(IO::ReadFileBytes(directory / name)) == f.Member(name), "sealing keeps source files unchanged");
    }
    auto extra = *f.edits->Terrain(); extra.vertices.front().groundHeightRaw += 1;
    Check(f.edits->SetTerrainAndPathing(Take(SerializeMapTerrain(extra)), f.Member("war3map.wpm")));
    Require(!seal().Succeeded(), "directory recipe rejects an extra net W3E change");
    Require(f.edits->Undo(), "undo extra raw edit");
    TerrainBrush restriction; restriction.kind = TerrainBrushKind::PathingRestriction;
    restriction.size = 1; restriction.pathingRestrictions = 0x04;
    Take(f.edits->EditTerrain(restriction, std::span(&f.point, 1), false));
    Require(!seal().Succeeded(), "directory recipe rejects extra WPM intent absent from the recipe");
    Require(f.edits->Undo(), "undo extra WPM edit");
    Take(seal());
    auto changedMetadata = f.Member("Units/DestructableMetaData.slk"); changedMetadata.push_back(std::byte{'\n'});
    Check(IO::WriteFileBytes(directory / "Units/DestructableMetaData.slk", changedMetadata));
    Require(!seal().Succeeded(), "directory snapshot refuses changed metadata before recipe output");
    Check(IO::WriteFileBytes(directory / "Units/DestructableMetaData.slk", f.Member("Units/DestructableMetaData.slk")));
    Check(IO::WriteFileBytes(directory / "WAR3MAP.W3D", Take(NativeObjectCodec(ObjectType::Doodad).Serialize(
        Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad))))));
    Require(!seal().Succeeded(), "directory snapshot binds a missing W3D classification source");
    const auto refreshed = Take(DirectoryWorkspace::Open(directory));
    auto wrong = refreshed.Snapshot(); wrong.aggregateSha256.assign(64, '0');
    Require(!TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.Member("war3map.w3e"), f.Member("war3map.wpm"),
        wrong, *f.edits, std::span(&intent, 1)).Succeeded(), "directory aggregate source identity is required");
}

void UniformDoodadsSaveWaterGroundAndCliffs() {
    Fixture water; UniformDoodad(water, true, true); water.Open(); water.Paint();
    auto deep = water.ExpectedPathing(true), shallow = water.ExpectedPathing(false);
    UniformDoodadExpectedFootprint(deep, true); UniformDoodadExpectedFootprint(shallow, true);
    water.SaveExpected("uniform-water.w3x", water.ExpectedTerrain(true), deep);
    Require(water.edits->Undo(), "undo saved water with overlapping and duplicate footprints");
    water.SaveExpected("uniform-water-undone.w3x", water.ExpectedTerrain(false), shallow);
    Require(water.edits->Redo(), "redo saved water with overlapping and duplicate footprints");
    water.SaveExpected("uniform-water-redone.w3x", water.ExpectedTerrain(true), deep);

    for (const bool cliff : {false, true}) {
        Fixture f; UniformDoodad(f, false, true);
        f.original.header.groundTilesets.push_back("Tg02");
        for (auto& vertex : f.original.vertices) vertex.textureAndFlagsRaw = 0;
        RefreshTerrainDerived(f.original, 0, 0, 9, 9);
        f.Member("war3map.w3e") = Take(SerializeMapTerrain(f.original));
        std::fill(f.Member("war3map.wpm").begin() + 16, f.Member("war3map.wpm").end(), std::byte{0x51});
        UniformDoodadExpectedFootprint(f.Member("war3map.wpm"));
        f.Member("TerrainArt/Terrain.slk") = Slk({"tileID", "walkable", "flyable", "buildable"},
            {{"Tg01", "1", "1", "1"}, {"Tg02", "0", "1", "1"}});
        f.Open();
        TerrainBrush brush; brush.radius = 0;
        if (cliff) { brush.kind = TerrainBrushKind::CliffLevel; brush.levelDelta = 1; }
        else { brush.kind = TerrainBrushKind::Texture; brush.groundTexture = "Tg02"; brush.variation = 0; }
        const TerrainStrokePoint edited{256, 512};
        Take(f.edits->EditTerrain(brush, std::span(&edited, 1), false));
        auto terrain = f.original;
        if (cliff) terrain.vertices[38].cliffAndLayerRaw = 3;
        else terrain.vertices[38].textureAndFlagsRaw = 1;
        auto pathing = f.Member("war3map.wpm");
        // 修改在占地西侧，确保材质/悬崖派生变化没有被中心静态位完全遮住。
        const std::size_t left = cliff ? 4 : 6, right = cliff ? 12 : 10, low = cliff ? 12 : 14, high = cliff ? 20 : 18;
        for (auto y = low; y < high; ++y) for (auto x = left; x < right; ++x)
            pathing[16 + y * 32 + x] = cliff ? std::byte{0xdb} : std::byte{0x53};
        const auto expected = Take(SerializeMapTerrain(terrain));
        f.SaveExpected(cliff ? "uniform-cliff.w3x" : "uniform-ground.w3x", expected, pathing);
        Require(f.edits->Undo(), "undo saved terrain edit while retaining nonempty footprint");
        f.SaveExpected("uniform-dry-undone.w3x", f.Member("war3map.w3e"), f.Member("war3map.wpm"));
        Require(f.edits->Redo(), "redo saved terrain edit while retaining nonempty footprint");
        f.SaveExpected("uniform-dry-redone.w3x", expected, pathing);
    }
}

void NativeDoodadFootprintsUseTheNormalSaveAndUndoChain() {
    // 覆盖继承、显式改路径、显式清空；旧/新WPM均为独立完整字节预期。
    for (const bool custom : {false, true}) for (const unsigned mode : {0u, 1u, 2u}) {
        Fixture f; UniformDoodad(f);
        if (custom) {
            auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
            placements.doodads.front().rawcode = "NewD";
            f.Member("war3map.doo") = Take(placements.Serialize());
        }
        if (mode == 1) f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category"},
            {{"Doo0", "Doo0", "PathTextures/unused-parent.tga", "E"}});
        f.members.push_back({"war3map.w3d", DoodadOverride(mode == 0 ? nullptr : mode == 1 ? kUniformDoodadTexture.data() : "", "dpt0", custom)});
        f.members.push_back({"Doodads/DoodadMetaData.slk", DoodadMetadata()});
        if (mode == 2) std::fill(f.Member("war3map.wpm").begin() + 16, f.Member("war3map.wpm").end(), std::byte{0x19});
        f.Open(); f.Paint();
        const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget));
        Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, owner.CaptureBudgetBytes()));
        Require(!TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, owner.CaptureBudgetBytes() - 1).Succeeded(),
            "native path and standard inheritance share the exact cumulative budget");
        const auto plan = f.Plan();
        const auto output = f.Output(plan);
        auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "native-frozen.w3x", output)).path));
        unsigned sequence = 0;
        for (const auto* name : {"war3map.w3d", "Doodads/DoodadMetaData.slk", "Doodads/Doodads.slk"}) {
            auto changed = f.Member(name);
            if (std::string_view(name) == "war3map.w3d") changed = DoodadOverride(mode == 2 ? kUniformDoodadTexture.data() : "", "dpt0", custom);
            else changed.push_back(std::byte{'\n'});
            auto altered = output; altered.push_back({name, changed});
            Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), altered).Succeeded(),
                "native record, metadata and actual standard parent stay frozen");
            const std::array<MemberReplacement, 1> replacement{{{name, changed}}};
            const auto tampered = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(
                f.root / ("native-tampered-" + std::to_string(sequence++) + ".w3x"), replacement)).path));
            Require(!plan.VerifyReadback(tampered).Succeeded(), "native inheritance is also checked on independent readback");
        }
        auto deep = f.ExpectedPathing(true), shallow = f.ExpectedPathing(false);
        if (mode != 2) { UniformDoodadExpectedFootprint(deep); UniformDoodadExpectedFootprint(shallow); }
        f.SaveExpected("native-deep.w3x", f.ExpectedTerrain(true), deep);
        Require(f.edits->Undo(), "undo saved native doodad terrain");
        f.SaveExpected("native-shallow.w3x", f.ExpectedTerrain(false), shallow);
        Require(f.edits->Redo(), "redo saved native doodad terrain");
        f.SaveExpected("native-deep-again.w3x", f.ExpectedTerrain(true), deep);
    }
    Fixture bad; UniformDoodad(bad);
    bad.members.push_back({"war3map.w3d", DoodadOverride(kUniformDoodadTexture.data())});
    bad.members.push_back({"Doodads/DoodadMetaData.slk", DoodadMetadata()});
    bad.Member("war3map.wpm")[16] ^= std::byte{0x04}; bad.Open(); bad.Paint();
    const auto sha = bad.archive->SourceSha256();
    const auto gate = bad.Plan().SaveGate();
    Require(!gate.Succeeded() && HasCode(gate, "TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
        "native nonempty path cannot bypass unexplained source WPM");
    Require(Take(Sha256File(bad.archive->SourcePath())) == sha && bad.edits->IsDirty(),
        "failed native terrain save preserves source and unsaved draft");
}

void UniformDoodadTextureIsFrozenAndBudgeted() {
    Fixture f; UniformDoodad(f); f.Open(); f.Paint();
    const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget));
    const auto exact = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, owner.CaptureBudgetBytes()));
    Require(exact.CaptureBudgetBytes() == owner.CaptureBudgetBytes(), "texture and decoded footprint have a reusable exact capture budget");
    const auto shortBudget = TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, owner.CaptureBudgetBytes() - 1);
    Require(!shortBudget.Succeeded() && !shortBudget.value, "one missing capture byte cannot return a partial texture owner");
    const auto plan = f.Plan(); const auto output = f.Output(plan);
    Check(owner.VerifyFrozenOutput(output, {}));
    const auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "uniform-frozen.w3x", output)).path));
    Check(owner.VerifyReadback(saved, kBudget));
    auto changed = f.Member(kUniformDoodadTexture); changed[17] = std::byte{0x20};
    const MemberReplacement replacement{std::string(kUniformDoodadTexture), changed};
    auto altered = output; altered.push_back(replacement);
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), altered).Succeeded(), "same uniform pixels with changed TGA bytes invalidate the frozen plan");
    auto tampered = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root / "uniform-replaced.w3x", std::span(&replacement, 1))).path));
    Require(!owner.VerifyReadback(tampered, kBudget).Succeeded(), "independent readback binds the exact TGA bytes");
    const std::array<std::string, 1> removed{std::string(kUniformDoodadTexture)};
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output, removed).Succeeded(), "referenced TGA removal is refused before publication");
    tampered = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root / "uniform-deleted.w3x", {}, removed)).path));
    Require(!owner.VerifyReadback(tampered, kBudget).Succeeded(), "readback refuses a missing referenced TGA");
    Require(!plan.VerifyForSave(*f.edits, std::string(64, '0'), output).Succeeded(), "nonempty footprint does not relax source SHA CAS");
    TerrainBrush raise; raise.kind = TerrainBrushKind::Raise; raise.radius = 0; raise.strength = 1;
    Take(f.edits->EditTerrain(raise, std::span(&f.point, 1), false));
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output).Succeeded(), "later terrain revision invalidates the nonempty footprint plan");
}

void UniformDoodadGameTextureWinnerRemainsBound() {
    Fixture f; UniformDoodad(f);
    const auto texture = f.Member(kUniformDoodadTexture);
    std::erase_if(f.members, [](const auto& member) { return member.name == kUniformDoodadTexture; });
    const auto game = f.root / "game"; Require(std::filesystem::create_directory(game), "private footprint game fixture");
    const std::array<MapArchiveMember, 1> gameMembers{{{std::string(kUniformDoodadTexture), texture}}};
    Take(WriteVerifiedArchive(game / "war3.mpq", gameMembers));
    f.Open(); f.Paint();
    const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), game, kBudget));
    const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.archive, *f.edits, game, {}));
    const auto output = f.Output(plan);
    Check(owner.VerifyFrozenOutput(output, {}));
    const auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "uniform-game.w3x", output)).path));
    Check(owner.VerifyReadback(saved, kBudget));
    auto expected = f.ExpectedPathing(true); UniformDoodadExpectedFootprint(expected);
    Require(Take(saved.ReadMember("war3map.wpm")) == expected, "actual game-winning texture still produces complete independent WPM bytes");
    auto inserted = output; inserted.push_back({std::string(kUniformDoodadTexture), texture});
    Require(!owner.VerifyFrozenOutput(inserted, {}).Succeeded(), "map insertion cannot replace even an identical game-winning texture");
    const auto tampered = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "uniform-inserted.w3x", inserted)).path));
    Require(!owner.VerifyReadback(tampered, kBudget).Succeeded(), "readback distinguishes identical bytes from the original winning source");
    const auto gameBytes = Take(IO::ReadFileBytes(game / "war3.mpq"));
    auto changed = texture; changed[17] = std::byte{0x20};
    const std::array<MapArchiveMember, 1> changedMembers{{{std::string(kUniformDoodadTexture), changed}}};
    const auto replacement = Take(WriteVerifiedArchive(game / "changed.mpq", changedMembers));
    Check(IO::WriteFileBytes(game / "war3.mpq", Take(IO::ReadFileBytes(replacement.path))));
    Require(!owner.VerifyFrozenOutput(output, {}).Succeeded(), "changed game texture winner is rechecked before publication");
    Check(IO::WriteFileBytes(game / "war3.mpq", gameBytes));
    Check(owner.VerifyFrozenOutput(output, {}));
}

void UniformDoodadLongestPathHasAnExactCaptureBudget() {
    Fixture f; UniformDoodad(f);
    const std::string prefix = "PathTextures/";
    const auto path = prefix + std::string(259 - prefix.size() - 4, 'p') + ".tga";
    Require(path.size() == 259, "exercise the longest supported relative footprint path");
    for (auto& member : f.members) if (member.name == kUniformDoodadTexture) member.name = path;
    f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category"}, {{"Doo0", "Doo0", path, "E"}});
    f.Open(); f.Paint();
    const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget));
    const auto exact = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, owner.CaptureBudgetBytes()));
    Require(exact.CaptureBudgetBytes() == owner.CaptureBudgetBytes(), "long path storage is included in the reusable exact budget");
    const auto shortBudget = TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, owner.CaptureBudgetBytes() - 1);
    Require(!shortBudget.Succeeded() && !shortBudget.value, "one missing byte rejects long path capture without a partial owner");
}

void UniformDoodadDuplicateInstancesHaveABoundedPixelWorkload() {
    Fixture f; UniformDoodad(f);
    f.Member(kUniformDoodadTexture) = UniformDoodadTexture(256);
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    const auto placed = placements.doodads.front();
    placements.doodads.assign(129, placed);
    for (std::size_t i = 0; i < placements.doodads.size(); ++i) placements.doodads[i].creationId = static_cast<std::int32_t>(i + 1);
    f.Member("war3map.doo") = Take(placements.Serialize());
    // 256²占地覆盖整个32²网格；原WPM由独立规则手写，不让损坏原图抢先触发拒绝。
    auto& wpm = f.Member("war3map.wpm"); std::fill(wpm.begin() + 16, wpm.end(), std::byte{0xdb});
    f.Open(); f.Paint();
    // 重复实例仍须逐个计费：129×256² = 8 MiB + 65536，不能只按纹理或唯一落点计一次。
    const auto captured = TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget);
    Require(!captured.Succeeded() && !captured.value && HasCode(captured, "TERRAIN_NATIVE_FOOTPRINT_WORK"),
        "duplicate instances exceeding the cumulative pixel workload return the specific work limit failure");
}

void UniformDoodadRejectionDoesNotWeakenOldSaves() {
    for (const unsigned mode : {0u, 1u, 2u}) {
        Fixture f; UniformDoodad(f); auto& texture = f.Member(kUniformDoodadTexture);
        if (mode == 0) texture.pop_back();
        if (mode == 1) texture[21] = std::byte{0};
        if (mode == 2) { texture[12] = std::byte{2}; texture.resize(18 + 2 * 4 * 3); }
        f.Open(); f.Paint();
        const auto capture = TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget);
        Require(!capture.Succeeded() && !capture.value, "truncated textures or angle-dependent textures without actual fixedRot cannot become footprint owners");
    }
    for (const auto axis : {0u, 1u}) {
        Fixture f; UniformDoodad(f);
        auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
        placements.doodads.front().position[axis] = std::nextafter(544.0f, 512.0f);
        f.Member("war3map.doo") = Take(placements.Serialize());
        f.Open(); f.Paint();
        const auto sourceSha = Take(Sha256File(f.archive->SourcePath()));
        const auto terrainDraft = Take(SerializeMapTerrain(*f.edits->Terrain()));
        const auto capture = TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget);
        Require(!capture.Succeeded() && !capture.value && HasCode(capture, "TERRAIN_UNIFORM_SNAP_AMBIGUOUS"),
            "ambiguous object coordinates cannot create a partial native footprint owner");
        const auto plan = f.Plan();
        Require(!plan.SaveGate().Succeeded() && HasCode(plan.SaveGate(), "TERRAIN_UNIFORM_SNAP_AMBIGUOUS"),
            "the normal save gate preserves the precise coordinate failure");
        Require(Take(Sha256File(f.archive->SourcePath())) == sourceSha && f.edits->IsDirty()
            && Take(SerializeMapTerrain(*f.edits->Terrain())) == terrainDraft,
            "coordinate rejection preserves the original archive and unsaved terrain draft");
    }
    for (const auto& [offset, mask] : std::array<std::pair<std::size_t, unsigned>, 3>{{{16, 0x06}, {16, 0x88}, {16 + 16 * 32 + 16, 0x60}}}) {
        Fixture f; UniformDoodad(f); f.Member("war3map.wpm")[offset] ^= static_cast<std::byte>(mask); f.Open(); f.Paint();
        const auto plan = f.Plan();
        Require(!plan.SaveGate().Succeeded() && HasCode(plan.SaveGate(), "TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
            "multiple unexplained old WPM bits outside or inside the footprint still refuse saving");
    }
    Fixture old; StandardDoodad(old); old.Open(); old.Paint();
    const auto before = Take(TerrainPathingSave::CaptureNative(*old.archive, old.original, *old.edits->Terrain(), {}, kBudget));
    old.members.push_back({std::string(kUniformDoodadTexture), Text("broken but unrelated imported texture")});
    old.Open(); old.Paint();
    const auto after = Take(TerrainPathingSave::CaptureNative(*old.archive, old.original, *old.edits->Terrain(), {}, kBudget));
    Require(before.SourceBytes() == after.SourceBytes() && before.CaptureBudgetBytes() == after.CaptureBudgetBytes(),
        "zero-footprint saves do not read or budget unrelated pathing textures");
    old.Save("zero-footprint-unrelated-texture.w3x", true);
}

void PatternedDoodadsUseTheNormalSaveAndUndoChain() {
    const auto addExpected = [](Bytes& wpm) {
        // top-origin源矩阵(黑蓝绿/红青紫)，wire零角在(512,512)吸附到(544,544)。
        // 原版映射为2x3(黑红/蓝青/绿紫)，从(16,15)开始；这些是WPM文件位。
        constexpr std::array<unsigned, 6> masks{0,0xc2,8,0x0c,4,0xca};
        for (unsigned i=0; i<masks.size(); ++i) wpm[16+(15+i/2)*32+16+i%2] |= static_cast<std::byte>(masks[i]);
    };
    for (const unsigned nativeKind : {0u,1u,2u}) for (const unsigned operation : {0u,1u,2u}) {
        Fixture f; StandardDoodad(f);
        f.Member("Doodads/Doodads.slk") = Slk({"doodID","code","pathTex","category","fixedRot"},
            {{"Doo0","Doo0",std::string(kUniformDoodadTexture),"E","-1"}});
        Bytes texture(18); texture[2]=std::byte{2}; texture[12]=std::byte{3}; texture[14]=std::byte{2};
        texture[16]=std::byte{24}; texture[17]=std::byte{32};
        for (unsigned b : {0,0,0, 1,0,0, 0,1,0, 0,0,1, 1,1,0, 1,0,1}) texture.push_back(static_cast<std::byte>(b));
        f.members.push_back({std::string(kUniformDoodadTexture),texture});
        if (nativeKind != 0) {
            f.members.push_back({"war3map.w3d", DoodadOverride(kUniformDoodadTexture.data(), "dpt0", nativeKind == 2)});
            f.members.push_back({"Doodads/DoodadMetaData.slk", DoodadMetadata()});
            if (nativeKind == 2) {
                auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
                placements.doodads.front().rawcode = "NewD";
                f.Member("war3map.doo") = Take(placements.Serialize());
            }
        }
        if (operation != 0) {
            f.original.header.groundTilesets.push_back("Tg02");
            for (auto& vertex:f.original.vertices) vertex.textureAndFlagsRaw=0;
            RefreshTerrainDerived(f.original,0,0,9,9);
            f.Member("war3map.w3e")=Take(SerializeMapTerrain(f.original));
            std::fill(f.Member("war3map.wpm").begin()+16,f.Member("war3map.wpm").end(),std::byte{0x51});
            f.Member("TerrainArt/Terrain.slk")=Slk({"tileID","walkable","flyable","buildable"},
                {{"Tg01","1","1","1"},{"Tg02","0","1","1"}});
        }
        addExpected(f.Member("war3map.wpm")); f.Open();
        Bytes expectedW3e, expectedWpm;
        if (operation==0) {
            f.Paint(); expectedW3e=f.ExpectedTerrain(true); expectedWpm=f.ExpectedPathing(true); addExpected(expectedWpm);
        } else {
            TerrainBrush brush; brush.radius=0;
            const bool cliff=operation==2;
            if (cliff) {brush.kind=TerrainBrushKind::CliffLevel;brush.levelDelta=1;}
            else {brush.kind=TerrainBrushKind::Texture;brush.groundTexture="Tg02";brush.variation=0;}
            const TerrainStrokePoint point{256,512}; Take(f.edits->EditTerrain(brush,std::span(&point,1),false));
            auto terrain=f.original;
            if (cliff) terrain.vertices[38].cliffAndLayerRaw=3; else terrain.vertices[38].textureAndFlagsRaw=1;
            expectedW3e=Take(SerializeMapTerrain(terrain)); expectedWpm=f.Member("war3map.wpm");
            for (unsigned y=cliff?12:14;y<(cliff?20u:18u);++y) for(unsigned x=cliff?4:6;x<(cliff?12u:10u);++x)
                expectedWpm[16+y*32+x]=cliff?std::byte{0xdb}:std::byte{0x53};
        }
        const auto owner=Take(TerrainPathingSave::CaptureNative(*f.archive,f.original,*f.edits->Terrain(),{},kBudget));
        Take(TerrainPathingSave::CaptureNative(*f.archive,f.original,*f.edits->Terrain(),{},owner.CaptureBudgetBytes()));
        Require(!TerrainPathingSave::CaptureNative(*f.archive,f.original,*f.edits->Terrain(),{},owner.CaptureBudgetBytes()-1).Succeeded(),
            "pattern capture retains the exact byte budget");
        const auto plan=f.Plan(); auto output=f.Output(plan);
        output.push_back({"Doodads/Doodads.slk",Slk({"doodID","code","pathTex","category","fixedRot"},
            {{"Doo0","Doo0",std::string(kUniformDoodadTexture),"E","90"}})});
        Require(!plan.VerifyForSave(*f.edits,f.archive->SourceSha256(),output).Succeeded(),"fixedRot source change invalidates the frozen save");
        f.SaveExpected("patterned.w3x",expectedW3e,expectedWpm);
        Require(f.edits->Undo(),"undo saved terrain with patterned footprint");
        f.SaveExpected("patterned-undone.w3x",f.Member("war3map.w3e"),f.Member("war3map.wpm"));
        Require(f.edits->Redo(),"redo saved terrain with patterned footprint");
        f.SaveExpected("patterned-redone.w3x",expectedW3e,expectedWpm);
    }
}

void SpecialDoodad(Fixture& f, bool dry = false) {
    // 原创特殊地形：每行黑、蓝、绿、红；alpha全0，地格(2,2)仅覆盖一个地格。
    DoodadPlacements placements; placements.specialDoodads.push_back({"Doo0", 0, {2,2}});
    f.Member("war3map.doo") = Take(placements.Serialize());
    f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), "C", "45"}});
    Bytes texture(18); texture[2] = std::byte{2}; texture[12] = texture[14] = std::byte{4};
    texture[16] = std::byte{32}; texture[17] = std::byte{40};
    for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
        for (const unsigned value : {x == 1 ? 255u : 0u, x == 2 ? 255u : 0u, x == 3 ? 255u : 0u, 0u})
            texture.push_back(static_cast<std::byte>(value));
    f.members.push_back({std::string(kUniformDoodadTexture), std::move(texture)});
    auto& config = f.Member("UI/MiscData.txt"); const auto extra = Text("[Terrain]\nMaxHeight=1536\n");
    config.insert(config.end(), extra.begin(), extra.end());
    // 严格可证明的缺失来源来自自己的最小归档，不把未知本机回退当作缺失。
    f.gameRoot = f.root / "game"; Require(std::filesystem::create_directory(f.gameRoot), "private special game fallback");
    const std::array<MapArchiveMember, 1> gameMembers{{{"synthetic-marker.txt", Text("original fixture")}}};
    Take(WriteVerifiedArchive(f.gameRoot / "war3.mpq", gameMembers));
    if (dry) {
        for (auto& vertex : f.original.vertices) vertex.textureAndFlagsRaw = 0;
        RefreshTerrainDerived(f.original, 0, 0, 9, 9);
        f.Member("war3map.w3e") = Take(SerializeMapTerrain(f.original));
        std::fill(f.Member("war3map.wpm").begin()+16, f.Member("war3map.wpm").end(), std::byte{0x51});
    }
    constexpr std::array<unsigned, 4> shallow{0x19,0x19,0x1d,0xdb}, dryMask{0x11,0x19,0x15,0xd3};
    for (unsigned y = 8; y < 12; ++y) for (unsigned x = 8; x < 12; ++x)
        f.Member("war3map.wpm")[16+y*32+x] = static_cast<std::byte>((dry ? dryMask : shallow)[x-8]);
}

void SpecialDoodadsSaveWaterGroundAndCliffs() {
    for (const unsigned operation : {0u,1u,2u}) {
        Fixture f; SpecialDoodad(f, operation != 0);
        f.original.header.groundTilesets.push_back("Tg02");
        f.Member("war3map.w3e") = Take(SerializeMapTerrain(f.original));
        f.Member("TerrainArt/Terrain.slk") = Slk({"tileID","walkable","flyable","buildable"},
            {{"Tg01","1","1","1"},{"Tg02","0","1","1"}});
        f.Open();
        auto terrain = f.original; auto pathing = f.Member("war3map.wpm");
        if (operation == 0) {
            f.Paint();
            for (auto& vertex : terrain.vertices) vertex.waterHeightAndFlagsRaw = 8192 + 80 * 4;
            std::fill(pathing.begin()+16, pathing.end(), std::byte{0x1b});
            constexpr std::array<unsigned,4> deep{0x1b,0x1b,0x1f,0xdb};
            for (unsigned y=8; y<12; ++y) for (unsigned x=8; x<12; ++x) pathing[16+y*32+x] = static_cast<std::byte>(deep[x-8]);
        } else {
            const bool cliff = operation == 2;
            TerrainBrush brush; brush.radius = 0;
            if (cliff) { brush.kind = TerrainBrushKind::CliffLevel; brush.levelDelta = 1; }
            else { brush.kind = TerrainBrushKind::Texture; brush.groundTexture = "Tg02"; brush.variation = 0; }
            const TerrainStrokePoint point{768,768}; Take(f.edits->EditTerrain(brush, std::span(&point,1), false));
            if (cliff) terrain.vertices[60].cliffAndLayerRaw = 3; else terrain.vertices[60].textureAndFlagsRaw = 1;
            for (unsigned y=cliff?20:22; y<(cliff?28u:26u); ++y) for (unsigned x=cliff?20:22; x<(cliff?28u:26u); ++x)
                pathing[16+y*32+x] = cliff ? std::byte{0xdb} : std::byte{0x53};
        }
        const auto expected = Take(SerializeMapTerrain(terrain));
        const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, kBudget));
        Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, owner.CaptureBudgetBytes()));
        Require(!TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, owner.CaptureBudgetBytes()-1).Succeeded(),
            "special profile, texture, decoded layer and inputs share an exact bounded capture");
        const auto plan = f.Plan(); const auto output = f.Output(plan);
        auto stale = output; stale.push_back({"Units/MiscData.txt", Text("[Terrain]\nMaxHeight=1536\n")});
        Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), stale).Succeeded(),
            "previously absent special profile cannot be inserted into frozen output");
        auto changedTexture = f.Member(kUniformDoodadTexture); changedTexture[18] = std::byte{255};
        const MemberReplacement changed{std::string(kUniformDoodadTexture), changedTexture};
        auto tampered = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "special-tampered.w3x", std::span(&changed,1))).path));
        Require(!owner.VerifyReadback(tampered, kBudget).Succeeded(), "readback binds actual special TGA bytes");
        f.SaveExpected("special-edited.w3x", expected, pathing);
        Require(f.edits->Undo(), "undo terrain with preserved special models");
        f.SaveExpected("special-undone.w3x", f.Member("war3map.w3e"), f.Member("war3map.wpm"));
        Require(f.edits->Redo(), "redo terrain with preserved special models");
        f.SaveExpected("special-redone.w3x", expected, pathing);
    }
    Fixture bad; SpecialDoodad(bad, true); bad.Open();
    TerrainBrush cliff; cliff.kind = TerrainBrushKind::CliffLevel; cliff.radius = 0; cliff.levelDelta = 1;
    const TerrainStrokePoint point{256,256}; Take(bad.edits->EditTerrain(cliff, std::span(&point,1), false));
    const auto originalSha = bad.archive->SourceSha256();
    const auto refusal = bad.Plan().SaveGate();
    Require(!refusal.Succeeded() && HasCode(refusal, "TERRAIN_SPECIAL_CLIFF_CHANGED"),
        "cliff edit that would delete the special model is refused explicitly");
    Require(bad.edits->IsDirty() && Take(Sha256File(bad.archive->SourcePath())) == originalSha,
        "special-model refusal preserves the source and unsaved terrain");
}

void SharedSpecialAndOrdinaryResourcesRoundTripOnce() {
    Fixture f; SpecialDoodad(f);
    auto& texture = f.Member(kUniformDoodadTexture);
    std::fill(texture.begin() + 18, texture.end(), std::byte{0});
    std::fill(f.Member("war3map.wpm").begin() + 16, f.Member("war3map.wpm").end(), std::byte{0x19});
    f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), "C", "45"},
         {"Ord0", "Ord0", std::string(kUniformDoodadTexture), "E", "-1"}});
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    DoodadPlacement ordinary; ordinary.rawcode = "Ord0"; ordinary.position = {512, 512, 0};
    ordinary.scale = {1, 1, 1}; ordinary.flags = 2; ordinary.creationId = 1;
    placements.doodads.push_back(ordinary);
    f.Member("war3map.doo") = Take(placements.Serialize());
    f.Open(); f.Paint();
    // 完整独立字节预期：黑色占地图不阻挡，所有地格从浅水08变为深水0a，保护11保留。
    // 普通/特殊对象共用贴图，水规则/MaxHeight共用配置，最终成员回读不能重复请求同名来源。
    f.Save("shared-dependencies-deep.w3x", true);
    Require(f.edits->Undo(), "undo saved water with shared ordinary/special dependencies");
    f.Save("shared-dependencies-undone.w3x", false);
    Require(f.edits->Redo(), "redo saved water with shared ordinary/special dependencies");
    f.Save("shared-dependencies-redone.w3x", true);
}
} // namespace

int main() {
    try {
        StandardDoodadsUseTheNormalSaveAndUndoChain();
        StandardDoodadsAllowActualCliffAndGroundPathingChanges();
        DoodadOriginalUsesTheNormalSaveAndUndoChain();
        DoodadMetadataIsDemandLoadedAndWinnerBound();
        TypedOverrideUsesTheNormalSaveAndUndoChain();
        MissingW3bStillUsesStrictClassificationAndTheSaveChain();
        DependencyReplacementRemovalAndReadbackStayBound();
        NativeObjectsDoNotWeakenSourceCasOrOldPathingChecks();
        MapAndGameMetadataWinnersRemainBound();
        DirectoryRecipePreservesReplayAndSourceChecks();
        UniformDoodadsSaveWaterGroundAndCliffs();
        NativeDoodadFootprintsUseTheNormalSaveAndUndoChain();
        UniformDoodadTextureIsFrozenAndBudgeted();
        UniformDoodadGameTextureWinnerRemainsBound();
        UniformDoodadLongestPathHasAnExactCaptureBudget();
        UniformDoodadDuplicateInstancesHaveABoundedPixelWorkload();
        UniformDoodadRejectionDoesNotWeakenOldSaves();
        PatternedDoodadsUseTheNormalSaveAndUndoChain();
        SpecialDoodadsSaveWaterGroundAndCliffs();
        SharedSpecialAndOrdinaryResourcesRoundTripOnce();
        std::cout << "Native object terrain save tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
