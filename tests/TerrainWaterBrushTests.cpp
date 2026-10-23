#include "War3MapCore/MapData/TerrainWaterBrush.h"
#include "../src/Core/MapData/TerrainWaterBrushInternal.h"
#include "MapDataFixtures.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::MapData;
namespace Math = War3Map::MapData::TerrainWaterBrushDetail;

namespace {

void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }

TerrainGridSnapshot Flat(std::uint32_t size = 33, std::uint16_t ground = 8192) {
    TerrainGridSnapshot grid;
    grid.header.version = 11; grid.header.mainTileset = 'L';
    grid.header.width = size; grid.header.height = size;
    grid.header.groundTilesets = {"Ldrt"}; grid.header.cliffTilesets = {"CLdi", "CLgr"};
    grid.vertices.resize(std::size_t(size) * size);
    for (auto& vertex : grid.vertices) {
        vertex.groundHeightRaw = ground; vertex.waterHeightAndFlagsRaw = 7936;
        vertex.textureAndFlagsRaw = 0x20; vertex.variationRaw = 0xa3; vertex.cliffAndLayerRaw = 2;
    }
    RefreshTerrainDerived(grid, 0, 0, size, size); return grid;
}

TerrainVertex& At(TerrainGridSnapshot& grid, std::uint32_t x, std::uint32_t y) {
    return grid.vertices[std::size_t(y) * grid.header.width + x];
}

const TerrainVertex& At(const TerrainGridSnapshot& grid, std::uint32_t x, std::uint32_t y) {
    return grid.vertices[std::size_t(y) * grid.header.width + x];
}

TerrainWaterLoadedRules Rules(bool enforce = true) {
    // 这是测试明确提供的配置，不是产品默认值。
    return {90.0f, -2048.0f, 2047.0f, 0.0f, 128.0f, 14, enforce, {{"Ldrt", true}}, {"CLdi", "CLgr"}};
}

DoodadPlacements EmptyDoodads() {
    auto parsed = ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11());
    Require(parsed.Succeeded(), "actual empty special segment fixture parses"); return std::move(*parsed.value);
}

TerrainWaterBrush Brush(TerrainGridSnapshot& terrain, TerrainWaterBrushKind kind = TerrainWaterBrushKind::Shallow,
    TerrainBrushShape shape = TerrainBrushShape::Square, std::uint32_t x = 8, std::uint32_t y = 8) {
    RefreshTerrainDerived(terrain, 0, 0, terrain.header.width, terrain.header.height);
    const auto anchor = CaptureTerrainWaterStrokeAnchor(terrain, {double(x) * 128.0, double(y) * 128.0});
    Require(anchor.Succeeded(), "fixed anchor captures from original terrain");
    return {kind, shape, 1, 0, 1, *anchor.value};
}

std::vector<std::byte> Bytes(const TerrainGridSnapshot& grid) {
    auto result = SerializeMapTerrain(grid); Require(result.Succeeded(), "fixture serializes"); return *result.value;
}

std::vector<std::byte> DoodadBytes(const DoodadPlacements& doodads) {
    const auto result = doodads.Serialize(); Require(result.Succeeded(), "owned DOO fixture serializes"); return *result.value;
}

DoodadPlacements SpecialFixture(std::initializer_list<SpecialDoodadPlacement> special) {
    auto doodads = EmptyDoodads();
    DoodadPlacement ordinary;
    ordinary.rawcode = "LTlt"; ordinary.variation = 3; ordinary.position = {123.5f, 456.25f, -17.0f};
    ordinary.rotation = 0.75f; ordinary.scale = {1.0f, 1.25f, 0.5f}; ordinary.flags = 2;
    ordinary.lifePercent = 87; ordinary.creationId = 971;
    PlacementItemSet drops; drops.items = {{"gold", 73}, {"lmbr", 27}};
    ordinary.itemSets.push_back(std::move(drops));
    doodads.doodads.push_back(ordinary); doodads.specialDoodads.assign(special);
    const auto wire = DoodadBytes(doodads);
    auto parsed = ParseDoodadPlacements(wire);
    Require(parsed.Succeeded(), "nonempty owned DOO fixture parses before the brush");
    return std::move(*parsed.value);
}

TerrainWaterSpecialDoodadShape FlatSpecialShape(const DoodadPlacements& doodads, std::uint32_t source,
    std::uint32_t width = 1, std::uint32_t height = 1) {
    const auto& row = doodads.specialDoodads[source];
    const auto x = static_cast<std::uint32_t>(row.position[0]), y = static_cast<std::uint32_t>(row.position[1]);
    return {source, row.rawcode, row.variation, row.position, x, y, x + width, y + height,
        std::vector<std::int8_t>(std::size_t(width + 1) * (height + 1), 0)};
}

void SpecialOrder(const DoodadPlacements& actual, const DoodadPlacements& source,
    std::initializer_list<std::uint32_t> expectedSourceOrder, const char* message) {
    auto expected = source; expected.specialDoodads.clear();
    for (const auto row : expectedSourceOrder) expected.specialDoodads.push_back(source.specialDoodads[row]);
    Require(DoodadBytes(actual) == DoodadBytes(expected), message);
}

