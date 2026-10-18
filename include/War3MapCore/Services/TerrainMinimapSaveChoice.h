#pragma once

namespace War3Map::Services {

enum class TerrainMinimapSaveChoice {
    // 正常的保存/另存入口：根据本次地形是否变化及源图成员状态自动决定
    // 重建或沿用小地图。该选择不代表放宽保存门，实际写出仍由
    // TerrainMinimapSave 与 TerrainSavePlan 复核。
    Automatic = 3,
    Preserve = 0,
    GenerateMissingGroundOnly = 1,
    GenerateMissingDeclaredGroundOnly = 2
};

} // namespace War3Map::Services
