#pragma once

#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/MapData/MapPathing.h"
#include "War3MapCore/Render/TerrainMinimapPostprocess.h"
#include "War3MapCore/Types.h"

#include <array>
#include <cstdint>
#include <optional>

namespace War3Map::Render {

// 经典 1.27a 编辑器“输出微缩地图 → 包括路径地图”的普通模式叠加。
// 每个地形格取其西南角通行格，按三个限制位叠色：不可走为红、不可飞为绿、不可建为蓝，
// 组合按位相加（如不可走+不可建为品红，三者全有为白），透明度 64/255；没有限制时不叠加。
// 只表达普通模式；编辑器“查看 → 路径-海路”打开时原版改用另一套状态，这里不模拟。
// 返回 RGBA；cell 为 WPM 原始字节，只读取 0x02/0x04/0x08 三位。
[[nodiscard]] std::optional<std::array<std::uint8_t, 4>> ClassicMinimapPathingColor(std::uint8_t cell) noexcept;

// 把通行叠加合成到已完成后处理的 256 画布（RGBA）。rectangle 为底图使用的地形格范围
// [x0, y0, x1, y1)，facts 为同一底图的后处理事实；每个画布像素取与底图相同的源格。
// 合成按原版单精度 base*(1-a)+color*a 截断取整，保留底图 alpha。
// 通行图尺寸须为地形格的 4 倍，范围须落在通行图内；否则拒绝，不画部分结果。
[[nodiscard]] Status CompositeClassicMinimapPathing(Image::DecodedImage& canvas,
    const TerrainMinimapPostprocessFacts& facts, const std::array<std::uint32_t, 4>& rectangle,
    const MapData::PathingGridSnapshot& pathing);

} // namespace War3Map::Render
