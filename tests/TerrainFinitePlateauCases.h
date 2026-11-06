#pragma once

#include "War3MapCore/MapData/TerrainWaterBrush.h"

#include <stdexcept>

namespace War3Map::TerrainFinitePlateauCases {
using namespace MapData;
inline void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
inline TerrainGridSnapshot Grid(unsigned size) {
    TerrainGridSnapshot grid;
    grid.header.version = 11; grid.header.mainTileset = 'L';
    grid.header.width = grid.header.height = 65; grid.header.offset = {-4096, -4096};
    grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"Cl01"};
    grid.vertices.resize(65 * 65);
    const unsigned low = 36 - size / 2, high = 36 + size / 2;
    // 原版尺寸3/5保存的拓扑分别为5×5/9×9高层、7×7/11×11的0号材质。
    // 非地形分类字段给不同值，验证坡道不会抹掉与规则无关的原字节。
    for (unsigned y = 0; y < 65; ++y) for (unsigned x = 0; x < 65; ++x) {
        auto& v = grid.vertices[y * 65 + x];
        v.groundHeightRaw = v.waterHeightAndFlagsRaw = 8192;
        v.variationRaw = std::uint8_t((x + y * 3) & 31);
        const bool inside = x >= low && x <= high && y >= low && y <= high;
        const bool rim = x >= low - 1 && x <= high + 1 && y >= low - 1 && y <= high + 1;
        v.cliffAndLayerRaw = std::uint8_t((rim ? 0 : 0xf0) | (inside ? 3 : 2));
    }
    RefreshTerrainDerived(grid, 0, 0, 65, 65); return grid;
}
inline void Run() {
    const TerrainWaterLoadedRules rules{90, -256, 1536, 0, 128, 14, true, {{"Tg01", true}}, {"Cl01"}};
    TerrainBrush brush; brush.kind = TerrainBrushKind::StraightRamp; brush.value = true;
    for (const unsigned size : {5u, 9u}) {
        auto grid = Grid(size), expected = grid;
        const unsigned highY = 36 - size / 2;
        const TerrainStrokePoint point{544, float((int(highY) - 1) * 128 - 4096 + 32)};
        const auto before = SerializeMapTerrain(grid); Require(before.Succeeded(), "finite input encode");
        for (unsigned y = highY - 2; y <= highY; ++y) expected.vertices[y * 65 + 36].textureAndFlagsRaw |= 0x10;
        const auto candidate = PrepareTerrainStraightRampStroke(grid, rules, brush, std::span(&point, 1), {}, {});
        Require(candidate.Succeeded() && candidate.value, "native finite plateau rejected");
        const auto actual = SerializeMapTerrain(candidate.value->terrain), wanted = SerializeMapTerrain(expected);
        Require(actual.Succeeded() && wanted.Succeeded() && actual.value == wanted.value, "finite R strip differs from native oracle");
        Require(SerializeMapTerrain(grid).value == before.value, "finite preparation modified source");
        // 缺角、非方台和未证明的中间尺寸不能通过有限方台分支。
        auto broken = grid; broken.vertices[highY * 65 + highY].cliffAndLayerRaw = 2;
        Require(!PrepareTerrainStraightRampStroke(broken, rules, brush, std::span(&point, 1), {}, {}).Succeeded(), "incomplete plateau admitted");
    }
    const auto unproven = Grid(7);
    const TerrainStrokePoint unprovenPoint{544, 32};
    Require(!PrepareTerrainStraightRampStroke(unproven, rules, brush, std::span(&unprovenPoint, 1), {}, {}).Succeeded(), "unproven plateau size admitted");
    auto negative = Grid(9);
    // 第三个原版对照：相邻5×5低层落在x35..39/y27..31，与高层构成两级落差；原版不改R。
    for (unsigned y = 26; y <= 32; ++y) for (unsigned x = 34; x <= 40; ++x) {
        auto& v = negative.vertices[y * 65 + x];
        v.cliffAndLayerRaw &= 0x0f;
        if (x >= 35 && x <= 39 && y >= 27 && y <= 31) v.cliffAndLayerRaw = 1;
    }
    RefreshTerrainDerived(negative, 0, 0, 65, 65);
    const TerrainStrokePoint point{544, -96};
    Require(!PrepareTerrainStraightRampStroke(negative, rules, brush, std::span(&point, 1), {}, {}).Succeeded(), "two-level native negative admitted");
}
} // namespace War3Map::TerrainFinitePlateauCases
