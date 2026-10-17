#include "War3MapCore/Image/BlpImage.h"

#include <algorithm>
#include <array>
#include <map>
#include <new>
#include <stdexcept>
#include <utility>

// BLP1 调色板编码：头 156 字节（magic、content、alphaBits、宽高、extra、hasMipmaps、16 个 mip 偏移与长度），
// 紧接 256 色 BGRA 调色板，之后每级 mipmap 为索引平面 [+ alpha 平面]。按公开的格式说明独立实现。
namespace War3Map::Image {
namespace {

constexpr std::size_t kHeaderSize = 156;
constexpr std::size_t kPaletteSize = 1024;

void PutU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes[offset + static_cast<std::size_t>(shift / 8)] = static_cast<std::byte>((value >> shift) & 0xFFu);
    }
}

std::uint8_t At(const DecodedImage& image, std::size_t pixel, int channel) {
    return std::to_integer<std::uint8_t>(image.rgba[pixel * 4 + static_cast<std::size_t>(channel)]);
}

// 2×2 平均缩小一级（奇数边按边缘像素处理）。
DecodedImage Downsample(const DecodedImage& image) {
    DecodedImage next;
    next.width = std::max<std::uint32_t>(1, image.width / 2);
    next.height = std::max<std::uint32_t>(1, image.height / 2);
    next.rgba.resize(static_cast<std::size_t>(next.width) * next.height * 4);
    for (std::uint32_t y = 0; y < next.height; ++y) {
        for (std::uint32_t x = 0; x < next.width; ++x) {
            for (int channel = 0; channel < 4; ++channel) {
                unsigned sum = 0;
                for (std::uint32_t dy = 0; dy < 2; ++dy) {
                    for (std::uint32_t dx = 0; dx < 2; ++dx) {
                        const std::uint32_t sx = std::min(image.width - 1, x * 2 + dx);
                        const std::uint32_t sy = std::min(image.height - 1, y * 2 + dy);
                        sum += At(image, static_cast<std::size_t>(sy) * image.width + sx, channel);
                    }
                }
                next.rgba[(static_cast<std::size_t>(y) * next.width + x) * 4 + static_cast<std::size_t>(channel)] =
                    static_cast<std::byte>((sum + 2) / 4);
            }
        }
    }
    return next;
}

std::uint32_t ColorKey(const DecodedImage& image, std::size_t pixel) {
    return (static_cast<std::uint32_t>(At(image, pixel, 0)) << 16) |
        (static_cast<std::uint32_t>(At(image, pixel, 1)) << 8) | At(image, pixel, 2);
}

// 两个入口共用原有容器布局；调用方已经给出全链共享颜色，写出不再生成 mip 或量化。
std::vector<std::byte> WritePaletted(const std::vector<DecodedImage>& levels,
    std::map<std::uint32_t, std::uint8_t> palette, bool alpha) {
    const auto& image = levels.front();
    std::vector<std::byte> bytes(kHeaderSize + kPaletteSize, std::byte{0});
    std::size_t outputBytes = bytes.size();
    for (const auto& mip : levels)
        outputBytes += static_cast<std::size_t>(mip.width) * mip.height * (alpha ? 2u : 1u);
    bytes.reserve(outputBytes);
    const char magic[4] = {'B', 'L', 'P', '1'};
    for (int index = 0; index < 4; ++index) bytes[static_cast<std::size_t>(index)] = static_cast<std::byte>(magic[index]);
    PutU32(bytes, 4, 1);
    PutU32(bytes, 8, alpha ? 8u : 0u);
    PutU32(bytes, 12, image.width);
    PutU32(bytes, 16, image.height);
    PutU32(bytes, 20, alpha ? 4u : 5u);
    PutU32(bytes, 24, 1);
    std::uint8_t next = 0;
    for (auto& [color, index] : palette) {
        index = next;
        const std::size_t entry = kHeaderSize + static_cast<std::size_t>(next) * 4;
        bytes[entry + 0] = static_cast<std::byte>(color & 0xFFu);
        bytes[entry + 1] = static_cast<std::byte>((color >> 8) & 0xFFu);
        bytes[entry + 2] = static_cast<std::byte>((color >> 16) & 0xFFu);
        ++next;
    }
    for (std::size_t level = 0; level < levels.size(); ++level) {
        const auto& mip = levels[level];
        const std::size_t pixels = static_cast<std::size_t>(mip.width) * mip.height;
        PutU32(bytes, 28 + level * 4, static_cast<std::uint32_t>(bytes.size()));
        PutU32(bytes, 92 + level * 4, static_cast<std::uint32_t>(pixels * (alpha ? 2 : 1)));
        for (std::size_t pixel = 0; pixel < pixels; ++pixel)
            bytes.push_back(static_cast<std::byte>(palette.at(ColorKey(mip, pixel))));
        if (alpha) for (std::size_t pixel = 0; pixel < pixels; ++pixel)
            bytes.push_back(mip.rgba[pixel * 4 + 3]);
    }
    return bytes;
}

