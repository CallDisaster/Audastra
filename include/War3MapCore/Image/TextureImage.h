#pragma once

#include "War3MapCore/Image/BlpImage.h"
#include <string_view>

namespace War3Map::Image {
// 原版资源预览共用解码；BLP 沿用既有解码器，TGA 支持无色表24/32位真彩色及RLE，
// JPEG（.jpg/.jpeg 或 JPEG 文件头）支持灰度与三分量（YCbCr，Adobe 标记为 RGB 时原样），
// PNG（.png 或 PNG 文件头）支持全部标准颜色类型与位深、交错与透明块。
// 其它编码明确拒绝，完整尺寸不因预览缩放而改变。
[[nodiscard]] Result<DecodedImage> DecodeTexture(std::string_view path, std::span<const std::byte> data,
    std::size_t maxPixels = 16 * 1024 * 1024);
}
