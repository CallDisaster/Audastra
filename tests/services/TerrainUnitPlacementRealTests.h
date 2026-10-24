#pragma once

#include "War3MapCore/Services/MapEditSaveService.h"

namespace War3Map::TerrainSaveTests {
// 原创图、实际资源与真实脚本工具链的单位摆放+地形同次保存；不代替原版/游戏验收。
void CheckRealTerrainUnitPlacementSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game);
}
