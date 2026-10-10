#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace War3Map::Image::detail {

// JPEG 解码结果：交错像素，每像素 components 个字节，全分辨率、行优先。
struct JpegDecoded {
    int width = 0;
    int height = 0;
    int components = 0;
    std::vector<std::uint8_t> pixels; // width * height * components
};

// 私有调用约束；公共 DecodeBlp API 不变。预算计入输入、调用方保留字节、
// 系数、组件像素、单平面以及后续 RGBA 的峰值估算，不是进程总内存上限。
struct JpegDecodeOptions {
    std::uint32_t expectedWidth = 0;
    std::uint32_t expectedHeight = 0;
    std::size_t callerRetainedBytes = 0;
    std::size_t maxWorkingBytes = 256u * 1024u * 1024u; // 只能收紧
    // 经典 BLP1 的顺序 JPEG 在最终 EOI 前使用全零 padding；不放宽 RST/中间扫描。
    bool allowBlpFinalZeroPadding = false;
};

inline constexpr std::size_t kMaxJpegInputBytes = 64u * 1024u * 1024u;

// 解码顺序（SOF0/SOF1）与渐进式（SOF2）Huffman JPEG。支持 1/3/4 组件、色度子采样、
// 重启间隔、非交错扫描与逐次逼近精化；不支持无损、算术编码与 12 位精度。
// 不做任何色彩空间转换：输出即各组件原始采样值（YCbCr/CMYK/BGRA 由调用方解释）。
[[nodiscard]] Result<JpegDecoded> DecodeJpeg(
    std::span<const std::byte> data, const JpegDecodeOptions& options = {});

} // namespace War3Map::Image::detail
