#pragma once

#include "War3MapCore/MapArchiveSession.h"

#include <filesystem>

namespace War3Map::TerrainSaveTests {
// 原创已有干/湿R64坡道，读取实际资源后走公共浅/深水保存与完整归档回读。
// 预期复用Core及通行/MMP生成器，只证明保存编排，不构成原版或游戏oracle。
void CheckRealExistingRampWaterSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game);
void CheckRealCornerRampSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game);
void CheckRealStraightRampSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game);
}
