#pragma once

#include "War3MapCore/MapArchiveSession.h"

#include <filesystem>

namespace War3Map::TerrainSaveTests {
// 原创65²地图经实际资源、脚本编译及普通保存，独立核对同尺寸可玩区缩进/恢复。
// 不启动原版编辑器或游戏；不能据此宣称原版重开、镜头或实际通行验收。
void CheckRealPlayableBoundsSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game);
}
