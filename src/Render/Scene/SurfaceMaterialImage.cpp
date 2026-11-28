#include "War3MapCore/Render/SurfaceMaterialImage.h"

#include <algorithm>
#include <cmath>

namespace War3Map::Render {
namespace {
using Format = SurfaceMaterialImageFormat;

struct Bytes {
    std::span<const std::byte> data;
    [[nodiscard]] bool Has(std::size_t at, std::size_t count) const noexcept { return at <= data.size() && count <= data.size() - at; }
    [[nodiscard]] unsigned U8(std::size_t at) const noexcept { return std::to_integer<unsigned>(data[at]); }
    [[nodiscard]] std::uint32_t Le16(std::size_t at) const noexcept { return U8(at) | (U8(at + 1) << 8); }
    [[nodiscard]] std::uint32_t Be16(std::size_t at) const noexcept { return (U8(at) << 8) | U8(at + 1); }
    [[nodiscard]] std::uint32_t Le32(std::size_t at) const noexcept { return Le16(at) | (Le16(at + 2) << 16); }
    [[nodiscard]] std::uint32_t Be32(std::size_t at) const noexcept { return (Be16(at) << 16) | Be16(at + 2); }
    [[nodiscard]] bool Text(std::size_t at, std::string_view text) const noexcept {
        if (!Has(at, text.size())) return false;
        for (std::size_t i = 0; i < text.size(); ++i)
            if (U8(at + i) != static_cast<unsigned>(static_cast<unsigned char>(text[i]))) return false;
        return true;
    }
};

bool EndsWithTga(std::string_view path) noexcept {
    if (path.size() < 4) return false;
    const auto tail = path.substr(path.size() - 4);
    return tail[0] == '.' && (tail[1] | 0x20) == 't' && (tail[2] | 0x20) == 'g' && (tail[3] | 0x20) == 'a';
}

void JpegSize(const Bytes& b, SurfaceMaterialImageHeader& out) noexcept {
    std::size_t at = 2;
    while (b.Has(at, 2)) {
        if (b.U8(at) != 0xFF) return;
        const auto marker = b.U8(at + 1);
        if (marker == 0xFF) { ++at; continue; }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) { at += 2; continue; }
        if (marker == 0xD9 || marker == 0xDA || !b.Has(at + 2, 2)) return;
        const auto length = b.Be16(at + 2);
        const bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (frame) {
            if (length < 7 || !b.Has(at + 4, 5)) return;
            out.height = b.Be16(at + 5); out.width = b.Be16(at + 7);
            return;
        }
        if (length < 2) return;
        at += 2 + length;
    }
}

void TiffSize(const Bytes& b, bool little, SurfaceMaterialImageHeader& out) noexcept {
    const auto u16 = [&](std::size_t at) { return little ? b.Le16(at) : b.Be16(at); };
    const auto u32 = [&](std::size_t at) { return little ? b.Le32(at) : b.Be32(at); };
    if (!b.Has(4, 4)) return;
    const std::size_t ifd = u32(4);
    if (!b.Has(ifd, 2)) return;
    const auto count = u16(ifd);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto entry = ifd + 2 + static_cast<std::size_t>(i) * 12;
        if (!b.Has(entry, 12)) return;
        const auto tag = u16(entry), type = u16(entry + 2);
        if (tag != 256 && tag != 257) continue;
        // 单个 SHORT 存在值字段前两字节，LONG 占满四字节。
        const auto value = type == 3 ? u16(entry + 8) : type == 4 ? u32(entry + 8) : 0u;
        (tag == 256 ? out.width : out.height) = value;
    }
}
}

