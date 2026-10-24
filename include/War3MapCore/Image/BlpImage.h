#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace War3Map::Image {

// 解码后的 32 位图像，像素为 RGBA8888（每像素 4 字节，行优先，无填充）。
struct DecodedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> rgba; // size == width * height * 4

    [[nodiscard]] bool Valid() const noexcept {
        return width > 0 && height > 0 &&
            rgba.size() == static_cast<std::size_t>(width) * height * 4;
    }
};

// 解码 Warcraft III 的 BLP 纹理为 RGBA。
// 支持：
//   * BLP1 + JPEG 压缩（content=0）：4 组件按 BGRA 原样映射（BLP 约定，不做 CMYK 变换）。
//   * BLP1 + 调色板（content=1）：256 色 BGRA 调色板 + 索引 + 可选 alpha 平面。
// 不支持的格式（如 BLP2/DXT）返回 Failure，绝不产出损坏像素（fail-closed）。
[[nodiscard]] Result<DecodedImage> DecodeBlp(std::span<const std::byte> data);

// 有界调用使用同一解码器；额度只能收紧内置硬上限。工作量估算包含原始输入、
// JPEG拼接、系数、临时平面与RGBA峰值，不是进程RSS。严格核对mip0声明范围。
struct BlpDecodeLimits {
    std::size_t maxDimension = 4096;
    std::size_t maxInputBytes = 64u * 1024u * 1024u;
    std::size_t maxWorkingBytes = 256u * 1024u * 1024u;
};
struct BudgetedBlpImage {
    DecodedImage image;
    std::size_t workingBytes = 0;
};
[[nodiscard]] Result<BudgetedBlpImage> DecodeBlpBudgeted(
    std::span<const std::byte> data, const BlpDecodeLimits& limits);

// 编码为 BLP1 调色板格式（content=1）并生成完整 mipmap 链（BlpEncode.cpp）。
// 调色板由图像（含各级 mipmap）的实际颜色组成，超过 256 色时失败，不做有损量化；灰度图总能编码。
// alpha 全不透明时不写 alpha 平面。
[[nodiscard]] Result<std::vector<std::byte>> EncodeBlpPaletted(const DecodedImage& image);

struct BlpOpaqueQuantizeLimits {
    // 默认值也是硬上限，只能收紧；工作预算是载荷估算，不是进程 RSS。
    std::size_t maxDimension = 512;
    std::size_t maxPixels = 512u * 512u;
    std::size_t maxTotalMipPixels = 349525;
    std::size_t maxWorkingBytes = 8u * 1024u * 1024u;
    std::size_t maxOutputBytes = 512u * 1024u;
};

enum class BlpOpaquePaletteMethod { Exact, WeightedMedianCut5Bit };

struct BlpOpaqueQuantized {
    std::vector<std::byte> bytes;
    std::uint32_t paletteColorCount = 0;
    std::uint32_t occupiedHistogramBins = 0;
    std::uint32_t mipCount = 0;
    std::size_t totalMipPixels = 0;
    BlpOpaquePaletteMethod method = BlpOpaquePaletteMethod::Exact;
    bool quantized = false;
};

// 显式有损、仅全不透明图像。先按既有 floor-halving/2×2 平均规则生成原始完整 mip 链，
// 再用全链加权的 5bit RGB histogram / median-cut 生成共享 palette，逐原像素最近色映射。
// box 选 range*weight 最大者（同值保留较小稳定槽位）；轴同值 R/G/B 顺序；分割累计半权重；
// nearest RGB 平方距离同值取 RGB 字典序较小颜色。无 dither，不做 sRGB 转换。
// 全链联合颜色 <=256 时与 EncodeBlpPaletted 逐字相同，旧无损 API 的语义不变。
[[nodiscard]] Result<BlpOpaqueQuantized> EncodeBlpOpaqueQuantized(const DecodedImage& image,
    const BlpOpaqueQuantizeLimits& limits = {});

// 暂停 / 禁用按钮图标（DISBTN / DISPAS）的外观：去色并压暗，保留 alpha。
[[nodiscard]] DecodedImage MakeDisabledIcon(const DecodedImage& image);

} // namespace War3Map::Image
