#pragma once

#include "War3MapCore/Image/BlpImage.h"

#include <cstddef>
#include <span>

namespace War3Map::Image::detail {
// PNG 解码为 RGBA8：灰度/真彩色/调色板/灰度透明/真彩色透明，位深按规范全部组合，Adam7 交错，tRNS 透明。
// 16 位取高字节；不做 gAMA/sRGB/iCCP 色彩换算（贴图按数据使用）。逐块校验 CRC，未知关键块拒绝。
[[nodiscard]] Result<DecodedImage> DecodePng(std::span<const std::byte> data, std::size_t maxPixels);
}
