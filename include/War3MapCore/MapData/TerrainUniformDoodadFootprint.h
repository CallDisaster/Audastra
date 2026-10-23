#pragma once

#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapTerrain.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace War3Map::MapData {

struct TerrainUniformDoodadFootprint {
    std::uint32_t side = 0;
    std::uint8_t attributes = 0; // 原版 selector 2 属性，不是 WPM 文件中的位。
};

// 仅解无ID、无色表、无交错、左原点的未压缩24位或32位真彩色TGA。
// 正方形边长1..256；所有像素的完整BGRA必须一致，24位补alpha255。
// 允许顶部/底部原点，以及无附加区、两个offset为零的标准26字节footer。
// 成功路径不分配图像缓冲；来源和总资源字节预算由捕获owner核对。
[[nodiscard]] Result<TerrainUniformDoodadFootprint> DecodeUniformDoodadFootprintTga(
    std::span<const std::byte> bytes);

// 完整selector 2网格，尺寸为 (width-1)*4 × (height-1)*4；仅OR，不做移除。
// 调用方负责证明普通装饰物类别、实际来源与完整最终对象集合。snapCells只能为
// 2或4，必须来自原版类别规则。这里不以DOO flags推断内存注册标志，也不改DOO。
// 经典v11地形沿用原版九位坐标域；实例XY须有限且绝对值不超过2^24。
// 32对齐坐标精确吸附；其它坐标仅在原版浮点舍入包络内格点唯一时接受。
// 调用线程须使用默认舍入且关闭FTZ/DAZ；不更改调用者的舍入或下溢模式。
// rotation须有限且在[-360,360]内；正方形同色占地不受旋转或有限scale影响。
// 先吸附实例XY，再做半格与向零投影。所有检查在首次写入前完成，失败无部分写。
// 成功路径无分配，已有属性原样保留；增删计数或跨来源事务由上层处理。
[[nodiscard]] Status AddUniformDoodadFootprint(
    std::span<std::uint8_t> staticAttributes, const TerrainHeader& terrain,
    const DoodadPlacement& placement, const TerrainUniformDoodadFootprint& footprint,
    std::uint8_t snapCells);

// owner须先确认实际TGA完整BGRA同色且正方形；这里复用既有2/4格投影，
// 两种投影均合法且该实例的完整地图内贡献相同才OR一次。先比较再与其它实例合并，
// 零属性或双空贡献仍必须经过全部姿态/浮点和两次投影检查。失败不改目标。
// 成功路径无分配；捕获owner须逐实例预收双投影描述符和固定计算的保守预算。
inline constexpr std::size_t TerrainUniformGridInvariantBudgetBytes = 256;
[[nodiscard]] Status AddGridInvariantUniformDoodadFootprint(
    std::span<std::uint8_t> staticAttributes, const TerrainHeader& terrain,
    const DoodadPlacement& placement, const TerrainUniformDoodadFootprint& footprint);

} // namespace War3Map::MapData
