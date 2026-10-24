#pragma once

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Services/MapEditSaveService.h"

#include <filesystem>

namespace War3Map::TerrainSaveTests {
void CheckRealWalkableBridgeSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game, const ObjectData::StandardObjectCatalog& catalog,
    const ObjectData::ObjectFieldSchema& schema, const Game::GameDataSource& mounted);
}
