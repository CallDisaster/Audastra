#include "War3MapCore/MapData/TerrainWaterBrush.h"
#include "../src/Core/MapData/TerrainWaterBrushInternal.h"
#include "MapDataFixtures.h"

#include <algorithm>
#include <cmath>
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

void SpecialDoodadFactsAndImpact() {
    const TerrainStrokePoint point{1024, 1024}; auto grid = Flat(); auto brush = Brush(grid);
    auto doodads = EmptyDoodads(); doodads.specialDoodads.push_back({"YTfb", 0, {28, 28}});
    TerrainWaterSpecialDoodadShape far{0, "YTfb", 0, {28, 28}, 28, 28, 29, 29, {0, 1, 0, 0}};
    Require(ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, std::span(&far, 1)).Succeeded(),
        "unrelated special record mismatch is outside the actual processing region");
    grid = Flat(); brush = Brush(grid); doodads.specialDoodads.front().position = {8, 8};
    TerrainWaterSpecialDoodadShape local{0, "YTfb", 0, {8, 8}, 8, 8, 9, 9, {0, 0, 0, 0}};
    const auto original = Bytes(grid);
    const auto rejected = ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, std::span(&local, 1));
    Require(!rejected.Succeeded() && Bytes(grid) == original, "actual special deletion requirement rejects before input commit");
    Require(std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "TERRAIN_WATER_SPECIAL_DOODAD_DELETE";
    }), "special deletion is a concrete structured diagnostic");
    local.relativeCliffLevels = {0, 1, 1, 1};
    Require(ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), doodads, std::span(&local, 1)).Succeeded(),
        "an impacted special record whose relative levels still match needs no deletion");
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
        DryAnchorAndExactCoupling(); WetAnchorExactStepAndFixedStroke(); WaterLimitsAndOrderedShore();
        AtomicFailureAndLocalDomain(); PropagationAndDepthBudget(); SpecialDoodadFactsAndImpact(); HeightFalloffIndependentBoundaries();
        std::cout << "TerrainWaterBrush: 7 scenario groups passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "TerrainWaterBrush: " << error.what() << '\n'; return 1; }
}
