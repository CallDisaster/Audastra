#include "PngDecoder.h"

#include "../Mpq/MpqDecompress.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace War3Map::Image::detail {
namespace {
using Output = Result<DecodedImage>;
Output Fail(std::string code, std::string message) {
    return Output::Failure("Image.Png", std::move(code), std::move(message));
}
std::uint32_t Be32(std::span<const std::byte> d, std::size_t at) {
    return std::to_integer<std::uint32_t>(d[at]) << 24 | std::to_integer<std::uint32_t>(d[at + 1]) << 16
        | std::to_integer<std::uint32_t>(d[at + 2]) << 8 | std::to_integer<std::uint32_t>(d[at + 3]);
}
std::uint16_t Be16(std::span<const std::byte> d, std::size_t at) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(d[at]) << 8 | std::to_integer<unsigned>(d[at + 1]));
}
std::uint32_t Crc(std::span<const std::byte> bytes) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (const auto b : bytes) c = table[(c ^ std::to_integer<std::uint32_t>(b)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
std::uint8_t Paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return static_cast<std::uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}
// 一行反滤波；prior 是同一遍的上一行（首行全零），bpp 为至少 1 的整像素字节数。
bool Unfilter(unsigned type, std::uint8_t* row, const std::uint8_t* prior, std::size_t bytes, std::size_t bpp) {
    const auto left = [&](std::size_t i) { return i >= bpp ? row[i - bpp] : 0; };
    switch (type) {
    case 0: return true;
    case 1: for (std::size_t i = bpp; i < bytes; ++i) row[i] = static_cast<std::uint8_t>(row[i] + row[i - bpp]); return true;
    case 2: for (std::size_t i = 0; i < bytes; ++i) row[i] = static_cast<std::uint8_t>(row[i] + prior[i]); return true;
    case 3: for (std::size_t i = 0; i < bytes; ++i) row[i] = static_cast<std::uint8_t>(row[i] + (left(i) + prior[i]) / 2); return true;
    case 4:
        for (std::size_t i = 0; i < bytes; ++i)
            row[i] = static_cast<std::uint8_t>(row[i] + Paeth(left(i), prior[i], i >= bpp ? prior[i - bpp] : 0));
        return true;
    default: return false;
    }
}
struct Pass { std::uint32_t x0, y0, dx, dy; };
constexpr std::array<Pass, 7> kAdam7{{{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}}};
constexpr std::array<Pass, 1> kSingle{{{0, 0, 1, 1}}};
}

