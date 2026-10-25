#pragma once

#include "TerrainInnerCornerRampFixtures.h"

namespace War3Map::TerrainInnerCornerRampCases {
using namespace MapData;
using namespace TerrainInnerCornerRampFixtures;

inline void FourDirectionsCreateRemoveAndExactStreams() {
    static_assert(static_cast<unsigned>(TerrainCornerRampAlgorithm::NativeCorners) == 3);
    const std::array ground{TerrainNativeGroundRule{"Tg01", true, true, true}};
    const DoodadPlacements doodads;
    for (const int dx : {1, -1}) for (const int dy : {1, -1}) {
        const auto source = Grid(17, dx, dy); const auto point = Point(17, dx, dy);
        const auto original = TerrainLiteral(false, 17, dx, dy);
        Require(Take(SerializeMapTerrain(source)) == original, "source full W3E matches independently encoded literal");
        const auto plan = Take(PlanTerrainCornerRamp(source, point, TerrainCornerRampAlgorithm::NativeCorners));
        Require(plan.inner && plan.highX == unsigned(8 + dx) && plan.highY == unsigned(8 + dy)
            && plan.directionX == -dx && plan.directionY == -dy, "inner plan chooses native opposite high corner and both signs");
        Require(!PlanTerrainCornerRamp(source, point, TerrainCornerRampAlgorithm::DryOnly).Succeeded()
            && !PlanTerrainCornerRamp(source, point, TerrainCornerRampAlgorithm::WetTerrain).Succeeded(), "historical algorithm1/2 do not acquire inner geometry");
        const auto pathing = [&](const TerrainGridSnapshot& terrain) {
            TerrainNativePathingInput input; input.terrain = &terrain; input.groundRules = ground;
            input.playableBounds = {0, 0, 2048, 2048}; input.maximumCliffLevel = 14;
            return Take(GenerateTerrainNativePathing(input));
        };
        Require(pathing(source) == PathingLiteral(false, 17, dx, dy), "complete inner source WPM equals independent original edge matrix");
        const auto created = Take(PrepareTerrainCornerRampStroke(source, Rules(), Brush(), std::span(&point, 1), doodads,
            {}, {}, TerrainCornerRampAlgorithm::NativeCorners));
        Require(created.result.changedVertices == 9 && Take(SerializeMapTerrain(created.terrain)) == TerrainLiteral(true, 17, dx, dy),
            "inner creation changes exactly the independent nine R records in full W3E");
        Require(Take(SerializeMapTerrain(source)) == original, "pure candidate never mutates original terrain");
        Require(pathing(created.terrain) == PathingLiteral(true, 17, dx, dy), "complete inner created WPM equals independent shoulders and three opened cells");
        const auto labels = Take(ClassifyNativeDryRampCorners(created.terrain, 14));
        constexpr std::array<std::string_view, 5> expected{"AAABA", "ALRHA", "ARSHA", "BHHHA", "AAAAA"};
        for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x)
            Require(labels[std::size_t(8 + y * dy) * 17 + 8 + x * dx] == expected[y + 2][x + 2], "inner native labels equal independent H/R/S96 matrix");
        Require(Take(QueryNativeTerrainRamp(created.terrain, 14, 8, 8)).heightOffset == 96,
            "inner special midpoint is96 rather than outer32 or straight64");
        const auto noop = Take(PrepareTerrainCornerRampStroke(created.terrain, Rules(), Brush(), std::span(&point, 1), doodads,
            {}, {}, TerrainCornerRampAlgorithm::NativeCorners));
        Require(noop.result.changed.Empty() && Take(SerializeMapTerrain(noop.terrain)) == TerrainLiteral(true, 17, dx, dy), "whole inner repeated creation is an exact no-op");
        const auto removed = Take(PrepareTerrainCornerRampStroke(created.terrain, Rules(), Brush(false), std::span(&point, 1), doodads,
            {}, {}, TerrainCornerRampAlgorithm::NativeCorners));
        Require(Take(SerializeMapTerrain(removed.terrain)) == original && pathing(removed.terrain) == PathingLiteral(false, 17, dx, dy),
            "whole inner removal restores both complete independent streams");
        const auto removeNoop = Take(PrepareTerrainCornerRampStroke(source, Rules(), Brush(false), std::span(&point, 1), doodads,
            {}, {}, TerrainCornerRampAlgorithm::NativeCorners));
        Require(removeNoop.result.changed.Empty(), "removing absent inner ramp is validated no-op");

        auto outer = source;
        for (auto& vertex : outer.vertices) vertex.cliffAndLayerRaw = vertex.CliffLevel() == 2 ? 3 : 2;
        RefreshTerrainDerived(outer, 0, 0, 17, 17);
        const auto oldOuter = Take(PrepareTerrainCornerRampStroke(outer, Rules(), Brush(), std::span(&point, 1), doodads,
            {}, {}, TerrainCornerRampAlgorithm::WetTerrain));
        const auto newOuter = Take(PrepareTerrainCornerRampStroke(outer, Rules(), Brush(), std::span(&point, 1), doodads,
            {}, {}, TerrainCornerRampAlgorithm::NativeCorners));
        Require(!Take(PlanTerrainCornerRamp(outer, point, TerrainCornerRampAlgorithm::NativeCorners)).inner
            && Take(SerializeMapTerrain(oldOuter.terrain)) == Take(SerializeMapTerrain(newOuter.terrain)), "new algorithm retains old outer output in all four directions");
        // 内角干域限制不应收窄已取证的外角水域；同时覆盖九点内与实际halo内的水标志。
        auto wetOuter = outer;
        wetOuter.vertices[std::size_t(8 + dy) * 17 + 8 + dx].textureAndFlagsRaw |= 0x40;
        wetOuter.vertices[std::size_t(8 - 4 * dy) * 17 + 8 - 4 * dx].textureAndFlagsRaw |= 0x40;
        RefreshTerrainDerived(wetOuter, 0, 0, 17, 17);
        const auto oldWetOuter = Take(PrepareTerrainCornerRampStroke(wetOuter, Rules(), Brush(), std::span(&point, 1), doodads,
            {}, {}, TerrainCornerRampAlgorithm::WetTerrain));
        const auto newWetOuter = Take(PrepareTerrainCornerRampStroke(wetOuter, Rules(), Brush(), std::span(&point, 1), doodads,
            {}, {}, TerrainCornerRampAlgorithm::NativeCorners));
        Require(!Take(PlanTerrainCornerRamp(wetOuter, point, TerrainCornerRampAlgorithm::NativeCorners)).inner
            && Take(SerializeMapTerrain(oldWetOuter.terrain)) == Take(SerializeMapTerrain(newWetOuter.terrain)),
            "new inner dry-domain guard preserves complete proven wet outer output in every direction");
    }
}