void SourceUnchanged(const TerrainGridSnapshot& terrain, const std::vector<std::byte>& terrainBefore,
    const DoodadPlacements& doodads, const std::vector<std::byte>& doodadsBefore) {
    Require(Bytes(terrain) == terrainBefore && DoodadBytes(doodads) == doodadsBefore
        && doodads.OriginalBytes() == doodadsBefore,
        "preparation preserves source W3E fields, DOO records and original parsed DOO bytes");
}

void OwnedCandidateMatchesApplyAndPreservesInput() {
    const auto doodads = EmptyDoodads();
    const std::array points{TerrainStrokePoint{1024, 1024}, TerrainStrokePoint{1280, 1152}};
    const auto sameRange = [](const TerrainStrokeResult& a, const TerrainStrokeResult& b) {
        return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1 && a.headerChanged == b.headerChanged;
    };
    for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
        auto source = Flat(); const auto brush = Brush(source, kind);
        const auto original = Bytes(source);
        const auto prepared = PrepareTerrainWaterStroke(source, Rules(), brush, points, doodads);
        Require(prepared.Succeeded() && prepared.value && Bytes(source) == original,
            "owned preparation succeeds without changing its const input");
        auto applied = source;
        const auto result = ApplyTerrainWaterStroke(applied, Rules(), brush, points, doodads);
        Require(result.Succeeded() && result.value && Bytes(applied) == Bytes(prepared.value->terrain),
            "Apply moves the same deterministic owned terrain candidate");
        const auto& a = *result.value; const auto& b = prepared.value->result;
        Require(sameRange(a.changed, b.changed) && sameRange(a.processed, b.processed)
            && a.changedVertices == b.changedVertices && a.work == b.work && a.finalRandomState == b.finalRandomState
            && a.requiresPathingRebuild == b.requiresPathingRebuild
            && a.requiresObjectValidityRefresh == b.requiresObjectValidityRefresh
            && a.removedSpecialSourceRecords == b.removedSpecialSourceRecords
            && DoodadBytes(prepared.value->doodads) == DoodadBytes(doodads),
            "owned and in-place adapters retain exact ranges, budgets, random stream and derived work flags");
    }
    auto source = Flat(); const auto brush = Brush(source); const auto original = Bytes(source);
    for (const bool allocation : {false, true}) {
        TerrainWaterBrushLimits limits;
        if (allocation) limits.maximumCandidateBytes = 1;
        else limits.maximumWork = 4500;
        const auto prepared = PrepareTerrainWaterStroke(source, Rules(), brush, points, doodads, {}, limits);
        const auto applied = ApplyTerrainWaterStroke(source, Rules(), brush, points, doodads, {}, limits);
        Require(!prepared.Succeeded() && !prepared.value && !applied.Succeeded() && !applied.value && Bytes(source) == original,
            "budget failure publishes no owned candidate and neither entry point mutates input");
        Require(!prepared.diagnostics.empty() && !applied.diagnostics.empty()
            && prepared.diagnostics.front().code == applied.diagnostics.front().code,
            "owned and Apply failures preserve the same diagnostic");
    }
    At(source, 20, 20).textureAndFlagsRaw |= 0x10; RefreshTerrainDerived(source, 0, 0, 33, 33);
    const auto rejected = Bytes(source);
    const std::array invalidPoints{points.front(), TerrainStrokePoint{2560, 2560}};
    const auto invalid = PrepareTerrainWaterStroke(source, Rules(), brush, invalidPoints, doodads);
    Require(!invalid.Succeeded() && !invalid.value && Bytes(source) == rejected,
        "late terrain-domain failure leaves the source intact and exposes no partial candidate");
}

void DryAnchorAndExactCoupling() {
    const auto doodads = EmptyDoodads(); const TerrainStrokePoint point{1024, 1024};
    for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
        auto grid = Flat(33, 8320); // B=32，L=2，锚点 G=32。
        At(grid, 10, 10).groundHeightRaw = 8192;
        const auto brush = Brush(grid, kind);
        const auto result = ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads);
        Require(result.Succeeded() && result.value->requiresPathingRebuild && result.value->requiresObjectValidityRefresh,
            "complete water transaction reports derived work");
        const auto& center = At(grid, 8, 8);
        Require(center.HasWater() && center.waterHeightAndFlagsRaw == 8320 && center.groundHeightRaw == 8320,
            "dry anchor stores G as R and preserves its B");
        Require(center.CliffLevel() == (kind == TerrainWaterBrushKind::Deep ? 0 : 1), "dry target is exactly L-2 or L-1");
        Require(At(grid, 10, 10).groundHeightRaw == 8320, "size+2 square platform includes its outer corner");
        Require(!At(grid, 7, 8).HasWater() && At(grid, 7, 8).waterHeightAndFlagsRaw == 8320,
            "dry shore copies R without gaining HasWater");
        Require(!center.IsBlight() && !At(grid, 7, 8).IsBlight() && At(grid, 15, 15).IsBlight(),
            "water and adjacent cliff clear local blight while distant blight remains");
        Require(center.CliffVariation() == 2 && center.GroundVariation() == 3, "first explicit seed=1 variation is 41 mod 3");
        constexpr std::array<std::array<int, 3>, 8> expected{{
            {-1,-1,2}, {-1,0,1}, {-1,1,1}, {0,-1,2}, {0,1,1}, {1,-1,0}, {1,0,0}, {1,1,1}}};
        for (const auto& item : expected)
            Require(At(grid, std::uint32_t(8 + item[0]), std::uint32_t(8 + item[1])).CliffVariation() == item[2],
                "neighbor variation consumes the known MSVCRT sequence in x/y order");
        const auto wire = Bytes(grid); const auto parsed = ParseMapTerrain(wire);
        Require(parsed.Succeeded() && Bytes(*parsed.value->Snapshot()) == wire, "candidate raw fields roundtrip losslessly");
    }
    auto circle = Flat(33, 8320); At(circle, 10, 10).groundHeightRaw = 8192;
    const auto circleBrush = Brush(circle, TerrainWaterBrushKind::Shallow, TerrainBrushShape::Circle);
    Require(ApplyTerrainWaterStroke(circle, Rules(), circleBrush, std::span(&point, 1), doodads).Succeeded(), "circle transaction applies");
    Require(At(circle, 10, 10).groundHeightRaw == 8192, "expanded circle excludes a corner beyond radius two");
}

