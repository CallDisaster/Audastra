#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/TerrainPathingSave.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::ObjectData;
using Services::TerrainPathingSave;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kBudget = 8u * 1024u * 1024u;
constexpr std::string_view kOldPath = "PathTextures/dual-stage-original.tga";
constexpr std::string_view kNewPath = "PathTextures/dual-stage-final.tga";

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Check(Status status) {
    if (status.Succeeded()) return;
    std::string text;
    for (const auto& diagnostic : status.diagnostics) text += diagnostic.code + ": " + diagnostic.message + '\n';
    throw std::runtime_error(text);
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) { Check({result.diagnostics}); throw std::runtime_error("missing result"); }
    return std::move(*result.value);
}
template<class T> bool HasCode(const T& value, std::string_view code) {
    return std::any_of(value.diagnostics.begin(), value.diagnostics.end(), [&](const auto& d) { return d.code == code; });
}
Bytes Text(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}
Bytes Slk(const std::vector<std::string>& columns, std::initializer_list<std::vector<std::string>> rows) {
    std::string text = "ID;PWXL;N;E\n";
    const auto append = [&](const auto& cells, std::size_t y) {
        for (std::size_t x = 0; x < cells.size(); ++x)
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y) + ";K\"" + cells[x] + "\"\n";
    };
    append(columns, 1); std::size_t row = 2;
    for (const auto& cells : rows) append(cells, row++);
    return Text(text + "E\n");
}
Bytes Texture(unsigned side, bool magenta) {
    Bytes bytes(18 + side * side * 3, std::byte{});
    bytes[2] = std::byte{2}; bytes[12] = bytes[14] = static_cast<std::byte>(side); bytes[16] = std::byte{24};
    for (std::size_t at = 18; at < bytes.size(); at += 3) {
        if (magenta) bytes[at] = bytes[at + 2] = std::byte{255};
        else bytes[at + 1] = std::byte{255};
    }
    return bytes;
}
Bytes PathOverride(std::string path) {
    const auto type = ObjectType::Doodad;
    auto document = Take(NativeObjectDocument::CreateEmpty(type));
    const auto id = "Doo0";
    Check(document.AddOriginalObject(id));
    Check(document.AddModification({type, id}, {"dpt0"}, NativeValueType::String, std::move(path)));
    return Take(NativeObjectCodec(type).Serialize(document));
}

struct Fixture {
    std::filesystem::path root, gameRoot;
    TerrainGridSnapshot original;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> archive;
    std::unique_ptr<MapDataEditSession> edits;
    unsigned sequence = 0;

