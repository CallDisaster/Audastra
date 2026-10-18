#pragma once

#include "War3MapCore/Image/BlpImage.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace War3Map::Render {

enum class TerrainMinimapTextureRole { Ground, Cliff };

struct TerrainMinimapRepresentativeColor {
    std::array<std::uint8_t, 4> rgba{};
    std::size_t averagedPixels = 0;
};

// 经典编辑器的代表色取样：先算矩形面积，再从解码正文开头连续累加该数量的像素。
// 它保留原版的线性前缀行为，不能改成逐行截取左上方矩形或整张atlas平均。
// 地面W>H时面积为(W/4)*(H/8)，其余地面及悬崖为(W/4)*(H/4)。
// 忽略源alpha，RGB分别整除、输出alpha255。默认颜色尚未取证，因此面积0时明确拒绝。
// 仅消费已解码图像，不证明编码来源、原版保存或游戏兼容；不执行I/O。
[[nodiscard]] Result<TerrainMinimapRepresentativeColor> ComputeTerrainMinimapRepresentativeColor(
    const Image::DecodedImage& image, TerrainMinimapTextureRole role);

} // namespace War3Map::Render