void WetAnchorExactStepAndFixedStroke() {
    const auto doodads = EmptyDoodads(); const TerrainStrokePoint point{1024, 1024};
    auto wet = Flat(); At(wet, 8, 8).textureAndFlagsRaw |= 0x40;
    At(wet, 8, 8).waterHeightAndFlagsRaw = 7680; // R=-128, G=0, L=2。
    auto brush = Brush(wet);
    Require(ApplyTerrainWaterStroke(wet, Rules(false), brush, std::span(&point, 1), doodads).Succeeded(), "wet anchor applies");
    Require(At(wet, 8, 8).CliffLevel() == 0 && At(wet, 8, 8).waterHeightAndFlagsRaw == 7680,
        "negative exact 128 step uses truncation and explicit extra minus one");
    auto grid = Flat(33, 8320); At(grid, 20, 20).cliffAndLayerRaw = 4; At(grid, 20, 20).groundHeightRaw = 8448;
    brush = Brush(grid); const std::array points{point, TerrainStrokePoint{2560, 2560}};
    Require(ApplyTerrainWaterStroke(grid, Rules(), brush, points, doodads).Succeeded(), "multiple points share one fixed anchor");
    Require(At(grid, 20, 20).groundHeightRaw == 8320 && At(grid, 20, 20).CliffLevel() == 1
        && At(grid, 20, 20).waterHeightAndFlagsRaw == 8320, "later point reuses first B/L/R despite different original terrain");
}

void WaterLimitsAndOrderedShore() {
    const auto doodads = EmptyDoodads(); const TerrainStrokePoint point{1024, 1024};
    for (const bool enforce : {false, true}) {
        auto grid = Flat(); const auto brush = Brush(grid); auto rules = Rules(enforce); rules.waterLayerOffset = -512;
        Require(ApplyTerrainWaterStroke(grid, rules, brush, std::span(&point, 1), doodads).Succeeded(), "explicit water-limits option applies");
        Require(At(grid, 8, 8).HasWater() != enforce && At(grid, 8, 8).waterHeightAndFlagsRaw == 8192,
            "only enabled cleanup clears lower water while retaining raw R");
    }
    auto grid = Flat(); At(grid, 7, 6).textureAndFlagsRaw |= 0x40; At(grid, 7, 6).waterHeightAndFlagsRaw = 8193;
    const auto brush = Brush(grid, TerrainWaterBrushKind::Shallow, TerrainBrushShape::Square, 6, 6);
    const TerrainStrokePoint interior{768, 768};
    Require(ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&interior, 1), doodads).Succeeded(), "ordered unequal-water cleanup applies");
    Require(!At(grid, 6, 6).HasWater() && At(grid, 6, 6).waterHeightAndFlagsRaw == 8192,
        "first processed wet center clears on unequal R and retains its raw water");
    Require(At(grid, 7, 6).HasWater() && At(grid, 7, 6).waterHeightAndFlagsRaw == 8193,
        "later existing wet neighbor observes the live earlier clear");
}

