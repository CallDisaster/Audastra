#pragma once

#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"

namespace War3Map::MapData {

// 同尺寸地形中的可玩地格半开矩形；不改变地形尺寸、原点或摆放坐标。
struct TerrainPlayableRectangle {
    std::uint32_t west = 0, south = 0, east = 0, north = 0;
    bool operator==(const TerrainPlayableRectangle&) const = default;
};

struct TerrainBoundsEditIntent {
    TerrainPlayableRectangle playable;
    std::uint8_t algorithm = 1;
};

// 纯投影只消费数值，不认证CameraMargins来源；正式编辑须由实际资源owner准备。
// CameraMargins按左、右、下、上排列，单位为地块。
[[nodiscard]] Result<MapSettings> ProjectTerrainPlayableBounds(const TerrainGridSnapshot& terrain,
    const MapSettings& settings, const std::array<float, 4>& cameraMargins,
    const TerrainBoundsEditIntent& intent, std::size_t maximumSettingsBytes = 1024u * 1024u);

// 原版5E7700只更新各地格SW顶点的0x4000；末行、末列及其余原始位保持。
// 完整预检后才落笔，失败不留下已修改前缀；maximumWork按地格数计，只能收紧硬上限。
[[nodiscard]] Result<std::size_t> ApplyTerrainPlayableEdges(TerrainGridSnapshot& terrain,
    const TerrainPlayableRectangle& playable, std::size_t maximumWork = 2048u * 2048u);

// WPM使用未取整的原版F32世界范围，不能用小地图整数crop代替。
// 此函数也只做计算，不证明输入W3I、规则或地形的实际来源。
[[nodiscard]] Result<TerrainNativePlayableBounds> ComputeTerrainNativePlayableBounds(
    const TerrainHeader& terrain, const MapSettings& settings, const std::array<float, 4>& cameraMargins);

} // namespace War3Map::MapData