struct HistogramBin {
    std::uint64_t count = 0;
    std::array<std::uint64_t, 3> sum{};
};

struct ColorBox {
    std::size_t begin = 0, end = 0;
    std::uint64_t weight = 0;
    std::uint32_t range = 0, axis = 0;
};

std::uint16_t BinKey(std::uint32_t color) {
    return static_cast<std::uint16_t>(((color >> 19) << 10)
        | (((color >> 11) & 31u) << 5) | ((color >> 3) & 31u));
}

unsigned BinChannel(std::uint16_t key, unsigned axis) {
    return (key >> (10u - 5u * axis)) & 31u;
}

ColorBox MeasureBox(std::size_t begin, std::size_t end,
    const std::vector<std::uint16_t>& keys, const std::vector<HistogramBin>& bins) {
    ColorBox box{begin, end};
    std::array<unsigned, 3> low{31, 31, 31}, high{};
    for (auto index = begin; index < end; ++index) {
        const auto key = keys[index];
        box.weight += bins[key].count;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const auto channel = BinChannel(key, axis);
            low[axis] = std::min(low[axis], channel);
            high[axis] = std::max(high[axis], channel);
        }
    }
    for (unsigned axis = 0; axis < 3; ++axis) if (high[axis] - low[axis] > box.range) {
        box.range = high[axis] - low[axis];
        box.axis = axis;
    }
    return box;
}

std::map<std::uint32_t, std::uint8_t> MedianPalette(std::vector<std::uint16_t>& keys,
    const std::vector<HistogramBin>& bins) {
    std::vector<ColorBox> boxes;
    boxes.reserve(256);
    boxes.push_back(MeasureBox(0, keys.size(), keys, bins));
    while (boxes.size() < 256) {
        std::size_t selected = boxes.size();
        std::uint64_t score = 0;
        for (std::size_t index = 0; index < boxes.size(); ++index) {
            const auto& box = boxes[index];
            const auto candidate = box.weight * box.range;
            if (box.end - box.begin > 1 && (selected == boxes.size() || candidate > score)) {
                selected = index;
                score = candidate;
            }
        }
        if (selected == boxes.size()) break;
        const auto parent = boxes[selected];
        const auto axis = parent.axis;
        std::sort(keys.begin() + static_cast<std::ptrdiff_t>(parent.begin),
            keys.begin() + static_cast<std::ptrdiff_t>(parent.end), [axis](auto left, auto right) {
                const auto a = BinChannel(left, axis), b = BinChannel(right, axis);
                return a != b ? a < b : left < right;
            });
        std::uint64_t cumulative = 0;
        auto split = parent.begin;
        const auto half = (parent.weight + 1) / 2;
        do { cumulative += bins[keys[split++]].count; }
        while (split < parent.end - 1 && cumulative < half);
        // 一个重桶跨过中位数时仍保留两个非空子盒；直方图桶不拆成虚构颜色。
        split = std::min(split, parent.end - 1);
        boxes[selected] = MeasureBox(parent.begin, split, keys, bins);
        boxes.push_back(MeasureBox(split, parent.end, keys, bins));
    }
    std::map<std::uint32_t, std::uint8_t> palette;
    for (const auto& box : boxes) {
        std::array<std::uint64_t, 3> sum{};
        for (auto index = box.begin; index < box.end; ++index)
            for (unsigned channel = 0; channel < 3; ++channel) sum[channel] += bins[keys[index]].sum[channel];
        std::uint32_t color = 0;
        for (unsigned channel = 0; channel < 3; ++channel)
            color = (color << 8) | static_cast<std::uint32_t>((sum[channel] + box.weight / 2) / box.weight);
        palette.emplace(color, std::uint8_t{0});
    }
    return palette;
}