    Fixture() {
        static std::atomic<unsigned> serial{0};
        root = std::filesystem::temp_directory_path() / ("Audastra-dual-stage-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(serial++));
        Require(std::filesystem::create_directory(root), "create owned synthetic fixture");
        gameRoot = root / "game"; Require(std::filesystem::create_directory(gameRoot), "create synthetic game root");
        Take(WriteVerifiedArchive(gameRoot / "war3.mpq", std::vector<MapArchiveMember>{{"synthetic-marker.txt", Text("owned fixture")}}));
        original.header.width = original.header.height = 9; original.header.mainTileset = 'L';
        original.header.groundTilesets = {"Tg01"}; original.header.cliffTilesets = {"Cl01"}; original.vertices.resize(81);
        for (auto& vertex : original.vertices) {
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 8192 + 48 * 4;
            vertex.textureAndFlagsRaw = 0x40; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(original, 0, 0, 9, 9);
        MapSettings settings; settings.formatVersion = 25; settings.custom.emplace(); settings.tileset = 'L';
        settings.playableWidth = settings.playableHeight = 8; settings.flags |= 0x400;
        DoodadPlacements doodads; DoodadPlacement placed;
        placed.rawcode = "Doo0"; placed.position = {512, 512, 0};
        placed.scale = {1, 1, 1}; placed.flags = 2; placed.lifePercent = 100; placed.creationId = 1;
        doodads.doodads.push_back(placed);
        members = {{"war3map.w3e", Take(SerializeMapTerrain(original))}, {"war3map.wpm", ExpectedPathing(false, 4, false)},
            {"war3map.w3i", Take(settings.Serialize())}, {"war3map.doo", Take(doodads.Serialize())},
            {"war3mapUnits.doo", Take(UnitPlacements{}.Serialize())},
            {"TerrainArt/Terrain.slk", Slk({"tileID", "walkable", "flyable", "buildable"}, {{"Tg01", "1", "1", "1"}})},
            {"TerrainArt/Water.slk", Slk({"waterID", "height", "impassable"}, {{"LSha", "0", "0"}})},
            {"UI/MiscData.txt", Text("[Water]\nDeepLevelPathing=64\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")},
            {"UI/WorldEditData.txt", Text("[Other]\nValue=1\n")},
            {"Units/DestructableData.slk", Slk({"destructableID", "code", "walkable", "pathTex", "fixedRot"},
                {{"Dst0", "Dst0", "0", std::string(kOldPath), "0"}})},
            {"Units/DestructableMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "data", "index"},
                {{"bwal", "walkable", "DestructableData", "bool", "0", "0", "0"},
                 {"bptx", "pathTex", "DestructableData", "pathingTexture", "0", "0", "0"}})},
            {"Doodads/Doodads.slk", Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
                {{"Doo0", "Doo0", std::string(kOldPath), "E", "0"}})},
            {"Doodads/DoodadMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "index"},
                {{"dpt0", "pathTex", "DoodadData", "pathingTexture", "0", "0"}})},
            {std::string(kOldPath), Texture(4, false)}, {"war3map.j", Text("// owned dual-stage fixture\n")},
            {"war3mapMap.tga", Text("owned preserved minimap")}, {"opaque-original.bin", Text("must remain unchanged")}};
        Open();
    }
    ~Fixture() { edits.reset(); archive.reset(); std::error_code error; std::filesystem::remove_all(root, error); }
    Bytes& Member(std::string_view name) {
        auto found = std::find_if(members.begin(), members.end(), [&](const auto& m) { return m.name == name; });
        Require(found != members.end(), "fixture member exists"); return found->data;
    }
    void Open() {
        archive = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(
            root / ("source-" + std::to_string(sequence++) + ".w3x"), members)).path)));
        edits = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(members));
        Check(edits->AttachTerrain(Member("war3map.w3e"))); Check(edits->AttachPathing(Member("war3map.wpm")));
    }
    void Paint() {
        TerrainBrush brush; brush.kind = TerrainBrushKind::WaterHeight; brush.shape = TerrainBrushShape::Square;
        brush.radius = 8; brush.waterHeight = 80; const TerrainStrokePoint point{512, 512};
        Take(edits->EditTerrain(brush, std::span(&point, 1), false));
    }
    static Bytes ExpectedPathing(bool deep, unsigned side, bool magenta) {
        Bytes bytes = Text("MP3W");
        for (const auto n : {0u, 32u, 32u}) for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::byte>((n >> shift) & 255));
        bytes.resize(16 + 32 * 32, deep ? std::byte{0x1b} : std::byte{0x19});
        // 独立字面预期：中心512、g=2；4格起点14、8格起点12。绿=04，紫=ca。
        const auto first = 16 - side / 2;
        for (unsigned y = first; y < first + side; ++y) for (unsigned x = first; x < first + side; ++x)
            bytes[16 + y * 32 + x] |= magenta ? std::byte{0xca} : std::byte{4};
        return bytes;
    }
    Bytes ExpectedTerrain(bool deep) const {
        auto grid = original;
        if (deep) for (auto& vertex : grid.vertices) vertex.waterHeightAndFlagsRaw = 8192 + 80 * 4;
        return Take(SerializeMapTerrain(grid));
    }
    std::vector<MemberReplacement> Final(bool nativeOverride = false) const {
        auto output = edits->Replacements();
        if (nativeOverride) {
            output.push_back({"war3map.w3d", PathOverride(std::string(kNewPath))});
            output.push_back({std::string(kNewPath), Texture(8, true)});
        } else output.push_back({std::string(kOldPath), Texture(8, true)});
        return output;
    }
    TerrainSavePlan Plan(std::span<const MemberReplacement> output, std::span<const std::string> removals = {}) const {
        return Take(TerrainPathingSave::PrepareForSave(*archive, *edits, gameRoot, {}, output, removals));
    }
    MapArchiveSession Save(TerrainSavePlan& plan, std::vector<MemberReplacement> output,
        const Bytes& expectedWpm, std::span<const std::string> removals = {}) {
        Check(plan.SaveGate()); Require(plan.RebuiltPathing(), "mixed terrain resource save must rebuild pathing");
        Check(plan.InstallPathingOutput(*edits, output));
        Check(plan.VerifyForSave(*edits, archive->SourceSha256(), output, removals));
        const auto sourcePath = archive->SourcePath();
        const auto sourceSha = Take(Sha256File(sourcePath));
        Services::MapSaveTask task; Check(task.FreezeArchive(*archive)); task.target = root / ("saved-" + std::to_string(sequence++) + ".w3x");
        task.replacements = output; task.removals.assign(removals.begin(), removals.end());
        task.beforePublish = [&] { return plan.VerifyForSave(*edits, task.sourceSha256, task.replacements, task.removals); };
        Services::MapSaveService::Run(task);
        if (!task.succeeded) Check({task.diagnostics});
        Require(task.succeeded && task.nextSession, "formal shared save worker publishes and reopens mixed output");
        auto saved = Take(MapArchiveSession::Open(task.written)); Check(plan.VerifyReadback(saved));
        Require(Take(saved.ReadMember("war3map.wpm")) == expectedWpm, "complete WPM equals independent static footprint and water expectation");
        Require(Take(saved.ReadMember("war3map.w3e")) == Take(SerializeMapTerrain(*edits->Terrain())), "complete edited W3E survives formal archive readback");
        for (const auto& replacement : output)
            Require(Take(saved.ReadMember(replacement.name)) == replacement.data, "every frozen final member survives exact readback");
        for (const auto& removed : removals) Require(!saved.ReadMember(removed).Succeeded(), "removed final map member remains absent");
        Require(Take(saved.ReadMember("opaque-original.bin")) == Member("opaque-original.bin"), "unrelated opaque member remains byte exact");
        Require(Take(Sha256File(sourcePath)) == sourceSha, "source archive remains unchanged");
        return saved;
    }
};

