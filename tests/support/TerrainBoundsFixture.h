#pragma once

#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/Services/TerrainPathingSave.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>

namespace AudastraTest::TerrainBounds {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;
inline void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
inline void Check(Status status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& diagnostic : status.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
    throw std::runtime_error(message);
}
template<class T> inline T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) { Check({result.diagnostics}); throw std::runtime_error("missing value"); }
    return std::move(*result.value);
}
inline Bytes Text(std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text.data(), text.size())); return {bytes.begin(), bytes.end()};
}
inline bool HasCode(const Status& status, std::string_view code) {
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

} // namespace AudastraTest::TerrainBounds
