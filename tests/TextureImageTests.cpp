#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/Image/TextureImage.h"

#include "fixtures/JpegFixtures.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 独立图片文件（地图材质常用的 .png/.jpg）解码：原创合成数据，逐像素对照，坏数据明确拒绝。
namespace {
using War3Map::Image::DecodedImage;
using War3Map::Image::DecodeTexture;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename T>
void RequireError(const War3Map::Result<T>& result, const std::string& code) {
    Require(!result.Succeeded(), "应拒绝输入：" + code);
    std::string seen;
    for (const auto& diagnostic : result.diagnostics) { if (diagnostic.code == code) return; seen += " " + diagnostic.code; }
    throw std::runtime_error("缺少预期诊断：" + code + "，实际" + seen);
}
DecodedImage Decoded(const War3Map::Result<DecodedImage>& result, const std::string& name) {
    std::string reason;
    for (const auto& diagnostic : result.diagnostics) reason += " " + diagnostic.code + ": " + diagnostic.message;
    Require(result.Succeeded() && result.value && result.value->Valid(), name + " 解码失败" + reason);
    return *result.value;
}

// ---- PNG 编码（测试自用：存储块 zlib，不压缩）----
using Bytes = std::vector<std::uint8_t>;
std::uint32_t Crc(const std::uint8_t* data, std::size_t size) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}
void Be32(Bytes& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void Chunk(Bytes& png, const char* type, const Bytes& body, bool breakCrc = false) {
    Be32(png, static_cast<std::uint32_t>(body.size()));
    const auto start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), body.begin(), body.end());
    Be32(png, Crc(png.data() + start, png.size() - start) ^ (breakCrc ? 1u : 0u));
}
Bytes Zlib(const Bytes& raw) {
    Bytes out{0x78, 0x01};
    std::size_t at = 0;
    do {
        const auto size = std::min<std::size_t>(raw.size() - at, 65535);
        out.push_back(at + size == raw.size() ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(size)); out.push_back(static_cast<std::uint8_t>(size >> 8));
        out.push_back(static_cast<std::uint8_t>(~size)); out.push_back(static_cast<std::uint8_t>(~size >> 8));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
    } while (at < raw.size());
    std::uint32_t a = 1, b = 0;
    for (const auto v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    Be32(out, b << 16 | a);
    return out;
}
struct PngSpec {
    std::uint32_t width = 0, height = 0;
    std::uint8_t depth = 8, color = 6, interlace = 0;
    Bytes palette, transparency;
    std::vector<std::pair<std::string, Bytes>> extra; // IDAT 之前插入的数据块
};
Bytes Png(const PngSpec& spec, const Bytes& filteredRaw, bool breakCrc = false) {
    Bytes png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Bytes header;
    Be32(header, spec.width); Be32(header, spec.height);
    header.insert(header.end(), {spec.depth, spec.color, 0, 0, spec.interlace});
    Chunk(png, "IHDR", header);
    if (!spec.palette.empty()) Chunk(png, "PLTE", spec.palette);
    if (!spec.transparency.empty()) Chunk(png, "tRNS", spec.transparency);
    for (const auto& [type, body] : spec.extra) Chunk(png, type.c_str(), body);
    const auto compressed = Zlib(filteredRaw);
    // 拆成两个 IDAT，覆盖连续数据块拼接。
    const auto half = compressed.size() / 2;
    Chunk(png, "IDAT", Bytes(compressed.begin(), compressed.begin() + static_cast<std::ptrdiff_t>(half)), breakCrc);
    Chunk(png, "IDAT", Bytes(compressed.begin() + static_cast<std::ptrdiff_t>(half), compressed.end()));
    Chunk(png, "IEND", {});
    return png;
}
std::vector<std::byte> AsBytes(const Bytes& bytes) {
    std::vector<std::byte> out(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) out[i] = std::byte{bytes[i]};
    return out;
}
War3Map::Result<DecodedImage> DecodePngBytes(const Bytes& png, std::string_view name = "x.png",
    std::size_t maxPixels = 16 * 1024 * 1024) {
    return DecodeTexture(name, AsBytes(png), maxPixels);
}
std::uint8_t PaethPredict(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return static_cast<std::uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}
// 按给定滤波类型对未滤波的行做正向滤波（行首加类型字节）。
Bytes Filter(const std::vector<Bytes>& rows, std::size_t bpp, const std::vector<std::uint8_t>& types) {
    Bytes out;
    Bytes prior(rows.empty() ? 0 : rows[0].size(), 0);
    for (std::size_t y = 0; y < rows.size(); ++y) {
        const auto& row = rows[y];
        const auto type = types[y % types.size()];
        out.push_back(type);
        for (std::size_t i = 0; i < row.size(); ++i) {
            const int left = i >= bpp ? row[i - bpp] : 0, up = prior[i], upLeft = i >= bpp ? prior[i - bpp] : 0;
            int predicted = 0;
            if (type == 1) predicted = left;
            else if (type == 2) predicted = up;
            else if (type == 3) predicted = (left + up) / 2;
            else if (type == 4) predicted = PaethPredict(left, up, upLeft);
            out.push_back(static_cast<std::uint8_t>(row[i] - predicted));
        }
        prior = row;
    }
    return out;
}
void RequirePixel(const DecodedImage& image, std::uint32_t x, std::uint32_t y, std::array<int, 4> expected, const std::string& what) {
    const auto* p = image.rgba.data() + (static_cast<std::size_t>(y) * image.width + x) * 4;
    for (int c = 0; c < 4; ++c)
        Require(std::to_integer<int>(p[c]) == expected[c], what + " 像素 (" + std::to_string(x) + "," + std::to_string(y)
            + ") 通道 " + std::to_string(c) + " 期望 " + std::to_string(expected[c]) + " 实得 " + std::to_string(std::to_integer<int>(p[c])));
}

// 4×3 RGBA8，每行用不同滤波（含 Sub/Up/Average/Paeth）。
std::vector<Bytes> RgbaRows() {
    std::vector<Bytes> rows;
    for (std::uint32_t y = 0; y < 5; ++y) {
        Bytes row;
        for (std::uint32_t x = 0; x < 4; ++x)
            row.insert(row.end(), {static_cast<std::uint8_t>(x * 60 + y * 7), static_cast<std::uint8_t>(200 - y * 30),
                static_cast<std::uint8_t>((x * y * 37) & 0xFF), static_cast<std::uint8_t>(255 - x * 50)});
        rows.push_back(row);
    }
    return rows;
}
void TestPngFiltersAndChannels() {
    const auto rows = RgbaRows();
    PngSpec spec; spec.width = 4; spec.height = 5;
    const auto image = Decoded(DecodePngBytes(Png(spec, Filter(rows, 4, {0, 1, 2, 3, 4}))), "RGBA 滤波");
    for (std::uint32_t y = 0; y < 5; ++y) for (std::uint32_t x = 0; x < 4; ++x)
        RequirePixel(image, x, y, {rows[y][x * 4], rows[y][x * 4 + 1], rows[y][x * 4 + 2], rows[y][x * 4 + 3]}, "RGBA");
    // 未知的辅助块忽略；文件头决定格式，与扩展名无关。
    spec.extra = {{"tEXt", Bytes{'k', 0, 'v'}}};
    Require(Decoded(DecodePngBytes(Png(spec, Filter(rows, 4, {0})), "Textures\\a.tga"), "辅助块").rgba == image.rgba,
        "辅助块或文件头识别改变了像素");

    // 16 位 RGB 取高字节，色键透明按完整 16 位比较。
    PngSpec deep; deep.width = 2; deep.height = 1; deep.depth = 16; deep.color = 2;
    deep.transparency = {0x12, 0x34, 0x00, 0x00, 0xAB, 0xCD};
    const Bytes deepRow{0x12, 0x34, 0x00, 0x00, 0xAB, 0xCD, 0x12, 0x35, 0x00, 0x00, 0xAB, 0xCD};
    const auto rgb16 = Decoded(DecodePngBytes(Png(deep, Filter({deepRow}, 6, {1}))), "RGB16");
    RequirePixel(rgb16, 0, 0, {0x12, 0x00, 0xAB, 0}, "RGB16 色键");
    RequirePixel(rgb16, 1, 0, {0x12, 0x00, 0xAB, 255}, "RGB16 非色键");

    // 1 位灰度：位从高到低，0/1 扩展到 0/255；灰度色键。
    PngSpec bits; bits.width = 10; bits.height = 1; bits.depth = 1; bits.color = 0; bits.transparency = {0, 1};
    const auto gray1 = Decoded(DecodePngBytes(Png(bits, Bytes{0, 0b10110000, 0b01000000})), "灰度 1 位");
    const int expected[] = {1, 0, 1, 1, 0, 0, 0, 0, 0, 1};
    for (std::uint32_t x = 0; x < 10; ++x)
        RequirePixel(gray1, x, 0, {expected[x] * 255, expected[x] * 255, expected[x] * 255, expected[x] ? 0 : 255}, "灰度 1 位");

    // 2 位调色板 + 调色板透明；4 位灰度按 255/15 放大。
    PngSpec indexed; indexed.width = 3; indexed.height = 1; indexed.depth = 2; indexed.color = 3;
    indexed.palette = {10, 20, 30, 40, 50, 60, 70, 80, 90}; indexed.transparency = {255, 128};
    const auto palette = Decoded(DecodePngBytes(Png(indexed, Bytes{0, 0b00011000})), "调色板");
    RequirePixel(palette, 0, 0, {10, 20, 30, 255}, "调色板 0");
    RequirePixel(palette, 1, 0, {40, 50, 60, 128}, "调色板 1");
    RequirePixel(palette, 2, 0, {70, 80, 90, 255}, "调色板 2（无透明项）");
    PngSpec nibble; nibble.width = 2; nibble.height = 1; nibble.depth = 4; nibble.color = 0;
    const auto gray4 = Decoded(DecodePngBytes(Png(nibble, Bytes{0, 0xF7})), "灰度 4 位");
    RequirePixel(gray4, 0, 0, {255, 255, 255, 255}, "灰度 4 位");
    RequirePixel(gray4, 1, 0, {119, 119, 119, 255}, "灰度 4 位");

    // 灰度+透明 8 位。
    PngSpec grayAlpha; grayAlpha.width = 1; grayAlpha.height = 1; grayAlpha.color = 4;
    RequirePixel(Decoded(DecodePngBytes(Png(grayAlpha, Bytes{0, 90, 33})), "灰度透明"), 0, 0, {90, 90, 90, 33}, "灰度透明");
}

// Adam7 交错与非交错同一幅图逐像素一致（含小于 8 的尺寸，部分遍为空）。
void TestPngInterlace() {
    for (const auto [width, height] : {std::pair<std::uint32_t, std::uint32_t>{5, 3}, {1, 1}, {9, 10}}) {
        std::vector<Bytes> rows;
        for (std::uint32_t y = 0; y < height; ++y) {
            Bytes row;
            for (std::uint32_t x = 0; x < width; ++x)
                row.insert(row.end(), {static_cast<std::uint8_t>(x * 25), static_cast<std::uint8_t>(y * 25), static_cast<std::uint8_t>(x ^ y)});
            rows.push_back(row);
        }
        PngSpec plain; plain.width = width; plain.height = height; plain.color = 2;
        const auto expected = Decoded(DecodePngBytes(Png(plain, Filter(rows, 3, {4, 1}))), "非交错");
        constexpr std::uint32_t passes[7][4]{{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
        Bytes raw;
        for (const auto& pass : passes) {
            std::vector<Bytes> sub;
            for (std::uint32_t y = pass[1]; y < height; y += pass[3]) {
                Bytes row;
                for (std::uint32_t x = pass[0]; x < width; x += pass[2])
                    row.insert(row.end(), rows[y].begin() + x * 3, rows[y].begin() + x * 3 + 3);
                if (!row.empty()) sub.push_back(row);
            }
            const auto filtered = Filter(sub, 3, {2, 3, 4});
            raw.insert(raw.end(), filtered.begin(), filtered.end());
        }
        auto interlaced = plain; interlaced.interlace = 1;
        Require(Decoded(DecodePngBytes(Png(interlaced, raw)), "交错").rgba == expected.rgba,
            "Adam7 交错与非交错像素不一致 " + std::to_string(width) + "x" + std::to_string(height));
    }
}

// 坏数据：校验和、截断、未知关键块、调色板越界、解压后超长（包括小输入解出大数据）、像素预算。
void TestPngRejects() {
    const auto rows = RgbaRows();
    PngSpec spec; spec.width = 4; spec.height = 5;
    const auto raw = Filter(rows, 4, {0});
    RequireError(DecodePngBytes(Png(spec, raw, true)), "PNG_CRC");
    auto truncated = Png(spec, raw); truncated.resize(truncated.size() - 20);
    RequireError(DecodePngBytes(truncated), "PNG_TRUNCATED");
    auto critical = spec; critical.extra = {{"ABCD", Bytes{1}}};
    RequireError(DecodePngBytes(Png(critical, raw)), "PNG_CHUNK_UNSUPPORTED");
    auto longer = raw; longer.push_back(0);
    RequireError(DecodePngBytes(Png(spec, longer)), "PNG_DATA_INVALID");
    // 1×1 图却带 1 MiB 解压数据：必须在超过尺寸时停下，不把数据全部解出。
    PngSpec tiny; tiny.width = 1; tiny.height = 1;
    RequireError(DecodePngBytes(Png(tiny, Bytes(1024 * 1024, 0))), "PNG_DATA_INVALID");
    auto badFilter = raw; badFilter[0] = 5;
    RequireError(DecodePngBytes(Png(spec, badFilter)), "PNG_FILTER_INVALID");
    PngSpec indexed; indexed.width = 1; indexed.height = 1; indexed.color = 3; indexed.palette = {1, 2, 3};
    RequireError(DecodePngBytes(Png(indexed, Bytes{0, 1})), "PNG_PALETTE_INDEX");
    indexed.palette.clear();
    RequireError(DecodePngBytes(Png(indexed, Bytes{0, 0})), "PNG_PALETTE_INVALID");
    auto badDepth = spec; badDepth.depth = 4;
    RequireError(DecodePngBytes(Png(badDepth, raw)), "PNG_FORMAT_UNSUPPORTED");
    RequireError(DecodePngBytes(Png(spec, raw), "x.png", 19), "PNG_PIXEL_LIMIT");
    Require(DecodePngBytes(Png(spec, raw), "x.png", 20).Succeeded(), "恰好等于像素预算的 PNG 被拒绝");
    RequireError(DecodeTexture("x.png", AsBytes(Bytes{1, 2, 3})), "PNG_SIGNATURE");
}

// 独立 .jpg 与 BLP 内嵌 JPEG 用同一解码器、同一 YCbCr 换算；文件头优先于扩展名；四分量明确拒绝。
std::vector<std::byte> Jpeg(std::span<const unsigned char> bytes) {
    std::vector<std::byte> out(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) out[i] = std::byte{bytes[i]};
    return out;
}
std::vector<std::byte> WrapBlp(std::span<const unsigned char> jpeg, std::uint32_t width, std::uint32_t height) {
    std::vector<std::byte> blp(160);
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) blp[at + i] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
    };
    blp[0] = std::byte{'B'}; blp[1] = std::byte{'L'}; blp[2] = std::byte{'P'}; blp[3] = std::byte{'1'};
    put(4, 0); put(8, 0); put(12, width); put(16, height); put(20, 4); put(24, 0); put(28, 160);
    put(92, static_cast<std::uint32_t>(jpeg.size())); put(156, 0);
    for (const unsigned char b : jpeg) blp.push_back(static_cast<std::byte>(b));
    return blp;
}
void TestJpegFiles() {
    const auto rgb = Decoded(DecodeTexture("Sonne\\Textures\\a.jpg", Jpeg(kRgbBaseline)), "RGB JPEG");
    const auto blp = Decoded(War3Map::Image::DecodeBlp(WrapBlp(kRgbBaseline, 37, 21)), "BLP 内 JPEG");
    Require(rgb.width == 37 && rgb.height == 21 && rgb.rgba == blp.rgba, "独立 JPEG 与 BLP 内 JPEG 像素不同");
    Require(Decoded(DecodeTexture("b.JPEG", Jpeg(kRgbProgressive)), "渐进式").rgba == rgb.rgba, "渐进式 JPEG 像素不同");
    Require(Decoded(DecodeTexture("c.tga", Jpeg(kRgbBaseline)), "文件头识别").rgba == rgb.rgba, "JPEG 文件头未优先于扩展名");
    const auto gray = Decoded(DecodeTexture("g.jpg", Jpeg(kGrayBaseline)), "灰度 JPEG");
    for (std::size_t i = 0; i < gray.rgba.size(); i += 4)
        Require(gray.rgba[i] == gray.rgba[i + 1] && gray.rgba[i] == gray.rgba[i + 2] && gray.rgba[i + 3] == std::byte{255},
            "灰度 JPEG 未展开为不透明灰色");
    RequireError(DecodeTexture("k.jpg", Jpeg(kCmykBaseline)), "JPEG_COMPONENTS_UNSUPPORTED");
}
}

int main() {
    try {
        TestPngFiltersAndChannels();
        TestPngInterlace();
        TestPngRejects();
        TestJpegFiles();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "texture image: PNG channels/depths/filters/interlace/rejects and JPEG files passed\n";
    return 0;
}