void ActualStaticTextureAndNativeOverridesSaveTogether() {
    for (const bool nativeOverride : {false, true}) {
        Fixture f; f.Paint(); auto output = f.Final(nativeOverride); auto plan = f.Plan(output);
        auto saved = f.Save(plan, output, Fixture::ExpectedPathing(true, 8, true));
        const auto identity = f.edits->SessionIdentity(), revision = f.edits->Revision(), sourceRevision = f.edits->TerrainSourceRevision();
        Check(plan.CommitSaved(*f.edits, MapDocument::FromArchive(saved)));
        Require(!f.edits->IsDirty() && f.edits->SessionIdentity() == identity && f.edits->Revision() == revision
            && f.edits->TerrainSourceRevision() == sourceRevision + 1, "mixed save rebases the existing session without adding history");
        f.archive = std::make_unique<MapArchiveSession>(std::move(saved));
        Require(f.edits->Undo(), "saved terrain Undo remains available after final resource replacement");
        auto undoOutput = f.edits->Replacements(); auto undoPlan = f.Plan(undoOutput);
        auto undone = f.Save(undoPlan, undoOutput, Fixture::ExpectedPathing(false, 8, true));
        Require(Take(undone.ReadMember("war3map.w3e")) == f.ExpectedTerrain(false), "saved Undo uses the new archive resource baseline and original shallow terrain");
        Check(undoPlan.CommitSaved(*f.edits, MapDocument::FromArchive(undone)));
        f.archive = std::make_unique<MapArchiveSession>(std::move(undone));
        Require(f.edits->Redo(), "saved terrain Redo remains available with the replaced resource baseline");
        auto redoOutput = f.edits->Replacements(); auto redoPlan = f.Plan(redoOutput);
        auto redone = f.Save(redoPlan, redoOutput, Fixture::ExpectedPathing(true, 8, true));
        Require(Take(redone.ReadMember("war3map.w3e")) == f.ExpectedTerrain(true), "saved Redo retains final assets and restores full deep-water terrain");
    }
}

void NewResourcesCannotExplainAnInvalidOldSource() {
    Fixture f;
    // 仅新紫色8格资源才解释得通的旧WPM；旧绿色4格来源必须先被G0拒绝。
    f.Member("war3map.wpm") = Fixture::ExpectedPathing(false, 8, true); f.Open(); f.Paint();
    const auto output = f.Final(); const auto plan = f.Plan(output);
    Require(!plan.SaveGate().Succeeded() && HasCode(plan.SaveGate(), "TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
        "new final assets cannot contaminate the explanation of the actual old WPM");
}

void NonWalkableDestructableOverridesRemainDynamic() {
    Fixture f;
    auto placed = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    auto dynamic = placed.doodads.front(); dynamic.rawcode = "Dst0"; dynamic.position = {256, 256, 0}; dynamic.creationId = 2;
    placed.doodads.push_back(dynamic); f.Member("war3map.doo") = Take(placed.Serialize()); f.Open(); f.Paint();
    auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Destructable));
    Check(document.AddOriginalObject("Dst0"));
    Check(document.AddModification({ObjectType::Destructable, "Dst0"}, {"bptx"}, NativeValueType::String, std::string(kNewPath)));
    auto output = f.edits->Replacements();
    output.push_back({"war3map.w3b", Take(NativeObjectCodec(ObjectType::Destructable).Serialize(document))});
    output.push_back({std::string(kNewPath), Texture(8, true)});
    // walkable=0的普通可破坏物保留运行时动态占地，不能把新pathTex烘进静态WPM。
    auto plan = f.Plan(output); f.Save(plan, output, Fixture::ExpectedPathing(true, 4, false));
}

