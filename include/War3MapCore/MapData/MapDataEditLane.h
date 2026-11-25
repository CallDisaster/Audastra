#pragma once

#include <cstddef>
#include <cstdint>

namespace War3Map::MapData {

// 地图数据的外层撤销分区：同一编辑会话由世界页、地图设置页和声音页分别持有命令。
// 世界分区包含地形、通行、摆放、区域、镜头以及可玩范围/地图尺寸这类跨成员事务。
// 分区内保持后进先出；不同分区的命令可以交错撤销，触及同一数据时由会话按字节核对拒绝。
enum class MapDataEditLane : std::uint8_t { World = 0, Settings = 1, Sounds = 2 };

inline constexpr std::size_t MapDataEditLaneCount = 3;

} // namespace War3Map::MapData
