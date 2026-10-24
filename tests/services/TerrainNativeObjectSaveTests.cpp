#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainUniformDoodadFootprint.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/TerrainEditService.h"
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
Bytes ConflictingMapMisc() {
    // 原创地图独立Misc；四项均与startup规则冲突，保留CRLF和未知字段以核对整成员字节。
    return Text("// original independent map Misc\r\n[Terrain]\r\nMaxSlope=1\r\nMinHeight=-1024\r\nMaxHeight=-128\r\n"
        "[Water]\r\nDeepLevelPathing=4096\r\n[Fixture]\r\nMarker=preserve-map-misc-bytes\r\n");
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
    void SaveExpected(const char* name, const Bytes& terrain, const Bytes& pathing, const Bytes* doodads = nullptr,
        bool formal = false, bool expectRebuilt = true) {
        const auto sourcePath = archive->SourcePath();
        const auto sourceSha = Take(Sha256File(sourcePath));
        const auto identity = edits->SessionIdentity(), revision = edits->Revision(), sourceRevision = edits->TerrainSourceRevision();
        const auto plan = Plan(); Check(plan.SaveGate());
        if (plan.RebuiltPathing() != expectRebuilt)
            throw std::runtime_error(std::string("native pathing rebuild expectation: ") + name);
        if (!expectRebuilt) {
            const auto& member = plan.Members()[1];
            Require(member.name == "war3map.wpm" && member.action == TerrainSaveMemberAction::Preserve
                && member.sourceSha256.has_value() && member.sourceSha256 == member.outputSha256,
                "elevation-only save preserves the source pathing member identity");
            for (const auto& replacement : plan.Replacements())
                Require(replacement.name != "war3map.wpm", "elevation-only plan has no pathing replacement");
        }
        const auto output = Output(plan);
        auto written = root / name;
        if (formal) {
            Services::MapSaveTask task; Check(task.FreezeArchive(*archive));
            task.target = written; task.replacements = output;
            task.beforePublish = [&] { return plan.VerifyForSave(*edits, task.sourceSha256, task.replacements, task.removals); };
            Services::MapSaveService::Run(task);
            if (!task.succeeded) for (const auto& diagnostic : task.diagnostics)
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            Require(task.executed && task.succeeded && task.nextSession, "formal save worker publishes and reopens the complete expected state");
            written = task.written;
        } else {
            written = Take(archive->SaveCandidate(written, output)).path;
        }
        auto saved = Take(MapArchiveSession::Open(written));
        Check(plan.VerifyReadback(saved));
        Require(Take(saved.ReadMember("war3map.w3e")) == terrain, "full independent W3E expected bytes");
        Require(Take(saved.ReadMember("war3map.wpm")) == pathing, "full independent WPM expected bytes");
        for (const auto& member : members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm") {
            const auto& expected = doodads && member.name == "war3map.doo" ? *doodads : member.data;
            Require(Take(saved.ReadMember(member.name)) == expected, "all expected DOO and unrelated members preserved exactly");
        }
        Require(Take(Sha256File(sourcePath)) == sourceSha, "save does not rewrite source archive");
        Check(plan.CommitSaved(*edits, MapDocument::FromArchive(saved)));
        Require(!edits->IsDirty() && edits->Replacements().empty() && edits->SessionIdentity() == identity
            && edits->Revision() == revision && edits->TerrainSourceRevision() == sourceRevision + 1,
            "verified save rebases the same clean session without adding commands or erasing history");
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

void CGridExpectedFootprint(Bytes& wpm, bool duplicate = false) {
    // 8x8绿色alpha0：内部02编码为文件04；中心512的两格投影均为[12,20)。
    for (std::size_t y = 12; y < 20; ++y) for (std::size_t x = 12; x < 20; ++x) wpm[16 + y * 32 + x] |= std::byte{4};
    if (duplicate) for (std::size_t y = 4; y < 12; ++y) for (std::size_t x = 4; x < 12; ++x) wpm[16 + y * 32 + x] |= std::byte{4};
}
void CGridDoodad(Fixture& fixture, bool duplicate = false) {
    StandardDoodad(fixture);
    fixture.gameRoot = fixture.root / "game";
    Require(std::filesystem::create_directory(fixture.gameRoot), "private actual startup source root");
    Take(WriteVerifiedArchive(fixture.gameRoot / "war3.mpq", std::vector<MapArchiveMember>{
        {"UI/WorldEditData.txt", Text("[DoodadCategories]\nC=Custom\n")}}));
    fixture.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), "C", "270"}});
    Bytes texture(18 + 8 * 8 * 4); texture[2] = std::byte{2};
    texture[12] = texture[14] = std::byte{8}; texture[16] = std::byte{32}; texture[17] = std::byte{8};
    for (std::size_t at = 18; at < texture.size(); at += 4) texture[at + 1] = std::byte{255};
    fixture.members.push_back({std::string(kUniformDoodadTexture), std::move(texture)});
    if (duplicate) {
        auto placements = Take(ParseDoodadPlacements(fixture.Member("war3map.doo")));
        auto second = placements.doodads.front(); second.position = {256, 256, 0}; second.creationId = 3;
        placements.doodads.push_back(second); fixture.Member("war3map.doo") = Take(placements.Serialize());
    }
    CGridExpectedFootprint(fixture.Member("war3map.wpm"), duplicate);
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

void StartupCategoryCUsesTheNormalSaveAndUndoChain() {
    Fixture water; CGridDoodad(water, true); water.Open(); water.Paint();
    auto deep = water.ExpectedPathing(true), shallow = water.ExpectedPathing(false);
    CGridExpectedFootprint(deep, true); CGridExpectedFootprint(shallow, true);
    water.SaveExpected("grid-c-water.w3x", water.ExpectedTerrain(true), deep);
    Require(water.edits->Undo(), "undo saved water with two actual C instances");
    water.SaveExpected("grid-c-water-undone.w3x", water.ExpectedTerrain(false), shallow);
    Require(water.edits->Redo(), "redo saved water with two actual C instances");
    water.SaveExpected("grid-c-water-redone.w3x", water.ExpectedTerrain(true), deep);
    for (const bool cliff : {false, true}) {
        Fixture f; CGridDoodad(f);
        f.original.header.groundTilesets.push_back("Tg02");
        for (auto& vertex : f.original.vertices) vertex.textureAndFlagsRaw = 0;
        RefreshTerrainDerived(f.original, 0, 0, 9, 9);
        f.Member("war3map.w3e") = Take(SerializeMapTerrain(f.original));
        std::fill(f.Member("war3map.wpm").begin() + 16, f.Member("war3map.wpm").end(), std::byte{0x51});
        CGridExpectedFootprint(f.Member("war3map.wpm"));
        f.Member("TerrainArt/Terrain.slk") = Slk({"tileID", "walkable", "flyable", "buildable"},
            {{"Tg01", "1", "1", "1"}, {"Tg02", "0", "1", "1"}});
        f.Open(); TerrainBrush brush; brush.radius = 0;
        if (cliff) { brush.kind = TerrainBrushKind::CliffLevel; brush.levelDelta = 1; }
        else { brush.kind = TerrainBrushKind::Texture; brush.groundTexture = "Tg02"; brush.variation = 0; }
        const TerrainStrokePoint point{256, 512}; Take(f.edits->EditTerrain(brush, std::span(&point, 1), false));
        auto terrain = f.original;
        if (cliff) terrain.vertices[38].cliffAndLayerRaw = 3; else terrain.vertices[38].textureAndFlagsRaw = 1;
        auto pathing = f.Member("war3map.wpm");
        const std::size_t left = cliff ? 4 : 6, right = cliff ? 12 : 10, low = cliff ? 12 : 14, high = cliff ? 20 : 18;
        for (auto y = low; y < high; ++y) for (auto x = left; x < right; ++x)
            pathing[16 + y * 32 + x] = cliff ? std::byte{0xdb} : std::byte{0x53};
        const auto expected = Take(SerializeMapTerrain(terrain));
        f.SaveExpected(cliff ? "grid-c-cliff.w3x" : "grid-c-texture.w3x", expected, pathing);
        Require(f.edits->Undo(), "undo saved dry terrain while retaining C contributions");
        f.SaveExpected("grid-c-dry-undone.w3x", f.Member("war3map.w3e"), f.Member("war3map.wpm"));
        Require(f.edits->Redo(), "redo saved dry terrain while retaining C contributions");
        f.SaveExpected("grid-c-dry-redone.w3x", expected, pathing);
    }
}

void StartupCategoryCBudgetSourcesAndInheritanceStayBound() {
    Fixture f; CGridDoodad(f, true); f.Open(); f.Paint();
    const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, kBudget));
    Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, owner.CaptureBudgetBytes()));
    const auto shortBudget = TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, owner.CaptureBudgetBytes() - 1);
    Require(!shortBudget.Succeeded() && !shortBudget.value, "C source snapshot and projection share the exact cumulative budget");
    const auto plan = f.Plan(); const auto output = f.Output(plan);
    auto changed = f.Member(kUniformDoodadTexture);
    for (std::size_t at = 21; at < changed.size(); at += 4) changed[at] = std::byte{255};
    auto altered = output; altered.push_back({std::string(kUniformDoodadTexture), changed});
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), altered).Succeeded(),
        "even unchanged RGB with changed uniform alpha remains an actual source replacement");
    auto tampered = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "grid-c-tampered.w3x", altered)).path));
    Require(!plan.VerifyReadback(tampered).Succeeded(), "C TGA provenance remains bound on independent archive readback");
    f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), "E", "270"}});
    f.Open(); f.Paint();
    const auto nonC = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, kBudget));
    Require(owner.CaptureBudgetBytes() > nonC.CaptureBudgetBytes(),
        "startup category sources and parser work are charged in addition to ordinary footprint work");
    for (const bool custom : {false, true}) {
        Fixture inherited; CGridDoodad(inherited);
        if (custom) {
            auto placements = Take(ParseDoodadPlacements(inherited.Member("war3map.doo")));
            placements.doodads.front().rawcode = "NewD"; inherited.Member("war3map.doo") = Take(placements.Serialize());
        }
        inherited.members.push_back({"war3map.w3d", DoodadOverride(kUniformDoodadTexture.data(), "dpt0", custom)});
        inherited.members.push_back({"Doodads/DoodadMetaData.slk", DoodadMetadata()});
        inherited.Open(); inherited.Paint();
        auto pathing = inherited.ExpectedPathing(true); CGridExpectedFootprint(pathing);
        inherited.SaveExpected("grid-c-inherited.w3x", inherited.ExpectedTerrain(true), pathing);
    }
}

void StartupCategoryCChoosesTheActualGridAndFreezesItsSources() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture f; CGridDoodad(f);
        std::filesystem::remove(f.gameRoot / "war3.mpq");
        const bool four = mode == 0;
        Take(WriteVerifiedArchive(f.gameRoot / "war3.mpq", std::vector<MapArchiveMember>{
            {"UI/WorldEditData.txt", Text(mode == 0 ? "[DoodadCategories]\nC=Custom\n"
                : mode == 1 ? "[DoodadCategories]\nc=Custom\n" : "[Other]\nC=ignored\n")}}));
        if (mode == 3) std::filesystem::remove(f.gameRoot / "war3.mpq");
        // 地图中故意存在大写C；它不是启动目录的输入。
        f.Member("UI/WorldEditData.txt") = Text("[DoodadCategories]\nC=map-must-not-qualify\n");
        auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
        placements.doodads.front().position = {544, 544, 0};
        f.Member("war3map.doo") = Take(placements.Serialize());
        const auto expected = [&](bool deep) {
            auto result = f.ExpectedPathing(deep);
            // 8x8 at544：4格中心512→[12,20)，2格中心576→[14,22)。
            const unsigned low = four ? 12 : 14;
            for (unsigned y = low; y < low + 8; ++y) for (unsigned x = low; x < low + 8; ++x)
                result[16 + y * 32 + x] |= std::byte{4};
            return result;
        };
        f.Member("war3map.wpm") = expected(false);
        f.Open(); f.Paint();
        const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, kBudget));
        const auto output = f.Output(f.Plan());
        const auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "startup-c-candidate.w3x", output)).path));
        Check(owner.VerifyReadback(saved, kBudget));
        const auto directory = Take(DirectoryWorkspace::Open(f.Directory()));
        const auto directoryOwner = Take(TerrainPathingSave::CaptureNative(directory.Snapshot(), f.original,
            *f.edits->Terrain(), f.gameRoot, kBudget));
        Check(directoryOwner.VerifyFrozenOutput(output, {}));
        auto expectedNative = expected(true);
        auto encoded = Take(owner.GenerateNative(*f.edits->Terrain(), kBudget));
        Require(encoded.size() == 32 * 32, "complete native selector grid");
        // 原生生成输出不携作者11位；逐格比较其实际selector结果。
        for (std::size_t i = 0; i < encoded.size(); ++i)
            Require((std::to_integer<unsigned char>(encoded[i]) & 0xee)
                == (std::to_integer<unsigned char>(expectedNative[16 + i]) & 0xee), "actual startup source selects the independent full native grid");
        f.SaveExpected("startup-c-saved.w3x", f.ExpectedTerrain(true), expected(true));
        Require(f.edits->Undo(), "undo saved C terrain with an actual startup source");
        f.SaveExpected("startup-c-undone.w3x", f.ExpectedTerrain(false), expected(false));
        Require(f.edits->Redo(), "redo saved C terrain with an actual startup source");
        f.SaveExpected("startup-c-redone.w3x", f.ExpectedTerrain(true), expected(true));

        const auto changed = Text("[DoodadCategories]\nC=same-qualification-different-source\n");
        Check(IO::WriteFileBytes(f.gameRoot / "WorldEditData.txt", changed));
        Require(!owner.VerifyFrozenOutput(output, {}).Succeeded() && !owner.VerifyReadback(saved, kBudget).Succeeded()
            && !directoryOwner.VerifyFrozenOutput(output, {}).Succeeded(), "new higher loose source invalidates both archive and directory owners before/after save");
        Require(f.edits->Undo(), "source-CAS refusal leaves saved undo history available");
    }
    Fixture unknown; CGridDoodad(unknown); unknown.gameRoot.clear(); unknown.Open(); unknown.Paint();
    const auto missing = unknown.Plan().SaveGate();
    Require(!missing.Succeeded() && HasCode(missing, "TERRAIN_CATEGORY_SOURCE_REQUIRED"), "unknown startup input cannot fall back to two cells");
    Fixture unexplained; CGridDoodad(unexplained); unexplained.Member("war3map.wpm")[16] ^= std::byte{4};
    unexplained.Open(); unexplained.Paint();
    Require(HasCode(unexplained.Plan().SaveGate(), "TERRAIN_PATHING_SOURCE_UNEXPLAINED"), "source directory does not weaken original full WPM explanation");
}

