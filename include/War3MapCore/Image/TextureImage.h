#pragma once

#include "War3MapCore/Image/BlpImage.h"
#include <string_view>

namespace War3Map::Image {
// 原版资源预览共用解码；BLP 沿用既有解码器，TGA 支持无色表24/32位真彩色及RLE。
// 其它编码明确拒绝，完整尺寸不因预览缩放而改变。
[[nodiscard]] Result<DecodedImage> DecodeTexture(std::string_view path, std::span<const std::byte> data,
    std::size_t maxPixels = 16 * 1024 * 1024);
}
