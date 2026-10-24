#include "War3MapCore/MapData/TerrainCliffEditing.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"
#include "War3MapCore/MapData/TerrainRampEditing.h"

#include <algorithm>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>

using namespace War3Map;
using namespace War3Map::MapData;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> value, const std::source_location& location = std::source_location::current()) {
    if (!value.Succeeded() || !value.value) {
        std::string message = std::string(location.function_name()) + ":" + std::to_string(location.line());
        for (const auto& diagnostic : value.diagnostics)
            message += "\n" + diagnostic.code + ": " + diagnostic.message;
        if (value.diagnostics.empty()) message += "\ncliff fixture operation returned no value or diagnostics";
        throw std::runtime_error(message);
    }
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
void ExistingRampsUseCompleteGroundAndClearOnlyInvalidBits() {
    const TerrainStrokePoint point{1024, 1024};
    auto isolated = Flat(); isolated.vertices[8 * 17 + 8].textureAndFlagsRaw |= 0x10;
    auto brush = Brush(isolated); brush.levelDelta = 0;
    const auto cleared = Take(PrepareTerrainCliffStroke(isolated, Rules(), brush, std::span(&point, 1), {}));
    Require(!cleared.terrain.vertices[8 * 17 + 8].IsRamp(), "native cleanup clears an isolated old L ramp bit");
    auto edge = Flat();
    for (auto& vertex : edge.vertices) vertex.groundHeightRaw = 8192;
    for (std::size_t y = 7; y <= 9; ++y) {
        auto& vertex = edge.vertices[y * 17]; vertex.textureAndFlagsRaw |= 0x10;
        if (y == 9) vertex.cliffAndLayerRaw = 3;
    }
    RefreshTerrainDerived(edge, 0, 0, 17, 17);
    const TerrainStrokePoint edgePoint{0, 1024};
    brush = Brush(edge); brush.levelDelta = 0;
    const auto edgeEdited = Take(PrepareTerrainCliffStroke(edge, Rules(), brush, std::span(&edgePoint, 1), {}));
    Require(edgeEdited.terrain.vertices[8 * 17].IsRamp()
        && SampleGroundHeight(edgeEdited.terrain, 0, 1024, TerrainHeightSource::RawWithRamps, 14) == 64,
        "ordinary cliff retains a valid NS R64 when the EW neighbors lie outside the map");
    for (const int offset : {32, 64, 96}) {
        auto source = Flat();
        for (auto& vertex : source.vertices) vertex.groundHeightRaw = 8192;
        for (std::size_t y = 7; y <= 9; ++y) for (std::size_t x = 7; x <= 9; ++x) {
            auto& vertex = source.vertices[y * 17 + x]; vertex.textureAndFlagsRaw |= 0x10;
            if ((offset == 32 && x == 9 && y == 9)
                || (offset == 64 && ((x == 7 && y == 7) || (x == 9 && y == 9)))
                || (offset == 96 && (x == 9 || y == 9))) vertex.cliffAndLayerRaw = 3;
        }
        RefreshTerrainDerived(source, 0, 0, 17, 17);
        Require(TerrainRampHeightOffset(source, 8, 8, 14) == float(offset)
            && SampleGroundHeight(source, point.x, point.y, TerrainHeightSource::RawWithRamps, 14) == float(offset),
            "exact editable ground uses the independent S32/64/96 oracle");
        brush = Brush(source); brush.levelDelta = 0;
        const auto retained = Take(PrepareTerrainCliffStroke(source, Rules(), brush, std::span(&point, 1), {}));
        for (std::size_t i = 0; i < source.vertices.size(); ++i)
            Require(retained.terrain.vertices[i].IsRamp() == source.vertices[i].IsRamp(),
                "a valid corner ramp keeps every preexisting ramp bit without creating any new one");
        if (offset == 32) {
            auto blighted = source;
            blighted.vertices[7 * 17 + 7].textureAndFlagsRaw |= 0x20;
            const auto low = Take(QueryNativeTerrainRamp(blighted, 14, 7, 7));
            Require(low.corner == 'L' && low.hasUnblockedRampCell,
                "low flat corner belongs to the independently stated open S32 cell");
            const auto cleaned = Take(PrepareTerrainCliffStroke(blighted, Rules(), brush, std::span(&point, 1), {}));
            Require(!cleaned.terrain.vertices[7 * 17 + 7].IsBlight(),
                "native blight predicate clears a flat L corner through mode1 rather than raw layer difference");
        }
        auto wet = source;
        for (auto& vertex : wet.vertices) {
            vertex.textureAndFlagsRaw |= 0x40; vertex.waterHeightAndFlagsRaw = 8192 + 300 * 4;
        }
        RefreshTerrainDerived(wet, 0, 0, 17, 17);
        auto limitedRules = Rules(); limitedRules.enforceWaterLimits = true;
        brush = Brush(wet); brush.levelDelta = -1;
        const auto limited = Take(PrepareTerrainCliffStroke(wet, limitedRules, brush, std::span(&point, 1), {}));
        Require(limited.terrain.vertices[8 * 17 + 8].CliffLevel() == (offset == 32 ? 2 : 1),
            "wet native cliff lower bound includes the existing complete ramp ground");
        TerrainWaterBrush water{TerrainWaterBrushKind::Shallow, TerrainBrushShape::Square, 1, 1, 1,
            {8192, std::uint16_t(8192 + 300 * 4), 2, true}, TerrainWaterAlgorithm::LegacyFlat};
        Require(!CaptureTerrainWaterStrokeAnchor(wet, point).Succeeded()
            && !PrepareTerrainWaterStroke(wet, limitedRules, water, std::span(&point, 1), {}).Succeeded(),
            "legacy water and its legacy anchor capture retain the old-ramp rejection domain");
        source.vertices[6 * 17 + 6].waterHeightAndFlagsRaw |= 0x8000;
        Require(!SampleGroundHeight(source, point.x, point.y, TerrainHeightSource::RawWithRamps, 14),
            "native follow sampling rejects an unknown second-halo input instead of using zero lift");
    }
}
} // namespace
int main() {
    try { LiteralNativeNeighbourWrites(); SpecialDeletionAndAtomicFailures(); ExistingRampsUseCompleteGroundAndClearOnlyInvalidBits();
        std::cout << "TerrainCliffEditingTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