Bytes CategoryOverride(std::string_view parent, std::string_view placed, const char* value) {
    auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
    if (parent == placed) Check(document.AddOriginalObject(std::string(placed)));
    else Check(document.AddCustomObject(std::string(placed), std::string(parent)));
    if (value) Check(document.AddModification({ObjectType::Doodad, std::string(placed)}, {"Cat9"},
        NativeValueType::String, std::string(value)));
    return Take(NativeObjectCodec(ObjectType::Doodad).Serialize(document));
}

Bytes CategoryExpectedPathing(Fixture& f, bool deep, bool four) {
    auto expected = f.ExpectedPathing(deep);
    // 独立预期：8x8绿色贴图放在544；C4为[12,20)，两格为[14,22)，仅OR文件04。
    const unsigned low = four ? 12 : 14;
    for (unsigned y = low; y < low + 8; ++y) for (unsigned x = low; x < low + 8; ++x)
        expected[16 + y * 32 + x] |= std::byte{4};
    return expected;
}

void EffectiveDoodadCategoriesUseTheNormalSaveAndUndoChain() {
    struct Case { const char* placed; const char* standard; const char* value; bool four; };
    unsigned sequence = 0;
    for (const auto& sample : std::vector<Case>{
        {"Doo0", "E", "C", true}, {"Doo0", "C", "E", false},
        {"XE01", "E", "C", true}, {"XC01", "C", "E", false},
        {"XE01", "C", nullptr, true}, {"XC01", "E", nullptr, false},
        {"XC01", "E", "", true}, {"XE01", "C", "_Csuffix", false}}) {
        Fixture f; CGridDoodad(f);
        auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
        placements.doodads.front().rawcode = sample.placed; placements.doodads.front().position = {544, 544, 0};
        f.Member("war3map.doo") = Take(placements.Serialize());
        f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
            {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), sample.standard, "270"}});
        f.members.push_back({"war3map.w3d", CategoryOverride("Doo0", sample.placed, sample.value)});
        f.members.push_back({"Doodads/DoodadMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "data", "index"},
            {{"dpt0", "pathTex", "DoodadData", "pathingTexture", "0", "0", "0"},
             {"Cat9", "category", "DoodadData", "doodadCategory", "0", "0", "0"}})});
        f.Member("war3map.wpm") = CategoryExpectedPathing(f, false, sample.four);
        f.Open(); f.Paint();
        const auto plan = f.Plan(); const auto output = f.Output(plan);
        const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, kBudget));
        auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "category-readback.w3x", output)).path));
        Check(owner.VerifyReadback(saved, kBudget));
        Require(Take(saved.ReadMember("war3map.wpm")) == CategoryExpectedPathing(f, true, sample.four),
            "effective category选择整张独立C4或非C2预期，Original/Custom覆盖与继承走相同正式保存链");
        f.SaveExpected("category-saved.w3x", f.ExpectedTerrain(true), CategoryExpectedPathing(f, true, sample.four));
        if (sequence == 0 || sequence == 6) {
            Require(f.edits->Undo(), "category覆盖保存后仍可撤销地形");
            f.SaveExpected("category-undone.w3x", f.ExpectedTerrain(false), CategoryExpectedPathing(f, false, sample.four));
            Require(f.edits->Redo(), "category覆盖保存后仍可重做地形");
            f.SaveExpected("category-redone.w3x", f.ExpectedTerrain(true), CategoryExpectedPathing(f, true, sample.four));
        }
        if (sequence == 2) {
            unsigned tamperedSequence = 0;
            for (const auto* name : {"war3map.w3d", "Doodads/DoodadMetaData.slk"}) {
                auto changed = name == std::string_view("war3map.w3d")
                    ? CategoryOverride("Doo0", sample.placed, "C-same-first-byte") : f.Member(name);
                if (name != std::string_view("war3map.w3d")) changed.push_back(std::byte{'\n'});
                auto altered = output; altered.push_back({name, changed});
                Require(!owner.VerifyFrozenOutput(altered, {}).Succeeded(), "同一类别结果不能绕过W3D/metadata字节冻结");
                const std::array<MemberReplacement, 1> replacement{{{name, changed}}};
                auto tampered = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(
                    f.root / ("category-tampered-" + std::to_string(tamperedSequence++) + ".w3x"), replacement)).path));
                Require(!owner.VerifyReadback(tampered, kBudget).Succeeded(), "独立回读拒绝同类别的原生/metadata来源变更");
            }
            Check(IO::WriteFileBytes(f.gameRoot / "WorldEditData.txt", Text("[DoodadCategories]\nC=same-category-new-winner\n")));
            Require(!owner.VerifyFrozenOutput(output, {}).Succeeded(), "native有效C仍绑定实际startup winner，而非最终C结果");
        }
        ++sequence;
    }
    Fixture unknown; CGridDoodad(unknown);
    unknown.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category"},
        {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), "E"}});
    unknown.members.push_back({"war3map.w3d", CategoryOverride("Doo0", "Doo0", "C")});
    unknown.members.push_back({"Doodads/DoodadMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "index"},
        {{"dpt0", "pathTex", "DoodadData", "pathingTexture", "0", "0"},
         {"Cat9", "category", "DoodadData", "doodadCategory", "0", "0"}})});
    unknown.gameRoot.clear(); unknown.Open(); unknown.Paint();
    Require(HasCode(unknown.Plan().SaveGate(), "TERRAIN_CATEGORY_SOURCE_REQUIRED"), "native有效C不能借标准E绕过未知startup来源");
    unknown.gameRoot = unknown.root / "game";
    unknown.Member("war3map.wpm")[16] ^= std::byte{4}; unknown.Open(); unknown.Paint();
    Require(HasCode(unknown.Plan().SaveGate(), "TERRAIN_PATHING_SOURCE_UNEXPLAINED"), "category覆盖不放宽完整旧WPM解释门");
}

void StartupCategoryCPatternedRectangleUsesTheNormalSaveChain() {
    Fixture f; CGridDoodad(f);
    Bytes texture(18 + 8 * 4 * 3); texture[2] = std::byte{2}; texture[12] = std::byte{8};
    texture[14] = std::byte{4}; texture[16] = std::byte{24}; texture[17] = std::byte{32};
    for (std::size_t at = 18; at < texture.size(); at += 3) texture[at + 1] = std::byte{255};
    texture[19] = std::byte{0}; texture[20] = std::byte{255}; // 原图左上红，其余绿。
    f.Member(kUniformDoodadTexture) = std::move(texture);
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    placements.doodads.front().position = {544,544,0}; placements.doodads.front().rotation = 4.712389f;
    f.Member("war3map.doo") = Take(placements.Serialize());
    const auto expected = [&](bool deep) {
        auto result = f.ExpectedPathing(deep);
        // 4格：宽8为偶，X中心512；高4为奇，Y中心576。270度使原图左上到左下。
        // 红像素内部属性为0x31，文件六位编码为0xC2，不能只保留最低红位0x02。
        for (unsigned y = 16; y < 20; ++y) for (unsigned x = 12; x < 20; ++x)
            result[16 + y * 32 + x] |= x == 12 && y == 19 ? std::byte{0xc2} : std::byte{4};
        return result;
    };
    f.Member("war3map.wpm") = expected(false); f.Open(); f.Paint();
    f.SaveExpected("startup-c-pattern.w3x", f.ExpectedTerrain(true), expected(true));
    Require(f.edits->Undo(), "undo saved C patterned rectangle");
    f.SaveExpected("startup-c-pattern-undone.w3x", f.ExpectedTerrain(false), expected(false));
    Require(f.edits->Redo(), "redo saved C patterned rectangle");
    f.SaveExpected("startup-c-pattern-redone.w3x", f.ExpectedTerrain(true), expected(true));
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
        constexpr std::array<std::uint8_t, 18> pixels{0,0,0, 1,0,0, 0,1,0, 0,0,1, 1,1,0, 1,0,1};
        for (const auto b : pixels) texture.push_back(static_cast<std::byte>(b));
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

void NativeFixedRotationUsesTheNormalSaveAndUndoChain() {
    const auto addExpected = [](Bytes& wpm, float rotation) {
        // 原创3x2 top-origin源(黑蓝绿/红青紫)，wire角0始终以2x3吸附尺寸定中心(544,544)。
        // fixedRot=90的最终3x2为(绿蓝黑/紫青红)，起点(15,16)；0/负值为2x3，起点(16,15)。
        constexpr std::array<unsigned, 6> at90{4, 8, 0, 0xca, 0x0c, 0xc2};
        constexpr std::array<unsigned, 6> at0{0, 0xc2, 8, 0x0c, 4, 0xca};
        const bool turned = rotation == 90;
        for (unsigned i = 0; i < 6; ++i) {
            const unsigned width = turned ? 3u : 2u, x = turned ? 15u : 16u, y = turned ? 16u : 15u;
            wpm[16 + (y + i / width) * 32 + x + i % width] |= static_cast<std::byte>((turned ? at90 : at0)[i]);
        }
    };
    const auto configure = [&](Fixture& fixture, bool custom, float rotation) {
        StandardDoodad(fixture);
        fixture.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
            {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), "E", rotation == 90 ? "0" : "90"}});
        Bytes texture(18); texture[2] = std::byte{2}; texture[12] = std::byte{3}; texture[14] = std::byte{2};
        texture[16] = std::byte{24}; texture[17] = std::byte{32};
        constexpr std::array<std::uint8_t, 18> pixels{0,0,0, 1,0,0, 0,1,0, 0,0,1, 1,1,0, 1,0,1};
        for (const auto value : pixels) texture.push_back(static_cast<std::byte>(value));
        fixture.members.push_back({std::string(kUniformDoodadTexture), std::move(texture)});
        auto placements = Take(ParseDoodadPlacements(fixture.Member("war3map.doo")));
        placements.doodads.front().rotation = 0;
        if (custom) placements.doodads.front().rawcode = "NewD";
        fixture.Member("war3map.doo") = Take(placements.Serialize());
        auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
        if (custom) Check(document.AddCustomObject("NewD", "Doo0")); else Check(document.AddOriginalObject("Doo0"));
        Check(document.AddModification({ObjectType::Doodad, custom ? "NewD" : "Doo0"}, {"Rot0"}, NativeValueType::Unreal, rotation));
        fixture.members.push_back({"war3map.w3d", Take(NativeObjectCodec(ObjectType::Doodad).Serialize(document))});
        fixture.members.push_back({"Doodads/DoodadMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "index"},
            {{"dpt0", "pathTex", "DoodadData", "pathingTexture", "0", "0"}, {"Rot0", "fixedRot", "DoodadData", "unreal", "0", "0"}})});
        addExpected(fixture.Member("war3map.wpm"), rotation);
    };
    for (const bool custom : {false, true}) for (const float rotation : {-1.0f, 0.0f, 90.0f}) {
        Fixture f; configure(f, custom, rotation); f.Open(); f.Paint();
        const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget));
        Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, owner.CaptureBudgetBytes()));
        Require(!TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, owner.CaptureBudgetBytes() - 1).Succeeded(),
            "native F32 output and its actual metadata retain the exact cumulative capture budget");
        const auto plan = f.Plan(); const auto output = f.Output(plan);
        const auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "rotation-frozen.w3x", output)).path));
        for (const char* name : {"war3map.w3d", "Doodads/DoodadMetaData.slk"}) {
            auto changed = f.Member(name);
            if (std::string_view(name) == "war3map.w3d") {
                const auto parsed = NativeObjectCodec(ObjectType::Doodad).Parse(changed);
                const auto* document = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
                Require(document != nullptr, "frozen rotation fixture parses as native W3D");
                Require(document->Objects().size() == 1 && document->Objects().front().modifications.size() == 1,
                    "frozen fixture has one exact native modification address");
                const auto& object = document->Objects().front();
                auto replacement = *document;
                Check(replacement.SetModification(object.key, object.modifications.front().key,
                    rotation == 90 ? Value{0.0f} : Value{90.0f}));
                changed = Take(NativeObjectCodec(ObjectType::Doodad).Serialize(replacement));
            } else changed.push_back(std::byte{'\n'});
            auto altered = output; altered.push_back({name, changed});
            Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), altered).Succeeded(), "numeric rotation and metadata remain frozen before save");
            const std::array<MemberReplacement, 1> replacement{{{name, changed}}};
            const auto tampered = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root / (std::string(name) == "war3map.w3d"
                ? "rotation-tampered.w3x" : "rotation-metadata-tampered.w3x"), replacement)).path));
            Require(!plan.VerifyReadback(tampered).Succeeded(), "independent readback also rejects changed numeric rotation or metadata bytes");
        }
        auto deep = f.ExpectedPathing(true); addExpected(deep, rotation);
        f.SaveExpected("rotation-deep.w3x", f.ExpectedTerrain(true), deep);
        Require(f.edits->Undo(), "undo saved terrain with a native F32 rotation");
        f.SaveExpected("rotation-undone.w3x", f.Member("war3map.w3e"), f.Member("war3map.wpm"));
        Require(f.edits->Redo(), "redo saved terrain with a native F32 rotation");
        f.SaveExpected("rotation-redone.w3x", f.ExpectedTerrain(true), deep);
    }
    for (const float rotation : {-361.0f, 361.0f, 45.0f, 90.0f}) {
        Fixture f; configure(f, false, rotation);
        if (rotation == 90) f.Member("war3map.wpm")[16] ^= std::byte{4};
        f.Open(); f.Paint(); const auto sourceSha = f.archive->SourceSha256();
        const auto draft = Take(SerializeMapTerrain(*f.edits->Terrain()));
        const auto gate = f.Plan().SaveGate();
        const char* code = rotation == 90 ? "TERRAIN_PATHING_SOURCE_UNEXPLAINED"
            : rotation == 45 ? "TERRAIN_DOODAD_ANGLE_AMBIGUOUS" : "TERRAIN_DOODAD_FIXED_ROTATION";
        Require(!gate.Succeeded() && HasCode(gate, code), "unsupported native rotation or unexplained WPM must fail before save");
        Require(f.edits->IsDirty() && Take(Sha256File(f.archive->SourcePath())) == sourceSha
            && Take(SerializeMapTerrain(*f.edits->Terrain())) == draft, "rotation rejection preserves source and the complete unsaved draft");
    }
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

