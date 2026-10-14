#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/Text/TextUtil.h"
#include <array>

namespace War3Map::Image {
Result<DecodedImage> DecodeTexture(std::string_view path, std::span<const std::byte> data, std::size_t maxPixels) {
    using Return = Result<DecodedImage>;
    const auto fail = [](std::string code, std::string message) { return Return::Failure("Image.Texture", std::move(code), std::move(message)); };
    if (maxPixels == 0 || maxPixels > 16 * 1024 * 1024) return fail("TEXTURE_PIXEL_LIMIT", "贴图像素预算无效。");
    // 文件头是 BLP 时按 BLP 解码（有的资源把 BLP 存成 .tga 名）；其余 .tga 按 TGA。
    const bool blpHeader = data.size() >= 4 && std::to_integer<char>(data[0]) == 'B' && std::to_integer<char>(data[1]) == 'L'
        && std::to_integer<char>(data[2]) == 'P';
    if (blpHeader || !Text::LowerAscii(path).ends_with(".tga")) {
        auto image = DecodeBlp(data);
        if (image.Succeeded() && static_cast<std::size_t>(image.value->width) * image.value->height > maxPixels)
            return fail("TEXTURE_PIXEL_LIMIT", "贴图像素数超过本次预算。");
        return image;
    }
    if (data.size() < 18) return fail("TGA_HEADER_TRUNCATED", "TGA头不完整。");
    const auto byte = [&data](std::size_t at) { return std::to_integer<unsigned>(data[at]); };
    const auto type = byte(2), depth = byte(16), descriptor = byte(17);
    if (byte(1) != 0 || (type != 2 && type != 10) || (depth != 24 && depth != 32)
        || (descriptor & 0xc0) != 0 || (descriptor & 15) != (depth == 32 ? 8u : 0u))
        return fail("TGA_FORMAT_UNSUPPORTED", "TGA预览仅支持无色表、无交错的24位RGB或32位RGBA真彩色（可带RLE）。");
    DecodedImage image; image.width = byte(12) | (byte(13) << 8); image.height = byte(14) | (byte(15) << 8);
    const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
    if (!image.width || !image.height || count > maxPixels) return fail("TGA_PIXEL_LIMIT", "TGA尺寸为零或超过像素预算。");
    const auto pixelBytes = depth / 8;
    std::size_t input = 18 + byte(0), output = 0;
    if (input > data.size()) return fail("TGA_ID_TRUNCATED", "TGA图像说明截断。");
    image.rgba.resize(count * 4);
    const auto store = [&](const std::array<std::byte, 4>& pixel) {
        const auto x = output % image.width, y = output / image.width;
        const auto targetX = descriptor & 16 ? image.width - 1 - x : x;
        const auto targetY = descriptor & 32 ? y : image.height - 1 - y;
        const auto target = (targetY * image.width + targetX) * 4;
        for (std::size_t channel = 0; channel < 4; ++channel) image.rgba[target + channel] = pixel[channel];
        ++output;
    };
    while (output < count) {
        unsigned run = 1; bool repeat = false;
        if (type == 10) {
            if (input >= data.size()) return fail("TGA_PACKET_TRUNCATED", "TGA压缩块头截断。");
            const auto header = byte(input++); run = (header & 127) + 1; repeat = (header & 128) != 0;
        }
        if (run > count - output) return fail("TGA_PACKET_OVERFLOW", "TGA压缩块越过像素边界。");
        std::array<std::byte, 4> pixel{};
        for (unsigned index = 0; index < run; ++index) {
            if (!repeat || index == 0) {
                if (pixelBytes > data.size() - input) return fail("TGA_PIXELS_TRUNCATED", "TGA像素载荷截断。");
                pixel = {data[input + 2], data[input + 1], data[input], depth == 32 ? data[input + 3] : std::byte{255}};
                input += pixelBytes;
            }
            store(pixel);
        }
    }
    return Return::Success(std::move(image));
}
}
