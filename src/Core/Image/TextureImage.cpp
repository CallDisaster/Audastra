#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/Text/TextUtil.h"
#include "JpegDecoder.h"
#include "PngDecoder.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace War3Map::Image {
namespace {
// 三分量 JPEG 默认是 YCbCr（JFIF/Exif）；Adobe APP14 段的 transform=0 表示直接存 RGB。只看 SOS 之前的标记段。
bool JpegStoresRgb(std::span<const std::byte> data) {
    const auto at = [&data](std::size_t index) { return std::to_integer<unsigned>(data[index]); };
    std::size_t index = 2;
    while (index + 4 <= data.size() && at(index) == 0xFF) {
        const auto marker = at(index + 1);
        if (marker == 0xDA || marker == 0xD9) break;
        const std::size_t length = (at(index + 2) << 8) | at(index + 3);
        if (length < 2 || length > data.size() - index - 2) break;
        // 段内：Adobe(5) 版本(2) flags0(2) flags1(2) transform(1)。
        if (marker == 0xEE && length >= 14 && at(index + 4) == 'A' && at(index + 5) == 'd' && at(index + 6) == 'o'
            && at(index + 7) == 'b' && at(index + 8) == 'e') return at(index + 15) == 0;
        index += 2 + length;
    }
    return false;
}
// 灰度或三分量 JPEG（含渐进式、色度子采样）。CMYK 等四分量明确拒绝，不猜色彩空间。
Result<DecodedImage> DecodeJpegTexture(std::span<const std::byte> data, std::size_t maxPixels) {
    using Return = Result<DecodedImage>;
    auto jpeg = detail::DecodeJpeg(data);
    if (!jpeg.Succeeded()) return {std::nullopt, std::move(jpeg.diagnostics)};
    const auto& source = *jpeg.value;
    if (source.width <= 0 || source.height <= 0
        || static_cast<std::size_t>(source.width) * static_cast<std::size_t>(source.height) > maxPixels)
        return Return::Failure("Image.Texture", "JPEG_PIXEL_LIMIT", "JPEG尺寸为零或超过像素预算。");
    if (source.components != 1 && source.components != 3)
        return Return::Failure("Image.Texture", "JPEG_COMPONENTS_UNSUPPORTED", "只支持灰度或三分量（YCbCr/RGB）JPEG，不支持CMYK。");
    DecodedImage image;
    image.width = static_cast<std::uint32_t>(source.width); image.height = static_cast<std::uint32_t>(source.height);
    const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
    image.rgba.resize(count * 4);
    const bool rgb = source.components == 3 && JpegStoresRgb(data);
    for (std::size_t i = 0; i < count; ++i) {
        auto* out = image.rgba.data() + i * 4;
        if (source.components == 1) {
            out[0] = out[1] = out[2] = static_cast<std::byte>(source.pixels[i]);
        } else if (rgb) {
            for (int c = 0; c < 3; ++c) out[c] = static_cast<std::byte>(source.pixels[i * 3 + c]);
        } else {
            const auto converted = detail::YCbCrToRgb(source.pixels[i * 3], source.pixels[i * 3 + 1], source.pixels[i * 3 + 2]);
            for (int c = 0; c < 3; ++c) out[c] = static_cast<std::byte>(converted[c]);
        }
        out[3] = std::byte{255};
    }
    return Return::Success(std::move(image));
}
}
Result<DecodedImage> DecodeTexture(std::string_view path, std::span<const std::byte> data, std::size_t maxPixels) {
    using Return = Result<DecodedImage>;
    const auto fail = [](std::string code, std::string message) { return Return::Failure("Image.Texture", std::move(code), std::move(message)); };
    if (maxPixels == 0 || maxPixels > 16 * 1024 * 1024) return fail("TEXTURE_PIXEL_LIMIT", "贴图像素预算无效。");
    // 文件头是 BLP 时按 BLP 解码（有的资源把 BLP 存成 .tga 名）；其余 .tga 按 TGA。
    const bool blpHeader = data.size() >= 4 && std::to_integer<char>(data[0]) == 'B' && std::to_integer<char>(data[1]) == 'L'
        && std::to_integer<char>(data[2]) == 'P';
    const bool jpegHeader = data.size() >= 3 && std::to_integer<unsigned>(data[0]) == 0xFF
        && std::to_integer<unsigned>(data[1]) == 0xD8 && std::to_integer<unsigned>(data[2]) == 0xFF;
    const auto lowerPath = Text::LowerAscii(path);
    const bool pngHeader = data.size() >= 8 && std::to_integer<unsigned>(data[0]) == 0x89
        && std::to_integer<char>(data[1]) == 'P' && std::to_integer<char>(data[2]) == 'N' && std::to_integer<char>(data[3]) == 'G';
    if (!blpHeader && (pngHeader || lowerPath.ends_with(".png"))) return detail::DecodePng(data, maxPixels);
    if (!blpHeader && (jpegHeader || lowerPath.ends_with(".jpg") || lowerPath.ends_with(".jpeg")))
        return DecodeJpegTexture(data, maxPixels);
    if (blpHeader || !lowerPath.ends_with(".tga")) {
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