void SpecialWaterDeleteFixture(Fixture& f) {
    SpecialDoodad(f, true);
    f.Member("UI/MiscData.txt") = Text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n"
        "[Water]\nDeepLevelPathing=64\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n");
    f.members.push_back({"TerrainArt/CliffTypes.slk", Slk({"cliffID"}, {{"Cl01"}})});
    f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), "C", "45"},
         {"Doo1", "Doo1", std::string(kUniformDoodadTexture), "C", "45"},
         {"Doo2", "Doo2", std::string(kUniformDoodadTexture), "C", "45"},
         {"Ord0", "Ord0", "_", "E", "-1"}});
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    placements.specialDoodads.push_back({"Doo1", 0, {5, 5}});
    placements.specialDoodads.push_back({"Doo2", 0, {6, 5}});
    DoodadPlacement ordinary; ordinary.rawcode = "Ord0"; ordinary.position = {512, 512, 37};
    ordinary.scale = {1, 1.25f, 0.75f}; ordinary.rotation = 0.625f; ordinary.flags = 2;
    ordinary.lifePercent = 83; ordinary.creationId = 912;
    PlacementItemSet drops; drops.items = {{"gold", 70}, {"lmbr", 30}};
    ordinary.itemSets.push_back(std::move(drops)); placements.doodads.push_back(std::move(ordinary));
    f.Member("war3map.doo") = Take(placements.Serialize());
    // 两个保留项在远区，使用独立字面通行列；不能从待测special/native生成器制作旧WPM。
    constexpr std::array<std::uint8_t, 4> dry{0x11, 0x19, 0x15, 0xd3};
    for (unsigned y = 20; y < 24; ++y) for (unsigned x = 20; x < 28; ++x)
        f.Member("war3map.wpm")[16 + y * 32 + x] = static_cast<std::byte>(dry[(x - 20) % 4]);
    f.Open();
}

void SpecialWaterDeletionUsesTheNormalSaveAndUndoChain() {
    Fixture f; SpecialWaterDeleteFixture(f);
    const auto currentDoodads = [&] {
        const auto document = Take(f.edits->Snapshot(MapDataKind::Doodads, kBudget));
        Require(document.Doodads().has_value(), "bounded DOO snapshot parses the actual current ordinary and special segments");
        return *document.Doodads();
    };
    const auto originalTerrain = f.Member("war3map.w3e"), originalPathing = f.Member("war3map.wpm");
    const auto originalDoodads = f.Member("war3map.doo");
    const auto before = currentDoodads();
    auto expectedDoodads = before;
    expectedDoodads.specialDoodads = {before.specialDoodads[2], before.specialDoodads[1]};
    const auto deletedDoodads = Take(expectedDoodads.Serialize());
    const auto rules = Take(TerrainPathingSave::CaptureWaterBrushRules(*f.archive, f.original,
        *f.edits->Terrain(), before, f.gameRoot, true, kBudget));
    const Services::MapDataQueryBinding document{"special-water-save", 1, 0, f.archive->SourceSha256()};
    const Services::TerrainEditBinding binding{document, f.edits->SessionIdentity(), f.edits->Revision(), f.edits->TerrainSourceRevision()};
    TerrainWaterStrokeOptions options; options.shape = TerrainBrushShape::Square; options.randomSeed = 1;
    const TerrainStrokePoint point{256, 256};
    const auto compatibility = TerrainPathingResources::CaptureWaterBrushRules(*f.archive, f.original,
        *f.edits->Terrain(), f.gameRoot, true, kBudget);
    Require(!compatibility.Succeeded() || !compatibility.value
        || !Services::PrepareTerrainWaterEdit(*f.edits, document, binding, *compatibility.value,
            options, std::span(&point, 1), false).Succeeded(),
        "Core compatibility capture cannot qualify a real nonempty special set without the Services resource adapter");
    const auto prepared = Take(Services::PrepareTerrainWaterEdit(*f.edits, document, binding, rules,
        options, std::span(&point, 1), false));
    Require(f.edits->Revision() == 0 && !f.edits->CanUndo() && Take(currentDoodads().Serialize()) == originalDoodads,
        "resource capture and service preparation do not publish the special deletion");
    Take(Services::ApplyTerrainEdit(*f.edits, document, prepared));
    Require(f.edits->Revision() == 1 && f.edits->CanUndo() && Take(currentDoodads().Serialize()) == deletedDoodads,
        "one water command commits full ordinary bytes and special swap-last order [source2,source1]");
    Require(f.edits->Terrain()->vertices[2 * 9 + 2].CliffLevel() == 1
        && f.edits->Terrain()->vertices[2 * 9 + 2].HasWater(), "the deletion belongs to a real shallow-water cliff edit");
    const auto waterTerrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
    auto expectedPathing = originalPathing;
    // 四个地格围住低一层的湿顶点：(4..11)^2为悬崖db，最近中心(6..9)^2为深判定1b。
    // 原special0占地(8..11)^2被这些真实地形位替换；远处special1/2仍保留原四列。
    for (unsigned y = 4; y < 12; ++y) for (unsigned x = 4; x < 12; ++x)
        expectedPathing[16 + y * 32 + x] = std::byte{0xdb};
    for (unsigned y = 6; y < 10; ++y) for (unsigned x = 6; x < 10; ++x)
        expectedPathing[16 + y * 32 + x] = std::byte{0x1b};

    const auto frozen = f.Plan(); const auto output = f.Output(frozen);
    const auto doodadOutput = std::find_if(output.begin(), output.end(), [](const auto& member) { return member.name == "war3map.doo"; });
    Require(doodadOutput != output.end() && doodadOutput->data == deletedDoodads,
        "normal terrain output carries the complete coupled DOO replacement");
    auto substituted = output;
    const auto substitute = std::find_if(substituted.begin(), substituted.end(), [](const auto& member) { return member.name == "war3map.doo"; });
    substitute->data = originalDoodads;
    Require(!frozen.VerifyForSave(*f.edits, f.archive->SourceSha256(), substituted).Succeeded(),
        "final-output CAS rejects restoring the deleted special row after preparation");
    auto reordered = expectedDoodads;
    std::reverse(reordered.specialDoodads.begin(), reordered.specialDoodads.end());
    substitute->data = Take(reordered.Serialize());
    Require(!frozen.VerifyForSave(*f.edits, f.archive->SourceSha256(), substituted).Succeeded(),
        "final-output CAS binds surviving special order as well as the surviving row identities");
    auto readback = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "special-water-frozen.w3x", output)).path));
    Check(frozen.VerifyReadback(readback));
    Require(Take(readback.ReadMember("war3map.wpm")) == expectedPathing
        && Take(readback.ReadMember("war3map.doo")) == deletedDoodads,
        "independent readback proves old special G0 and remaining-special plus terrain G1 use different DOO sets");
    const std::array<MemberReplacement, 1> restored{{{"war3map.doo", originalDoodads}}};
    const auto wrongDoodads = Take(MapArchiveSession::Open(Take(readback.SaveCandidate(f.root / "special-water-reinserted.w3x", restored)).path));
    Require(!frozen.VerifyReadback(wrongDoodads).Succeeded(), "readback CAS also rejects reinserted deleted special records");

    for (const bool ordinary : {false, true}) {
        auto unrecorded = currentDoodads();
        if (ordinary) unrecorded.doodads.front().lifePercent = 24;
        else unrecorded.specialDoodads.pop_back();
        Check(f.edits->EditDoodads(unrecorded));
        const auto prepared = TerrainPathingSave::PrepareForSave(*f.archive, *f.edits, f.gameRoot);
        if (ordinary) {
            Require(prepared.Succeeded() && prepared.value && prepared.value->SaveGate().Succeeded(),
                "ordinary edit obtains its own complete two-stage proof alongside the private water special lineage");
        } else {
            Require(!prepared.Succeeded() || !prepared.value || !prepared.value->SaveGate().Succeeded(),
                "private water deletion lineage never authorizes raw special edits");
        }
        Require(!frozen.VerifyForSave(*f.edits, f.archive->SourceSha256(), output).Succeeded(),
            "an independent DOO edit also invalidates the already frozen session revision");
        Require(f.edits->Undo() && Take(currentDoodads().Serialize()) == deletedDoodads,
            "undo unrecorded object edit restores exactly the authorized water DOO state");
    }
    Check(rules.VerifyReadback(*f.archive, kBudget));
    f.SaveExpected("special-water-saved.w3x", waterTerrain, expectedPathing, &deletedDoodads);
    Require(f.edits->Undo() && Take(SerializeMapTerrain(*f.edits->Terrain())) == originalTerrain
        && Take(currentDoodads().Serialize()) == originalDoodads, "saved water Undo restores W3E and complete original DOO tail together");
    f.SaveExpected("special-water-undo-saved.w3x", originalTerrain, originalPathing, &originalDoodads);
    Require(f.edits->Redo() && Take(SerializeMapTerrain(*f.edits->Terrain())) == waterTerrain
        && Take(currentDoodads().Serialize()) == deletedDoodads, "saved water Redo restores exact special order and ordinary data without rerunning deletion");
    f.SaveExpected("special-water-redo-saved.w3x", waterTerrain, expectedPathing, &deletedDoodads);
}

