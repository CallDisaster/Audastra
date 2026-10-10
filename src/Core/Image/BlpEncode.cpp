#include "War3MapCore/Image/BlpImage.h"

#include <algorithm>
#include <array>
#include <map>

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
    std::vector<std::byte> bytes(kHeaderSize + kPaletteSize, std::byte{0});
    const char magic[4] = {'B', 'L', 'P', '1'};
    for (int index = 0; index < 4; ++index) bytes[static_cast<std::size_t>(index)] = static_cast<std::byte>(magic[index]);
    PutU32(bytes, 4, 1);                 // content：调色板
    PutU32(bytes, 8, alpha ? 8u : 0u);   // alphaBits
    PutU32(bytes, 12, image.width);
    PutU32(bytes, 16, image.height);
    PutU32(bytes, 20, alpha ? 4u : 5u);  // extra（有 alpha 的调色板图常见为 4，无 alpha 为 5）
    PutU32(bytes, 24, 1);                // hasMipmaps
    std::uint8_t next = 0;
    for (auto& [color, index] : palette) {
        index = next;
        const std::size_t entry = kHeaderSize + static_cast<std::size_t>(next) * 4;
        bytes[entry + 0] = static_cast<std::byte>(color & 0xFFu);         // B
        bytes[entry + 1] = static_cast<std::byte>((color >> 8) & 0xFFu);  // G
        bytes[entry + 2] = static_cast<std::byte>((color >> 16) & 0xFFu); // R
        bytes[entry + 3] = std::byte{0};
        ++next;
    }
    for (std::size_t level = 0; level < levels.size(); ++level) {
        const auto& mip = levels[level];
        const std::size_t pixels = static_cast<std::size_t>(mip.width) * mip.height;
        PutU32(bytes, 28 + level * 4, static_cast<std::uint32_t>(bytes.size()));
        PutU32(bytes, 92 + level * 4, static_cast<std::uint32_t>(pixels * (alpha ? 2 : 1)));
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) bytes.push_back(static_cast<std::byte>(palette[ColorKey(mip, pixel)]));
        if (alpha) {
            for (std::size_t pixel = 0; pixel < pixels; ++pixel) bytes.push_back(mip.rgba[pixel * 4 + 3]);
        }
    }
    return ResultT::Success(std::move(bytes));
}

} // namespace War3Map::Image