void AtomicFailureAndLocalDomain() {
    const auto doodads = EmptyDoodads(); const TerrainStrokePoint point{1024, 1024};
    auto grid = Flat(); At(grid, 30, 30).textureAndFlagsRaw |= 0x10;
    auto brush = Brush(grid);
    Require(ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads).Succeeded(), "unrelated old ramp does not reject");
    Require(At(grid, 30, 30).IsRamp(), "unrelated ramp is preserved");
    grid = Flat(); At(grid, 30, 30).groundHeightRaw = 0x4000; At(grid, 30, 30).waterHeightAndFlagsRaw |= 0x8000; brush = Brush(grid);
    Require(ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads).Succeeded(), "unrelated unknown raw bits do not reject");
    Require(At(grid, 30, 30).groundHeightRaw == 0x4000 && (At(grid, 30, 30).waterHeightAndFlagsRaw & 0x8000), "unrelated unknown fields remain exact");
    grid = Flat(); At(grid, 20, 20).textureAndFlagsRaw |= 0x10; brush = Brush(grid);
    const auto original = Bytes(grid); const std::array points{point, TerrainStrokePoint{2560, 2560}};
    Require(!ApplyTerrainWaterStroke(grid, Rules(), brush, points, doodads).Succeeded() && Bytes(grid) == original,
        "later local ramp rejects and restores all earlier point writes");
    grid = Flat(); brush = Brush(grid); const auto clean = Bytes(grid);
    TerrainWaterBrushLimits work; work.maximumWork = 4500;
    Require(!ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, {}, work).Succeeded() && Bytes(grid) == clean,
        "work budget fails after candidate mutation without changing input");
    TerrainWaterBrushLimits memory; memory.maximumCandidateBytes = 1;
    Require(!ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, {}, memory).Succeeded() && Bytes(grid) == clean,
        "tiny memory budget cannot underflow its accounting");
    auto rules = Rules(); rules.availableCliffTilesets.clear();
    Require(!ApplyTerrainWaterStroke(grid, rules, brush, std::span(&point, 1), doodads).Succeeded() && Bytes(grid) == clean,
        "selected cliff requires an actually available resource");
    rules = Rules(); rules.groundRules.clear();
    Require(!ApplyTerrainWaterStroke(grid, rules, brush, std::span(&point, 1), doodads).Succeeded() && Bytes(grid) == clean,
        "local blight needs its actual ground resource rule");
    grid = Flat(); for (auto& v : grid.vertices) v.textureAndFlagsRaw &= static_cast<std::uint8_t>(~0x20);
    At(grid, 30, 30).textureAndFlagsRaw |= 0x20; brush = Brush(grid);
    Require(ApplyTerrainWaterStroke(grid, rules, brush, std::span(&point, 1), doodads).Succeeded(), "unrelated blight needs no local rule");
    grid = Flat(); At(grid, 20, 20).waterHeightAndFlagsRaw |= 0x8000; brush = Brush(grid);
    const auto unknown = Bytes(grid);
    Require(!ApplyTerrainWaterStroke(grid, Rules(), brush, points, doodads).Succeeded() && Bytes(grid) == unknown,
        "later unknown water bit is rejected atomically");
    grid = Flat(); brush = Brush(grid); At(grid, 1, 1).position[0] += 1;
    const auto inconsistent = Bytes(grid);
    Require(!ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads).Succeeded() && Bytes(grid) == inconsistent,
        "inconsistent source derived coordinates reject without raw mutation");
}

void PropagationAndDepthBudget() {
    const auto doodads = EmptyDoodads(); const TerrainStrokePoint point{1024, 1024}; auto grid = Flat();
    for (std::uint32_t x = 9; x < 33; ++x) for (std::uint32_t y = 0; y < 33; ++y) At(grid, x, y).cliffAndLayerRaw = 10;
    const auto brush = Brush(grid); auto limited = grid; const auto original = Bytes(grid);
    TerrainWaterBrushLimits depth; depth.maximumRecursionDepth = 1;
    Require(!ApplyTerrainWaterStroke(limited, Rules(), brush, std::span(&point, 1), doodads, {}, depth).Succeeded()
        && Bytes(limited) == original, "depth budget rejects a real propagation before input commit");
    const auto applied = ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads);
    Require(applied.Succeeded() && At(grid, 8, 8).CliffLevel() == 1 && At(grid, 9, 8).CliffLevel() == 3,
        "direct marker protects level one while its original level-ten neighbor clamps to three");
    Require(applied.value->changed.x1 > 10 && applied.value->processed.x1 > 10,
        "ordered convergence reaches beyond the one-vertex brush and its immediate neighbor");
}

void SpecialDoodadDeletesFirstMiddleAndLast() {
    const TerrainStrokePoint point{1024, 1024};
    for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
        for (std::uint32_t removed = 0; removed < 3; ++removed) {
            auto grid = Flat(); const auto brush = Brush(grid, kind);
            auto doodads = SpecialFixture({{"S000", 0, {24, 24}}, {"S001", 1, {26, 26}}, {"S002", 2, {28, 28}}});
            doodads.specialDoodads[removed].position = {8, 8};
            auto reparsed = ParseDoodadPlacements(DoodadBytes(doodads));
            Require(reparsed.Succeeded(), "changed owned fixture has actual initial DOO bytes");
            doodads = std::move(*reparsed.value);
            std::array shapes{FlatSpecialShape(doodads, 2), FlatSpecialShape(doodads, 0), FlatSpecialShape(doodads, 1)};
            const auto terrainBefore = Bytes(grid), doodadsBefore = DoodadBytes(doodads);
            const auto result = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, shapes);
            Require(result.Succeeded() && result.value
                && result.value->result.removedSpecialSourceRecords == std::vector<std::uint32_t>{removed},
                "first, middle and last deletions report the actual source row, independently of shape input order");
            Require(At(result.value->terrain, 8, 8).CliffLevel() == (kind == TerrainWaterBrushKind::Shallow ? 1 : 0)
                && result.value->result.requiresPathingRebuild && result.value->result.requiresObjectValidityRefresh,
                "successful special deletion retains the water transaction and both derived-work requirements");
            if (removed == 0) SpecialOrder(result.value->doodads, doodads, {2, 1}, "first deletion swaps the last row into slot zero");
            if (removed == 1) SpecialOrder(result.value->doodads, doodads, {0, 2}, "middle deletion swaps the last row into the hole");
            if (removed == 2) SpecialOrder(result.value->doodads, doodads, {0, 1}, "last deletion simply shrinks the live special array");
            SourceUnchanged(grid, terrainBefore, doodads, doodadsBefore);
        }
    }
}