constexpr std::string_view kBridgeTexture = "PathTextures/synthetic-bridge.tga";
constexpr std::string_view kBridgeBlockerTexture = "PathTextures/synthetic-bridge-blocker.tga";
void WalkableBridge(Fixture& f, bool blackPixel = false, bool duplicate = false) {
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    auto& bridge = placements.doodads.front(); bridge.rawcode="Br01"; bridge.position={544,512,0};
    bridge.flags=2; bridge.lifePercent=100;
    auto blocker=bridge; blocker.rawcode="Bl01"; blocker.position[0]=512; blocker.creationId=2;
    placements.doodads.push_back(blocker);
    if (duplicate) { auto second=placements.doodads.front(); second.creationId=3; placements.doodads.push_back(second); }
    f.Member("war3map.doo")=Take(placements.Serialize());
    f.Member("Units/DestructableData.slk")=Slk({"destructableID","code","walkable","pathTex","fixedRot"},
        {{"Br01","Br01","1",std::string(kBridgeTexture),"0"}});
    f.Member("Doodads/Doodads.slk")=Slk({"doodID","code","pathTex","category","fixedRot"},
        {{"Bl01","Bl01",std::string(kBridgeBlockerTexture),"E","-1"}});
    Bytes texture(18+16*4,std::byte{}); texture[2]=std::byte{2}; texture[12]=texture[14]=std::byte{4};
    texture[16]=std::byte{32}; texture[17]=std::byte{40};
    for (unsigned i=0;i<16;++i) texture[18+i*4]=texture[19+i*4]=texture[20+i*4]=std::byte{255};
    if (blackPixel) {
        texture[18]=texture[19]=texture[20]=std::byte{}; texture[21]=std::byte{255};
    }
    f.members.push_back({std::string(kBridgeTexture),std::move(texture)});
    Bytes blockerTexture(18+16*3,std::byte{}); blockerTexture[2]=std::byte{2};
    blockerTexture[12]=blockerTexture[14]=std::byte{4}; blockerTexture[16]=std::byte{24};
    for (unsigned i=0;i<16;++i) blockerTexture[19+i*3]=std::byte{255};
    f.members.push_back({std::string(kBridgeBlockerTexture),std::move(blockerTexture)});
    // 原fixture中的Dst0/W3B覆盖与桥无关；允许保留其它对象的记录。
    auto& wpm=f.Member("war3map.wpm");
    for (unsigned y=14;y<18;++y) for (unsigned x=14;x<18;++x) wpm[16+y*32+x]=std::byte{0x1d};
    for (unsigned y=14;y<18;++y) for (unsigned x=16;x<20;++x)
        if (!(blackPixel && x==16 && y==14)) wpm[16+y*32+x]=x<18 ? std::byte{0x15} : std::byte{0x11};
}
Bytes BridgeExpectedPathing(Fixture& f, bool deep, bool blackPixel) {
    auto expected=f.ExpectedPathing(deep);
    // 独立字面文件位：G=04来自绿色普通doodad；桥只移除terrain08/0a。
    for (unsigned y=14;y<18;++y) for (unsigned x=14;x<18;++x)
        expected[16+y*32+x]=deep ? std::byte{0x1f} : std::byte{0x1d};
    for (unsigned y=14;y<18;++y) for (unsigned x=16;x<20;++x)
        if (!(blackPixel && x==16 && y==14)) expected[16+y*32+x]=x<18 ? std::byte{0x15} : std::byte{0x11};
    return expected;
}
void BridgeSpecialExpectedLayers(Bytes& pathing) {
    // 独立字面footprint：桥(256,256)吸附2格后覆盖(6..9)^2，绿色普通物覆盖x8..11/y6..9。
    // 桥抑制全部terrain（包括special投影），作者11及普通绿色04仍保留。
    for (unsigned y = 6; y < 10; ++y) for (unsigned x = 6; x < 10; ++x)
        pathing[16 + y * 32 + x] = std::byte{0x11};
    for (unsigned y = 6; y < 10; ++y) for (unsigned x = 8; x < 12; ++x)
        pathing[16 + y * 32 + x] |= std::byte{4};
}
void BridgeSpecialWaterFixture(Fixture& f) {
    SpecialWaterDeleteFixture(f);
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    DoodadPlacement bridge; bridge.rawcode = "Br01"; bridge.position = {256, 256, 19};
    bridge.scale = {1, 1, 1}; bridge.flags = 2; bridge.lifePercent = 100; bridge.creationId = 913;
    auto blocker = bridge; blocker.rawcode = "Bl01"; blocker.position = {320, 256, 0}; blocker.creationId = 914;
    placements.doodads.push_back(std::move(bridge)); placements.doodads.push_back(std::move(blocker));
    f.Member("war3map.doo") = Take(placements.Serialize());
    f.Member("Units/DestructableData.slk") = Slk({"destructableID", "code", "walkable", "pathTex", "fixedRot"},
        {{"Br01", "Br01", "1", std::string(kBridgeTexture), "0"}});
    f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), "C", "45"},
         {"Doo1", "Doo1", std::string(kUniformDoodadTexture), "C", "45"},
         {"Doo2", "Doo2", std::string(kUniformDoodadTexture), "C", "45"},
         {"Ord0", "Ord0", "_", "E", "-1"},
         {"Bl01", "Bl01", std::string(kBridgeBlockerTexture), "E", "-1"}});
    Bytes texture(18 + 16 * 4, std::byte{}); texture[2] = std::byte{2};
    texture[12] = texture[14] = std::byte{4}; texture[16] = std::byte{32}; texture[17] = std::byte{40};
    for (unsigned i = 0; i < 16; ++i) texture[18 + i * 4] = texture[19 + i * 4] = texture[20 + i * 4] = std::byte{255};
    f.members.push_back({std::string(kBridgeTexture), std::move(texture)});
    Bytes blockerTexture(18 + 16 * 3, std::byte{}); blockerTexture[2] = std::byte{2};
    blockerTexture[12] = blockerTexture[14] = std::byte{4}; blockerTexture[16] = std::byte{24};
    for (unsigned i = 0; i < 16; ++i) blockerTexture[19 + i * 3] = std::byte{255};
    f.members.push_back({std::string(kBridgeBlockerTexture), std::move(blockerTexture)});
    BridgeSpecialExpectedLayers(f.Member("war3map.wpm")); f.Open();
}
TerrainPathingResources BridgeSpecialWaterRules(Fixture& f) {
    const auto before = Take(f.edits->Snapshot(MapDataKind::Doodads, kBudget));
    Require(before.Doodads().has_value(), "bridge water preparation has complete current DOO");
    auto rules = Take(TerrainPathingSave::CaptureWaterBrushRules(*f.archive, f.original,
        *f.edits->Terrain(), *before.Doodads(), f.gameRoot, true, kBudget));
    const auto* loaded = rules.WaterBrushRules();
    Require(loaded && loaded->maxSlopeDegrees == 90.0f && loaded->minHeight == -256.0f
        && loaded->maxHeight == 1536.0f && loaded->maximumCliffLevel == 14 && loaded->deepWaterThreshold == 64.0f,
        "real water command reads the fixed startup Terrain/Water rules even with conflicting map Misc");
    return rules;
}
void PaintBridgeSpecialWater(Fixture& f) {
    const auto rules = BridgeSpecialWaterRules(f);
    const Services::MapDataQueryBinding document{"bridge-special-water-save", 1, 0, f.archive->SourceSha256()};
    const Services::TerrainEditBinding binding{document, f.edits->SessionIdentity(), f.edits->Revision(), f.edits->TerrainSourceRevision()};
    TerrainWaterStrokeOptions options; options.shape = TerrainBrushShape::Square; options.randomSeed = 1;
    const TerrainStrokePoint point{256, 256};
    const auto prepared = Take(Services::PrepareTerrainWaterEdit(*f.edits, document, binding, rules,
        options, std::span(&point, 1), false));
    Take(Services::ApplyTerrainEdit(*f.edits, document, prepared));
}
void WalkableBridgeSpecialDeletionRestoresAndUsesFormalSaveHistory(bool withMapMisc = false) {
    for (const bool inPlace : {false, true}) {
        Fixture f; BridgeSpecialWaterFixture(f);
        if (withMapMisc) {
            const auto withoutMisc = BridgeSpecialWaterRules(f);
            const auto fingerprint = Take(withoutMisc.WaterBrushDependencyFingerprint());
            const auto sourceSha = f.archive->SourceSha256();
            f.members.push_back({"war3mapMisc.txt", ConflictingMapMisc()}); f.Open();
            const auto withMisc = BridgeSpecialWaterRules(f);
            Require(f.archive->SourceSha256() != sourceSha
                && Take(withMisc.WaterBrushDependencyFingerprint()) == fingerprint,
                "map Misc changes archive identity while preserving the versioned water dependency digest");
            Check(withoutMisc.VerifyReadback(*f.archive, kBudget));
            const std::array<MemberReplacement, 1> drift{{{"UI/MiscData.txt", Text("[Terrain]\nMaxHeight=1408\n")}}};
            const auto changed = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "water-startup-drift.w3x", drift)).path));
            Require(!withMisc.VerifyReadback(changed, kBudget).Succeeded(),
                "map Misc independence does not permit actual water startup profile drift");
        }
        const auto originalTerrain = f.Member("war3map.w3e"), originalPathing = f.Member("war3map.wpm");
        const auto originalDoodads = f.Member("war3map.doo");
        auto deleted = Take(ParseDoodadPlacements(originalDoodads));
        deleted.specialDoodads = {deleted.specialDoodads[2], deleted.specialDoodads[1]};
        const auto deletedDoodads = Take(deleted.Serialize());
        PaintBridgeSpecialWater(f);
        const auto waterTerrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
        auto waterPathing = originalPathing;
        // 手写完整G1：四个地格围住L1湿顶点，db悬崖环内最近顶点为1b；再叠桥和普通绿色物。
        for (unsigned y = 4; y < 12; ++y) for (unsigned x = 4; x < 12; ++x)
            waterPathing[16 + y * 32 + x] = std::byte{0xdb};
        for (unsigned y = 6; y < 10; ++y) for (unsigned x = 6; x < 10; ++x)
            waterPathing[16 + y * 32 + x] = std::byte{0x1b};
        BridgeSpecialExpectedLayers(waterPathing);
        const auto matches = [&](const MapDataEditSession& edits, bool water) {
            const auto snapshot = Take(edits.Snapshot(MapDataKind::Doodads, kBudget));
            return Take(SerializeMapTerrain(*edits.Terrain())) == (water ? waterTerrain : originalTerrain)
                && snapshot.Doodads() && Take(snapshot.Doodads()->Serialize()) == (water ? deletedDoodads : originalDoodads);
        };
        Require(matches(*f.edits, true) && f.edits->Revision() == 1 && f.edits->CanUndo(),
            "one real water command preserves every ordinary byte and deletes special source0 with swap-last order");
        const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits));
        const auto wire = Take(EncodeTerrainStrokeJournal(journal));
        Require(wire.size() >= 4 && wire[3] == std::byte{'3'} && journal.strokes.size() == 1
            && !journal.strokes.front().intent.followDoodads && journal.strokes.front().intent.waterBrush,
            "actual bridge plus special deletion records one non-followed typed FTJ3 water command");
        auto restored = Take(TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromArchive(*f.archive),
            originalTerrain, originalPathing, Take(DecodeTerrainStrokeJournal(wire)), *f.edits, f.gameRoot));
        Require(restored.receipts.size() == 1 && restored.session->MatchesTerrainJournalDraft(*f.edits)
            && matches(*restored.session, true), "owned FTJ3 replay restores full W3E/DOO and private special provenance");
        Require(restored.session->Undo() && !restored.session->CanUndo() && matches(*restored.session, false)
            && restored.session->Redo() && !restored.session->CanRedo() && matches(*restored.session, true),
            "restored real command walks both full terrain and ordinary/special DOO endpoints");
        f.edits = std::move(restored.session);
        const auto identity = f.edits->SessionIdentity();
        std::vector<std::pair<std::filesystem::path, std::string>> preserved;
        if (!inPlace) preserved.emplace_back(f.archive->SourcePath(), f.archive->SourceSha256());
        const auto saveAndCommit = [&](const char* name, bool water) {
            Require(matches(*f.edits, water), "formal save begins at the exact complete bridge/special history endpoint");
            const auto sourceSha = Take(Sha256File(f.archive->SourcePath()));
            const auto plan = f.Plan(); Check(plan.SaveGate()); Require(plan.RebuiltPathing(), "bridge special save rebuilds pathing");
            Services::MapSaveTask task; Check(task.FreezeArchive(*f.archive));
            task.target = inPlace ? f.archive->SourcePath() : f.root / name; task.inPlace = inPlace;
            task.inPlaceOptions.backupDirectory = f.root / "backups";
            task.replacements = f.edits->Replacements(); Check(plan.InstallPathingOutput(*f.edits, task.replacements));
            task.beforePublish = [&] { return plan.VerifyForSave(*f.edits, task.sourceSha256, task.replacements, task.removals); };
            Services::MapSaveService::Run(task);
            if (!task.succeeded) for (const auto& diagnostic : task.diagnostics)
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            Require(task.executed && task.succeeded && task.nextSession, "formal save worker publishes and reopens bridge/special state");
            if (inPlace) {
                Require(!task.backup.empty() && Take(Sha256File(task.backup)) == sourceSha,
                    "each in-place publication keeps a verified full pre-save archive backup");
                preserved.emplace_back(task.backup, sourceSha);
            }
            auto saved = Take(MapArchiveSession::Open(task.written)); Check(plan.VerifyReadback(saved));
            Require(Take(saved.ReadMember("war3map.w3e")) == (water ? waterTerrain : originalTerrain)
                && Take(saved.ReadMember("war3map.wpm")) == (water ? waterPathing : originalPathing)
                && Take(saved.ReadMember("war3map.doo")) == (water ? deletedDoodads : originalDoodads),
                "independent archive reopen matches complete W3E/DOO and literal full WPM including bridge/blocker overlap");
            for (const auto& member : f.members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm" && member.name != "war3map.doo")
                Require(Take(saved.ReadMember(member.name)) == member.data, "formal bridge special save preserves Units and every unrelated member");
            for (const auto& [path, sha] : preserved)
                Require(Take(Sha256File(path)) == sha, "subsequent save preserves each SaveAs source or each in-place backup");
            const auto revision = f.edits->Revision(), sourceRevision = f.edits->TerrainSourceRevision();
            Check(plan.CommitSaved(*f.edits, MapDocument::FromArchive(saved)));
            Require(!f.edits->IsDirty() && f.edits->Replacements().empty() && f.edits->SessionIdentity() == identity
                && f.edits->Revision() == revision && f.edits->TerrainSourceRevision() == sourceRevision + 1,
                "verified formal save rebases the same clean session without adding commands or erasing history");
            if (!inPlace) preserved.emplace_back(saved.SourcePath(), saved.SourceSha256());
            f.archive = std::make_unique<MapArchiveSession>(std::move(saved));
        };
        saveAndCommit("bridge-special-deleted.w3x", true);
        Require(f.edits->Undo() && !f.edits->CanUndo() && f.edits->CanRedo() && matches(*f.edits, false),
            "saved deletion Undo restores original special order and all ordinary wire bytes");
        saveAndCommit("bridge-special-undone.w3x", false);
        Require(f.edits->Redo() && !f.edits->CanRedo() && matches(*f.edits, true),
            "saved Undo Redo restores exactly the private deleted set without rerunning deletion");
        saveAndCommit("bridge-special-redone.w3x", true);
    }
}
void WalkableBridgeSpecialDeletionKeepsOrdinaryAndProvenanceGates() {
    Fixture f; BridgeSpecialWaterFixture(f); PaintBridgeSpecialWater(f);
    const auto sourceSha = Take(Sha256File(f.archive->SourcePath()));
    const auto waterTerrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto snapshot = Take(f.edits->Snapshot(MapDataKind::Doodads, kBudget));
    const auto deleted = *snapshot.Doodads(); const auto deletedDoodads = Take(deleted.Serialize());
    const auto plan = f.Plan(); const auto output = f.Output(plan);
    for (unsigned mode = 0; mode < 10; ++mode) {
        auto changed = deleted;
        if (mode == 0) changed.doodads[1].position[2] += 1;
        if (mode == 1) changed.doodads[0].position[2] += 1;
        if (mode == 2) changed.doodads[0].flags ^= 1;
        if (mode == 3) changed.doodads[0].position[0] += 1;
        if (mode == 4) changed.doodads[0].rotation += 0.25f;
        if (mode == 5) std::swap(changed.doodads[0], changed.doodads[2]);
        if (mode == 6) changed.doodads.pop_back();
        if (mode == 7) changed.doodads[0].itemSets[0].items[0].chance += 1;
        if (mode == 8) changed.doodads[0].itemSets[0].items[0].rawcode = "drop";
        if (mode == 9) changed.doodads[2].rotation = -0.0f;
        if (mode == 0) {
            const auto wire = Take(changed.Serialize());
            const auto refused = TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, kBudget, &wire);
            Require(!refused.Succeeded() && HasCode(refused, "TERRAIN_WALKABLE_DESTRUCTABLE_DOODADS_CHANGED"),
                "special deletion cannot authorize a bridge record change");
        } else {
            Fixture ordinary; BridgeSpecialWaterFixture(ordinary); PaintBridgeSpecialWater(ordinary);
            Check(ordinary.edits->EditDoodads(changed));
            auto expected = ordinary.Member("war3map.wpm");
            for (unsigned y = 4; y < 12; ++y) for (unsigned x = 4; x < 12; ++x)
                expected[16 + y * 32 + x] = std::byte{0xdb};
            for (unsigned y = 6; y < 10; ++y) for (unsigned x = 6; x < 10; ++x)
                expected[16 + y * 32 + x] = std::byte{0x1b};
            BridgeSpecialExpectedLayers(expected);
            if (mode == 6) for (unsigned y = 6; y < 10; ++y) for (unsigned x = 8; x < 12; ++x)
                expected[16 + y * 32 + x] &= std::byte{0xfb};
            const auto wire = Take(changed.Serialize());
            const auto name = "bridge-special-ordinary-" + std::to_string(mode) + ".w3x";
            ordinary.SaveExpected(name.c_str(),
                Take(SerializeMapTerrain(*ordinary.edits->Terrain())), expected, &wire, true);
        }
    }
    for (const bool reorder : {false, true}) {
        auto changed = deleted;
        if (reorder) std::reverse(changed.specialDoodads.begin(), changed.specialDoodads.end());
        else changed.specialDoodads.pop_back();
        Check(f.edits->EditDoodads(changed));
        const auto refused = f.Plan().SaveGate();
        Require(!refused.Succeeded() && HasCode(refused, "TERRAIN_SPECIAL_PROVENANCE_REQUIRED"),
            "manual special deletion or reordering never inherits the private water command provenance");
        Require(f.edits->Undo(), "Undo independent special mutation");
        const auto undone = Take(f.edits->Snapshot(MapDataKind::Doodads, kBudget));
        Require(undone.Doodads() && Take(undone.Doodads()->Serialize()) == deletedDoodads,
            "Undo of unrecorded special mutation returns the exact trusted DOO state");
    }
    for (const bool unknownTail : {false, true}) {
        auto wire = deletedDoodads;
        if (unknownTail) wire.push_back(std::byte{0});
        else wire[wire.size() - 8 - deleted.specialDoodads.size() * 16] = std::byte{1};
        const auto refused = TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, kBudget, &wire);
        Require(!refused.Succeeded() && HasCode(refused, unknownTail ? "PLACEMENT_TRAILING_DATA" : "PLACEMENT_SPECIAL_VERSION"),
            "prefix calculation cannot grant an owner to unknown trailing bytes or a nonzero special wire version");
    }
    const auto freshPlan = f.Plan(); const auto freshOutput = f.Output(freshPlan);
    auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "bridge-special-frozen.w3x", freshOutput)).path));
    for (const bool reorder : {false, true}) {
        auto changed = deleted;
        if (reorder) std::reverse(changed.specialDoodads.begin(), changed.specialDoodads.end());
        const auto wire = reorder ? Take(changed.Serialize()) : f.Member("war3map.doo");
        auto substituted = freshOutput;
        const auto entry = std::find_if(substituted.begin(), substituted.end(), [](const auto& member) { return member.name == "war3map.doo"; });
        Require(entry != substituted.end(), "coupled bridge/special output contains full DOO"); entry->data = wire;
        Require(!freshPlan.VerifyForSave(*f.edits, f.archive->SourceSha256(), substituted).Succeeded(),
            "frozen output binds the exact trusted new special order and rejects restoring an old tail");
        const std::array<MemberReplacement, 1> replacement{{{"war3map.doo", wire}}};
        auto wrong = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root / (reorder ? "bridge-special-reordered.w3x" : "bridge-special-reinserted.w3x"), replacement)).path));
        Require(!freshPlan.VerifyReadback(wrong).Succeeded(), "independent readback rejects the same changed special tail");
    }
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output).Succeeded()
        && Take(Sha256File(f.archive->SourcePath())) == sourceSha && f.edits->IsDirty()
        && Take(SerializeMapTerrain(*f.edits->Terrain())) == waterTerrain,
        "rejections preserve source and unsaved water draft while earlier frozen session revisions remain stale");
}
struct BridgeOriginalModification {
    std::string field;
    NativeValueType type;
    Value value;
};
Bytes BridgeOriginalMetadata(bool explicitScalar = false, std::string rotationType = "unreal",
    std::string walkableType = "bool", std::string pathType = "pathingTexture") {
    if (explicitScalar) return Slk({"ID", "field", "slk", "type", "index", "repeat", "data"},
        {{"bwlk", "walkable", "DestructableData", walkableType, "0", "0", "0"},
         {"bptx", "pathTex", "DestructableData", pathType, "0", "0", "0"},
         {"brot", "fixedRot", "DestructableData", rotationType, "0", "0", "0"}});
    // 实际原生表可缺repeat/data；index仍必须显式为0。
    return Slk({"ID", "field", "slk", "type", "index"},
        {{"bwlk", "walkable", "DestructableData", walkableType, "0"},
         {"bptx", "pathTex", "DestructableData", pathType, "0"},
         {"brot", "fixedRot", "DestructableData", rotationType, "0"}});
}
void BridgeOriginal(Fixture& f, std::initializer_list<BridgeOriginalModification> modifications, bool custom = false,
    std::string childRawcode = "NewB", std::string parentRawcode = "Br01") {
    auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Destructable));
    if (custom) {
        Check(document.AddCustomObject(childRawcode, parentRawcode));
        auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
        placements.doodads.front().rawcode = childRawcode;
        f.Member("war3map.doo") = Take(placements.Serialize());
    } else Check(document.AddOriginalObject("Br01"));
    for (const auto& modification : modifications)
        Check(document.AddModification({ObjectType::Destructable, custom ? childRawcode : "Br01"},
            {modification.field}, modification.type, modification.value));
    f.Member("war3map.w3b") = Take(NativeObjectCodec(ObjectType::Destructable).Serialize(document));
    f.Member("Units/DestructableMetaData.slk") = BridgeOriginalMetadata();
}
constexpr std::string_view kBridgeOriginalTexture = "PathTextures/synthetic-bridge-original.tga";
Bytes BridgeOriginalExpectedPathing(Fixture& f, bool deep, bool turned = false, bool walkable = true) {
    auto expected = f.ExpectedPathing(deep);
    for (unsigned y = 14; y < 18; ++y) for (unsigned x = 14; x < 18; ++x)
        expected[16 + y * 32 + x] = deep ? std::byte{0x1f} : std::byte{0x1d};
    // 独立完整字面预期：桥只抑制terrain，普通绿色阻挡04和作者保护11继续保留。
    // top-origin黑角在wire角0时位于(16,14)，fixedRot90后位于(19,14)。
    if (walkable) for (unsigned y = 14; y < 18; ++y) for (unsigned x = 16; x < 20; ++x)
        if (y != 14 || x != (turned ? 19u : 16u))
            expected[16 + y * 32 + x] = x < 18 ? std::byte{0x15} : std::byte{0x11};
    return expected;
}
void PatternedBridgeOriginal(Fixture& f, float rotation, NativeValueType rotationType = NativeValueType::Unreal,
    bool custom = false) {
    WalkableBridge(f);
    auto texture = f.Member(kBridgeTexture);
    texture[18] = texture[19] = texture[20] = std::byte{};
    texture[21] = std::byte{255};
    f.members.push_back({std::string(kBridgeOriginalTexture), std::move(texture)});
    // 标准walkable0和相反的标准角不能覆盖显式W3B值；标准path仍是全白另一张图。
    f.Member("Units/DestructableData.slk") = Slk({"destructableID", "code", "walkable", "pathTex", "fixedRot"},
        {{"Br01", "Br01", "0", std::string(kBridgeTexture), rotation == 90 ? "0" : "90"}});
    BridgeOriginal(f, {{"bwlk", NativeValueType::Int32, std::int32_t{1}},
        {"bptx", NativeValueType::String, std::string(kBridgeOriginalTexture)}, {"brot", rotationType, rotation}}, custom);
    f.Member("war3map.wpm") = BridgeOriginalExpectedPathing(f, false, rotation == 90);
}
void BridgeOriginalUsesTheNormalSaveAndUndoChain() {
    {
        Fixture f; WalkableBridge(f, true); BridgeOriginal(f, {});
        // 空Original不要求未用的pathTex/fixedRot元数据，标准字段仍是唯一实际来源。
        f.Member("Units/DestructableMetaData.slk") = Slk({"ID", "field", "slk", "type", "index"},
            {{"bwlk", "walkable", "DestructableData", "bool", "0"}});
        f.Open(); f.Paint();
        f.SaveExpected("bridge-empty-original.w3x", f.ExpectedTerrain(true), BridgeExpectedPathing(f, true, true));
    }
    for (const auto type : {NativeValueType::Real, NativeValueType::Unreal}) for (const float rotation : {-1.0f, 0.0f, 90.0f}) {
        Fixture f; PatternedBridgeOriginal(f, rotation, type);
        f.Member("Units/DestructableMetaData.slk") = BridgeOriginalMetadata(rotation == 0,
            type == NativeValueType::Real ? "real" : "unreal", rotation == -1 ? "int" : "bool",
            rotation == 90 ? "string" : "pathingTexture");
        f.Open(); f.Paint();
        const auto shallow = BridgeOriginalExpectedPathing(f, false, rotation == 90);
        const auto deep = BridgeOriginalExpectedPathing(f, true, rotation == 90);
        Require(shallow == f.Member("war3map.wpm"), "independent Original bridge source WPM is exact over the whole map");
        f.SaveExpected("bridge-original-deep.w3x", f.ExpectedTerrain(true), deep);
        Require(f.edits->Undo(), "undo saved bridge Original terrain change");
        f.SaveExpected("bridge-original-undone.w3x", f.ExpectedTerrain(false), shallow);
        Require(f.edits->Redo(), "redo saved bridge Original terrain change");
        f.SaveExpected("bridge-original-redone.w3x", f.ExpectedTerrain(true), deep);
    }
    for (const auto* type : {"bool", "int"}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {{"bwlk", NativeValueType::Int32, std::int32_t{0}}});
        f.Member("Units/DestructableMetaData.slk") = BridgeOriginalMetadata(false, "unreal", type);
        f.Member("war3map.wpm") = BridgeOriginalExpectedPathing(f, false, false, false);
        f.Open(); f.Paint();
        f.SaveExpected("bridge-original-walkable-zero.w3x", f.ExpectedTerrain(true), BridgeOriginalExpectedPathing(f, true, false, false));
    }
}
void BridgeOriginalRejectsUnsupportedValuesAndMetadata() {
    const auto refuse = [](Fixture& f, const char* code) {
        f.Open(); f.Paint();
        const auto source = Take(Sha256File(f.archive->SourcePath()));
        const auto draft = Take(SerializeMapTerrain(*f.edits->Terrain()));
        const auto plan = f.Plan();
        Require(!plan.SaveGate().Succeeded() && HasCode(plan.SaveGate(), code), "invalid Original bridge fails at its exact source, type or value gate");
        Require(f.edits->IsDirty() && f.edits->CanUndo() && Take(Sha256File(f.archive->SourcePath())) == source
            && Take(SerializeMapTerrain(*f.edits->Terrain())) == draft, "Original bridge refusal preserves source, complete draft and undo history");
    };
    for (const std::string path : {"", "_", "none"}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {{"bptx", NativeValueType::String, path}});
        refuse(f, "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED");
    }
    for (const auto& modification : std::initializer_list<BridgeOriginalModification>{
        {"bptx", NativeValueType::Int32, std::int32_t{0}}, {"brot", NativeValueType::Int32, std::int32_t{90}},
        {"brot", NativeValueType::Real, 90.0f}, {"brot", NativeValueType::String, std::string("90")}}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {modification});
        refuse(f, "TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE");
    }
    for (const auto& modification : std::initializer_list<BridgeOriginalModification>{
        {"bwlk", NativeValueType::Real, 1.0f}, {"bwlk", NativeValueType::Int32, std::int32_t{2}}}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {modification});
        refuse(f, "TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE");
    }
    for (const float value : {-361.0f, 361.0f}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {{"brot", NativeValueType::Unreal, value}});
        refuse(f, "TERRAIN_DOODAD_FIXED_ROTATION");
    }
    for (const std::uint32_t bits : {0x7fc00000u, 0x7f800000u, 0xff800000u}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {{"brot", NativeValueType::Unreal, 0.0f}});
        auto& bytes = f.Member("war3map.w3b");
        // 经典W3B单Original/单modifier：field@20、type@24、F32@28。
        Require(bytes.size() == 40 && bytes[20] == std::byte{'b'} && bytes[24] == std::byte{2}, "nonfinite fixture uses the exact classic W3B scalar address");
        for (unsigned i = 0; i < 4; ++i) bytes[28 + i] = static_cast<std::byte>((bits >> (8 * i)) & 255);
        refuse(f, "TERRAIN_DOODAD_FIXED_ROTATION");
    }
    for (unsigned mode = 0; mode < 10; ++mode) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {{"bptx", NativeValueType::String, std::string(kBridgeTexture)}});
        std::vector<std::string> columns{"ID", "field", "slk", "type", "index", "repeat", "data"};
        std::vector<std::string> walk{"bwlk", "walkable", "DestructableData", "bool", "0", "0", "0"};
        std::vector<std::string> path{"bptx", "pathTex", "DestructableData", "pathingTexture", "0", "0", "0"};
        std::vector<std::string> rotation{"brot", "fixedRot", "DestructableData", "unreal", "0", "0", "0"};
        if (mode == 0) path[2] = "DestructableData.slk";
        if (mode == 1) path[4] = "1";
        if (mode == 2) path[5] = "1";
        if (mode == 3) path[6] = "1";
        if (mode == 4) path[3] = "int";
        if (mode == 5) path[1] = " pathTex";
        if (mode == 6) path[4] = "zero";
        if (mode == 7) path[4] = "-1";
        if (mode == 8) { columns.erase(columns.begin() + 4); walk.erase(walk.begin() + 4); path.erase(path.begin() + 4); rotation.erase(rotation.begin() + 4); }
        if (mode == 9) {
            auto alias = path; alias[0] = "bp02";
            f.Member("Units/DestructableMetaData.slk") = Slk(columns, {walk, path, rotation, alias});
        } else f.Member("Units/DestructableMetaData.slk") = Slk(columns, {walk, path, rotation});
        refuse(f, "TERRAIN_NATIVE_OBJECT_METADATA");
    }
    {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {{"bnam", NativeValueType::String, std::string("renamed")}});
        f.Member("Units/DestructableMetaData.slk") = Slk({"ID", "field", "slk", "type", "index"},
            {{"bwlk", "walkable", "DestructableData", "bool", "0"}, {"bptx", "pathTex", "DestructableData", "pathingTexture", "0"},
             {"brot", "fixedRot", "DestructableData", "unreal", "0"}, {"bnam", "Name", "DestructableData", "string", "0"}});
        refuse(f, "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED");
    }
    {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {}, true);
        const auto parsed = NativeObjectCodec(ObjectType::Destructable).Parse(f.Member("war3map.w3b"));
        const auto* document = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
        Require(document != nullptr, "parent override refusal fixture has a native document");
        auto changed = *document; Check(changed.AddOriginalObject("Br01"));
        f.Member("war3map.w3b") = Take(NativeObjectCodec(ObjectType::Destructable).Serialize(changed));
        refuse(f, "TERRAIN_NATIVE_OBJECT_INHERITANCE");
    }
}
void BridgeCustomUsesTheNormalSaveAndUndoChain() {
    {
        Fixture f; WalkableBridge(f, true); BridgeOriginal(f, {}, true);
        // 空Custom只要求实际分类使用的walkable元数据；其它字段沿唯一标准父回退。
        f.Member("Units/DestructableMetaData.slk") = Slk({"ID", "field", "slk", "type", "index"},
            {{"bwlk", "walkable", "DestructableData", "bool", "0"}});
        f.Open(); f.Paint();
        const auto shallow = BridgeExpectedPathing(f, false, true), deep = BridgeExpectedPathing(f, true, true);
        f.SaveExpected("bridge-custom-empty-deep.w3x", f.ExpectedTerrain(true), deep);
        Require(f.edits->Undo(), "undo saved empty Custom bridge terrain change");
        f.SaveExpected("bridge-custom-empty-undone.w3x", f.ExpectedTerrain(false), shallow);
        Require(f.edits->Redo(), "redo saved empty Custom bridge terrain change");
        f.SaveExpected("bridge-custom-empty-redone.w3x", f.ExpectedTerrain(true), deep);
    }
    for (const auto type : {NativeValueType::Real, NativeValueType::Unreal}) for (const float rotation : {-1.0f, 0.0f, 90.0f}) {
        Fixture f; PatternedBridgeOriginal(f, rotation, type, true);
        f.Member("Units/DestructableMetaData.slk") = BridgeOriginalMetadata(rotation == 0,
            type == NativeValueType::Real ? "real" : "unreal", rotation == -1 ? "int" : "bool",
            rotation == 90 ? "string" : "pathingTexture");
        f.Open(); f.Paint();
        const auto shallow = BridgeOriginalExpectedPathing(f, false, rotation == 90);
        const auto deep = BridgeOriginalExpectedPathing(f, true, rotation == 90);
        Require(shallow == f.Member("war3map.wpm"), "Custom source WPM uses complete independent effective-field bytes");
        f.SaveExpected("bridge-custom-deep.w3x", f.ExpectedTerrain(true), deep);
        Require(f.edits->Undo(), "undo saved Custom bridge terrain change");
        f.SaveExpected("bridge-custom-undone.w3x", f.ExpectedTerrain(false), shallow);
        Require(f.edits->Redo(), "redo saved Custom bridge terrain change");
        f.SaveExpected("bridge-custom-redone.w3x", f.ExpectedTerrain(true), deep);
    }
    for (const auto* type : {"bool", "int"}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {{"bwlk", NativeValueType::Int32, std::int32_t{0}}}, true);
        f.Member("Units/DestructableMetaData.slk") = BridgeOriginalMetadata(false, "unreal", type);
        f.Member("war3map.wpm") = BridgeOriginalExpectedPathing(f, false, false, false);
        f.Open(); f.Paint();
        f.SaveExpected("bridge-custom-walkable-zero.w3x", f.ExpectedTerrain(true), BridgeOriginalExpectedPathing(f, true, false, false));
    }
    {
        Fixture f; WalkableBridge(f, true);
        f.Member("Units/DestructableData.slk") = Slk({"destructableID", "code", "walkable", "pathTex", "fixedRot"},
            {{"SC01", "SC01", "1", std::string(kBridgeTexture), "0"}});
        BridgeOriginal(f, {}, true, "NewB", "SC01");
        // 父第二字符C不能污染新ID的非C两格策略；未使用字段和无关字段的selector不参与分类。
        f.Member("Units/DestructableMetaData.slk") = Slk({"ID", "field", "slk", "type", "index", "useSpecific", "notSpecific"},
            {{"bwlk", "walkable", "DestructableData", "bool", "0", "", ""},
             {"bptx", "pathTex", "DestructableData", "pathingTexture", "0", "SC01", ""},
             {"brot", "fixedRot", "DestructableData", "unreal", "0", "", "NewB"},
             {"bnam", "Name", "DestructableData", "string", "0", "SC01", "NewB"}});
        f.Open(); f.Paint();
        f.SaveExpected("bridge-custom-parent-c-child-nonc.w3x", f.ExpectedTerrain(true), BridgeExpectedPathing(f, true, true));
    }
}
void BridgeCustomRejectsUnsupportedValuesMetadataAndParents() {
    const auto refuse = [](Fixture& f, const char* code) {
        f.Open(); f.Paint();
        const auto source = Take(Sha256File(f.archive->SourcePath()));
        const auto draft = Take(SerializeMapTerrain(*f.edits->Terrain()));
        const auto currentDoodads = [&] {
            const auto snapshot = Take(f.edits->Snapshot(MapDataKind::Doodads, kBudget));
            Require(snapshot.Doodads().has_value(), "Custom refusal fixture has a bounded parsed DOO snapshot");
            return Take(snapshot.Doodads()->Serialize());
        };
        const auto doodads = currentDoodads();
        const auto plan = f.Plan();
        Require(!plan.SaveGate().Succeeded() && HasCode(plan.SaveGate(), code), "invalid Custom bridge fails at its exact value, metadata, identity or state gate");
        Require(f.edits->IsDirty() && f.edits->CanUndo() && Take(Sha256File(f.archive->SourcePath())) == source
            && Take(SerializeMapTerrain(*f.edits->Terrain())) == draft
            && currentDoodads() == doodads,
            "Custom refusal preserves source, complete terrain and DOO draft, and undo history");
    };
    for (const std::string path : {"", "_", "none"}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {{"bptx", NativeValueType::String, path}}, true);
        refuse(f, "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED");
    }
    for (const auto& modification : std::initializer_list<BridgeOriginalModification>{
        {"bptx", NativeValueType::Int32, std::int32_t{0}}, {"brot", NativeValueType::Int32, std::int32_t{90}},
        {"brot", NativeValueType::Real, 90.0f}, {"brot", NativeValueType::String, std::string("90")}}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {modification}, true);
        refuse(f, "TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE");
    }
    for (const auto& modification : std::initializer_list<BridgeOriginalModification>{
        {"bwlk", NativeValueType::Real, 1.0f}, {"bwlk", NativeValueType::Int32, std::int32_t{2}}}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {modification}, true);
        refuse(f, "TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE");
    }
    for (const float value : {-361.0f, 361.0f}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {{"brot", NativeValueType::Unreal, value}}, true);
        refuse(f, "TERRAIN_DOODAD_FIXED_ROTATION");
    }
    for (const std::uint32_t bits : {0x7fc00000u, 0x7f800000u, 0xff800000u}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {{"brot", NativeValueType::Unreal, 0.0f}}, true);
        auto& bytes = f.Member("war3map.w3b");
        // 经典W3B单Custom/单modifier：field@24、type@28、F32@32。
        Require(bytes.size() == 40 && bytes[24] == std::byte{'b'} && bytes[28] == std::byte{2}, "Custom nonfinite fixture uses the classic scalar wire address");
        for (unsigned i = 0; i < 4; ++i) bytes[32 + i] = static_cast<std::byte>((bits >> (8 * i)) & 255);
        refuse(f, "TERRAIN_DOODAD_FIXED_ROTATION");
    }
    for (const bool excluded : {false, true}) for (const unsigned field : {0u, 1u, 2u}) {
        Fixture f; PatternedBridgeOriginal(f, 90, NativeValueType::Unreal, true);
        std::vector<std::string> walk{"bwlk", "walkable", "DestructableData", "bool", "0", ""};
        std::vector<std::string> path{"bptx", "pathTex", "DestructableData", "pathingTexture", "0", ""};
        std::vector<std::string> rotation{"brot", "fixedRot", "DestructableData", "unreal", "0", ""};
        auto* selected = field == 0 ? &walk : (field == 1 ? &path : &rotation);
        selected->back() = excluded ? "NewB" : "Br01";
        f.Member("Units/DestructableMetaData.slk") = Slk({"ID", "field", "slk", "type", "index", excluded ? "notSpecific" : "useSpecific"},
            {walk, path, rotation});
        refuse(f, "TERRAIN_NATIVE_OBJECT_METADATA");
    }
    for (const bool unknown : {false, true}) {
        Fixture f; WalkableBridge(f);
        BridgeOriginal(f, {{unknown ? "zzzz" : "bnam", NativeValueType::String, std::string("renamed")}}, true);
        if (!unknown) f.Member("Units/DestructableMetaData.slk") = Slk({"ID", "field", "slk", "type", "index"},
            {{"bwlk", "walkable", "DestructableData", "bool", "0"}, {"bnam", "Name", "DestructableData", "string", "0"}});
        refuse(f, unknown ? "TERRAIN_NATIVE_OBJECT_METADATA" : "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED");
    }
    for (unsigned mode = 0; mode < 8; ++mode) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {}, true, "NewB", mode == 2 ? "Gone" : "Br01");
        if (mode < 2) {
            const auto parsed = NativeObjectCodec(ObjectType::Destructable).Parse(f.Member("war3map.w3b"));
            const auto* document = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
            Require(document != nullptr, "Custom parent refusal fixture has a native document");
            auto changed = *document;
            if (mode == 0) Check(changed.AddOriginalObject("Br01"));
            else Check(changed.AddCustomObject("Br01", "Dst0"));
            f.Member("war3map.w3b") = Take(NativeObjectCodec(ObjectType::Destructable).Serialize(changed));
        }
        if (mode == 3) f.Member("Units/DestructableData.slk") = Slk({"destructableID", "code", "walkable", "pathTex", "fixedRot"},
            {{"Br01", "Br01", "1", std::string(kBridgeTexture), "0"}, {"NewB", "NewB", "1", std::string(kBridgeTexture), "0"}});
        if (mode == 4 || mode == 6) {
            const std::string identity = mode == 4 ? "Br01" : "NewB";
            f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
                {{"Bl01", "Bl01", std::string(kBridgeBlockerTexture), "E", "-1"}, {identity, identity, "_", "E", "0"}});
        }
        if (mode == 5 || mode == 7) {
            auto doodad = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
            Check(doodad.AddOriginalObject(mode == 5 ? "Br01" : "NewB"));
            f.members.push_back({"war3map.w3d", Take(NativeObjectCodec(ObjectType::Doodad).Serialize(doodad))});
        }
        refuse(f, mode < 6 ? "TERRAIN_NATIVE_OBJECT_INHERITANCE" : "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS");
    }
    {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {}, true, "NC01", "Br01");
        refuse(f, "TERRAIN_CATEGORY_SOURCE_REQUIRED");
    }
    for (const unsigned mode : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
        Fixture f; WalkableBridge(f); BridgeOriginal(f, {}, true);
        auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
        if (mode < 4) placements.doodads.front().flags = std::array<std::uint8_t, 4>{0, 1, 3, 4}[mode];
        else placements.doodads.front().lifePercent = std::array<std::uint8_t, 3>{0, 50, 99}[mode - 4];
        f.Member("war3map.doo") = Take(placements.Serialize());
        refuse(f, "TERRAIN_WALKABLE_DESTRUCTABLE_STATE");
    }
}
void BridgeOriginalDependenciesAndOldPathingRemainBound() {
    for (const bool custom : {false, true}) {
        Fixture f; PatternedBridgeOriginal(f, 90, NativeValueType::Unreal, custom); f.Open(); f.Paint();
        const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, kBudget));
        Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, owner.CaptureBudgetBytes()));
        Require(!TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), {}, owner.CaptureBudgetBytes() - 1).Succeeded(),
            "Original and Custom bridge W3B and actual metadata consume the exact cumulative capture budget");
        const auto plan = f.Plan(); const auto output = f.Output(plan);
        const auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "bridge-original-frozen.w3x", output)).path));
        for (const char* name : {"war3map.w3b", "Units/DestructableMetaData.slk"}) {
            auto changed = f.Member(name);
            if (std::string_view(name) == "war3map.w3b") {
                const auto parsed = NativeObjectCodec(ObjectType::Destructable).Parse(changed);
                const auto* document = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
                Require(document != nullptr, "bridge Original dependency fixture has a native document");
                auto replacement = *document;
                Check(replacement.SetModification({ObjectType::Destructable, custom ? "NewB" : "Br01"}, {"brot"}, Value{0.0f}));
                changed = Take(NativeObjectCodec(ObjectType::Destructable).Serialize(replacement));
            } else changed.push_back(std::byte{'\n'});
            auto altered = output; altered.push_back({name, changed});
            Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), altered).Succeeded(), "bridge Original W3B and metadata remain frozen before publication");
            const std::array<std::string, 1> removed{name};
            Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output, removed).Succeeded(), "bridge Original dependencies cannot be removed");
            const std::array<MemberReplacement, 1> replacement{{{name, changed}}};
            const auto tampered = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root / (std::string(name) == "war3map.w3b"
                ? "bridge-original-w3b-tampered.w3x" : "bridge-original-metadata-tampered.w3x"), replacement)).path));
            Require(!plan.VerifyReadback(tampered).Succeeded(), "independent readback rejects bridge Original W3B or metadata drift");
        }
        Require(!plan.VerifyForSave(*f.edits, std::string(64, '0'), output).Succeeded(), "bridge Original retains source SHA CAS");
        for (const unsigned row : {0u, 1u}) {
            Fixture changed; PatternedBridgeOriginal(changed, 90, NativeValueType::Unreal, custom); changed.Open(); changed.Paint();
            auto placements = Take(ParseDoodadPlacements(changed.Member("war3map.doo"))); placements.doodads[row].position[2] = 16;
            Check(changed.edits->EditDoodads(placements));
            if (row == 0) {
                const auto gate = changed.Plan().SaveGate();
                Require(!gate.Succeeded() && HasCode(gate, "TERRAIN_WALKABLE_DESTRUCTABLE_DOODADS_CHANGED"),
                    "Original bridge record remains frozen");
            } else {
                const auto wire = Take(placements.Serialize());
                changed.SaveExpected("bridge-original-unrelated-z.w3x", changed.ExpectedTerrain(true),
                    BridgeOriginalExpectedPathing(changed, true, true), &wire, true);
            }
        }
        Fixture unexplained; PatternedBridgeOriginal(unexplained, 90, NativeValueType::Unreal, custom);
        unexplained.Member("war3map.wpm")[16 + 16 * 32 + 18] ^= std::byte{2};
        unexplained.Open(); unexplained.Paint(); const auto gate = unexplained.Plan().SaveGate();
        Require(!gate.Succeeded() && HasCode(gate, "TERRAIN_PATHING_SOURCE_UNEXPLAINED"), "Original bridge cannot hide an unexplained source WPM bit under terrain suppression");
    }
}
Bytes CategoryCBridgeExpected(Fixture& f, bool deep, bool four, bool turned) {
    auto expected = f.ExpectedPathing(deep);
    // 普通绿色blocker的独立4x4预期；桥不能清掉静态04或作者11。
    for (unsigned y = 14; y < 18; ++y) for (unsigned x = 14; x < 18; ++x)
        expected[16 + y * 32 + x] = deep ? std::byte{0x1f} : std::byte{0x1d};
    // top-origin 4x2不对称图，黑、红、绿、蓝 / 蓝、黑、绿、红。
    // 先按wire角0吸附，再fixedRot0/90旋转；(544,544)下两格宽有不同完整起点。
    constexpr std::array<unsigned, 8> zero{0, 1, 1, 0, 1, 1, 1, 1};
    constexpr std::array<unsigned, 8> ninety{1, 1, 1, 0, 1, 1, 0, 1};
    const auto& pixels = turned ? ninety : zero;
    const unsigned width = turned ? 4 : 2;
    const unsigned x0 = turned ? (four ? 14 : 15) : (four ? 15 : 16), y0 = turned ? 17 : 16;
    for (unsigned i = 0; i < pixels.size(); ++i) if (pixels[i]) {
        const unsigned x = x0 + i % width, y = y0 + i / width;
        expected[16 + y * 32 + x] = x >= 14 && x < 18 && y >= 14 && y < 18 ? std::byte{0x15} : std::byte{0x11};
    }
    return expected;
}