Result<DecodedImage> DecodePng(std::span<const std::byte> data, std::size_t maxPixels) {
    static constexpr std::array<unsigned, 8> kSignature{137, 80, 78, 71, 13, 10, 26, 10};
    if (data.size() < 8 || !std::equal(kSignature.begin(), kSignature.end(), data.begin(),
            [](unsigned a, std::byte b) { return std::to_integer<unsigned>(b) == a; }))
        return Fail("PNG_SIGNATURE", "不是 PNG 文件。");
    std::uint32_t width = 0, height = 0;
    unsigned depth = 0, color = 0, interlace = 0;
    std::vector<std::byte> compressed;
    std::array<std::array<std::uint8_t, 4>, 256> palette{};
    std::size_t paletteSize = 0;
    std::optional<std::array<std::uint16_t, 3>> transparent;
    bool header = false, ended = false, seenData = false, dataClosed = false;
    std::size_t at = 8;
    while (!ended) {
        if (data.size() - at < 12) return Fail("PNG_TRUNCATED", "PNG 数据块不完整。");
        const auto length = Be32(data, at);
        if (length > data.size() - at - 12) return Fail("PNG_TRUNCATED", "PNG 数据块长度越界。");
        const auto typeAndBody = data.subspan(at + 4, 4 + std::size_t(length));
        if (Crc(typeAndBody) != Be32(data, at + 8 + length)) return Fail("PNG_CRC", "PNG 数据块校验和不符。");
        std::string type(4, ' ');
        for (int i = 0; i < 4; ++i) type[i] = std::to_integer<char>(typeAndBody[i]);
        const auto body = typeAndBody.subspan(4);
        const auto u8 = [&](std::size_t i) { return std::to_integer<unsigned>(body[i]); };
        if (!header && type != "IHDR") return Fail("PNG_HEADER", "PNG 第一个数据块不是 IHDR。");
        if (seenData && type != "IDAT") dataClosed = true;
        if (type == "IHDR") {
            if (header || length != 13) return Fail("PNG_HEADER", "PNG 的 IHDR 重复或长度不对。");
            width = Be32(body, 0); height = Be32(body, 4); depth = u8(8); color = u8(9); interlace = u8(12);
            const bool depthValid = (color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16))
                || (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8))
                || ((color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16));
            if (!depthValid || u8(10) != 0 || u8(11) != 0 || interlace > 1)
                return Fail("PNG_FORMAT_UNSUPPORTED", "PNG 颜色类型、位深、压缩、滤波或交错方式无效。");
            if (!width || !height || width > 0x7FFFFFFFu || height > 0x7FFFFFFFu
                || std::uint64_t(width) * height > maxPixels)
                return Fail("PNG_PIXEL_LIMIT", "PNG 尺寸为零或超过像素预算。");
            header = true;
        } else if (type == "PLTE") {
            if (seenData || paletteSize || !length || length % 3 || length > 768)
                return Fail("PNG_PALETTE_INVALID", "PNG 调色板位置或长度无效。");
            paletteSize = length / 3;
            for (std::size_t i = 0; i < paletteSize; ++i)
                palette[i] = {static_cast<std::uint8_t>(u8(i * 3)), static_cast<std::uint8_t>(u8(i * 3 + 1)),
                    static_cast<std::uint8_t>(u8(i * 3 + 2)), 255};
        } else if (type == "tRNS") {
            if (seenData) return Fail("PNG_TRNS_INVALID", "PNG 透明块出现在图像数据之后。");
            if (color == 3) {
                if (!paletteSize || length > paletteSize) return Fail("PNG_TRNS_INVALID", "PNG 调色板透明块长度无效。");
                for (std::size_t i = 0; i < length; ++i) palette[i][3] = static_cast<std::uint8_t>(u8(i));
            } else if (color == 0 && length == 2) {
                transparent = std::array<std::uint16_t, 3>{Be16(body, 0), 0, 0};
            } else if (color == 2 && length == 6) {
                transparent = std::array<std::uint16_t, 3>{Be16(body, 0), Be16(body, 2), Be16(body, 4)};
            } else {
                return Fail("PNG_TRNS_INVALID", "PNG 透明块与颜色类型不符。");
            }
        } else if (type == "IDAT") {
            if (dataClosed) return Fail("PNG_DATA_SPLIT", "PNG 图像数据块不连续。");
            seenData = true;
            compressed.insert(compressed.end(), body.begin(), body.end());
        } else if (type == "IEND") {
            ended = true;
        } else if ((std::to_integer<unsigned>(typeAndBody[0]) & 0x20) == 0) {
            return Fail("PNG_CHUNK_UNSUPPORTED", "PNG 含未知的关键数据块：" + type);
        }
        at += 12 + std::size_t(length);
    }
    if (!seenData) return Fail("PNG_NO_DATA", "PNG 没有图像数据。");
    if (color == 3 && !paletteSize) return Fail("PNG_PALETTE_INVALID", "调色板 PNG 缺少调色板。");

    const unsigned channels = color == 2 ? 3 : color == 4 ? 2 : color == 6 ? 4 : 1;
    const std::uint64_t bitsPerPixel = std::uint64_t(channels) * depth;
    const std::size_t bpp = static_cast<std::size_t>(std::max<std::uint64_t>(1, bitsPerPixel / 8));
    const std::span<const Pass> passes = interlace ? std::span<const Pass>(kAdam7) : std::span<const Pass>(kSingle);
    const auto extent = [](std::uint32_t size, std::uint32_t start, std::uint32_t step) {
        return size > start ? (size - start + step - 1) / step : 0u;
    };
    std::uint64_t rawSize = 0;
    for (const auto& pass : passes) {
        const auto w = extent(width, pass.x0, pass.dx), h = extent(height, pass.y0, pass.dy);
        if (w && h) rawSize += std::uint64_t(h) * (1 + (std::uint64_t(w) * bitsPerPixel + 7) / 8);
    }
    if (rawSize > 512ull * 1024 * 1024) return Fail("PNG_PIXEL_LIMIT", "PNG 解压后数据超过上限。");
    auto raw = Mpq::InflateLimited(compressed, static_cast<std::size_t>(rawSize));
    if (!raw.Succeeded() || raw.value->size() != rawSize)
        return Fail("PNG_DATA_INVALID", "PNG 图像数据解压失败或长度与尺寸不符。");

    DecodedImage image;
    image.width = width; image.height = height;
    image.rgba.resize(std::size_t(width) * height * 4);
    const auto maxSample = (1u << depth) - 1;
    const auto to8 = [&](unsigned v) -> std::uint8_t {
        if (depth == 16) return static_cast<std::uint8_t>(v >> 8);
        if (depth == 8) return static_cast<std::uint8_t>(v);
        return static_cast<std::uint8_t>(v * 255 / maxSample);
    };
    std::vector<std::uint8_t> row, prior;
    std::size_t offset = 0;
    for (const auto& pass : passes) {
        const auto w = extent(width, pass.x0, pass.dx), h = extent(height, pass.y0, pass.dy);
        if (!w || !h) continue;
        const auto rowBytes = static_cast<std::size_t>((std::uint64_t(w) * bitsPerPixel + 7) / 8);
        prior.assign(rowBytes, 0);
        for (std::uint32_t y = 0; y < h; ++y) {
            const auto filter = std::to_integer<unsigned>((*raw.value)[offset]);
            row.resize(rowBytes);
            for (std::size_t i = 0; i < rowBytes; ++i) row[i] = std::to_integer<std::uint8_t>((*raw.value)[offset + 1 + i]);
            offset += 1 + rowBytes;
            if (!Unfilter(filter, row.data(), prior.data(), rowBytes, bpp))
                return Fail("PNG_FILTER_INVALID", "PNG 行滤波类型无效。");
            const auto sample = [&](std::size_t index) -> unsigned {
                if (depth == 16) return unsigned(row[index * 2]) << 8 | row[index * 2 + 1];
                if (depth == 8) return row[index];
                const auto bit = index * depth;
                return (row[bit / 8] >> (8 - depth - bit % 8)) & maxSample;
            };
            for (std::uint32_t x = 0; x < w; ++x) {
                std::array<std::uint8_t, 4> pixel{0, 0, 0, 255};
                const std::size_t s = std::size_t(x) * channels;
                if (color == 3) {
                    const auto index = sample(s);
                    if (index >= paletteSize) return Fail("PNG_PALETTE_INDEX", "PNG 像素的调色板序号越界。");
                    pixel = palette[index];
                } else if (color == 0 || color == 4) {
                    const auto gray = sample(s);
                    pixel[0] = pixel[1] = pixel[2] = to8(gray);
                    if (color == 4) pixel[3] = to8(sample(s + 1));
                    else if (transparent && gray == (*transparent)[0]) pixel[3] = 0;
                } else {
                    const unsigned r = sample(s), g = sample(s + 1), b = sample(s + 2);
                    pixel = {to8(r), to8(g), to8(b), 255};
                    if (color == 6) pixel[3] = to8(sample(s + 3));
                    else if (transparent && r == (*transparent)[0] && g == (*transparent)[1] && b == (*transparent)[2]) pixel[3] = 0;
                }
                const auto target = (std::size_t(pass.y0 + y * pass.dy) * width + pass.x0 + std::size_t(x) * pass.dx) * 4;
                for (int c = 0; c < 4; ++c) image.rgba[target + c] = std::byte{pixel[c]};
            }
            std::swap(row, prior);
        }
    }
    return Output::Success(std::move(image));
}
}