void SpecialDoodadMultiDeleteUsesCollectedSourceOrder() {
    const TerrainStrokePoint point{1024, 1024}; auto grid = Flat(); const auto brush = Brush(grid);
    const auto doodads = SpecialFixture({{"S000", 0, {8, 8}}, {"S001", 1, {24, 24}},
        {"S002", 2, {7, 8}}, {"S003", 3, {26, 26}}, {"S004", 4, {8, 7}}});
    // 所有输入形态在平源上有效。三个单格各含 (8,8)，刷后才失配；shape 参数故意倒序。
    const std::array shapes{FlatSpecialShape(doodads, 4), FlatSpecialShape(doodads, 3), FlatSpecialShape(doodads, 2),
        FlatSpecialShape(doodads, 1), FlatSpecialShape(doodads, 0)};
    const auto terrainBefore = Bytes(grid), doodadsBefore = DoodadBytes(doodads);
    const auto result = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, shapes);
    Require(result.Succeeded() && result.value
        && result.value->result.removedSpecialSourceRecords == std::vector<std::uint32_t>{0, 2, 4},
        "collect all mismatching XY in current DOO order before performing any swap-last deletion");
    // 手写序列：[0,1,2,3,4] -> [4,1,2,3] -> [4,1,3] -> [3,1]。
    SpecialOrder(result.value->doodads, doodads, {3, 1},
        "multiple deletions retain the exact swap-last order instead of stable erase or source sorting");
    SourceUnchanged(grid, terrainBefore, doodads, doodadsBefore);
}

void SpecialDoodadDuplicateXyUsesCurrentFirstRow() {
    const TerrainStrokePoint point{1024, 1024}; auto grid = Flat(); const auto brush = Brush(grid);
    const auto doodads = SpecialFixture({{"S000", 0, {8, 8}}, {"S001", 1, {8, 8}}, {"S002", 2, {8, 8}}});
    const std::array shapes{FlatSpecialShape(doodads, 1), FlatSpecialShape(doodads, 2), FlatSpecialShape(doodads, 0)};
    const auto terrainBefore = Bytes(grid), doodadsBefore = DoodadBytes(doodads);
    const auto result = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, shapes);
    Require(result.Succeeded() && result.value
        && result.value->result.removedSpecialSourceRecords == std::vector<std::uint32_t>{0, 2, 1},
        "three equal-XY requests delete current first rows 0, then swapped row 2, then row 1");
    SpecialOrder(result.value->doodads, doodads, {}, "all equal-XY special records are removed while ordinary DOO records survive");
    SourceUnchanged(grid, terrainBefore, doodads, doodadsBefore);

    const auto pair = SpecialFixture({{"S000", 0, {8, 8}}, {"S001", 1, {8, 8}}});
    const auto pairBefore = DoodadBytes(pair);
    const std::array pairShapes{FlatSpecialShape(pair, 1), FlatSpecialShape(pair, 0)};
    const auto pairResult = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), pair, pairShapes);
    Require(pairResult.Succeeded() && pairResult.value
        && pairResult.value->result.removedSpecialSourceRecords == std::vector<std::uint32_t>{0, 1},
        "two equal-XY source records each consume one request instead of XY deduplication");
    SpecialOrder(pairResult.value->doodads, pair, {}, "two equal-XY records are both deleted");
    SourceUnchanged(grid, terrainBefore, pair, pairBefore);
}

void SpecialDoodadRepeatedPointsDoNotDeleteTwice() {
    auto grid = Flat(); const auto brush = Brush(grid);
    const auto doodads = SpecialFixture({{"S000", 0, {8, 8}}, {"S001", 1, {20, 20}}, {"S002", 2, {28, 28}}});
    const std::array shapes{FlatSpecialShape(doodads, 2), FlatSpecialShape(doodads, 0), FlatSpecialShape(doodads, 1)};
    const std::array points{TerrainStrokePoint{1024, 1024}, TerrainStrokePoint{1024, 1024},
        TerrainStrokePoint{2560, 2560}, TerrainStrokePoint{2560, 2560}, TerrainStrokePoint{1024, 1024}};
    const auto terrainBefore = Bytes(grid), doodadsBefore = DoodadBytes(doodads);
    const auto result = PrepareTerrainWaterStroke(grid, Rules(), brush, points, doodads, shapes);
    Require(result.Succeeded() && result.value
        && result.value->result.removedSpecialSourceRecords == std::vector<std::uint32_t>{0, 1},
        "later stroke points use the live special set and never delete an already removed source row again");
    SpecialOrder(result.value->doodads, doodads, {2}, "repeated points retain the single unrelated special row exactly");
    SourceUnchanged(grid, terrainBefore, doodads, doodadsBefore);
}