void CategoryCBridge(Fixture& f, bool four, bool turned = false, bool custom = true, bool typed = true) {
    WalkableBridge(f, false, true);
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    for (auto& placed : placements.doodads) if (placed.rawcode == "Br01") {
        placed.rawcode = "BC01"; placed.position = {544, 544, 0};
    }
    f.Member("war3map.doo") = Take(placements.Serialize());
    const std::string parent = custom ? "Br01" : "BC01";
    f.Member("Units/DestructableData.slk") = Slk({"destructableID", "code", "walkable", "pathTex", "fixedRot", "category"},
        {{parent, parent, typed ? "0" : "1", typed ? "_" : std::string(kBridgeTexture),
            typed ? (turned ? "0" : "90") : (turned ? "90" : "0"), "E"}});
    auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Destructable));
    if (custom) Check(document.AddCustomObject("BC01", parent)); else Check(document.AddOriginalObject("BC01"));
    if (typed) {
        Check(document.AddModification({ObjectType::Destructable, "BC01"}, {"bwlk"}, NativeValueType::Int32, std::int32_t{1}));
        Check(document.AddModification({ObjectType::Destructable, "BC01"}, {"bptx"}, NativeValueType::String, std::string(kBridgeTexture)));
        Check(document.AddModification({ObjectType::Destructable, "BC01"}, {"brot"}, NativeValueType::Unreal, turned ? 90.0f : 0.0f));
    }
    f.Member("war3map.w3b") = Take(NativeObjectCodec(ObjectType::Destructable).Serialize(document));
    f.Member("Units/DestructableMetaData.slk") = BridgeOriginalMetadata();
    Bytes texture(18 + 8 * 4); texture[2] = std::byte{2}; texture[12] = std::byte{4}; texture[14] = std::byte{2};
    texture[16] = std::byte{32}; texture[17] = std::byte{40};
    constexpr std::array<std::array<unsigned, 3>, 8> colors{{
        {0, 0, 0}, {0, 0, 255}, {0, 255, 0}, {255, 0, 0},
        {255, 0, 0}, {0, 0, 0}, {0, 255, 0}, {0, 0, 255}}};
    for (unsigned i = 0; i < colors.size(); ++i) {
        for (unsigned channel = 0; channel < 3; ++channel) texture[18 + i * 4 + channel] = static_cast<std::byte>(colors[i][channel]);
        texture[21 + i * 4] = colors[i] == std::array<unsigned, 3>{0, 0, 0} ? std::byte{255} : std::byte{};
    }
    f.Member(kBridgeTexture) = std::move(texture);
    f.gameRoot = f.root / "game"; Require(std::filesystem::create_directory(f.gameRoot), "private C bridge startup source");
    Take(WriteVerifiedArchive(f.gameRoot / "war3.mpq", std::vector<MapArchiveMember>{
        {"UI/WorldEditData.txt", Text(four ? "[DoodadCategories]\nC=bridge\n" : "[DoodadCategories]\nE=known-no-C\n")}}));
    f.Member("war3map.wpm") = CategoryCBridgeExpected(f, false, four, turned);
}

