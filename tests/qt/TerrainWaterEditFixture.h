#pragma once

#include "EditorProject.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

#include <stdexcept>

namespace War3Map::Tests {
template<class T> T WaterTake(Result<T> value) {
    if (!value.Succeeded()) {
        std::string message = "water fixture failed";
        for (const auto& diagnostic : value.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*value.value);
}
inline std::vector<std::byte> WaterText(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}
inline std::vector<MapArchiveMember> TerrainWaterMembers() {
    MapData::TerrainGridSnapshot grid;
    grid.header.width = grid.header.height = 9;
    grid.header.mainTileset = 'L';
    grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"Cl01"};
    grid.vertices.resize(81);
    for (auto& vertex : grid.vertices) {
        vertex.groundHeightRaw = 8192;
        vertex.waterHeightAndFlagsRaw = 8192;
        vertex.cliffAndLayerRaw = 2;
    }
    MapData::RefreshTerrainDerived(grid, 0, 0, 9, 9);
    return {{"war3map.w3e", WaterTake(MapData::SerializeMapTerrain(grid))},
        {"TerrainArt/Terrain.slk", WaterText("ID;PWXL;N;E\nB;X4;Y2;D0\nC;X1;Y1;K\"tileID\"\n"
            "C;X2;K\"walkable\"\nC;X3;K\"flyable\"\nC;X4;K\"buildable\"\n"
            "C;X1;Y2;K\"Tg01\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\nE\n")},
        {"TerrainArt/Water.slk", WaterText("ID;PWXL;N;E\nB;X2;Y2;D0\nC;X1;Y1;K\"waterID\"\n"
            "C;X2;K\"height\"\nC;X1;Y2;K\"LSha\"\nC;X2;K0.25\nE\n")},
        {"TerrainArt/CliffTypes.slk", WaterText("ID;PWXL;N;E\nB;X1;Y2;D0\nC;X1;Y1;K\"cliffID\"\n"
            "C;X1;Y2;K\"Cl01\"\nE\n")},
        {"UI/MiscData.txt", WaterText("[Terrain]\nMaxSlope=30\nMinHeight=-256\nMaxHeight=1536\n"
            "[Water]\nDeepLevelPathing=64\n")},
        {"UI/WorldEditData.txt", WaterText("[Unused]\nValue=1\n")}};
}

inline void InstallTerrainWaterProject(QtProject::EditorProject& project, const std::filesystem::path& root,
    bool archive, const std::vector<MapArchiveMember>& members = TerrainWaterMembers()) {
    project.war3GameRoot_ = root / "game";
    std::filesystem::create_directories(project.war3GameRoot_);
    const std::array gameMembers{MapArchiveMember{"unrelated.txt", WaterText("owned test archive")}};
    (void)WaterTake(WriteVerifiedArchive(project.war3GameRoot_ / "war3.mpq", gameMembers));
    project.projectRoot_ = root / "map";
    std::filesystem::create_directories(project.projectRoot_);
    if (archive) {
        project.sourceArchivePath_ = root / "source.w3x";
        (void)WaterTake(WriteVerifiedArchive(project.sourceArchivePath_, members));
        project.archiveSession_ = std::make_shared<MapArchiveSession>(WaterTake(MapArchiveSession::Open(project.sourceArchivePath_)));
        project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    } else {
        for (const auto& member : members) {
            const auto path = project.projectRoot_ / member.name;
            std::filesystem::create_directories(path.parent_path());
            if (!IO::WriteFileBytes(path, member.data).Succeeded()) throw std::runtime_error("write owned water source");
        }
        project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(WaterTake(DirectoryWorkspace::Open(project.projectRoot_)));
        project.mapDocument_ = std::make_unique<MapDocument>(WaterTake(MapDocument::FromDirectory(*project.directorySnapshot_)));
    }
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    if (!project.mapEdits_->AttachTerrain(members.front().data).Succeeded()) throw std::runtime_error("attach water terrain");
    project.ResetEditTracking();
}
} // namespace War3Map::Tests