inline void RejectsHaloNeighborsBudgetAndInvalidInputs() {
    const auto point = Point(); const DoodadPlacements doodads;
    const auto reject = [&](const TerrainGridSnapshot& source, const TerrainWaterBrushLimits& limits = TerrainWaterBrushLimits{}) {
        const auto original = Take(SerializeMapTerrain(source));
        Require(!PrepareTerrainCornerRampStroke(source, Rules(), Brush(), std::span(&point, 1), doodads,
            {}, limits, TerrainCornerRampAlgorithm::NativeCorners).Succeeded(), "invalid inner domain rejects before candidate publication");
        Require(Take(SerializeMapTerrain(source)) == original, "every inner rejection preserves all source bytes");
    };
    auto source = Grid(); source.vertices[8 * 17 + 8].textureAndFlagsRaw |= 0x10; reject(source);
    source = Grid(); source.vertices[4 * 17 + 4].textureAndFlagsRaw |= 0x10; reject(source);
    source = Grid(); source.vertices[4 * 17 + 4].cliffAndLayerRaw = 3; reject(source);
    source = Grid(); source.vertices[4 * 17 + 4].groundHeightRaw += 1; reject(source);
    source = Grid(); source.vertices[4 * 17 + 4].textureAndFlagsRaw |= 0x80; reject(source);
    source = Grid(); source.vertices[4 * 17 + 4].waterHeightAndFlagsRaw |= 0x4000; reject(source);
    source = Grid(); source.vertices[4 * 17 + 4].waterHeightAndFlagsRaw |= 0x8000; reject(source);
    // 即使水面低于地面，HasWater本身也越过本批内角干域，不能借看不见的水通过。
    for (unsigned y = 7; y <= 9; ++y) for (unsigned x = 7; x <= 9; ++x) {
        source = Grid(); source.vertices[y * 17 + x].textureAndFlagsRaw |= 0x40;
        Require(!PlanTerrainCornerRamp(source, point, TerrainCornerRampAlgorithm::NativeCorners).Succeeded(),
            "each selected inner vertex with HasWater rejects already during geometry planning");
        reject(source);
    }
    source = Grid(); source.vertices[4 * 17 + 4].textureAndFlagsRaw |= 0x40;
    Require(PlanTerrainCornerRamp(source, point, TerrainCornerRampAlgorithm::NativeCorners).Succeeded(),
        "dry inner nine-point preview alone grants no halo permission");
    reject(source);
    source = Grid(); TerrainWaterBrushLimits limits; limits.maximumCandidateBytes = 1; reject(source, limits);
    limits = {}; limits.maximumWork = 1; reject(source, limits);
    limits = {}; limits.maximumPoints = 0; reject(source, limits);
    auto complete = Take(PrepareTerrainCornerRampStroke(Grid(), Rules(), Brush(), std::span(&point, 1), doodads,
        {}, {}, TerrainCornerRampAlgorithm::NativeCorners)).terrain;
    auto wetComplete = complete; wetComplete.vertices[4 * 17 + 4].textureAndFlagsRaw |= 0x40;
    reject(wetComplete); // 完整九点noop仍须检查实际halo水标志。
    complete.vertices[4 * 17 + 4].textureAndFlagsRaw |= 0x10; reject(complete);
    for (const auto invalid : {TerrainStrokePoint{1024, 1056}, TerrainStrokePoint{1088, 1056},
        TerrainStrokePoint{std::numeric_limits<double>::infinity(), 1056}, TerrainStrokePoint{32, 32}})
        Require(!PrepareTerrainCornerRampStroke(source, Rules(), Brush(), std::span(&invalid, 1), doodads,
            {}, {}, TerrainCornerRampAlgorithm::NativeCorners).Succeeded(), "noncanonical, nonfinite and non-inner locations reject");
    auto special = doodads; special.specialDoodads.push_back({"Spc0", 0, {8, 8}});
    const std::array shapes{TerrainWaterSpecialDoodadShape{0, "Spc0", 0, {8, 8}, 8, 8, 9, 9, {0, 0, 0, 0}}};
    Require(!PrepareTerrainCornerRampStroke(source, Rules(), Brush(), std::span(&point, 1), special,
        shapes, {}, TerrainCornerRampAlgorithm::NativeCorners).Succeeded(), "inner ramp never deletes intersecting special terrain silently");
}
inline void Check() { FourDirectionsCreateRemoveAndExactStreams(); RejectsHaloNeighborsBudgetAndInvalidInputs(); }
} // namespace War3Map::TerrainInnerCornerRampCases
