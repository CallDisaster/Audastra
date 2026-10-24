#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Check(Status status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& diagnostic : status.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
    throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) { Check({result.diagnostics}); throw std::runtime_error("missing value"); }
    return std::move(*result.value);
}
Bytes Text(std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text.data(), text.size())); return {bytes.begin(), bytes.end()};
}
bool HasCode(const Status& status, std::string_view code) {
    return std::any_of(status.diagnostics.begin(), status.diagnostics.end(), [&](const auto& item) { return item.code == code; });
}

struct Fixture {
    std::filesystem::path directory;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> archive;
    std::unique_ptr<MapDataEditSession> edits;
    Fixture() {
        static std::atomic<unsigned> sequence{0};
        directory = std::filesystem::temp_directory_path() / ("Audastra-bounds-session-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
        Require(std::filesystem::create_directory(directory), "create private fixture directory");
        TerrainGridSnapshot grid;
        grid.header.width = 11; grid.header.height = 9; grid.header.mainTileset = 'L';
        grid.header.offset = {-640, -512}; grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"Cl01"};
        grid.vertices.resize(99);
        for (auto& vertex : grid.vertices) {
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 8192; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(grid, 0, 0, 11, 9);
        Bytes wpm = Text("MP3W");
        for (const auto value : {0u, 40u, 32u}) for (unsigned shift = 0; shift < 32; shift += 8)
            wpm.push_back(std::byte((value >> shift) & 255));
        wpm.resize(16 + 40 * 32, std::byte{0x51});
        MapSettings settings; settings.formatVersion = 25; settings.custom.emplace(); settings.tileset = 'L';
        settings.mapName = "synthetic bounds"; settings.flags = 0x400;
        settings.playableWidth = 10; settings.playableHeight = 8;
        settings.cameraBounds = {-512, -384, 384, 256, -512, 256, 384, -384};
        const DoodadPlacements doodads; const UnitPlacements units;
        members = {{"war3map.w3e", Take(SerializeMapTerrain(grid))}, {"war3map.wpm", std::move(wpm)},
            {"war3map.w3i", Take(settings.Serialize())}, {"war3map.doo", Take(doodads.Serialize())},
            {"war3mapUnits.doo", Take(units.Serialize())}, {"war3mapMap.tga", Text("preserved custom image")},
            {"TerrainArt/Terrain.slk", Text("ID;PWXL;N;E\nB;X4;Y2;D0\n"
                "C;X1;Y1;K\"tileID\"\nC;X2;K\"walkable\"\nC;X3;K\"flyable\"\nC;X4;K\"buildable\"\n"
                "C;X1;Y2;K\"Tg01\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\nE\n")},
            {"UI/MiscData.txt", Text("[CameraMargins]\nLeft=1\nRight=2\nBottom=1\nTop=2\n")},
            {"UI/WorldEditData.txt", Text("[Other]\nValue=1\n")}};
        archive = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(directory / "source.w3x", members)).path)));
        edits = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(members));
        Check(edits->AttachTerrain(members[0].data)); Check(edits->AttachPathing(members[1].data));
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(directory, error); }
    TerrainPathingResources Capture() const {
        const auto before = Take(ParseMapTerrain(Take(archive->ReadMember("war3map.w3e"))));
        return Take(TerrainPathingResources::CaptureNative(*archive, *before.Snapshot(), *edits->Terrain(), {}, 64u * 1024u * 1024u));
    }
    void Save(const char* name, bool rebuild = true) {
        const auto plan = Take(TerrainSavePlan::PrepareForSave(*archive, *edits, {}));
        Check(plan.SaveGate()); Require(plan.RebuiltPathing() == rebuild, "bounds save chooses native G0/G1");
        auto output = edits->Replacements(); Check(plan.InstallPathingOutput(*edits, output));
        Check(plan.VerifyForSave(*edits, archive->SourceSha256(), output));
        auto saved = Take(MapArchiveSession::Open(Take(archive->SaveCandidate(directory / name, output)).path));
        Check(plan.VerifyReadback(saved)); Check(plan.CommitSaved(*edits, MapDocument::FromArchive(saved)));
        Require(!edits->IsDirty(), "actual saved output becomes clean baseline");
        Require(Take(saved.ReadMember("war3mapMap.tga")) == members[5].data, "custom minimap remains byte-identical");
        archive = std::make_unique<MapArchiveSession>(std::move(saved));
    }
};