void SpecialDoodadRejectsLateShoreChange() {
    const TerrainStrokePoint point{1024, 1024};
    auto grid = Flat();
    At(grid, 8, 8).textureAndFlagsRaw |= 0x40;
    At(grid, 8, 8).waterHeightAndFlagsRaw = 10240; // R=512，B=0，锚点 L=2。
    At(grid, 9, 8).cliffAndLayerRaw = 0;
    At(grid, 8, 9).cliffAndLayerRaw = 0;
    At(grid, 9, 9).cliffAndLayerRaw = 0;
    const auto brush = Brush(grid);
    const auto source = Bytes(grid);
    auto unrestricted = grid;
    Require(ApplyTerrainWaterStroke(unrestricted, Rules(), brush, std::span(&point, 1), EmptyDoodads()).Succeeded(),
        "late-shore fixture succeeds without a special record");
    Require(At(unrestricted, 8, 8).CliffLevel() == 6 && At(unrestricted, 9, 8).CliffLevel() == 7
        && At(unrestricted, 8, 9).CliffLevel() == 7 && At(unrestricted, 9, 9).CliffLevel() == 7,
        "dry-shore cleanup raises the three level-four corners to seven after initial convergence");

    const auto doodads = SpecialFixture({{"YTfb", 0, {8, 8}}});
    const auto doodadsBefore = DoodadBytes(doodads);
    const TerrainWaterSpecialDoodadShape shape{0, "YTfb", 0, {8, 8}, 8, 8, 9, 9, {2, 0, 0, 0}};
    // 源角为 {2,0,0,0}；首轮收敛后 {6,4,4,4} 仍匹配，岸线处理后 {6,7,7,7} 不再匹配。
    const auto result = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, std::span(&shape, 1));
    // 原版删除阶段在岸线处理之前；最终核对只能拒绝，不能补做第二轮删除。
    Require(!result.Succeeded() && !result.value
        && std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "TERRAIN_WATER_SPECIAL_DOODAD_DELETE";
        }), "final special check rejects a shore mismatch after the original deletion phase");
    SourceUnchanged(grid, source, doodads, doodadsBefore);
}

void SpecialDoodadChecksDoNotExpandProcessing() {
    const TerrainStrokePoint point{1024, 1024};
    auto source = Flat();
    At(source, 8, 8).textureAndFlagsRaw |= 0x40;
    At(source, 8, 8).waterHeightAndFlagsRaw = 8192; // 已湿锚点保持 L=2，无岸线改层。
    const auto brush = Brush(source);
    const auto original = Bytes(source);
    const auto doodads = SpecialFixture({{"YTfb", 0, {8, 8}}, {"YTfb", 0, {28, 28}}});
    const auto doodadsBefore = DoodadBytes(doodads);
    // 第一个匹配形态跨至远区；两项源形态均有效，读取形态不应扩大真实笔刷处理域。
    const TerrainWaterSpecialDoodadShape bridge{0, "YTfb", 0, {8, 8}, 8, 8, 28, 28, std::vector<std::int8_t>(21 * 21, 0)};
    const TerrainWaterSpecialDoodadShape far{1, "YTfb", 0, {28, 28}, 28, 28, 29, 29, {0, 0, 0, 0}};
    std::array shapes{bridge, far};
    for (int order = 0; order < 2; ++order) {
        const auto result = PrepareTerrainWaterStroke(source, Rules(false), brush, std::span(&point, 1), doodads, shapes);
        Require(result.Succeeded() && result.value && Bytes(result.value->terrain) == original
            && result.value->result.removedSpecialSourceRecords.empty(), "matching local and far shapes retain every special record");
        SpecialOrder(result.value->doodads, doodads, {0, 1}, "a no-deletion candidate preserves complete DOO bytes and order");
        Require(result.value->result.processed.x1 < 28 && result.value->result.processed.y1 < 28,
            "pure special validation does not expand the actual processed bounds");
        SourceUnchanged(source, original, doodads, doodadsBefore);
        std::reverse(shapes.begin(), shapes.end());
    }
}

void OrdinaryDoodadExpansionIsBudgetedBeforeCopy() {
    auto terrain = Flat(9); const auto original = Bytes(terrain);
    const auto brush = Brush(terrain, TerrainWaterBrushKind::Shallow, TerrainBrushShape::Square, 4, 4);
    const TerrainStrokePoint point{512, 512};
    auto doodads = SpecialFixture({});
    doodads.doodads.front().itemSets.front().items.assign(4096, {"gold", 100});
    TerrainWaterBrushLimits memory; memory.maximumCandidateBytes = 64 * 1024;
    const auto tooLarge = PrepareTerrainWaterStroke(terrain, Rules(), brush, std::span(&point, 1), doodads, {}, memory);
    Require(!tooLarge.Succeeded() && !tooLarge.value && !tooLarge.diagnostics.empty()
        && tooLarge.diagnostics.front().code == "TERRAIN_WATER_MEMORY_BUDGET" && Bytes(terrain) == original,
        "nested ordinary DOO expansion is measured before candidate copy or wire serialization");
    TerrainWaterBrushLimits work; work.maximumWork = 1000;
    const auto tooMuchWork = PrepareTerrainWaterStroke(terrain, Rules(), brush, std::span(&point, 1), doodads, {}, work);
    Require(!tooMuchWork.Succeeded() && !tooMuchWork.value && !tooMuchWork.diagnostics.empty()
        && tooMuchWork.diagnostics.front().code == "TERRAIN_WATER_WORK_BUDGET"
        && doodads.doodads.front().itemSets.front().items.size() == 4096 && Bytes(terrain) == original,
        "nested ordinary DOO traversal cannot bypass the per-stroke work cap");
}

