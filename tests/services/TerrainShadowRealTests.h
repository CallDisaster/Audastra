#pragma once

#include "War3MapCore/MapArchiveSession.h"

#include <filesystem>

namespace War3Map::TerrainSaveTests {
// 实际工具链的原创平地图，显式计算阴影并走上层无笔划保存。
// 字面全零SHD只覆盖普通空对象平地，不构成原版编辑器或游戏验收。
void CheckRealTerrainShadowSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game);
}