void BoundsSaveHasLiteralFullByteOraclesAndSavedUndo() {
    Fixture f;
    auto expectedTerrain = f.members[0].data;
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 10; ++x)
        if (x < 1 || x >= 9 || y < 1 || y >= 7) expectedTerrain[45 + (y * 11 + x) * 7 + 3] |= std::byte{0x40};
    auto expectedSettings = Take(MapSettings::Parse(f.members[2].data));
    expectedSettings.cameraBounds = {-384, -256, 256, 128, -384, 128, 256, -256};
    expectedSettings.cameraMargins = {1, 1, 1, 1}; expectedSettings.playableWidth = 8; expectedSettings.playableHeight = 6;
    const auto expectedW3i = Take(expectedSettings.Serialize());
    auto expectedPathing = f.members[1].data;
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 40; ++x)
        if (x / 4 < 1 || x / 4 >= 9 || y / 4 < 1 || y / 4 >= 7) expectedPathing[16 + y * 40 + x] = std::byte{0xdf};
    const auto resources = f.Capture();
    const auto prepared = Take(f.edits->PrepareTerrainBoundsEdit(resources, {{1, 1, 9, 7}}));
    const auto revision = f.edits->Revision();
    Require(prepared.HasChanges() && prepared.ChangedMapEdges() == 32, "exact SW edge count for non-square inset");
    Check(f.edits->ApplyPreparedTerrainBoundsEdit(prepared));
    Require(f.edits->Revision() == revision + 1 && f.edits->HasTerrainBoundsChanges(), "one compound command advances one revision");
    Check(f.edits->VerifyTerrainSettingsForSave());
    Require(!f.edits->VerifyTerrainPathingForSave().Succeeded(), "preservation-only route cannot approve new bounds");
    f.Save("inset.w3x");
    Require(Take(f.archive->ReadMember("war3map.w3e")) == expectedTerrain, "complete W3E matches independent MapEdge byte oracle");
    Require(Take(f.archive->ReadMember("war3map.w3i")) == expectedW3i, "complete W3I matches independent camera/margin oracle");
    Require(Take(f.archive->ReadMember("war3map.wpm")) == expectedPathing, "complete WPM moves only native playable restrictions and preserves 0x11");
    Require(f.edits->Undo(), "undo bounds after native save"); Check(f.edits->VerifyTerrainSettingsForSave()); f.Save("undo.w3x");
    Require(Take(f.archive->ReadMember("war3map.w3e")) == f.members[0].data
        && Take(f.archive->ReadMember("war3map.w3i")) == f.members[2].data
        && Take(f.archive->ReadMember("war3map.wpm")) == f.members[1].data, "undo/save restores all three complete source members");
    Require(f.edits->Redo(), "redo bounds across second saved baseline"); f.Save("redo.w3x");
    Require(Take(f.archive->ReadMember("war3map.w3e")) == expectedTerrain
        && Take(f.archive->ReadMember("war3map.w3i")) == expectedW3i
        && Take(f.archive->ReadMember("war3map.wpm")) == expectedPathing, "redo/save recreates complete deterministic bytes");
}

void PreparationNoOpStaleAndPrepublishAreAtomic() {
    Fixture f;
    const auto owner = f.Capture();
    const auto noOp = Take(f.edits->PrepareTerrainBoundsEdit(owner, {{0, 0, 10, 8}}));
    Require(!noOp.HasChanges(), "already canonical full bounds form a no-op");
    Check(f.edits->ApplyPreparedTerrainBoundsEdit(noOp)); Require(f.edits->Revision() == 0 && !f.edits->CanUndo(), "no-op consumes no revision or history");
    const auto prepared = Take(f.edits->PrepareTerrainBoundsEdit(owner, {{1, 1, 9, 7}}));
    const auto bytes = prepared.PreparationBytes(), work = prepared.PreparationWork();
    Require(f.edits->PrepareTerrainBoundsEdit(owner, {{1, 1, 9, 7}}, bytes, work).Succeeded(), "exact candidate/work budgets succeed");
    Require(!f.edits->PrepareTerrainBoundsEdit(owner, {{1, 1, 9, 7}}, bytes - 1, work).Succeeded()
        && !f.edits->PrepareTerrainBoundsEdit(owner, {{1, 1, 9, 7}}, bytes, work - 1).Succeeded(), "one-unit-low budgets reject before mutation");
    const auto rejected = f.edits->ApplyPreparedTerrainBoundsEdit(prepared, [](const auto&) {
        return Status::Failure("Test", "BOUNDS_PREPUBLISH_REJECTED", "synthetic refusal");
    });
    Require(HasCode(rejected, "BOUNDS_PREPUBLISH_REJECTED") && f.edits->Revision() == 0 && !f.edits->IsDirty(), "prepublish refusal is fully atomic");
    auto settings = *f.edits->Snapshot().Settings(); settings.mapName = "changed description field";
    Check(f.edits->EditSettings(settings));
    Require(HasCode(f.edits->ApplyPreparedTerrainBoundsEdit(prepared), "TERRAIN_BOUNDS_EDIT_STALE"), "any intervening revision rejects prepared bounds");
    const auto fresh = Take(f.edits->PrepareTerrainBoundsEdit(owner, {{1, 1, 9, 7}}));
    Check(f.edits->ApplyPreparedTerrainBoundsEdit(fresh)); Check(f.edits->VerifyTerrainSettingsForSave());
    auto tampered = *f.edits->Snapshot().Settings(); tampered.cameraBounds[0] += 128;
    Require(HasCode(f.edits->EditSettings(tampered), "TERRAIN_BOUNDS_TYPED_COMMAND_REQUIRED"), "raw settings cannot forge typed bounds proof");
    Check(f.edits->VerifyTerrainSettingsForSave());
    f.Save("description-and-bounds.w3x");
    Require(f.edits->Snapshot().Settings()->mapName == "changed description field", "four description fields combine with owned geometry");
    auto afterSave = *f.edits->Snapshot().Settings(); afterSave.cameraMargins[0] += 1;
    const auto revision = f.edits->Revision();
    Require(HasCode(f.edits->EditSettings(afterSave), "TERRAIN_BOUNDS_TYPED_COMMAND_REQUIRED")
        && HasCode(f.edits->SetSerialized(MapDataKind::Settings, Take(afterSave.Serialize())), "TERRAIN_BOUNDS_TYPED_COMMAND_REQUIRED"),
        "both raw write paths reject camera-margin tampering even after bounds were saved");
    Require(f.edits->Revision() == revision && !f.edits->IsDirty(), "saved bounds tampering preserves revision and clean bytes");
    const auto unchangedPlan = Take(TerrainSavePlan::PrepareForSave(*f.archive, *f.edits, {}));
    Check(unchangedPlan.SaveGate());
}
}

int main() {
    try {
        BoundsSaveHasLiteralFullByteOraclesAndSavedUndo();
        PreparationNoOpStaleAndPrepublishAreAtomic();
        std::cout << "terrain bounds session tests passed\n"; return 0;
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