SurfaceMaterialImageHeader ReadSurfaceMaterialImageHeader(std::string_view path, std::span<const std::byte> bytes) noexcept {
    const Bytes b{bytes};
    SurfaceMaterialImageHeader out;
    if (b.Has(0, 8) && b.U8(0) == 0x89 && b.Text(1, "PNG\r\n\x1A\n")) {
        out.format = Format::Png; out.gameReadable = true;
        if (b.Text(12, "IHDR") && b.Has(16, 8)) { out.width = b.Be32(16); out.height = b.Be32(20); }
    } else if (b.Has(0, 3) && b.U8(0) == 0xFF && b.U8(1) == 0xD8 && b.U8(2) == 0xFF) {
        out.format = Format::Jpeg; out.gameReadable = true;
        JpegSize(b, out);
    } else if (b.Text(0, "BM")) {
        out.format = Format::Bmp; out.gameReadable = true;
        if (b.Has(14, 4) && b.Le32(14) == 12 && b.Has(18, 4)) { out.width = b.Le16(18); out.height = b.Le16(20); }
        else if (b.Has(14, 4) && b.Le32(14) >= 40 && b.Has(18, 8)) {
            const auto width = static_cast<std::int32_t>(b.Le32(18)), height = static_cast<std::int32_t>(b.Le32(22));
            if (width > 0 && height != 0 && height != INT32_MIN) {
                out.width = static_cast<std::uint32_t>(width);
                out.height = static_cast<std::uint32_t>(height < 0 ? -height : height);
            }
        }
    } else if (b.Text(0, std::string_view("II*\0", 4)) || b.Text(0, std::string_view("MM\0*", 4))) {
        out.format = Format::Tiff; // 宿主 WIC 能解，但游戏一侧不拷 .tif/.tiff，不算游戏读得了
        TiffSize(b, b.U8(0) == 'I', out);
    } else if (b.Text(0, "GIF87a") || b.Text(0, "GIF89a")) {
        out.format = Format::Gif; // 同上：游戏一侧不拷 .gif
        if (b.Has(6, 4)) { out.width = b.Le16(6); out.height = b.Le16(8); }
    } else if (b.Text(0, "BLP")) {
        out.format = Format::Blp;
    } else if (b.Text(0, "DDS ")) {
        out.format = Format::Dds;
    } else if (EndsWithTga(path)) {
        out.format = Format::Tga;
        if (b.Has(0, 18)) {
            const auto type = b.U8(2), depth = b.U8(16);
            out.width = b.Le16(12); out.height = b.Le16(14);
            out.gameReadable = b.U8(1) == 0 && (type == 2 || type == 10) && (depth == 24 || depth == 32);
        }
    }
    return out;
}

bool SurfaceMaterialImageExtensionAccepted(std::string_view path) noexcept {
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos || path.find_first_of("\\/", dot) != std::string_view::npos) return false;
    const auto extension = path.substr(dot + 1);
    for (const std::string_view accepted : {"tga", "png", "jpg", "jpeg", "bmp"}) {
        if (extension.size() != accepted.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < accepted.size() && same; ++i)
            same = static_cast<char>(extension[i] | 0x20) == accepted[i];
        if (same) return true;
    }
    return false;
}

std::string_view SurfaceMaterialImageFormatName(SurfaceMaterialImageFormat format) noexcept {
    switch (format) {
    case Format::Png: return "PNG";
    case Format::Jpeg: return "JPEG";
    case Format::Bmp: return "BMP";
    case Format::Tiff: return "TIFF";
    case Format::Gif: return "GIF";
    case Format::Tga: return "TGA";
    case Format::Blp: return "BLP";
    case Format::Dds: return "DDS";
    case Format::Unknown: break;
    }
    return "未知";
}

SurfaceMaterialPixelStats MeasureSurfaceMaterialPixels(std::span<const std::uint8_t> rgba) noexcept {
    SurfaceMaterialPixelStats stats;
    stats.pixels = rgba.size() / 4;
    if (!stats.pixels) return stats;
    std::array<double, 4> sum{}, squares{};
    std::array<std::uint8_t, 4> low{255, 255, 255, 255}, high{};
    double brightness = 0;
    for (std::size_t i = 0; i < stats.pixels; ++i) {
        const auto* p = rgba.data() + i * 4;
        for (std::size_t c = 0; c < 4; ++c) {
            sum[c] += p[c]; squares[c] += static_cast<double>(p[c]) * p[c];
            low[c] = std::min(low[c], p[c]); high[c] = std::max(high[c], p[c]);
        }
        brightness += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        if (p[2] > kSurfaceMaterialMetalThreshold) ++stats.metalTexels;
    }
    const auto count = static_cast<double>(stats.pixels);
    for (std::size_t c = 0; c < 4; ++c) {
        const double mean = sum[c] / count;
        const double variance = std::max(0.0, squares[c] / count - mean * mean);
        stats.channels[c] = {low[c] / 255.0f, static_cast<float>(mean / 255.0), high[c] / 255.0f,
            static_cast<float>(std::sqrt(variance) / 255.0)};
    }
    stats.brightnessMean = static_cast<float>(brightness / (255.0 * count));
    return stats;
}

float SurfaceMaterialDetailMean(const SurfaceMaterialPixelStats& stats, float mean) noexcept {
    return stats.pixels ? std::clamp(mean, kSurfaceMaterialDetailMeanFloor, 1.0f) : 1.0f;
}

bool SurfaceMaterialOrmUncompressed(const SurfaceMaterialPixelStats& stats) noexcept {
    return stats.pixels && stats.metalTexels * 1000 >= stats.pixels;
}
}
