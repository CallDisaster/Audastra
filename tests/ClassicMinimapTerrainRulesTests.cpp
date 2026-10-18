#include "War3MapCore/Render/ClassicMinimapTerrainRules.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Render;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T> T Take(Result<T> result) {
    Require(result.Succeeded() && result.value.has_value(), "地形规则查询失败");
    return std::move(*result.value);
}

TerrainGridSnapshot Grid(std::uint32_t width = 7, std::uint32_t height = 7) {
    TerrainGridSnapshot grid;
    grid.header.version = 11;
    grid.header.width = width;
    grid.header.height = height;
    grid.vertices.resize(static_cast<std::size_t>(width) * height);
    for (auto& vertex : grid.vertices) {
        vertex.position = {0.0f, 0.0f, 100.0f};
        vertex.cliffAndLayerRaw = 0xf2;
    }
    return grid;
}

TerrainVertex& At(TerrainGridSnapshot& grid, std::uint32_t x, std::uint32_t y) {
    return grid.vertices[static_cast<std::size_t>(y) * grid.header.width + x];
}

void Level(TerrainGridSnapshot& grid, std::uint32_t x, std::uint32_t y, std::uint8_t level) {
    auto& vertex = At(grid, x, y);
    vertex.cliffAndLayerRaw = static_cast<std::uint8_t>((vertex.cliffAndLayerRaw & 0xf0) | level);
}

void Texture(TerrainGridSnapshot& grid, std::uint32_t x, std::uint32_t y, std::uint8_t texture) {
    auto& vertex = At(grid, x, y);
    vertex.cliffAndLayerRaw = static_cast<std::uint8_t>((texture << 4) | vertex.CliffLevel());
}

void OrdinaryAndPaletteOrder() {
    auto grid = Grid();
    Require(!Take(ClassicMinimapCliffTextureAt(grid, 3, 3)), "平层无悬崖");
    Level(grid, 3, 3, 3);
    Texture(grid, 3, 3, 4);
    Texture(grid, 4, 3, 5);
    Texture(grid, 3, 4, 6);
    Require(Take(ClassicMinimapCliffTextureAt(grid, 3, 3)) == 4, "SW角的首个非15悬崖材质优先");
    Texture(grid, 3, 3, 15);
    Require(Take(ClassicMinimapCliffTextureAt(grid, 3, 3)) == 5, "SE角应先于NW角");
}

void NeighborhoodAndDefaultPalette() {
    auto grid = Grid();
    Level(grid, 3, 3, 3);
    Texture(grid, 1, 4, 7);
    Texture(grid, 2, 1, 8);
    Require(Take(ClassicMinimapCliffTextureAt(grid, 3, 3)) == 7, "5x5范围按X优先扫描");
    Texture(grid, 1, 4, 15);
    Texture(grid, 2, 1, 15);
    Require(Take(ClassicMinimapCliffTextureAt(grid, 3, 3)) == 0, "没有有效悬崖材质时使用原版槽0");
}

void TerminalVertexAndFirstQuad() {
    auto grid = Grid(3, 3);
    Level(grid, 2, 2, 3);
    Texture(grid, 1, 1, 9);
    Require(Take(ClassicMinimapCliffTextureAt(grid, 2, 2)) == 9, "东北终端顶点通过有效负邻接查询");
    Require(Take(ClassicMinimapGroundHeightAt(grid, 0, 0)) == 100.0, "普通顶点保留原始地高");
}

