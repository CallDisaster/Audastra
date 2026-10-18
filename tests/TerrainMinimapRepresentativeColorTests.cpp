#include "War3MapCore/Render/TerrainMinimapRepresentativeColor.h"

#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Render;

namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Image::DecodedImage MakeImage(std::uint32_t width, std::uint32_t height) {
    return {width, height, std::vector<std::byte>(static_cast<std::size_t>(width) * height * 4, std::byte{255})};
}
void Pixel(Image::DecodedImage& image, std::size_t at, std::array<unsigned, 4> value) {
    for (std::size_t channel = 0; channel < 4; ++channel) image.rgba[at * 4 + channel] = static_cast<std::byte>(value[channel]);
}
void SquareUsesLinearPrefixAndIntegerMean() {
    auto image = MakeImage(8, 8); // N=2*2=4：必须取第0行前4像素，不是左上2x2。
    Pixel(image, 0, {1, 20, 200, 0}); Pixel(image, 1, {2, 30, 201, 255});
    Pixel(image, 2, {3, 40, 202, 0}); Pixel(image, 3, {5, 50, 204, 17});
    Pixel(image, 8, {240, 240, 240, 255}); Pixel(image, 9, {240, 240, 240, 255});
    for (const auto role : {TerrainMinimapTextureRole::Ground, TerrainMinimapTextureRole::Cliff}) {
        const auto color = ComputeTerrainMinimapRepresentativeColor(image, role);
        Require(color.Succeeded() && color.value->averagedPixels == 4, "square prefix area");
        Require(color.value->rgba == std::array<std::uint8_t, 4>{2, 35, 201, 255}, "linear prefix and truncating mean ignore alpha");
    }
}
void WideGroundAndCliffUseDifferentAreas() {
    auto image = MakeImage(16, 8); // 地面N=4；悬崖N=8。
    for (std::size_t i = 0; i < 4; ++i) Pixel(image, i, {20, 40, 60, 0});
    for (std::size_t i = 4; i < 8; ++i) Pixel(image, i, {100, 120, 140, 255});
    const auto ground = ComputeTerrainMinimapRepresentativeColor(image, TerrainMinimapTextureRole::Ground);
    const auto cliff = ComputeTerrainMinimapRepresentativeColor(image, TerrainMinimapTextureRole::Cliff);
    Require(ground.Succeeded() && ground.value->averagedPixels == 4
        && ground.value->rgba == std::array<std::uint8_t, 4>{20, 40, 60, 255}, "wide ground top eighth prefix");
    Require(cliff.Succeeded() && cliff.value->averagedPixels == 8
        && cliff.value->rgba == std::array<std::uint8_t, 4>{60, 80, 100, 255}, "cliff retains quarter height rule");
    constexpr std::array<std::array<std::uint32_t, 2>, 2> shapes{{{256, 256}, {512, 256}}};
    for (const auto shape : shapes) {
        auto original = MakeImage(shape[0], shape[1]);
        const auto color = ComputeTerrainMinimapRepresentativeColor(original, TerrainMinimapTextureRole::Ground);
        Require(color.Succeeded() && color.value->averagedPixels == 4096, "original atlas representative area");
    }
}
void InvalidInputCannotGuessDefault() {
    auto tiny = MakeImage(3, 4);
    const auto missing = ComputeTerrainMinimapRepresentativeColor(tiny, TerrainMinimapTextureRole::Ground);
    Require(!missing.Succeeded() && !missing.value
        && missing.diagnostics.front().code == "MINIMAP_REPRESENTATIVE_DEFAULT_COLOR_REQUIRED", "zero area needs actual default evidence");
    auto bad = MakeImage(8, 8); bad.rgba.pop_back();
    Require(!ComputeTerrainMinimapRepresentativeColor(bad, TerrainMinimapTextureRole::Ground).Succeeded(), "truncated RGBA");
    bad = {}; bad.width = 0xffffffffu; bad.height = 0xffffffffu;
    Require(!ComputeTerrainMinimapRepresentativeColor(bad, TerrainMinimapTextureRole::Cliff).Succeeded(), "overflow dimensions rejected first");
    bad = MakeImage(8, 8);
    Require(!ComputeTerrainMinimapRepresentativeColor(bad, static_cast<TerrainMinimapTextureRole>(999)).Succeeded(), "unknown role");
}
}

int main() {
    try {
        SquareUsesLinearPrefixAndIntegerMean(); WideGroundAndCliffUseDifferentAreas(); InvalidInputCannotGuessDefault();
        std::cout << "Terrain minimap representative color: 3 groups passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
