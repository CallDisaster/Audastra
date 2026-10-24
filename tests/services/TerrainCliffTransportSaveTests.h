#pragma once

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Services/MapEditSaveService.h"

namespace War3Map::TerrainSaveTests {
// 复用现有原创地图与实际游戏来源；不创建额外游戏沙盒，不接受调用方规则。
void CheckRealCliffTextureSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game, const Game::GameDataSource& mounted);
}