void CategoryCBridgesUseTheSharedSaveAndUndoChain() {
    for (const bool custom : {false, true}) for (const bool typed : {false, true})
        for (const bool four : {false, true}) for (const bool turned : {false, true}) {
        Fixture f; CategoryCBridge(f, four, turned, custom, typed); f.Open(); f.Paint();
        const auto shallow = CategoryCBridgeExpected(f, false, four, turned), deep = CategoryCBridgeExpected(f, true, four, turned);
        Require(shallow == f.Member("war3map.wpm"), "C桥完整旧WPM使用独立网格和非对称颜色矩阵");
        f.SaveExpected("c-bridge-saved.w3x", f.ExpectedTerrain(true), deep);
        if (typed && turned) {
            Require(f.edits->Undo(), "C桥保存后可撤销地形，原生来源和DOO保持");
            f.SaveExpected("c-bridge-undone.w3x", f.ExpectedTerrain(false), shallow);
            Require(f.edits->Redo(), "C桥保存后可重做地形");
            f.SaveExpected("c-bridge-redone.w3x", f.ExpectedTerrain(true), deep);
        }
    }
}

void CategoryCBridgeSourcesAndOldPathingStayBound() {
    for (const bool four : {false, true}) {
        Fixture f; CategoryCBridge(f, four, true); f.Open(); f.Paint();
        // 与正常保存plan相同，绑定最终DOO；未提供它的独立捕获仍允许普通对象仅Z变化。
        const auto& currentDoodads = f.Member("war3map.doo");
        const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, kBudget, &currentDoodads));
        Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, owner.CaptureBudgetBytes(), &currentDoodads));
        Require(!TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, owner.CaptureBudgetBytes() - 1, &currentDoodads).Succeeded(),
            "C桥startup来源与抑制层共享精确捕获预算");
        const auto plan = f.Plan(); const auto output = f.Output(plan);
        auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "c-bridge-frozen.w3x", output)).path));
        Check(owner.VerifyReadback(saved, kBudget));
        Check(plan.VerifyReadback(saved));
        for (const auto* name : {"Units/DestructableMetaData.slk", "war3map.w3b", "war3map.doo"}) {
            auto changed = f.Member(name);
            if (name == std::string_view("Units/DestructableMetaData.slk")) changed.push_back(std::byte{'\n'});
            else if (name == std::string_view("war3map.doo")) {
                auto placements = Take(ParseDoodadPlacements(changed)); placements.doodads[1].position[2] = 7;
                changed = Take(placements.Serialize());
            } else {
                const auto parsed = NativeObjectCodec(ObjectType::Destructable).Parse(changed);
                const auto* document = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
                Require(document != nullptr, "C桥冻结夹具W3B可完整读取");
                auto replacement = *document;
                Check(replacement.SetModification({ObjectType::Destructable, "BC01"}, {"brot"}, Value{0.0f}));
                changed = Take(NativeObjectCodec(ObjectType::Destructable).Serialize(replacement));
            }
            Require(changed != f.Member(name), "C桥冻结负例须实际变更来源成员字节");
            auto altered = output; altered.push_back({name, changed});
            Require(!owner.VerifyFrozenOutput(altered, {}).Succeeded(),
                ("C桥不放宽实际metadata/W3B及完整DOO冻结：" + std::string(name)).c_str());
            Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), altered).Succeeded(),
                "正常保存plan须拒绝同一冻结成员变化");
            const std::array<MemberReplacement, 1> replacement{{{name, changed}}};
            auto tampered = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root / (std::string(name) == "war3map.w3b"
                ? "c-bridge-native-changed.w3x" : std::string(name) == "war3map.doo" ? "c-bridge-doo-changed.w3x" : "c-bridge-meta-changed.w3x"), replacement)).path));
            Require(!owner.VerifyReadback(tampered, kBudget).Succeeded(), "C桥独立回读拒绝来源或无关摆放Z变化");
            Require(!plan.VerifyReadback(tampered).Succeeded(), "正常保存plan独立回读拒绝同一冻结成员变化");
        }
        Check(IO::WriteFileBytes(f.gameRoot / "WorldEditData.txt",
            Text(four ? "[DoodadCategories]\nC=same-C-new-source\n" : "[DoodadCategories]\nE=same-no-C-new-source\n")));
        Require(!owner.VerifyFrozenOutput(output, {}).Succeeded() && !owner.VerifyReadback(saved, kBudget).Succeeded(),
            "相同最终C选择不能代替startup来源CAS");
    }
    Fixture unknown; CategoryCBridge(unknown, true); unknown.gameRoot.clear(); unknown.Open(); unknown.Paint();
    const auto sourceSha = Take(Sha256File(unknown.archive->SourcePath()));
    const auto draft = unknown.ExpectedTerrain(true);
    Require(HasCode(unknown.Plan().SaveGate(), "TERRAIN_CATEGORY_SOURCE_REQUIRED") && unknown.edits->CanUndo()
        && unknown.edits->IsDirty() && Take(Sha256File(unknown.archive->SourcePath())) == sourceSha
        && Take(SerializeMapTerrain(*unknown.edits->Terrain())) == draft, "未知startup拒绝C桥保存并保留完整草稿与来源");
    for (const bool four : {false, true}) {
        Fixture wrongGrid; CategoryCBridge(wrongGrid, four);
        wrongGrid.Member("war3map.wpm") = CategoryCBridgeExpected(wrongGrid, false, !four, false);
        wrongGrid.Open(); wrongGrid.Paint();
        Require(HasCode(wrongGrid.Plan().SaveGate(), "TERRAIN_PATHING_SOURCE_UNEXPLAINED"), "C桥不能把错误旧网格当成作者保护位");
    }
}

