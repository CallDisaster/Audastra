#include "War3MapCore/Image/BlpImage.h"

#include "JpegDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>

namespace War3Map::Image {
namespace {

std::uint32_t ReadU32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

std::uint8_t Clamp8(double v) {
    if (v < 0.0) return 0;
    if (v > 255.0) return 255;
    return static_cast<std::uint8_t>(std::lround(v));
}

// 将 JPEG 解码结果按 BLP 约定映射为 RGBA。
Result<DecodedImage> JpegToRgba(
    const detail::JpegDecoded& jpeg, std::uint32_t alphaBits) {
    DecodedImage img;
    img.width = static_cast<std::uint32_t>(jpeg.width);
    img.height = static_cast<std::uint32_t>(jpeg.height);
    const std::size_t count = static_cast<std::size_t>(jpeg.width) * jpeg.height;
    img.rgba.resize(count * 4);

    const int nc = jpeg.components;
    const std::uint8_t* src = jpeg.pixels.data();
    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t r = 0, g = 0, b = 0, a = 255;
        if (nc == 4) {
            // BLP1 JPEG：分量顺序为 B, G, R, A。
            b = src[i * 4 + 0];
            g = src[i * 4 + 1];
            r = src[i * 4 + 2];
            a = (alphaBits >= 8) ? src[i * 4 + 3] : 255;
        } else if (nc == 3) {
            // 标准 YCbCr → RGB。
            const double y = src[i * 3 + 0];
            const double cb = src[i * 3 + 1] - 128.0;
            const double cr = src[i * 3 + 2] - 128.0;
            r = Clamp8(y + 1.402 * cr);
            g = Clamp8(y - 0.344136 * cb - 0.714136 * cr);
            b = Clamp8(y + 1.772 * cb);
        } else if (nc == 1) {
            r = g = b = src[i];
        } else {
            return Result<DecodedImage>::Failure(
                "Image.Blp", "jpeg-components",
                "不支持的 JPEG 组件数。");
        }
        img.rgba[i * 4 + 0] = static_cast<std::byte>(r);
        img.rgba[i * 4 + 1] = static_cast<std::byte>(g);
        img.rgba[i * 4 + 2] = static_cast<std::byte>(b);
        img.rgba[i * 4 + 3] = static_cast<std::byte>(a);
    }
    return Result<DecodedImage>::Success(std::move(img));
}

Result<DecodedImage> DecodeBlpImpl(std::span<const std::byte> data,
    const BlpDecodeLimits& limits, bool strictMip, std::size_t& workingBytes) try {
    constexpr std::size_t hardWorkingBytes = 256u * 1024u * 1024u;
    if (limits.maxDimension == 0 || limits.maxDimension > 4096
        || limits.maxInputBytes > detail::kMaxJpegInputBytes
        || limits.maxWorkingBytes == 0 || limits.maxWorkingBytes > hardWorkingBytes)
        return Result<DecodedImage>::Failure("Image.Blp", "decode-limits", "BLP 解码额度只能收紧内置上限。");
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::size_t size = data.size();
    if (size > limits.maxInputBytes || size > limits.maxWorkingBytes) {
        return Result<DecodedImage>::Failure("Image.Blp", "input-budget", "BLP 输入超过本次读取或工作额度。");
    }
    if (size < 160) {
        return Result<DecodedImage>::Failure(
            "Image.Blp", "too-short", "BLP 数据过短。");
    }
    if (std::memcmp(p, "BLP1", 4) != 0) {
        // 目前只实现 BLP1（经典版命令按钮图标）。
        return Result<DecodedImage>::Failure(
            "Image.Blp", "not-blp1",
            "仅支持 BLP1（当前非 BLP1，可能是 Reforged BLP2）。");
    }

    const std::uint32_t compression = ReadU32(p + 4);
    const std::uint32_t alphaBits = ReadU32(p + 8);
    const std::uint32_t width = ReadU32(p + 12);
    const std::uint32_t height = ReadU32(p + 16);
    if (width == 0 || height == 0 || width > limits.maxDimension || height > limits.maxDimension) {
        return Result<DecodedImage>::Failure(
            "Image.Blp", "bad-dimensions", "BLP 尺寸非法。");
    }

    std::array<std::uint32_t, 16> mipOffsets{};
    std::array<std::uint32_t, 16> mipSizes{};
    for (int i = 0; i < 16; ++i) {
        mipOffsets[i] = ReadU32(p + 28 + i * 4);
        mipSizes[i] = ReadU32(p + 92 + i * 4);
    }
    if (strictMip && alphaBits != 0 && alphaBits != 1 && alphaBits != 4 && alphaBits != 8)
        return Result<DecodedImage>::Failure("Image.Blp", "alpha-depth", "BLP alpha 位数不是明确支持的 0/1/4/8。");

    if (compression == 0) {
        if (strictMip && alphaBits != 0 && alphaBits != 8)
            return Result<DecodedImage>::Failure("Image.Blp", "alpha-depth", "BLP JPEG 仅支持明确的 0/8 位 alpha。");
        // JPEG 压缩：共享头 + mip0 数据。
        const std::uint32_t jpegHeaderSize = ReadU32(p + 156);
        const std::size_t headerStart = 160;
        if (headerStart + jpegHeaderSize > size) {
            return Result<DecodedImage>::Failure(
                "Image.Blp", "jpeg-header", "JPEG 共享头越界。");
        }
        const std::uint32_t mipOff = mipOffsets[0];
        const std::uint32_t mipSize = mipSizes[0];
        if (mipOff == 0 || static_cast<std::size_t>(mipOff) + mipSize > size
            || (strictMip && (mipSize == 0 || mipOff < headerStart + jpegHeaderSize))) {
            return Result<DecodedImage>::Failure(
                "Image.Blp", "mip0", "mip0 数据越界。");
        }
        // 拼接完整 JPEG 流。
        const auto joinedBytes = static_cast<std::size_t>(jpegHeaderSize) + mipSize;
        if (joinedBytes > detail::kMaxJpegInputBytes) {
            return Result<DecodedImage>::Failure("Image.Blp", "jpeg-budget", "BLP 拼接 JPEG 超过 64 MiB 上限。");
        }
        // 拼接也在JPEG自身预检之前分配，须先收取与原BLP并存的容量。
        if (joinedBytes > limits.maxWorkingBytes - size)
            return Result<DecodedImage>::Failure("Image.Blp", "jpeg-budget", "BLP 拼接 JPEG 超过本次剩余工作额度。");
        std::vector<std::byte> jpeg;
        jpeg.reserve(joinedBytes);
        jpeg.insert(jpeg.end(),
                    reinterpret_cast<const std::byte*>(p + headerStart),
                    reinterpret_cast<const std::byte*>(p + headerStart + jpegHeaderSize));
        jpeg.insert(jpeg.end(),
                    reinterpret_cast<const std::byte*>(p + mipOff),
                    reinterpret_cast<const std::byte*>(p + mipOff + mipSize));
        detail::JpegDecodeOptions options;
        options.expectedWidth = width;
        options.expectedHeight = height;
        options.callerRetainedBytes = data.size();
        options.maxWorkingBytes = limits.maxWorkingBytes;
        options.allowBlpFinalZeroPadding = true;
        auto decoded = detail::DecodeJpeg(jpeg, options);
        if (!decoded.Succeeded() || !decoded.value.has_value()) {
            Result<DecodedImage> r;
            r.diagnostics = std::move(decoded.diagnostics);
            if (r.diagnostics.empty()) {
                r.diagnostics.push_back(Diagnostic{
                    DiagnosticSeverity::Error, "Image.Blp", "jpeg-decode",
                    "BLP 内嵌 JPEG 解码失败。"});
            }
            return r;
        }
        workingBytes = decoded.value->workingBytes;
        return JpegToRgba(*decoded.value, alphaBits);
    }

    if (compression == 1) {
        // 调色板：256 色 BGRA 调色板（紧接 156 字节头）+ 索引平面 + 可选 alpha 平面。
        const std::size_t paletteStart = 156;
        if (paletteStart + 1024 > size) {
            return Result<DecodedImage>::Failure(
                "Image.Blp", "palette", "调色板越界。");
        }
        const std::uint8_t* palette = p + paletteStart;
        const std::uint32_t mipOff = mipOffsets[0];
        const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
        if (mipOff == 0 || static_cast<std::size_t>(mipOff) + pixelCount > size) {
            return Result<DecodedImage>::Failure(
                "Image.Blp", "indices", "索引平面越界。");
        }
        const std::uint8_t* indices = p + mipOff;
        // alpha 平面紧随索引：8 位逐像素；1/4 位按像素顺序从字节低位起打包（与游戏一致）。
        const std::uint32_t bits = alphaBits >= 8 ? 8 : alphaBits >= 4 ? 4 : alphaBits >= 1 ? 1 : 0;
        const auto planeBytes = pixelCount + (pixelCount * bits + 7) / 8;
        if (strictMip && (mipOff < paletteStart + 1024 || mipSizes[0] < planeBytes
            || static_cast<std::size_t>(mipOff) + mipSizes[0] > size))
            return Result<DecodedImage>::Failure("Image.Blp", "mip0", "BLP 调色板 mip0 声明没有完整覆盖像素平面。");
        const std::uint8_t* alphaPlane = bits ? (indices + pixelCount) : nullptr;
        if (bits && static_cast<std::size_t>(mipOff) + pixelCount + (pixelCount * bits + 7) / 8 > size) {
            return Result<DecodedImage>::Failure(
                "Image.Blp", "alpha-plane", "alpha 平面越界。");
        }
        const auto alphaAt = [&](std::size_t i) -> std::uint8_t {
            if (bits == 8) return alphaPlane[i];
            if (bits == 4) return static_cast<std::uint8_t>(((alphaPlane[i / 2] >> ((i % 2) * 4)) & 0x0f) * 17);
            if (bits == 1) return (alphaPlane[i / 8] >> (i % 8)) & 1 ? 255 : 0;
            return 255;
        };

        if (pixelCount * 4 > limits.maxWorkingBytes - size)
            return Result<DecodedImage>::Failure("Image.Blp", "working-budget", "BLP 像素超过本次剩余工作额度，尚未分配。");
        workingBytes = size + pixelCount * 4;
        DecodedImage img;
        img.width = width;
        img.height = height;
        img.rgba.resize(pixelCount * 4);
        for (std::size_t i = 0; i < pixelCount; ++i) {
            const std::uint8_t idx = indices[i];
            // 调色板项为 BGRA。
            const std::uint8_t b = palette[idx * 4 + 0];
            const std::uint8_t g = palette[idx * 4 + 1];
            const std::uint8_t r = palette[idx * 4 + 2];
            const std::uint8_t a = alphaAt(i);
            img.rgba[i * 4 + 0] = static_cast<std::byte>(r);
            img.rgba[i * 4 + 1] = static_cast<std::byte>(g);
            img.rgba[i * 4 + 2] = static_cast<std::byte>(b);
            img.rgba[i * 4 + 3] = static_cast<std::byte>(a);
        }
        return Result<DecodedImage>::Success(std::move(img));
    }

    return Result<DecodedImage>::Failure(
        "Image.Blp", "compression",
        "不支持的 BLP 压缩类型: " + std::to_string(compression));
} catch (const std::bad_alloc&) {
    return Result<DecodedImage>::Failure("Image.Blp", "allocation", "BLP 解码内存分配失败。");
} catch (const std::length_error&) {
    return Result<DecodedImage>::Failure("Image.Blp", "allocation", "BLP 解码分配长度非法。");
}

} // namespace

Result<DecodedImage> DecodeBlp(std::span<const std::byte> data) {
    std::size_t workingBytes = 0;
    return DecodeBlpImpl(data, BlpDecodeLimits{}, false, workingBytes);
}

Result<BudgetedBlpImage> DecodeBlpBudgeted(
    std::span<const std::byte> data, const BlpDecodeLimits& limits) {
    std::size_t workingBytes = 0;
    auto decoded = DecodeBlpImpl(data, limits, true, workingBytes);
    if (!decoded.Succeeded() || !decoded.value) return {{}, std::move(decoded.diagnostics)};
    return Result<BudgetedBlpImage>::Success({std::move(*decoded.value), workingBytes});
}

} // namespace War3Map::Image