void StraightRampAllRotations() {
    for (int rotation = 0; rotation < 4; ++rotation) {
        auto grid = Grid();
        for (std::uint32_t y = 0; y < 7; ++y) for (std::uint32_t x = 0; x < 7; ++x) {
            const int axis = rotation < 2 ? static_cast<int>(x) : static_cast<int>(y);
            const bool high = rotation % 2 == 0 ? axis <= 2 : axis >= 4;
            Level(grid, x, y, static_cast<std::uint8_t>(high ? 3 : 2));
        }
        for (std::uint32_t y = 2; y <= 4; ++y)
            for (std::uint32_t x = 2; x <= 4; ++x) At(grid, x, y).textureAndFlagsRaw |= 0x10;
        Require(Take(ClassicMinimapGroundHeightAt(grid, 3, 3)) == 164.0, "四向R直坡中点抬高64");
        const auto highX = rotation < 2 ? (rotation == 0 ? 2u : 4u) : 3u;
        const auto highY = rotation >= 2 ? (rotation == 2 ? 2u : 4u) : 3u;
        Require(Take(ClassicMinimapGroundHeightAt(grid, highX, highY)) == 100.0, "H高端不重复抬升");
        Require(!Take(ClassicMinimapCliffTextureAt(grid, 3, 3)), "R坡面不画悬崖墙面颜色");
    }
}

void SaddleRampQuarterHeights() {
    for (int shape = 0; shape < 3; ++shape) for (int rotation = 0; rotation < 4; ++rotation) {
        auto grid = Grid();
        for (std::uint32_t y = 0; y < 7; ++y) for (std::uint32_t x = 0; x < 7; ++x) {
            int u = static_cast<int>(x), v = static_cast<int>(y);
            for (int turn = 0; turn < rotation; ++turn) { const int oldU = u; u = 6 - v; v = oldU; }
            const bool high = shape == 0 ? u >= 4 || v >= 4 : shape == 1 ? u >= 4 && v >= 4
                : (u >= 4 && v >= 4) || (u <= 2 && v <= 2);
            Level(grid, x, y, static_cast<std::uint8_t>(high ? 3 : 2));
        }
        for (std::uint32_t y = 2; y <= 4; ++y)
            for (std::uint32_t x = 2; x <= 4; ++x) At(grid, x, y).textureAndFlagsRaw |= 0x10;
        const double expected = shape == 0 ? 196.0 : shape == 1 ? 132.0 : 164.0;
        Require(Take(ClassicMinimapGroundHeightAt(grid, 3, 3)) == expected,
            "四向S坡按一低三高/三低一高/对角两低两高抬升96/32/64");
    }
}

void UnknownRampAndMalformedInput() {
    auto grid = Grid();
    for (std::uint32_t y = 2; y <= 4; ++y)
        for (std::uint32_t x = 2; x <= 4; ++x) At(grid, x, y).textureAndFlagsRaw |= 0x10;
    const auto unknown = ClassicMinimapGroundHeightAt(grid, 3, 3);
    Require(!unknown.Succeeded() && !unknown.diagnostics.empty()
        && unknown.diagnostics.front().code == "CLASSIC_MINIMAP_RAMP_UNRESOLVED", "全平坡道未知形态明确拒绝");
    grid.header.version = 12;
    Require(!ClassicMinimapCliffTextureAt(grid, 1, 1).Succeeded(), "非经典版本拒绝");
    grid.header.version = 11;
    Require(!ClassicMinimapGroundHeightAt(grid, 7, 1).Succeeded(), "越界顶点拒绝");
    At(grid, 1, 1).position[2] = std::numeric_limits<float>::quiet_NaN();
    Require(!ClassicMinimapGroundHeightAt(grid, 1, 1).Succeeded(), "非有限地高拒绝");
    grid.vertices.pop_back();
    Require(!ClassicMinimapCliffTextureAt(grid, 1, 1).Succeeded(), "不完整网格拒绝");
}

} // namespace

int main() {
    try {
        OrdinaryAndPaletteOrder();
        NeighborhoodAndDefaultPalette();
        TerminalVertexAndFirstQuad();
        StraightRampAllRotations();
        SaddleRampQuarterHeights();
        UnknownRampAndMalformedInput();
        std::cout << "ClassicMinimapTerrainRulesTests: all scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ClassicMinimapTerrainRulesTests: " << error.what() << '\n';
        return 1;
    }
}
