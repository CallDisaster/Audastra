#pragma once
#include "War3MapCore/Services/MapEditSaveService.h"

namespace War3Map::TerrainSaveTests {
void CheckRealTerrainBoundarySave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game);
}
