#pragma once

#include "War3MapCore/Image/BlpImage.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace War3Map::Image {

// 原版 WE 保存小地图使用的有界 BLP1/JPEG 单层编码合同。
struct BlpJpegEncodeLimits {
    // 默认值也是硬上限，只能收紧；本入口只接受 256×256 opaque 输入。
    std::size_t maxDimension = 256;
    std::size_t maxPixels = 256u * 256u;
    std::size_t maxWorkingBytes = 16u * 1024u * 1024u;
    std::size_t maxOutputBytes = 1024u * 1024u;
    std::uint8_t quality = 40;
};

struct BlpJpegSingleMip {
    std::vector<std::byte> bytes;
    std::uint32_t width = 0, height = 0;
    std::size_t jpegHeaderBytes = 0;
    std::size_t jpegMipBytes = 0;
    std::uint8_t quality = 40;
    bool fourComponentBgra = true;
    bool opaque = true;
};

// 独立 baseline JPEG 编码器：四个 1×1 采样分量按 B,G,R,A 交错写出，
// quality=40 使用标准 JPEG luminance 量化表，BLP1 只声明 mip0，alphaBits=0。
// 共享 JPEG 头位于 BLP 主头之后，mip0 保存 SOF0、SOS、熵流及 EOI；不复用调色板编码器。
[[nodiscard]] Result<BlpJpegSingleMip> EncodeBlpJpegSingleMip(
    const DecodedImage& image, const BlpJpegEncodeLimits& limits = {});

} // namespace War3Map::Image
