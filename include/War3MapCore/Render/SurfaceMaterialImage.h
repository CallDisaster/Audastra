#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace War3Map::Render {
// 表面材质图片的文件头与像素统计：世界视口的细节均值、材质校验和材质面板共用。
// 游戏一侧（WarVK）用 WIC 读 PNG、JPEG、BMP、TIFF、GIF，另有自带的 24/32 位 TGA 读取；不读 BLP、DDS。
enum class SurfaceMaterialImageFormat : std::uint8_t { Unknown, Png, Jpeg, Bmp, Tiff, Gif, Tga, Blp, Dds };

struct SurfaceMaterialImageHeader {
    SurfaceMaterialImageFormat format = SurfaceMaterialImageFormat::Unknown;
    std::uint32_t width = 0, height = 0; // 0 表示文件头里读不出尺寸
    bool gameReadable = false;           // 格式在 WarVK 支持之列（TGA 还要求 24/32 位无色表真彩色）
};
// 按文件头判断格式与尺寸；TGA 没有文件头标记，只在扩展名为 .tga 时按 TGA 头读。
[[nodiscard]] SurfaceMaterialImageHeader ReadSurfaceMaterialImageHeader(std::string_view path,
    std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::string_view SurfaceMaterialImageFormatName(SurfaceMaterialImageFormat format) noexcept;

// 宿主上限：任一边超过 8192 读不了；超过 2048 合法但占内存（编辑器预览也缩到 2048）。
inline constexpr std::uint32_t kSurfaceMaterialImageMaxSide = 8192;
inline constexpr std::uint32_t kSurfaceMaterialImageLargeSide = 2048;
// 宿主：B 通道大于 24 的像素达到千分之一时，ORM 不能压成 BC5，保留未压缩 BGRA8。
inline constexpr std::uint8_t kSurfaceMaterialMetalThreshold = 24;
// 着色器拿“像素值 ÷ 全图平均值”做细节比例；平均值的下限，防止除零。
inline constexpr float kSurfaceMaterialDetailMeanFloor = 1.0f / 1024.0f;

struct SurfaceMaterialChannelStats { float minimum = 0, mean = 0, maximum = 0, deviation = 0; }; // 0–1
struct SurfaceMaterialPixelStats {
    std::size_t pixels = 0;
    std::array<SurfaceMaterialChannelStats, 4> channels{}; // R、G、B、A
    float brightnessMean = 0;   // Rec.709 亮度的平均值，0–1
    std::size_t metalTexels = 0; // B > kSurfaceMaterialMetalThreshold 的像素数
};
// rgba 为逐像素 R、G、B、A 字节；长度不是 4 的倍数时忽略末尾不足一像素的部分。
[[nodiscard]] SurfaceMaterialPixelStats MeasureSurfaceMaterialPixels(std::span<const std::uint8_t> rgba) noexcept;
// 细节图的比例分母：平均值夹在 [kSurfaceMaterialDetailMeanFloor, 1]；没有像素时为 1。
[[nodiscard]] float SurfaceMaterialDetailMean(const SurfaceMaterialPixelStats& stats, float mean) noexcept;
[[nodiscard]] bool SurfaceMaterialOrmUncompressed(const SurfaceMaterialPixelStats& stats) noexcept;
}
