#pragma once

#include <cstddef>

namespace War3Map::MapData {

// 地形保存中只读解析的短暂峰值额度：保存前的通行相关性探测、装饰物/单位成员的完整往返核对。
// 它们在保存计划额度（默认 64 MiB，同时是各成员的读取上限）之外单独记账：大地图的装饰物
// （约 2 MB DOO）新旧两份完整往返约需 120 MB，计划额度装不下。
// 额度随调用方的计划额度按 16 倍同比收紧，最多 1 GiB；只是记账上界，不预先分配。只此一处定义。
inline constexpr std::size_t kTerrainParsePeakScale = 16;
inline constexpr std::size_t kTerrainParsePeakMaximumBytes = 1024ull * 1024 * 1024;

[[nodiscard]] constexpr std::size_t TerrainParsePeakBytes(std::size_t planBytes) noexcept {
    return planBytes > kTerrainParsePeakMaximumBytes / kTerrainParsePeakScale
        ? kTerrainParsePeakMaximumBytes : planBytes * kTerrainParsePeakScale;
}

} // namespace War3Map::MapData
