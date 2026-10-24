#pragma once

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Services/MapEditSaveService.h"

namespace War3Map::TerrainSaveTests {
// 使用原创图和实际地形表走公共保存；留下可供原版/游戏另行验收的副本。
void CheckRealGroundBrushSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game, const Game::GameDataSource& mounted);
void CheckRealExistingRampCliffSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game);
void CheckRealRemoveWaterSave(const MapArchiveSession& shallow, const MapArchiveSession& deep,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game);
}