void WalkableBridgesUseTheNormalSaveAndUndoChain() {
    for (const bool black : {false,true}) {
        Fixture f; WalkableBridge(f,black,true); f.Open(); f.Paint();
        const auto deep=BridgeExpectedPathing(f,true,black), shallow=BridgeExpectedPathing(f,false,black);
        Require(shallow==f.Member("war3map.wpm"),"literal bridge source and independent expected bytes agree");
        f.SaveExpected("bridge-deep.w3x",f.ExpectedTerrain(true),deep);
        Require(f.edits->Undo(),"undo saved bridge map water change");
        f.SaveExpected("bridge-shallow.w3x",f.ExpectedTerrain(false),shallow);
        Require(f.edits->Redo(),"redo saved bridge map water change");
        f.SaveExpected("bridge-deep-again.w3x",f.ExpectedTerrain(true),deep);
    }
}
void WalkableBridgesPreserveOverlappingSavedRestrictionIntent() {
    Fixture f; WalkableBridge(f); f.Open();
    TerrainBrush restriction; restriction.kind=TerrainBrushKind::PathingRestriction;
    restriction.shape=TerrainBrushShape::Square; restriction.size=1; restriction.pathingRestrictions=0x06; restriction.value=1;
    const TerrainStrokePoint overlap{544,512};
    Take(f.edits->EditTerrain(restriction,std::span(&overlap,1),false)); f.Paint();
    auto restricted=BridgeExpectedPathing(f,true,false); restricted[16+16*32+17]=std::byte{0x17};
    f.SaveExpected("bridge-saved-restriction.w3x",f.ExpectedTerrain(true),restricted);
    Require(f.edits->Undo(),"undo water after restriction save retains declared saved intent");
    restriction.value=0; Take(f.edits->EditTerrain(restriction,std::span(&overlap,1),false));
    const auto removed=BridgeExpectedPathing(f,false,false);
    Require(removed[16+16*32+17]==std::byte{0x15},"U11 G04 savedS06 currentS00 yields literal15, not11");
    f.SaveExpected("bridge-removed-restriction.w3x",f.ExpectedTerrain(false),removed);
}
void WalkableBridgeStateAndWholeDoodadFreezeRejectBeforePublication() {
    for (const unsigned mode : {0u,1u,2u,3u,4u,5u,6u}) {
        Fixture f; WalkableBridge(f);
        auto placements=Take(ParseDoodadPlacements(f.Member("war3map.doo")));
        if (mode<4) placements.doodads[0].flags=std::array<std::uint8_t,4>{0,1,3,4}[mode];
        else placements.doodads[0].lifePercent=std::array<std::uint8_t,3>{0,50,99}[mode-4];
        f.Member("war3map.doo")=Take(placements.Serialize()); f.Open(); f.Paint();
        const auto sourceSha=Take(Sha256File(f.archive->SourcePath()));
        const auto draft=Take(SerializeMapTerrain(*f.edits->Terrain())); const auto plan=f.Plan();
        Require(!plan.SaveGate().Succeeded() && HasCode(plan.SaveGate(),"TERRAIN_WALKABLE_DESTRUCTABLE_STATE"),
            "unsupported bridge flags or life cannot receive a save gate");
        Require(Take(Sha256File(f.archive->SourcePath()))==sourceSha && f.edits->IsDirty()
            && Take(SerializeMapTerrain(*f.edits->Terrain()))==draft,"bridge state refusal preserves original and unsaved draft");
    }
    for (const unsigned row : {0u,1u}) {
        Fixture f; WalkableBridge(f); f.Open(); f.Paint();
        auto placements=Take(ParseDoodadPlacements(f.Member("war3map.doo"))); placements.doodads[row].position[2]=16;
        Check(f.edits->EditDoodads(placements));
        if (row == 0) {
            const auto plan=f.Plan();
            Require(!plan.SaveGate().Succeeded() && HasCode(plan.SaveGate(),"TERRAIN_WALKABLE_DESTRUCTABLE_DOODADS_CHANGED"),
                "bridge Z remains frozen");
        } else {
            const auto wire=Take(placements.Serialize());
            f.SaveExpected("bridge-unrelated-z.w3x", f.ExpectedTerrain(true), BridgeExpectedPathing(f,true,false), &wire, true);
        }
    }
}
void WalkableBridgeSourceAndResourceDependenciesRemainBound() {
    Fixture f; WalkableBridge(f,true); f.Open(); f.Paint();
    const auto owner=Take(TerrainPathingSave::CaptureNative(*f.archive,f.original,*f.edits->Terrain(),{},kBudget));
    auto changed=f.Member(kBridgeTexture); changed[21]=std::byte{};
    const MemberReplacement replacement{std::string(kBridgeTexture),changed};
    Require(!owner.VerifyFrozenOutput(std::span(&replacement,1),{}).Succeeded(),"bridge TGA alpha bytes remain frozen even when RGB mask is unchanged");
    const std::array<std::string,1> removed{std::string(kBridgeTexture)};
    Require(!owner.VerifyFrozenOutput({},removed).Succeeded(),"bridge TGA removal cannot fall back to an unknown winner");
    auto changedPlan=f.Plan(); auto output=f.Output(changedPlan); output.push_back(replacement);
    Require(!changedPlan.VerifyForSave(*f.edits,f.archive->SourceSha256(),output).Succeeded(),"public plan rechecks bridge resource replacements");
    Fixture unexplained; WalkableBridge(unexplained); unexplained.Member("war3map.wpm")[16+16*32+18]^=std::byte{2};
    unexplained.Open(); unexplained.Paint(); const auto refused=unexplained.Plan();
    Require(!refused.SaveGate().Succeeded() && HasCode(refused.SaveGate(),"TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
        "bridge support cannot hide unexplained old WPM bits under the suppressed terrain");
    TerrainSavePlanLimits limits; limits.maximumBytes=owner.CaptureBudgetBytes();
    const auto bounded=TerrainPathingSave::PrepareForSave(*f.archive,*f.edits,f.gameRoot,limits);
    Require(!bounded.Succeeded() || (bounded.value && !bounded.value->SaveGate().Succeeded()),
        "capture bytes alone do not waive the complete save-plan output budget");
    const auto& diagnostics=bounded.Succeeded() && bounded.value ? bounded.value->SaveGate().diagnostics : bounded.diagnostics;
    Require(std::any_of(diagnostics.begin(),diagnostics.end(),[](const auto& diagnostic) {
        return diagnostic.code.find("BUDGET")!=std::string::npos;
    }),"bridge budget refusal must identify a budget gate rather than an unrelated failure");
}
void WalkableBridgesUseTheDirectoryRecipe() {
    Fixture f; WalkableBridge(f,true); f.Open(); const auto directory=f.Directory();
    const auto workspace=Take(DirectoryWorkspace::Open(directory)); f.Paint();
    const TerrainStrokeIntent intent{f.Water(),{f.point},false};
    const auto patch=Take(TerrainPathingSave::Seal(MapDocument::FromMembers(f.members),f.Member("war3map.w3e"),
        f.Member("war3map.wpm"),workspace.Snapshot(),*f.edits,std::span(&intent,1)));
    const auto found=std::find_if(patch.Files().begin(),patch.Files().end(),[](const auto& file){return file.relativePath.filename()=="war3map.wpm";});
    Require(found!=patch.Files().end() && found->replacement==BridgeExpectedPathing(f,true,true),
        "directory replay shares the bridge layer and complete independent WPM expected bytes");
    Require(Take(IO::ReadFileBytes(directory/"war3map.doo"))==f.Member("war3map.doo"),"directory sealing preserves full source DOO");
}

void ExistingDryRampsComposeWithOrdinaryAndSpecialSaveLayers(bool withMapMisc = false) {
    for (const bool special : {false, true}) {
        Fixture f; SpecialDoodad(f, true);
        // 精确累计解码/解析预算不等于微小MPQ成员的存储峰值；用合法空白固定本用例前提。
        f.Member("UI/WorldEditData.txt") = Text("[Other]\nValue=1\n" + std::string(128, '\n'));
        f.original.vertices[60].textureAndFlagsRaw = 0x10; // 等高孤立点分类L，不伪造可通行R。
        f.original.header.groundTilesets.push_back("Tg02");
        f.Member("war3map.w3e") = Take(SerializeMapTerrain(f.original));
        f.Member("TerrainArt/Terrain.slk") = Slk({"tileID","walkable","flyable","buildable"},
            {{"Tg01","1","1","1"},{"Tg02","0","1","1"}});
        auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
        if (!special) {
            placements.specialDoodads.clear();
            std::fill(f.Member("war3map.wpm").begin()+16, f.Member("war3map.wpm").end(), std::byte{0x51});
        }
        DoodadPlacement ordinary; ordinary.rawcode = "Ord0"; ordinary.position = {512,512,0};
        ordinary.scale = {1,1,1}; ordinary.flags = 2; ordinary.creationId = 1;
        placements.doodads.push_back(ordinary);
        f.Member("war3map.doo") = Take(placements.Serialize());
        f.Member("Doodads/Doodads.slk") = Slk({"doodID","code","pathTex","category","fixedRot"},
            {{"Doo0","Doo0",std::string(kUniformDoodadTexture),"C","45"},{"Ord0","Ord0","_","E","-1"}});
        if (withMapMisc) f.members.push_back({"war3mapMisc.txt", ConflictingMapMisc()});
        f.Open();
        TerrainBrush brush; brush.kind = TerrainBrushKind::Texture; brush.radius = 0;
        brush.groundTexture = "Tg02"; brush.variation = 0;
        const TerrainStrokePoint point{768,768}; Take(f.edits->EditTerrain(brush, std::span(&point,1), false));
        auto terrain = f.original; terrain.vertices[60].textureAndFlagsRaw = 0x11;
        auto pathing = f.Member("war3map.wpm");
        for (unsigned y=22; y<26; ++y) for (unsigned x=22; x<26; ++x) pathing[16+y*32+x] = std::byte{0x53};
        const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, kBudget));
        Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, owner.CaptureBudgetBytes()));
        Require(!TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot,
            owner.CaptureBudgetBytes()-1).Succeeded(),
            "typed ordinary parse work and ramp/special profile reservations retain one exact shared budget");
        if (withMapMisc) {
            const auto sourceSha = Take(Sha256File(f.archive->SourcePath()));
            const auto plan = f.Plan(); const auto output = f.Output(plan);
            auto changedMisc = output;
            auto miscBytes = ConflictingMapMisc(); const auto marker = Text("// independent map profile revision\r\n");
            miscBytes.insert(miscBytes.end(), marker.begin(), marker.end());
            changedMisc.push_back({"war3mapMisc.txt", std::move(miscBytes)});
            Check(owner.VerifyFrozenOutput(changedMisc, {}));
            Check(plan.VerifyForSave(*f.edits, sourceSha, changedMisc));
            const auto changed = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "ramp-map-misc-drift.w3x", changedMisc)).path));
            Check(owner.VerifyReadback(changed, kBudget)); Check(plan.VerifyReadback(changed));
            Require(!plan.VerifyFrozenOutput(std::string(64, '0'), changedMisc).Succeeded(),
                "independent map Misc never relaxes the save plan source archive identity");

            // 固定startup是实际计算依赖，值相同的注释漂移也必须被字节身份拒绝。
            auto startupBytes = f.Member("UI/MiscData.txt"); startupBytes.insert(startupBytes.end(), marker.begin(), marker.end());
            auto changedStartup = output; changedStartup.push_back({"UI/MiscData.txt", std::move(startupBytes)});
            Require(!owner.VerifyFrozenOutput(changedStartup, {}).Succeeded()
                && !plan.VerifyForSave(*f.edits, sourceSha, changedStartup).Succeeded(),
                "static ramp and special save still freeze every byte of the actual startup profile");
            const auto drift = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "ramp-startup-drift.w3x", changedStartup)).path));
            Require(!owner.VerifyReadback(drift, kBudget).Succeeded() && !plan.VerifyReadback(drift).Succeeded(),
                "static ramp and special readback rejects actual startup drift after map Misc was excluded");
            Require(Take(Sha256File(f.archive->SourcePath())) == sourceSha && f.edits->IsDirty()
                && f.edits->Revision() == 1 && f.edits->CanUndo() && !f.edits->CanRedo(),
                "dependency probes preserve the complete original archive and unsaved command history");
        }
        const auto expectedTerrain = Take(SerializeMapTerrain(terrain));
        f.SaveExpected("dry-ramp-layered.w3x", expectedTerrain, pathing, nullptr, withMapMisc);
        Require(f.edits->Undo(), "undo saved texture on existing dry ramp with typed object layers");
        f.SaveExpected("dry-ramp-layered-undone.w3x", f.Member("war3map.w3e"), f.Member("war3map.wpm"), nullptr, withMapMisc);
        Require(f.edits->Redo(), "redo saved texture on existing dry ramp with typed object layers");
        f.SaveExpected("dry-ramp-layered-redone.w3x", expectedTerrain, pathing, nullptr, withMapMisc);
    }
}
void MixedObjectAndResourceSavesUseActualPathingDependencies() {
    const auto unitBytes = [](std::int32_t life) {
        auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Unit));
        Check(document.AddOriginalObject("hfoo"));
        Check(document.AddModification({ObjectType::Unit, "hfoo"}, {"uhpm"}, NativeValueType::Int32, life));
        return Take(NativeObjectCodec(ObjectType::Unit).Serialize(document));
    };
    for (const bool existing : {false, true}) for (const bool blight : {false, true}) {
        Fixture f;
        if (existing) f.members.push_back({"war3map.w3u", unitBytes(0)});
        f.Open();
        if (blight) {
            TerrainBrush brush; brush.kind = TerrainBrushKind::Blight; brush.shape = TerrainBrushShape::Square;
            brush.radius = 8; brush.value = true;
            Take(f.edits->EditTerrain(brush, std::span(&f.point, 1), false));
        } else f.Paint();
        const auto sourceSha = Take(Sha256File(f.archive->SourcePath()));
        auto output = f.edits->Replacements();
        output.push_back({"war3map.w3u", unitBytes(1)});
        output.push_back({"war3mapImported/independent.blp", Text("independent author resource")});
        const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.archive, *f.edits, f.gameRoot, {}, output));
        Check(plan.SaveGate()); Require(plan.RebuiltPathing(), "mixed save replays complete source pathing before allowing unit HP");
        Check(plan.InstallPathingOutput(*f.edits, output));
        Check(plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output));
        const auto target = f.root / "mixed.w3x";
        auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(target, output)).path));
        Check(plan.VerifyReadback(saved));
        Require(Take(saved.ReadMember("war3map.w3u")) == unitBytes(1), "mixed unit HP is written exactly");
        auto expectedTerrain = f.original;
        if (blight) for (auto& vertex : expectedTerrain.vertices) vertex.textureAndFlagsRaw |= 0x20;
        else for (auto& vertex : expectedTerrain.vertices) vertex.waterHeightAndFlagsRaw = 8192 + 80 * 4;
        auto expectedPathing = f.ExpectedPathing(!blight);
        if (blight) for (std::size_t i = 16; i < expectedPathing.size(); ++i) expectedPathing[i] |= std::byte{0x20};
        Require(Take(saved.ReadMember("war3map.w3e")) == Take(SerializeMapTerrain(expectedTerrain)), "mixed save keeps complete independently expected terrain");
        Require(Take(saved.ReadMember("war3map.wpm")) == expectedPathing, "mixed save keeps complete independently expected WPM");
        Require(Take(saved.ReadMember("war3mapImported/independent.blp")) == Text("independent author resource"),
            "unrelated import is preserved in the same save");
        Require(Take(Sha256File(f.archive->SourcePath())) == sourceSha, "mixed save leaves source untouched");

        auto mutated = output;
        const auto unit = std::find_if(mutated.begin(), mutated.end(), [](const auto& member) { return member.name == "war3map.w3u"; });
        unit->data = unitBytes(2);
        Require(!plan.VerifyFrozenOutput(f.archive->SourceSha256(), mutated).Succeeded(), "final unit output cannot change after preparation");
        auto wrong = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "wrong-unit.w3x", mutated)).path));
        Require(!plan.VerifyReadback(wrong).Succeeded(), "readback independently catches modified unit HP including blight-only saves");
        mutated = output;
        std::erase_if(mutated, [](const auto& member) { return member.name == "war3map.w3u"; });
        Require(!plan.VerifyFrozenOutput(f.archive->SourceSha256(), mutated).Succeeded(), "prepared unit output cannot be silently omitted");
        Require(!plan.VerifyFrozenOutput(std::string(64, '0'), output).Succeeded(), "mixed save retains source identity check");
        Require(f.edits->Undo(), "mixed terrain edit remains undoable");
        Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output).Succeeded(), "mixed save retains revision check");
    }
    for (const bool remove : {false, true}) {
        Fixture f; f.Paint();
        auto output = f.edits->Replacements();
        std::vector<std::string> removals;
        if (remove) removals.push_back("TerrainArt/Terrain.slk");
        else output.push_back({"TerrainArt/Terrain.slk",
            Slk({"tileID", "walkable", "flyable", "buildable"}, {{"Tg01", "0", "1", "1"}})});
        const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.archive, *f.edits, f.gameRoot, {}, output, removals));
        Check(plan.SaveGate()); Check(plan.InstallPathingOutput(*f.edits, output));
        Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output, removals).Succeeded(),
            "actual static pathing dependency replacement or removal is still refused");
        Require(!std::filesystem::exists(f.root / "refused.w3x"), "dependency refusal does not publish partial output");
    }
    {
        Fixture f; f.Member("war3map.wpm")[16] |= std::byte{0x04}; f.Open(); f.Paint();
        auto output = f.edits->Replacements(); output.push_back({"war3map.w3u", unitBytes(1)});
        const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.archive, *f.edits, f.gameRoot, {}, output));
        Require(!plan.SaveGate().Succeeded() && HasCode(plan.SaveGate(), "TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
            "mixed save keeps the exact old-WPM rejection and its useful diagnostic");
    }
}
#include "TerrainOrdinaryDoodadSaveCases.h"
#include "TerrainOrdinarySpecialSaveCases.h"
#include "TerrainOrdinaryJournalSaveCases.h"
#include "TerrainUnitsJournalSaveCases.h"
#include "TerrainBridgeOrdinarySaveCases.h"
#include "TerrainGroundPaletteAppendSaveCases.h"
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
        StartupCategoryCUsesTheNormalSaveAndUndoChain();
        StartupCategoryCBudgetSourcesAndInheritanceStayBound();
        StartupCategoryCChoosesTheActualGridAndFreezesItsSources();
        EffectiveDoodadCategoriesUseTheNormalSaveAndUndoChain();
        StartupCategoryCPatternedRectangleUsesTheNormalSaveChain();
        NativeDoodadFootprintsUseTheNormalSaveAndUndoChain();
        UniformDoodadTextureIsFrozenAndBudgeted();
        UniformDoodadGameTextureWinnerRemainsBound();
        UniformDoodadLongestPathHasAnExactCaptureBudget();
        UniformDoodadDuplicateInstancesHaveABoundedPixelWorkload();
        UniformDoodadRejectionDoesNotWeakenOldSaves();
        PatternedDoodadsUseTheNormalSaveAndUndoChain();
        NativeFixedRotationUsesTheNormalSaveAndUndoChain();
        SpecialDoodadsSaveWaterGroundAndCliffs();
        SharedSpecialAndOrdinaryResourcesRoundTripOnce();
        SpecialWaterDeletionUsesTheNormalSaveAndUndoChain();
        ExistingDryRampsComposeWithOrdinaryAndSpecialSaveLayers();
        ExistingDryRampsComposeWithOrdinaryAndSpecialSaveLayers(true);
        WalkableBridgesUseTheNormalSaveAndUndoChain();
        WalkableBridgeSpecialDeletionRestoresAndUsesFormalSaveHistory();
        WalkableBridgeSpecialDeletionRestoresAndUsesFormalSaveHistory(true);
        WalkableBridgeSpecialDeletionKeepsOrdinaryAndProvenanceGates();
        CategoryCBridgesUseTheSharedSaveAndUndoChain();
        CategoryCBridgeSourcesAndOldPathingStayBound();
        BridgeOriginalUsesTheNormalSaveAndUndoChain();
        BridgeOriginalRejectsUnsupportedValuesAndMetadata();
        BridgeCustomUsesTheNormalSaveAndUndoChain();
        BridgeCustomRejectsUnsupportedValuesMetadataAndParents();
        BridgeOriginalDependenciesAndOldPathingRemainBound();
        WalkableBridgesPreserveOverlappingSavedRestrictionIntent();
        WalkableBridgeStateAndWholeDoodadFreezeRejectBeforePublication();
        WalkableBridgeSourceAndResourceDependenciesRemainBound();
        WalkableBridgesUseTheDirectoryRecipe();
        MixedObjectAndResourceSavesUseActualPathingDependencies();
        OrdinaryDoodadMutationsUseBothSaveStages();
        OrdinaryDoodadMoveSurvivesZeroNetTerrainAndHistory();
        OrdinaryDoodadSaveKeepsActualDependenciesAndBudgets();
        OrdinaryMutationsDoNotAcquireSpecialOrBridgeAuthority();
        OrdinaryMoveThenNativeWaterUsesTheCurrentDoodadOwner();
        OrdinarySpecialMutationsUseBothStagesAndSavedHistory();
        OrdinarySpecialProofKeepsPrivateSourcesAndFrozenOutput();
        OrdinarySpecialProofKeepsCompleteBridgeFreeze();
        OrdinaryJournalRecoveryPreservesBothOperationOrders();
        OrdinaryJournalRecoveryRejectsTamperingAndPublicPlaceholders();
        OrdinaryJournalRecoveryRetainsRepeatedInterleaving();
        UnitsJournalRecoveryRetainsWaterBothOrdersAndCompleteRaw();
        UnitsJournalInterleavingRejectsSourceOrderBudgetAndFinalDrift();
        BridgeAllowsOnlyUnrelatedOrdinaryMutationsThroughFormalHistory();
        BridgeRecordBytesOrderAndClassificationRemainMandatory();
        BridgeOrdinaryFrozenOutputSourcesAndBudgetRemainBound();
        GroundAppendJournalRestoresBothWaterOrdersAndFormalSave();
        GroundAppendCapturesTheFullResourceAndBeforeTable();
        GroundAppendsRetainOrderedIntermediateTablesAndWaterLineage();
        GroundAppendSixteenthEntryAndResourceFailuresStayBounded();
        std::cout << "Native object terrain save tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