void FinalDuplicateAndRemovalConflictsAreRejected() {
    Fixture f; f.Paint();
    for (unsigned kind = 0; kind < 4; ++kind) {
        auto output = f.Final(); std::vector<std::string> removals;
        if (kind == 0) output.push_back({"patHTexTures\\DUAL-STAGE-ORIGINAL.tga", Texture(4, false)});
        if (kind == 1) removals.emplace_back("PATHTEXTURES\\DUAL-STAGE-ORIGINAL.TGA");
        if (kind == 2) { removals.emplace_back("not-present.bin"); removals.emplace_back("NOT-PRESENT.bin"); }
        if (kind == 3) output.push_back({"../unsafe.tga", Texture(4, false)});
        const auto result = TerrainPathingSave::PrepareForSave(*f.archive, *f.edits, f.gameRoot, {}, output, removals);
        Require(!result.Succeeded() && !result.value, "canonical duplicates, replace/remove conflicts and unsafe paths fail before qualification");
        if (kind < 3) Require(HasCode(result, "TERRAIN_FINAL_INPUT_CONFLICT"), "canonical conflict has an explicit diagnostic");
    }
    const auto before = f.edits->Revision();
    auto output = f.Final(); TerrainSavePlanLimits tiny{32};
    Require(!TerrainPathingSave::PrepareForSave(*f.archive, *f.edits, f.gameRoot, tiny, output).Succeeded(),
        "overlay metadata does not escape a narrowed save budget");
    Require(f.edits->Revision() == before, "failed final overlay preparation never edits the session");
}

void FinalViewMustContainTheFrozenSessionAndRemainImmutable() {
    Fixture f; f.Paint();
    auto placed = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    placed.doodads.front().position[2] = 1;
    Check(f.edits->EditDoodads(placed));
    for (const auto omitted : {"war3map.w3e", "war3map.doo"}) {
        auto output = f.Final();
        std::erase_if(output, [&](const auto& member) { return member.name == omitted; });
        const auto result = TerrainPathingSave::PrepareForSave(*f.archive, *f.edits, f.gameRoot, {}, output);
        Require(!result.Succeeded() || !result.value || !result.value->SaveGate().Succeeded(),
            "G1 final view cannot silently restore the source W3E or DOO over session changes");
    }
    auto output = f.Final(); auto plan = f.Plan(output); Check(plan.SaveGate());
    Check(plan.InstallPathingOutput(*f.edits, output));
    auto asset = std::find_if(output.begin(), output.end(), [](const auto& member) { return member.name == kOldPath; });
    Require(asset != output.end(), "find final frozen texture"); asset->data = Texture(4, false);
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output).Succeeded(),
        "post-capture final texture mutation fails the original save plan");
}

void DeletionFallsBackToActualGameAndNeverOldMapBytes() {
    for (const bool gameHasTexture : {false, true}) {
        Fixture f;
        if (gameHasTexture) Take(WriteVerifiedArchive(f.gameRoot / "war3patch.mpq",
            std::vector<MapArchiveMember>{{std::string(kOldPath), Texture(8, true)}}));
        f.Paint(); auto output = f.edits->Replacements(); const std::array<std::string, 1> removals{std::string(kOldPath)};
        const auto prepared = TerrainPathingSave::PrepareForSave(*f.archive, *f.edits, f.gameRoot, {}, output, removals);
        if (!gameHasTexture) {
            Require(!prepared.Succeeded() || !prepared.value || !prepared.value->SaveGate().Succeeded(),
                "deleted map texture without actual game fallback cannot reuse old map bytes");
            continue;
        }
        auto plan = Take(prepared); f.Save(plan, output, Fixture::ExpectedPathing(true, 8, true), removals);
    }
}

