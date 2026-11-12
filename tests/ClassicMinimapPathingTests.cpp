#include "War3MapCore/Render/ClassicMinimapPathing.h"
#include "War3MapCore/Render/TerrainMinimapPostprocess.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace War3Map;
using namespace War3Map::Render;
using Rgba = std::array<unsigned, 4>;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

TerrainMinimapPostprocessResult Take(Result<TerrainMinimapPostprocessResult> result) {
    if (!result.Succeeded() || !result.value) throw std::runtime_error("postprocess fixture failed");
    return std::move(*result.value);
}

TerrainMinimapPostprocessSettings Unfiltered() {
    TerrainMinimapPostprocessSettings settings;
    settings.medianRadius = 0; settings.blurRadius = 0; settings.noisePercent = 0;
    return settings;
}

Rgba Pixel(const std::vector<std::byte>& rgba, std::size_t width, std::uint32_t x, std::uint32_t y) {
    const auto at = (static_cast<std::size_t>(y) * width + x) * 4;
    Rgba result{};
    for (unsigned c = 0; c < 4; ++c) result[c] = std::to_integer<unsigned>(rgba[at + c]);
    return result;
}

// 画布像素到源像素的公开映射必须与底图缩放、补边和翻转逐像素一致。
void CanvasSourceMatchesPostprocess() {
    for (const auto [width, height] : {std::pair{52u, 52u}, {208u, 120u}, {37u, 300u}, {256u, 256u}, {2u, 2u}, {600u, 3u}}) {
        TerrainMinimapPostprocessInput input{width, height, std::vector<std::byte>(std::size_t(width) * height * 4)};
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto at = (std::size_t(y) * width + x) * 4;
                input.rgba[at] = std::byte(x & 255); input.rgba[at + 1] = std::byte(y & 255);
                input.rgba[at + 2] = std::byte(((x >> 8) & 15) | (((y >> 8) & 15) << 4)); input.rgba[at + 3] = std::byte{200};
            }
        const auto output = Take(PostprocessTerrainMinimap(input, Unfiltered()));
        for (std::uint32_t y = 0; y < 256; ++y)
            for (std::uint32_t x = 0; x < 256; ++x) {
                const auto source = TerrainMinimapCanvasSource(output.facts, x, y);
                const auto actual = Pixel(output.finalImage.rgba, 256, x, y);
                if (!source) {
                    Require(actual == Rgba{0, 0, 0, 255}, "canvas padding has no source pixel");
                    continue;
                }
                Require(Pixel(input.rgba, width, (*source)[0], (*source)[1]) == actual, "canvas source equals the scaled pixel");
            }
    }
    TerrainMinimapPostprocessFacts empty;
    Require(!TerrainMinimapCanvasSource(empty, 0, 0), "empty facts map nothing");
}

void PathingColorsFollowNativeTable() {
    using Color = std::optional<std::array<std::uint8_t, 4>>;
    Require(ClassicMinimapPathingColor(0x40) == Color{}, "plain land has no overlay");
    for (const int unrelated : {0x01, 0x10, 0x20, 0x80, 0xf1})
        Require(ClassicMinimapPathingColor(static_cast<std::uint8_t>(unrelated)) == Color{}, "only walk/fly/build bits colour a cell");
    Require(ClassicMinimapPathingColor(0x02) == Color{{255, 0, 0, 64}}, "not walkable is red");
    Require(ClassicMinimapPathingColor(0x04) == Color{{0, 255, 0, 64}}, "not flyable is green");
    Require(ClassicMinimapPathingColor(0x08) == Color{{0, 0, 255, 64}}, "not buildable is blue (shallow water)");
    Require(ClassicMinimapPathingColor(0x0a) == Color{{255, 0, 255, 64}}, "deep water is magenta");
    Require(ClassicMinimapPathingColor(0xca) == Color{{255, 0, 255, 64}}, "cliff edge is magenta");
    Require(ClassicMinimapPathingColor(0xce) == Color{{255, 255, 255, 64}}, "boundary is white");
    Require(ClassicMinimapPathingColor(0x06) == Color{{255, 255, 0, 64}}, "walk+fly is yellow");
    Require(ClassicMinimapPathingColor(0x0c) == Color{{0, 255, 255, 64}}, "fly+build is cyan");
}

// 2x2 地形格、8x8 通行格；每格只看西南角通行格。底色 (144,100,53)，透明度 64/255 手算截断。
void CompositeUsesTileCornerAndNativeBlend() {
    TerrainMinimapPostprocessInput input{2, 2, std::vector<std::byte>(16, std::byte{0})};
    const auto base = Take(PostprocessTerrainMinimap(input, Unfiltered()));
    Image::DecodedImage canvas; canvas.width = canvas.height = 256;
    canvas.rgba.resize(256 * 256 * 4);
    for (std::size_t at = 0; at < canvas.rgba.size(); at += 4) {
        canvas.rgba[at] = std::byte{144}; canvas.rgba[at + 1] = std::byte{100};
        canvas.rgba[at + 2] = std::byte{53}; canvas.rgba[at + 3] = std::byte{255};
    }
    MapData::PathingGridSnapshot pathing;
    pathing.width = pathing.height = 8;
    pathing.cells.assign(64, 0x40);
    pathing.cells[0] = 0x02;          // 格(0,0) 西南角：不可走
    pathing.cells[5] = 0x0e;          // 格(1,0) 非西南角，忽略
    pathing.cells[4 * 8] = 0x0a;      // 格(0,1) 西南角：深水
    pathing.cells[4 * 8 + 4] = 0xce;  // 格(1,1) 西南角：边界
    const std::array<std::uint32_t, 4> rectangle{0, 0, 2, 2};
    Require(CompositeClassicMinimapPathing(canvas, base.facts, rectangle, pathing).Succeeded(), "composite succeeds");
    // 画布下半为南（格行0），左半为西（格列0）。
    Require(Pixel(canvas.rgba, 256, 10, 200) == Rgba{171, 74, 39, 255}, "red blend truncates like the original");
    Require(Pixel(canvas.rgba, 256, 200, 200) == Rgba{144, 100, 53, 255}, "non-corner cells do not colour a tile");
    Require(Pixel(canvas.rgba, 256, 10, 10) == Rgba{171, 74, 103, 255}, "magenta blend");
    Require(Pixel(canvas.rgba, 256, 200, 10) == Rgba{171, 138, 103, 255}, "white blend");

    auto shifted = rectangle; shifted[2] = 3;
    const auto mismatch = CompositeClassicMinimapPathing(canvas, base.facts, shifted, pathing);
    Require(!mismatch.Succeeded() && mismatch.diagnostics.front().code == "MINIMAP_PATHING_RECTANGLE", "rectangle must match the base");
    auto small = pathing; small.width = small.height = 4; small.cells.resize(16);
    const auto grid = CompositeClassicMinimapPathing(canvas, base.facts, rectangle, small);
    Require(!grid.Succeeded() && grid.diagnostics.front().code == "MINIMAP_PATHING_GRID", "pathing must cover the rectangle");
}

} // namespace

int main() {
    try {
        CanvasSourceMatchesPostprocess();
        PathingColorsFollowNativeTable();
        CompositeUsesTileCornerAndNativeBlend();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "classic minimap pathing tests passed\n";
    return 0;
}
