#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace War3Map::Render {

// 经典 MDX（版本 800）里写明的范围：MODL 一条、SEQS 每个动作一条、GEOS 每个几何体一条主范围加逐动作范围。
// 每条 28 字节：半径、最小 XYZ、最大 XYZ。游戏 1.27a 按这些写明的数值（不是顶点）决定装饰物是否绘制，
// 取景、导入检查与加宽都从这里读写，不另写一套块遍历。
inline constexpr std::size_t kMdxExtentBytes = 28;

struct MdxDeclaredExtent {
    std::size_t offset = 0; // 半径在文件中的字节位置，其后依次为最小、最大
    float radius = 0;
    std::array<float, 3> minimum{}, maximum{};
};

// 有限、半径非负、每轴最小不大于最大。
[[nodiscard]] bool MdxExtentUsable(const MdxDeclaredExtent& extent) noexcept;

enum class MdxExtentScope : std::uint8_t { ModelAndSequences, All };

struct MdxDeclaredExtents {
    MdxDeclaredExtent model;
    std::vector<MdxDeclaredExtent> sequences;
    std::vector<MdxDeclaredExtent> geosets;         // scope = All：每个几何体的主范围
    std::vector<MdxDeclaredExtent> geosetSequences; // scope = All：几何体的逐动作范围，按几何体顺序展开
};

// 要求 MDLX 头、VERS = 800，VERS/MODL/SEQS/GEOS 各至多一块，MODL 恰好 372 字节，SEQS 为 132 字节的整数倍。
// scope = All 时按经典字段顺序（VRTX NRMS PTYP PCNT PVTX GNDX MTGC MATS）走到每个几何体的范围字段，
// 对不上就拒绝。数值原样读出，是否可用由调用方用 MdxExtentUsable 判断。
[[nodiscard]] Result<MdxDeclaredExtents> ReadMdxDeclaredExtents(std::span<const std::byte> bytes, MdxExtentScope scope);

struct MdxWidenedExtents {
    std::vector<std::byte> bytes;
    std::size_t changed = 0; // 改写的范围条数
};

// 把全部写明的范围（MODL、SEQS、GEOS 主范围与逐动作范围）加宽到包含 [minimum, maximum]：已包含的原样不动；
// 其余取并集，半径取新盒对角线的一半（不小于原半径），换成单精度时向外取。只改这些 28 字节字段，
// 文件长度和其余字节不变。有不可用的范围或加宽后超出单精度时拒绝。
[[nodiscard]] Result<MdxWidenedExtents> WidenMdxDeclaredExtents(std::span<const std::byte> bytes,
    const std::array<double, 3>& minimum, const std::array<double, 3>& maximum);

} // namespace War3Map::Render
