#include "War3MapCore/MapData/TerrainCliffEditing.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::MapData;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> value) {
    if (!value.Succeeded() || !value.value) throw std::runtime_error("cliff fixture operation failed");
    return std::move(*value.value);
}
TerrainGridSnapshot Flat() {
    TerrainGridSnapshot grid;
    grid.header.version = 11; grid.header.width = grid.header.height = 17;
    grid.header.mainTileset = 'L'; grid.header.groundTilesets = {"Tg01"};
    grid.header.cliffTilesets = {"Cl01", "Cl02"}; grid.vertices.resize(17 * 17);
    for (std::size_t i = 0; i < grid.vertices.size(); ++i) {
        auto& vertex = grid.vertices[i];
        vertex.groundHeightRaw = static_cast<std::uint16_t>(8192 + i % 7);
        vertex.waterHeightAndFlagsRaw = 7936; vertex.variationRaw = 3; vertex.cliffAndLayerRaw = 2;
    }
    RefreshTerrainDerived(grid, 0, 0, 17, 17); return grid;
}
TerrainWaterLoadedRules Rules() {
    return {90, -2048, 2047, 0, 128, 14, false, {{"Tg01", true}}, {"Cl01", "Cl02"}};
}
TerrainCliffBrush Brush(const TerrainGridSnapshot& terrain) {
    return {TerrainBrushShape::Square, 1, 1, 1, Take(CaptureTerrainStrokeAnchor(terrain, {1024, 1024})), 1};
}
void LiteralNativeNeighbourWrites() {
    const auto source = Flat(); const auto original = Take(SerializeMapTerrain(source));
    const TerrainStrokePoint point{1024, 1024};
    const auto result = Take(PrepareTerrainCliffStroke(source, Rules(), Brush(source), std::span(&point, 1), {}));
    auto expected = source;
    // seed=1的原版MSVCRT序列：中心2，其后按dx外/dy内为2,1,1,2,1,0,0,1。
    constexpr std::array<std::uint8_t, 9> variations{2, 1, 1, 2, 2, 1, 0, 0, 1};
    for (std::uint32_t dx = 0; dx < 3; ++dx) for (std::uint32_t dy = 0; dy < 3; ++dy) {
        auto& vertex = expected.vertices[(7 + dy) * 17 + 7 + dx];
        vertex.cliffAndLayerRaw = static_cast<std::uint8_t>(0x10 | (dx == 1 && dy == 1 ? 3 : 2));
        vertex.variationRaw = static_cast<std::uint8_t>(3 | (variations[dx * 3 + dy] << 5));
    }
    const auto bytes = Take(SerializeMapTerrain(result.terrain));
    Require(bytes == Take(SerializeMapTerrain(expected)), "native centre and eight neighbours match literal raw writes");
    Require(Take(SerializeMapTerrain(source)) == original, "owned candidate leaves source bytes intact");
    Require(std::none_of(result.terrain.vertices.begin(), result.terrain.vertices.end(),
        [](const auto& vertex) { return vertex.HasWater(); }), "ordinary cliff creates no water");
    Require(Take(SerializeMapTerrain(*Take(ParseMapTerrain(bytes)).Snapshot())) == bytes, "cliff W3E roundtrip is exact");
    Require(result.result.requiresPathingRebuild && result.result.requiresObjectValidityRefresh,
        "native derived layers remain explicit obligations");
}
void SpecialDeletionAndAtomicFailures() {
    const auto source = Flat(); const auto original = Take(SerializeMapTerrain(source));
    DoodadPlacements doodads;
    doodads.specialDoodads = {{"sp01", 0, {8, 8}}};
    const TerrainWaterSpecialDoodadShape shape{0, "sp01", 0, {8, 8}, 8, 8, 9, 9, {0, 0, 0, 0}};
    const TerrainStrokePoint point{1024, 1024};
    const auto result = Take(PrepareTerrainCliffStroke(source, Rules(), Brush(source), std::span(&point, 1),
        doodads, std::span(&shape, 1)));
    Require(result.doodads.specialDoodads.empty() && result.result.removedSpecialSourceRecords == std::vector<std::uint32_t>{0},
        "native cliff removes the special model whose corner levels no longer fit");
    auto brush = Brush(source); brush.size = 0;
    Require(!PrepareTerrainCliffStroke(source, Rules(), brush, std::span(&point, 1), {}).Succeeded(), "invalid classic size rejects");
    brush = Brush(source); brush.levelDelta = 2;
    Require(!PrepareTerrainCliffStroke(source, Rules(), brush, std::span(&point, 1), {}).Succeeded(), "non-native delta rejects");
    TerrainWaterBrushLimits limits; limits.maximumWork = 1;
    Require(!PrepareTerrainCliffStroke(source, Rules(), Brush(source), std::span(&point, 1), {}, {}, limits).Succeeded(),
        "insufficient work cannot publish a partial cliff");
    auto unknown = source; unknown.vertices[8 * 17 + 9].waterHeightAndFlagsRaw |= 0x8000;
    const auto failed = PrepareTerrainCliffStroke(unknown, Rules(), Brush(unknown), std::span(&point, 1), {});
    Require(!failed.Succeeded(), "actually visited unknown neighbour rejects");
    Require(Take(SerializeMapTerrain(source)) == original && doodads.specialDoodads.size() == 1,
        "all rejection paths preserve original terrain and DOO");
}
} // namespace
int main() {
    try { LiteralNativeNeighbourWrites(); SpecialDeletionAndAtomicFailures();
        std::cout << "TerrainCliffEditingTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