std::uint32_t Nearest(std::uint32_t color, const std::map<std::uint32_t, std::uint8_t>& palette) {
    auto chosen = palette.begin()->first;
    unsigned best = 3u * 255u * 255u + 1u;
    for (const auto& entry : palette) {
        unsigned distance = 0;
        for (unsigned shift : {0u, 8u, 16u}) {
            const auto delta = static_cast<int>((color >> shift) & 255u)
                - static_cast<int>((entry.first >> shift) & 255u);
            distance += static_cast<unsigned>(delta * delta);
        }
        if (distance < best) { best = distance; chosen = entry.first; }
    }
    return chosen;
}

} // namespace

DecodedImage MakeDisabledIcon(const DecodedImage& image) {
    DecodedImage result = image;
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    for (std::size_t pixel = 0; pixel < pixels && image.Valid(); ++pixel) {
        const unsigned luma = (299u * At(image, pixel, 0) + 587u * At(image, pixel, 1) + 114u * At(image, pixel, 2)) / 1000u;
        const auto gray = static_cast<std::byte>(luma * 55u / 100u); // 原版暂停图标约为去色后 55% 亮度
        result.rgba[pixel * 4 + 0] = gray;
        result.rgba[pixel * 4 + 1] = gray;
        result.rgba[pixel * 4 + 2] = gray;
    }
    return result;
}

Result<std::vector<std::byte>> EncodeBlpPaletted(const DecodedImage& image) {
    using ResultT = Result<std::vector<std::byte>>;
    if (!image.Valid() || image.width > 4096 || image.height > 4096) {
        return ResultT::Failure("Image.Blp", "encode-dimensions", "BLP 编码的图像尺寸非法。");
    }
    std::vector<DecodedImage> levels{image};
    while (levels.size() < 16 && (levels.back().width > 1 || levels.back().height > 1)) {
        levels.push_back(Downsample(levels.back()));
    }
    // 调色板：全部级别的实际颜色。
    std::map<std::uint32_t, std::uint8_t> palette;
    bool alpha = false;
    for (const auto& level : levels) {
        const std::size_t pixels = static_cast<std::size_t>(level.width) * level.height;
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            palette.emplace(ColorKey(level, pixel), std::uint8_t{0});
            alpha = alpha || At(level, pixel, 3) != 255;
        }
    }
    if (palette.size() > 256) {
        return ResultT::Failure("Image.Blp", "encode-palette", "图像颜色超过 256 种，不能无损编码为调色板 BLP。");
    }
    return ResultT::Success(WritePaletted(levels, std::move(palette), alpha));
}

