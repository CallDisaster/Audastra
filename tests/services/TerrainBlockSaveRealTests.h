#pragma once

#include "War3MapCore/MapArchiveSession.h"

namespace War3Map::TerrainSaveTests {
void CheckRealTerrainBlockSave(const MapArchiveSession& original, const std::filesystem::path& directory,
    const std::filesystem::path& tool, const std::filesystem::path& game);
}