void SpecialDoodadSourceFactsAndFailuresAreAtomic() {
    const TerrainStrokePoint point{1024, 1024}; auto grid = Flat(); const auto brush = Brush(grid);
    const auto doodads = SpecialFixture({{"S000", 0, {8, 8}}, {"S001", 1, {28, 28}}});
    const std::array validShapes{FlatSpecialShape(doodads, 0), FlatSpecialShape(doodads, 1)};
    const auto terrainBefore = Bytes(grid), doodadsBefore = DoodadBytes(doodads);
    const auto refused = [&](const auto& result, const char* code) {
        Require(!result.Succeeded() && !result.value && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; }),
            "failed special transaction publishes no partial terrain, DOO candidate or deletion list and reports its cause");
        SourceUnchanged(grid, terrainBefore, doodads, doodadsBefore);
    };
    auto invalid = validShapes; invalid[1].relativeCliffLevels = {0, 1, 0, 0};
    refused(PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, invalid),
        "TERRAIN_WATER_SPECIAL_DOODAD_SOURCE_SHAPE");
    // 即使不在笔刷附近，源形态失配也不能被当作本笔造成的合法删除。
    invalid = validShapes; invalid[0].relativeCliffLevels = {0, 1, 0, 0};
    refused(PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, invalid),
        "TERRAIN_WATER_SPECIAL_DOODAD_SOURCE_SHAPE");
    invalid = validShapes; invalid[1].sourceRecordIndex = 0;
    refused(PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, invalid),
        "TERRAIN_WATER_SPECIAL_DOODAD_FACTS");
    invalid = validShapes; invalid[1].rawcode = "BAD!";
    refused(PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, invalid),
        "TERRAIN_WATER_SPECIAL_DOODAD_FACTS");
    invalid = validShapes; invalid[1].relativeCliffLevels.pop_back();
    refused(PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, invalid),
        "TERRAIN_WATER_SPECIAL_DOODAD_FACTS");
    refused(PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, std::span(validShapes).first(1)),
        "TERRAIN_WATER_SPECIAL_DOODAD_FACTS");

    auto missingRules = Rules(); missingRules.availableCliffTilesets.clear();
    refused(PrepareTerrainWaterStroke(grid, missingRules, brush, std::span(&point, 1), doodads, validShapes),
        "TERRAIN_WATER_CLIFF_RESOURCE");
    TerrainWaterBrushLimits memory; memory.maximumCandidateBytes = 1;
    refused(PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, validShapes, memory),
        "TERRAIN_WATER_MEMORY_BUDGET");
    TerrainWaterBrushLimits work; work.maximumWork = 4500;
    refused(PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, validShapes, work),
        "TERRAIN_WATER_WORK_BUDGET");

    auto late = grid; At(late, 20, 20).textureAndFlagsRaw |= 0x10;
    RefreshTerrainDerived(late, 0, 0, 33, 33); const auto lateBefore = Bytes(late);
    const std::array points{point, TerrainStrokePoint{2560, 2560}};
    const auto lateResult = PrepareTerrainWaterStroke(late, Rules(), brush, points, doodads, validShapes);
    Require(!lateResult.Succeeded() && !lateResult.value, "a later terrain failure hides the earlier special deletion candidate");
    SourceUnchanged(late, lateBefore, doodads, doodadsBefore);
}

void SpecialDoodadDuplicateXyCannotLeaveInvalidSurvivor() {
    auto grid = Flat(); const auto brush = Brush(grid); const TerrainStrokePoint point{1024, 1024};
    const auto doodads = SpecialFixture({{"S000", 0, {6, 6}}, {"S001", 1, {6, 6}}});
    const std::array shapes{FlatSpecialShape(doodads, 0), FlatSpecialShape(doodads, 1, 3, 3)};
    const auto terrainBefore = Bytes(grid), doodadsBefore = DoodadBytes(doodads);
    // 窄形态四角留平；宽形态包含新水顶点而失配。一次 XY 请求按原版先删源0，源1仍不合法。
    const auto result = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, shapes);
    Require(!result.Succeeded() && !result.value && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "TERRAIN_WATER_SPECIAL_DOODAD_DELETE"; }),
        "current-first XY deletion must refuse an invalid surviving sibling instead of performing a second cleanup phase");
    SourceUnchanged(grid, terrainBefore, doodads, doodadsBefore);
}

