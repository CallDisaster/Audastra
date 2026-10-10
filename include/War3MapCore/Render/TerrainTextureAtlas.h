#pragma once

#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/Resources/ResourceResolver.h"

#include <memory>

namespace War3Map::Render {

struct TerrainAtlasLimits {
    // 默认值也是硬上限；只能收紧。Resolver 回调须自行限制读取时的源文件分配。
    std::size_t maxInputBytes = 16 * 1024 * 1024;
    std::size_t maxDimension = 2048;
    std::size_t maxRgbaBytes = 16 * 1024 * 1024;
    // 保留的原始 mip 链总量，独立于逐级输出限制。
    std::size_t maxRetainedRgbaBytes = 24 * 1024 * 1024;
    std::size_t maxTotalMipPixels = 8 * 1024 * 1024;
    // 同时存活的输入、包装、JPEG 系数/平面和 RGBA 载荷的保守估算，不含分配器开销。
    std::size_t maxWorkingBytes = 128 * 1024 * 1024;
    std::size_t maxJpegSegments = 4096;
    std::size_t maxJpegScans = 256;
};

struct TerrainAtlasCell {
    std::uint32_t column = 0;
    std::uint32_t row = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct TerrainAtlasSnapshot {
    Resources::ResourceLocation source;
    std::string sourceSha256;
    std::size_t sourceBytes = 0;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::uint32_t validatedMipCount = 0;
    std::uint32_t columns = 0; // 4 或 8
    static constexpr std::uint32_t rows = 4;
    // mip0 RGBA8888，行优先、栅格左上角为 (0,0)，不翻转 Y、不预乘 alpha。
    // 各 cell 为整张像素缓冲中的矩形，不复制 16/32 份子图；不赋予游戏混合 mask 语义。
    Image::DecodedImage image;
    // 原文件 mip1..N；不生成缺级，也不复制 mip0。完整验证后一起发布。
    std::vector<Image::DecodedImage> additionalMips;
    std::vector<TerrainAtlasCell> cells; // row * columns + column

    [[nodiscard]] std::size_t RgbaByteCount() const noexcept {
        std::size_t bytes = image.rgba.size();
        for (const auto& mip : additionalMips) bytes += mip.rgba.size();
        return bytes;
    }

    [[nodiscard]] const TerrainAtlasCell* Cell(std::uint32_t column, std::uint32_t row) const noexcept {
        if (column >= columns || row >= rows) return nullptr;
        const auto index = static_cast<std::size_t>(row) * columns + column;
        return index < cells.size() ? &cells[index] : nullptr;
    }
};

// Core DecodeBlp 是唯一像素解码实现。这里在调用前验证经典 BLP1 所有声明 mip 的范围、
// JPEG 帧尺寸/扫描结构与工作集预算，再逐 mip 解码检查；任一失败不发布部分快照。
// 支持 JPEG 及调色板 0/8-bit alpha。Core 未支持的 1/4-bit alpha、BLP2/DXT 明确拒绝。
// 仅接受 4x4 或 8x4 方形单元格 atlas；没有 GPU 上传、UV 变换或原版混合层生成。
[[nodiscard]] Result<std::shared_ptr<const TerrainAtlasSnapshot>> DecodeTerrainTextureAtlas(
    const Resources::ResolvedResource& resource, const TerrainAtlasLimits& limits = {});

[[nodiscard]] Result<std::shared_ptr<const TerrainAtlasSnapshot>> LoadTerrainTextureAtlas(
    const Resources::ResourceResolver& resolver, std::string_view virtualPath,
    const TerrainAtlasLimits& limits = {});

} // namespace War3Map::Render
