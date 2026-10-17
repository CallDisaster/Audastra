#pragma once

#include "War3MapCore/Image/BlpImage.h"

namespace War3Map::Image {

struct BlpMipChainLimits {
    // 默认值也是硬上限，只允许收紧；调用方须约束读取源文件时的分配。
    std::size_t maxInputBytes = 16 * 1024 * 1024;
    std::size_t maxDimension = 2048;
    std::size_t maxRgbaBytes = 16 * 1024 * 1024;
    std::size_t maxRetainedRgbaBytes = 24 * 1024 * 1024;
    std::size_t maxTotalMipPixels = 8 * 1024 * 1024;
    std::size_t maxWorkingBytes = 128 * 1024 * 1024;
    std::size_t maxJpegSegments = 4096;
    std::size_t maxJpegScans = 256;
};

struct BlpMipChainDimensions {
    std::uint32_t width = 0, height = 0;
};

enum class BlpMipChainStopReason {
    Complete,           // 已到1x1，或遍历完16个声明槽；不读取终点之后的层。
    UndeclaredMip,      // 当前offset/size均为0；hasLaterDeclarations可区分中间空洞。
    InvalidMipRange,
    TotalPixelBudget,
    RetainedRgbaBudget,
    WorkingBudget,
    JpegBudget,
    DecodeFailure,     // 包装/工作集/JPEG/Core失败；细节保留在stop.diagnostics。
};

struct BlpMipChainStop {
    BlpMipChainStopReason reason = BlpMipChainStopReason::Complete;
    std::uint32_t level = 0; // 首个未保留层；Complete时是已保留数。
    bool hasLaterDeclarations = false;
    std::vector<Diagnostic> diagnostics;
};

struct BlpMipChain {
    DecodedImage image;
    // 原始mip1..N，不重复存储top，也不生成缺级。
    std::vector<DecodedImage> additionalMips;
    std::uint32_t validatedMipCount = 0;
    BlpMipChainStop stop;

    [[nodiscard]] std::size_t RgbaByteCount() const noexcept {
        std::size_t bytes = image.rgba.size();
        for (const auto& mip : additionalMips) bytes += mip.rgba.size();
        return bytes;
    }
};

// 只执行限额、输入、BLP1格式和尺寸前置门，未验证RGBA/alpha/mip或解码像素。
// 供调用方在解码前验证自身布局；共享层本身接受小尺寸、矩形及非2次幂纹理。
[[nodiscard]] Result<BlpMipChainDimensions> ReadBlpMipChainDimensions(
    std::span<const std::byte> bytes, const BlpMipChainLimits& limits = {});

// 提取自TerrainAtlas的连续原始链检查。唯一像素解码器仍是DecodeBlp。
// mip0失败返回Failure；后级坏层/预算截止保留原有效前缀并显式记录stop事实。
// 成功diagnostics仅包含成功层的JPEG尾数据警告；被截后级诊断在stop内，调用方决定准入。
// 包装、JPEG结构/工作集、输入及保留RGBA限额均在分配相应载荷前检查。
[[nodiscard]] Result<BlpMipChain> DecodeBlpMipChain(
    std::span<const std::byte> bytes, const BlpMipChainLimits& limits = {});

} // namespace War3Map::Image