void LastOrdinaryObjectAndNativeTableDeletionStayMapOnly() {
    Fixture f;
    f.members.push_back({"war3map.w3d", PathOverride(std::string(kOldPath))});
    // 地图结构不是游戏资源；即便游戏MPQ含同名坏W3D，删除后也必须保持map missing。
    Take(WriteVerifiedArchive(f.gameRoot / "war3patch.mpq",
        std::vector<MapArchiveMember>{{"war3map.w3d", Text("not a map object table")}}));
    f.Open(); f.Paint();
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    placements.doodads.clear(); Check(f.edits->EditDoodads(placements));
    auto output = f.edits->Replacements(); const std::array<std::string, 1> removals{"war3map.w3d"};
    auto plan = f.Plan(output, removals);
    auto saved = f.Save(plan, output, Fixture::ExpectedPathing(true, 0, false), removals);
    Require(Take(saved.ReadMember("war3map.doo")) == Take(placements.Serialize()), "last ordinary object is really absent from saved DOO");
    Require(!saved.ReadMember("war3map.w3d").Succeeded(), "removed native map table is not replaced by a game member");
    Require(Take(saved.ReadMember(std::string(kOldPath))) == Texture(4, false), "unconsumed original footprint asset stays byte exact");
}

void SharedBridgeTextureCannotChangeThroughStaticObject() {
    Fixture f;
    auto placed = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    auto bridge = placed.doodads.front(); bridge.rawcode = "Br01"; bridge.creationId = 2; placed.doodads.push_back(bridge);
    f.Member("war3map.doo") = Take(placed.Serialize());
    f.Member("Units/DestructableData.slk") = Slk({"destructableID", "code", "walkable", "pathTex", "fixedRot"},
        {{"Br01", "Br01", "1", std::string(kOldPath), "0"}});
    auto& wpm = f.Member("war3map.wpm");
    for (unsigned y = 14; y < 18; ++y) for (unsigned x = 14; x < 18; ++x) wpm[16 + y * 32 + x] = std::byte{0x15};
    f.Open(); f.Paint(); const auto unchangedOutput = f.edits->Replacements();
    auto unchanged = f.Plan(unchangedOutput); Check(unchanged.SaveGate());
    auto expected = Fixture::ExpectedPathing(true, 4, false);
    for (unsigned y = 14; y < 18; ++y) for (unsigned x = 14; x < 18; ++x) expected[16 + y * 32 + x] = std::byte{0x15};
    f.Save(unchanged, unchangedOutput, expected);
    auto output = f.Final();
    const auto changed = TerrainPathingSave::PrepareForSave(*f.archive, *f.edits, f.gameRoot, {}, output);
    Require(!changed.Succeeded() || !changed.value || !changed.value->SaveGate().Succeeded(),
        "a static object import cannot change a pathing resource also consumed by a frozen walkable bridge");
}

void DirectorySourcesUseTheSameFinalView() {
    Fixture f; f.Paint(); const auto directory = f.root / "workspace";
    Require(std::filesystem::create_directory(directory), "create owned directory source");
    for (const auto& member : f.members) {
        const auto path = directory / member.name; std::filesystem::create_directories(path.parent_path());
        Check(IO::WriteFileBytes(path, member.data));
    }
    const auto workspace = Take(DirectoryWorkspace::Open(directory)); auto output = f.Final(true);
    auto plan = Take(TerrainPathingSave::PrepareForSave(workspace.Snapshot(), *f.edits, f.gameRoot, {}, output));
    Check(plan.SaveGate()); Require(plan.RebuiltPathing(), "workspace source receives the same trusted G0/G1 reconstruction");
    Check(plan.InstallPathingOutput(*f.edits, output));
    const auto found = std::find_if(output.begin(), output.end(), [](const auto& member) { return member.name == "war3map.wpm"; });
    Require(found != output.end() && found->data == Fixture::ExpectedPathing(true, 8, true),
        "directory final view produces the same complete independent WPM");
    Check(workspace.VerifyUnchanged());
}
#include "TerrainFinalDependenciesSaveCases.h"
}

int main() {
    try {
        ActualStaticTextureAndNativeOverridesSaveTogether();
        NewResourcesCannotExplainAnInvalidOldSource();
        NonWalkableDestructableOverridesRemainDynamic();
        FinalDuplicateAndRemovalConflictsAreRejected();
        FinalViewMustContainTheFrozenSessionAndRemainImmutable();
        DeletionFallsBackToActualGameAndNeverOldMapBytes();
        LastOrdinaryObjectAndNativeTableDeletionStayMapOnly();
        SharedBridgeTextureCannotChangeThroughStaticObject();
        DirectorySourcesUseTheSameFinalView();
        FinalDependencyCases();
        std::cout << "Terrain dual-stage pathing save cases passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
