#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Services/TerrainPathingSave.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::ObjectData;
using War3Map::Services::TerrainPathingSave;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kBudget = 8u * 1024u * 1024u;

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
        return Take(TerrainPathingSave::PrepareForSave(*archive, *edits, {}, limits));
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
    void Save(const char* name, bool deep) {
        const auto sourcePath = archive->SourcePath();
        const auto sourceSha = Take(Sha256File(sourcePath));
        const auto plan = Plan(); Check(plan.SaveGate()); Require(plan.RebuiltPathing(), "native pathing rebuilt");
        const auto output = Output(plan);
        auto saved = Take(MapArchiveSession::Open(Take(archive->SaveCandidate(root / name, output)).path));
        Check(plan.VerifyReadback(saved));
        Require(Take(saved.ReadMember("war3map.w3e")) == ExpectedTerrain(deep), "full independent W3E expected bytes");
        Require(Take(saved.ReadMember("war3map.wpm")) == ExpectedPathing(deep), "full independent WPM expected bytes");
        for (const auto& member : members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm")
            Require(Take(saved.ReadMember(member.name)) == member.data, "all unrelated members preserved exactly");
        Require(Take(Sha256File(sourcePath)) == sourceSha, "save does not rewrite source archive");
        Check(plan.CommitSaved(*edits, MapDocument::FromArchive(saved)));
        Require(!edits->IsDirty(), "verified saved source becomes the clean baseline");
        archive = std::make_unique<MapArchiveSession>(std::move(saved));
    }
    std::filesystem::path Directory() const {
        const auto directory = root / "directory"; Require(std::filesystem::create_directory(directory), "private directory fixture");
        for (const auto& member : members) {
            const auto path = directory / member.name; std::filesystem::create_directories(path.parent_path());
            Check(IO::WriteFileBytes(path, member.data));
        }
        return directory;
    }
};

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
    Require(!refused.SaveGate().Succeeded() && HasCode(refused.SaveGate(), "TERRAIN_NATIVE_SPECIAL_UNSUPPORTED"),
        "special DOO never inherits normal typed object qualification");
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
} // namespace

int main() {
    try {
        TypedOverrideUsesTheNormalSaveAndUndoChain();
        DependencyReplacementRemovalAndReadbackStayBound();
        NativeObjectsDoNotWeakenSourceCasOrOldPathingChecks();
        MapAndGameMetadataWinnersRemainBound();
        DirectoryRecipePreservesReplayAndSourceChecks();
        std::cout << "Native object terrain save tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
