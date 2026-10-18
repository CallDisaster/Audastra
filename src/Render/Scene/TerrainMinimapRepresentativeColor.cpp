#include "War3MapCore/Render/TerrainMinimapRepresentativeColor.h"

namespace War3Map::Render {

Result<TerrainMinimapRepresentativeColor> ComputeTerrainMinimapRepresentativeColor(
    const Image::DecodedImage& image, TerrainMinimapTextureRole role) {
    using Output = Result<TerrainMinimapRepresentativeColor>;
    const auto fail = [](const char* code, const char* message) {
        return Output::Failure("TerrainMinimap", code, message);
    };
    if (role != TerrainMinimapTextureRole::Ground && role != TerrainMinimapTextureRole::Cliff)
        return fail("MINIMAP_REPRESENTATIVE_ROLE", "小地图代表色的纹理角色无效。");
    constexpr std::uint32_t maximumDimension = 4096;
    constexpr std::size_t maximumRgbaBytes = 64u * 1024u * 1024u;
    if (!image.width || !image.height || image.width > maximumDimension || image.height > maximumDimension)
        return fail("MINIMAP_REPRESENTATIVE_DIMENSIONS", "小地图代表色图像尺寸无效或超过4096上限。");
    const auto pixels = static_cast<std::size_t>(image.width) * image.height;
    if (image.rgba.size() != pixels * 4 || image.rgba.capacity() > maximumRgbaBytes)
        return fail("MINIMAP_REPRESENTATIVE_PIXELS", "小地图代表色的RGBA正文不完整或保留容量超过64MiB。");
    const auto verticalDivisor = role == TerrainMinimapTextureRole::Ground && image.width > image.height ? 8u : 4u;
    const auto count = static_cast<std::size_t>(image.width / 4u) * (image.height / verticalDivisor);
    if (!count)
        return fail("MINIMAP_REPRESENTATIVE_DEFAULT_COLOR_REQUIRED", "代表色取样面积为零；默认地面色仍需原版证据。");
    std::array<std::uint64_t, 3> sum{};
    for (std::size_t pixel = 0; pixel < count; ++pixel)
        for (std::size_t channel = 0; channel < sum.size(); ++channel)
            sum[channel] += std::to_integer<std::uint8_t>(image.rgba[pixel * 4 + channel]);
    TerrainMinimapRepresentativeColor result{{0, 0, 0, 255}, count};
    for (std::size_t channel = 0; channel < sum.size(); ++channel)
        result.rgba[channel] = static_cast<std::uint8_t>(sum[channel] / count);
    return Output::Success(result);
}

} // namespace War3Map::Render