void SpecialDoodadLegacyApplyCannotDiscardOwnedDeletion() {
    auto grid = Flat(); const auto brush = Brush(grid); const TerrainStrokePoint point{1024, 1024};
    const auto doodads = SpecialFixture({{"S000", 0, {8, 8}}});
    const auto shape = FlatSpecialShape(doodads, 0);
    const auto terrainBefore = Bytes(grid), doodadsBefore = DoodadBytes(doodads);
    const auto prepared = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, std::span(&shape, 1));
    Require(prepared.Succeeded() && prepared.value
        && prepared.value->result.removedSpecialSourceRecords == std::vector<std::uint32_t>{0},
        "owned preparation exposes the coupled DOO deletion before the compatibility adapter is checked");
    const auto applied = ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, std::span(&shape, 1));
    Require(!applied.Succeeded() && !applied.value,
        "terrain-only Apply cannot commit W3E while silently losing its required special-DOO deletion");
    SourceUnchanged(grid, terrainBefore, doodads, doodadsBefore);
}

void HeightFalloffIndependentBoundaries() {
    Require(!Math::MakeHeightFalloffCenter(-256, -200, 100) && !Math::MakeHeightFalloffCenter(100, -200, 100)
        && !Math::MakeHeightFalloffCenter(0, -200, 100), "legal closed bounds produce no falloff");
    Require(!Math::MakeHeightFalloffCenter(-300, -300, 100), "minimum below initialized floor remains the low bound");
    const auto inverted = Math::MakeHeightFalloffCenter(-384, -200, -512);
    Require(inverted && inverted->delta == -128, "inverted bounds keep high branch priority without swapping");
    constexpr float epsilon = 0x1p-22f;
    const float predecessor = std::nextafter(epsilon, 0.0f);
    Require(!Math::MakeHeightFalloffCenter(predecessor, -256, 0), "epsilon predecessor returns");
    Require(Math::MakeHeightFalloffCenter(epsilon, -256, 0).has_value(), "epsilon equality proceeds");
    Require(Math::MakeHeightFalloffCenter(std::nextafter(epsilon, 1.0f), -256, 0).has_value(), "epsilon successor proceeds");
    const auto fixedAngle = Math::MakeHeightFalloffCenter(256, -256, 128);
    Require(fixedAngle && fixedAngle->extent == 3 && fixedAngle->radius > 221.70 && fixedAngle->radius < 221.71,
        "128 correction has fixed thirty-degree radius and truncation plus two extent");
    const Math::HeightFalloffCenter kernel{128, 16384, 128, 3};
    Require(Math::HeightFalloffWeight(kernel, 0) == 128, "kernel center retains full signed delta");
    Require(Math::HeightFalloffWeight(kernel, 1) == 63.999996185302734375f, "rho one retains the precise float half-offset");
    Require(!Math::HeightFalloffWeight(kernel, 2), "rho beyond one is excluded");
    Require(!Math::HeightFalloffWrites(99, -300, 100, -1) && !Math::HeightFalloffWrites(-301, -300, 100, -1),
        "both translated-bound equalities are excluded");
    Require(Math::HeightFalloffWrites(100, -300, 100, -1) && Math::HeightFalloffWrites(-302, -300, 100, -1)
        && !Math::HeightFalloffWrites(50, -300, 100, -1), "same signed offset gates candidates on both sides");
    Require(Math::HeightFalloffWrites(std::nextafter(99.0, 100.0), -300, 100, -1)
        && Math::HeightFalloffWrites(std::nextafter(-301.0, -302.0), -300, 100, -1), "strict outside successors write");
    Require(!Math::HeightFalloffWrites(100.0 + 0x1p-25, -300, 100, 0x1p-24f), "threshold sum has no extra float rounding");
    Require(Math::EncodeBaseHeight(-0.01) == 8191 && Math::EncodeBaseHeight(0.01) == 8192
        && Math::EncodeBaseHeight(-0.25) == 8191 && Math::EncodeBaseHeight(0.25) == 8193,
        "biased truncation has asymmetric tiny-offset quantization");
    Require(!Math::EncodeBaseHeight(-2048.25) && !Math::EncodeBaseHeight(2048)
        && !Math::EncodeBaseHeight(std::numeric_limits<double>::infinity()), "unsupported height output fails closed");
}

} // namespace

int main() {
    try {
        OwnedCandidateMatchesApplyAndPreservesInput();
        DryAnchorAndExactCoupling(); WetAnchorExactStepAndFixedStroke(); WaterLimitsAndOrderedShore();
        AtomicFailureAndLocalDomain(); PropagationAndDepthBudget(); SpecialDoodadDeletesFirstMiddleAndLast();
        SpecialDoodadMultiDeleteUsesCollectedSourceOrder(); SpecialDoodadDuplicateXyUsesCurrentFirstRow();
        SpecialDoodadRepeatedPointsDoNotDeleteTwice(); SpecialDoodadRejectsLateShoreChange();
        SpecialDoodadChecksDoNotExpandProcessing(); OrdinaryDoodadExpansionIsBudgetedBeforeCopy(); SpecialDoodadSourceFactsAndFailuresAreAtomic();
        SpecialDoodadDuplicateXyCannotLeaveInvalidSurvivor(); SpecialDoodadLegacyApplyCannotDiscardOwnedDeletion();
        HeightFalloffIndependentBoundaries();
        std::cout << "TerrainWaterBrush: 17 scenario groups passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "TerrainWaterBrush: " << error.what() << '\n'; return 1; }
}