Result<BlpOpaqueQuantized> EncodeBlpOpaqueQuantized(const DecodedImage& image,
    const BlpOpaqueQuantizeLimits& limits) {
    using Output = Result<BlpOpaqueQuantized>;
    const auto fail = [](const char* code, const char* message) { return Output::Failure("Image.Blp", code, message); };
    const BlpOpaqueQuantizeLimits hard;
    if (limits.maxDimension > hard.maxDimension || limits.maxPixels > hard.maxPixels
        || limits.maxTotalMipPixels > hard.maxTotalMipPixels || limits.maxWorkingBytes > hard.maxWorkingBytes
        || limits.maxOutputBytes > hard.maxOutputBytes)
        return fail("BLP_OPAQUE_LIMITS", "调用方只能收紧不透明 BLP 编码硬预算。");
    // 先验证硬尺寸，再乘法；恶意 uint32 尺寸不能经 DecodedImage::Valid 乘法回绕准入。
    if (image.width == 0 || image.height == 0 || image.width > limits.maxDimension || image.height > limits.maxDimension)
        return fail("BLP_OPAQUE_DIMENSIONS", "不透明 BLP 编码尺寸非法或超出预算。");
    const auto pixels = static_cast<std::size_t>(image.width) * image.height;
    if (pixels > limits.maxPixels || image.rgba.size() != pixels * 4)
        return fail("BLP_OPAQUE_PIXELS", "不透明 BLP 的像素载荷不完整或超出预算。");
    std::size_t total = 0, count = 0;
    auto width = image.width, height = image.height;
    for (;;) {
        const auto levelPixels = static_cast<std::size_t>(width) * height;
        if (total > limits.maxTotalMipPixels || levelPixels > limits.maxTotalMipPixels - total)
            return fail("BLP_OPAQUE_MIP_BUDGET", "不透明 BLP 完整 mip 链超过像素预算。");
        total += levelPixels;
        ++count;
        if (width == 1 && height == 1) break;
        width = std::max(1u, width / 2); height = std::max(1u, height / 2);
    }
    const auto outputBytes = kHeaderSize + kPaletteSize + total;
    if (outputBytes > limits.maxOutputBytes)
        return fail("BLP_OPAQUE_OUTPUT_BUDGET", "不透明 BLP 输出超过字节预算。");
    // 固定 histogram、keys、boxes、map 节点保守载荷及容器写出增长余量。
    // 该值不含借入 image，不声称覆盖分配器管理开销或进程 RSS。
    const auto workingBytes = total * 4 + count * sizeof(DecodedImage)
        + 32768u * (sizeof(HistogramBin) + sizeof(std::uint16_t))
        + 256u * sizeof(ColorBox) + 514u * 64u + outputBytes * 2;
    if (workingBytes > limits.maxWorkingBytes)
        return fail("BLP_OPAQUE_WORKING_BUDGET", "不透明 BLP 编码工作载荷超过预算。");
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) if (At(image, pixel, 3) != 255)
        return fail("BLP_OPAQUE_ALPHA", "此显式编码入口只接受全部 alpha 为 255 的图像。");
    try {
        std::vector<DecodedImage> levels;
        levels.reserve(count);
        levels.push_back(image);
        while (levels.size() < count) levels.push_back(Downsample(levels.back()));
        std::vector<HistogramBin> bins(32768);
        std::map<std::uint32_t, std::uint8_t> palette;
        for (const auto& mip : levels) for (std::size_t pixel = 0; pixel < mip.rgba.size() / 4; ++pixel) {
            const auto color = ColorKey(mip, pixel);
            if (palette.size() <= 256) palette.emplace(color, std::uint8_t{0});
            auto& bin = bins[BinKey(color)];
            ++bin.count;
            for (unsigned channel = 0; channel < 3; ++channel) bin.sum[channel] += At(mip, pixel, static_cast<int>(channel));
        }
        std::vector<std::uint16_t> keys;
        keys.reserve(32768);
        for (std::size_t index = 0; index < bins.size(); ++index)
            if (bins[index].count) keys.push_back(static_cast<std::uint16_t>(index));
        BlpOpaqueQuantized result;
        result.occupiedHistogramBins = static_cast<std::uint32_t>(keys.size());
        result.mipCount = static_cast<std::uint32_t>(count);
        result.totalMipPixels = total;
        if (palette.size() > 256) {
            result.method = BlpOpaquePaletteMethod::WeightedMedianCut5Bit;
            palette = MedianPalette(keys, bins);
            for (auto& mip : levels) for (std::size_t pixel = 0; pixel < mip.rgba.size() / 4; ++pixel) {
                const auto original = ColorKey(mip, pixel);
                const auto chosen = Nearest(original, palette);
                result.quantized |= original != chosen;
                for (unsigned channel = 0; channel < 3; ++channel)
                    mip.rgba[pixel * 4 + channel] = static_cast<std::byte>((chosen >> (16u - channel * 8u)) & 255u);
            }
        }
        result.paletteColorCount = static_cast<std::uint32_t>(palette.size());
        result.bytes = WritePaletted(levels, std::move(palette), false);
        return Output::Success(std::move(result));
    } catch (const std::bad_alloc&) {
        return fail("BLP_OPAQUE_ALLOCATION", "不透明 BLP 编码分配失败，无部分输出。");
    } catch (const std::length_error&) {
        return fail("BLP_OPAQUE_ALLOCATION", "不透明 BLP 编码容量不足，无部分输出。");
    }
}

} // namespace War3Map::Image
