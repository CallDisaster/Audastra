#include "War3MapCore/MapData/TerrainNativePathing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "operation failed";
        for (const auto& item : result.diagnostics) message += " | " + item.code + ": " + item.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}

template<class T> void Refused(const Result<T>& result, std::string_view code, std::string_view location = {}) {
    Require(!result.Succeeded() && !result.value, "failure cannot return a partial grid");
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
    Require(found != result.diagnostics.end(), "failure must name its actual unsupported input");
    if (!location.empty()) Require(found->message.find(location) != std::string::npos, "failure locates the offending input");
}

struct Fixture {
    TerrainGridSnapshot grid;
    std::vector<TerrainNativeGroundRule> ground{{"Tg01", true, true, true}};
    TerrainNativePlayableBounds bounds;
    std::optional<TerrainNativeWaterRules> water;
    std::vector<std::uint8_t> objects;
    std::size_t specialCount = 0;

    Fixture(std::uint32_t columns = 2, std::uint32_t rows = 2) {
        grid.header.width = columns; grid.header.height = rows;
        grid.header.groundTilesets = {"Tg01"};
        grid.header.cliffTilesets = {"Cl01"};
        grid.vertices.resize(static_cast<std::size_t>(columns) * rows);
        for (auto& vertex : grid.vertices) {
            vertex.groundHeightRaw = 8192;
            vertex.waterHeightAndFlagsRaw = 8192;
            vertex.cliffAndLayerRaw = 2;
        }
        bounds = {0, 0, (columns - 1) * 128.0, (rows - 1) * 128.0};
    }

    TerrainNativePathingInput Input() const {
        return {&grid, bounds, ground, water, objects, specialCount};
    }

    Result<Bytes> Generate(std::size_t maximumBytes = 64 * 1024 * 1024) const {
        return GenerateTerrainNativePathing(Input(), maximumBytes);
    }

    void Wet(std::uint16_t surfaceRaw, bool impassable = false) {
        water = TerrainNativeWaterRules{0, 52, impassable};
        for (auto& vertex : grid.vertices) {
            vertex.textureAndFlagsRaw |= 0x40;
            vertex.waterHeightAndFlagsRaw = surfaceRaw;
        }
    }
};

void Uniform(const Fixture& fixture, unsigned expected, const char* message) {
    const auto grid = Take(fixture.Generate());
    Require(std::all_of(grid.begin(), grid.end(), [&](auto value) { return std::to_integer<unsigned>(value) == expected; }), message);
    Require(Take(fixture.Generate()) == grid, "native output is deterministic for identical inputs");
}

void SixModesUseTheirDistinctRules() {
    Fixture dry;
    Uniform(dry, 0x40, "ordinary dry ground blocks floating only");
    dry.ground.front() = {"Tg01", false, false, false};
    Uniform(dry, 0x4e, "terrain table independently controls walking flying and building");

    Fixture equal;
    equal.Wet(8192);
    Uniform(equal, 0x48, "water equal to ground is shallow but not strictly covered");
    Fixture shallow;
    shallow.Wet(8193);
    Uniform(shallow, 0x08, "all strictly covered shallow corners permit floating and amphibious travel");
    Fixture deep;
    deep.Wet(8192 + 52 * 4);
    Uniform(deep, 0x0a, "equality at the actual deep threshold blocks walking and building");
    Fixture impassable;
    impassable.Wet(8193, true);
    Uniform(impassable, 0xca, "impassable water rejects four ground or water modes but not flight");
    impassable.ground.front().flyable = false;
    Uniform(impassable, 0xce, "flight still depends on the actual terrain material");

    Fixture exposed;
    exposed.Wet(8191, true);
    Uniform(exposed, 0x40, "water below ground has no impassable-water effect");
    Fixture zeroThreshold;
    zeroThreshold.Wet(8192);
    zeroThreshold.water->deepLevelPathing = 0;
    Uniform(zeroThreshold, 0x0a, "zero threshold makes equality deep without inventing strict coverage");
}

void NearestVertexDiffersFromTileOwner() {
    Fixture fixture;
    fixture.grid.header.groundTilesets = {"Tg01", "Tg02", "Tg03", "Tg04"};
    fixture.ground = {{"Tg01", false, true, true}, {"Tg02", true, false, true},
        {"Tg03", true, true, false}, {"Tg04", true, true, true}};
    for (std::size_t i = 0; i < 4; ++i) {
        fixture.grid.vertices[i].textureAndFlagsRaw = static_cast<std::uint8_t>(i | (i == 3 ? 0x20 : 0));
        fixture.grid.vertices[i].position = {1000000, -1000000, std::numeric_limits<float>::quiet_NaN()};
        fixture.grid.vertices[i].waterBaseHeight = std::numeric_limits<float>::infinity();
    }
    const auto generated = Take(fixture.Generate());
    // 手写四个2x2象限，不使用产品最近顶点或位编码函数建立预期。
    constexpr std::array<unsigned, 16> expected{
        0x42, 0x42, 0x44, 0x44, 0x42, 0x42, 0x44, 0x44,
        0x48, 0x48, 0x60, 0x60, 0x48, 0x48, 0x60, 0x60};
    for (std::size_t i = 0; i < expected.size(); ++i)
        Require(std::to_integer<unsigned>(generated[i]) == expected[i], "material and blight follow nearest raw vertex, not stale display fields");
}

void StrictWaterRequiresAllFourCorners() {
    Fixture fixture;
    fixture.Wet(8193);
    fixture.grid.vertices[3].waterHeightAndFlagsRaw = 8192;
    Uniform(fixture, 0x48, "one equal corner removes the all-corners floating exemption");

    fixture.grid.vertices[3].waterHeightAndFlagsRaw = 8192 + 52 * 4;
    fixture.grid.vertices[0].textureAndFlagsRaw = 0;
    const auto generated = Take(fixture.Generate());
    constexpr std::array<unsigned, 16> expected{
        0x40, 0x40, 0x48, 0x48, 0x40, 0x40, 0x48, 0x48,
        0x48, 0x48, 0x0a, 0x0a, 0x48, 0x48, 0x0a, 0x0a};
    for (std::size_t i = 0; i < expected.size(); ++i)
        Require(std::to_integer<unsigned>(generated[i]) == expected[i], "deep nearest vertex overrides missing full-tile water coverage");
}

void SharedWaterClassifierKeepsNativePrecision() {
    Fixture fixture;
    auto vertex = fixture.grid.vertices.front();
    vertex.textureAndFlagsRaw = 0x40;
    vertex.groundHeightRaw = 12390; // 1049.5，原始四分之一世界单位。
    vertex.waterHeightAndFlagsRaw = 12288; // 基础水面1024。
    const TerrainNativeWaterRules rules{0.2f, 0.1f, false};
    const auto state = Take(ClassifyNativeTerrainWater(vertex, rules));
    Require(state == TerrainNativeWaterState{2, true}, "surface addition must not be rounded to binary32 before the deep comparison");
    vertex.groundHeightRaw = 12391;
    Require(Take(ClassifyNativeTerrainWater(vertex, rules)) == TerrainNativeWaterState{}, "surface below ground is dry despite the water flag");
    vertex.groundHeightRaw = 8192;
    vertex.waterHeightAndFlagsRaw = 8192;
    Require(Take(ClassifyNativeTerrainWater(vertex, {0, 52, false})) == TerrainNativeWaterState{1, false},
        "water equality and strict coverage are separate states");
    vertex.cliffAndLayerRaw = 3;
    vertex.waterHeightAndFlagsRaw = 8192 + 128 * 4;
    Require(Take(ClassifyNativeTerrainWater(vertex, {0, 52, false})) == TerrainNativeWaterState{1, false},
        "native effective ground includes the cliff level even without a cliff edge");
}

void OrdinaryCliffsUseLevelsAndRawGroundPalette() {
    Fixture cliff;
    cliff.grid.vertices[3].cliffAndLayerRaw = 3;
    Uniform(cliff, 0xca, "a dry ordinary cliff blocks walking building floating and amphibious travel");
    cliff.grid.header.groundTilesets.push_back("Tg02");
    cliff.ground.push_back({"Tg02", true, false, true});
    cliff.grid.vertices[1].textureAndFlagsRaw = 1;
    const auto mixed = Take(cliff.Generate());
    constexpr std::array<unsigned, 16> expected{
        0xca, 0xca, 0xce, 0xce, 0xca, 0xca, 0xce, 0xce,
        0xca, 0xca, 0xca, 0xca, 0xca, 0xca, 0xca, 0xca};
    for (std::size_t i = 0; i < expected.size(); ++i)
        Require(std::to_integer<unsigned>(mixed[i]) == expected[i], "ordinary cliff keeps the nearest raw ground palette for flight");

    Fixture submerged;
    submerged.grid.vertices[3].cliffAndLayerRaw = 3;
    submerged.Wet(8192 + 129 * 4);
    Uniform(submerged, 0x0a, "four strictly covered cliff corners permit both water movement modes");
    submerged.Wet(8192 + 128 * 4);
    const auto shoreline = Take(submerged.Generate());
    constexpr std::array<unsigned, 16> equalCorner{
        0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
        0x0a, 0x0a, 0xca, 0xca, 0x0a, 0x0a, 0xca, 0xca};
    for (std::size_t i = 0; i < equalCorner.size(); ++i)
        Require(std::to_integer<unsigned>(shoreline[i]) == equalCorner[i], "cliff amphibious and floating rules distinguish deep water from equal shallow water");
}

void PlayableBoundsAreWorldCoordinates() {
    Fixture fixture;
    fixture.grid.header.offset = {-128, -128};
    fixture.bounds = {-128, -128, 0, 0};
    Uniform(fixture, 0x40, "all four world corners on inclusive bounds are playable");
    fixture.bounds.left = -127.875;
    Uniform(fixture, 0xce, "a fractional world crop cannot be rounded to a terrain tile");
    fixture.bounds.left = -128;
    fixture.bounds.top = -0x1p-10;
    Uniform(fixture, 0xce, "the northeast corner must also fit the actual world range");
    fixture.bounds.top = 0;
    fixture.grid.vertices[3].textureAndFlagsRaw |= 0x80;
    Uniform(fixture, 0x40, "boundary flags belong to tile SW rather than being ORed across all four corners");
    fixture.grid.vertices[3].textureAndFlagsRaw = 0;
    fixture.grid.vertices[0].textureAndFlagsRaw |= 0x80;
    Uniform(fixture, 0xce, "native boundary flag rejects every non-blight movement mode");
    fixture.grid.vertices[0].textureAndFlagsRaw = 0;
    fixture.grid.vertices[0].waterHeightAndFlagsRaw |= 0x4000;
    Uniform(fixture, 0xce, "map-edge flag rejects every non-blight movement mode");
    for (auto& vertex : fixture.grid.vertices) vertex.textureAndFlagsRaw |= 0x20;
    Uniform(fixture, 0xee, "blight survives world and tile boundary rejection");
}

void BoundsRetainNativeFloatLandingsAndReverseProjection() {
    Fixture secondary(2, 3);
    secondary.grid.header.offset = {0, -128};
    secondary.bounds = {0, -128, 128, std::nextafter(128.0f, 0.0f)};
    // 上边比顶点128略低；原版的次轴跨度/差值在binary32都落为256。
    Uniform(secondary, 0x40, "native secondary projection can include a tiny upper overshoot");
    secondary.bounds.bottom = std::nextafter(-128.0f, 0.0f);
    secondary.bounds.top = 128;
    Uniform(secondary, 0x40, "reverse secondary projection can include a tiny lower overshoot");
    secondary.bounds.bottom = -128;
    secondary.bounds.top = 127.99f;
    const auto clipped = Take(secondary.Generate());
    for (std::size_t i = 0; i < clipped.size(); ++i)
        Require(clipped[i] == (i < 16 ? std::byte{0x40} : std::byte{0xce}),
            "a representable larger overshoot stays outside despite float landings");

    Fixture subnormal;
    subnormal.grid.header.offset = {0, -128};
    subnormal.bounds = {0, -128, 128, -std::numeric_limits<float>::denorm_min()};
    Uniform(subnormal, 0x40, "secondary range comparison preserves native binary32 rounding near zero");

    Fixture reverse;
    reverse.grid.header.offset = {16256, 0};
    reverse.bounds = {0x1p-11, 0, 16384, 128};
    // 正向dx在16384处舍入为16384，大于原版仍留x87的25位跨度。
    // 反向dx为0，端点仍合法；若删去NE重试，这一格会全变边界。
    Uniform(reverse, 0x40, "reverse projection rescues an inclusive endpoint rounded outward from SW");
    reverse.bounds.left = 0x1p-12;
    Uniform(reverse, 0x40, "a provably exact 26-bit primary span remains supported");

    Fixture primary;
    primary.bounds = {0x1p-11, 0, 16384, 128};
    Uniform(primary, 0xce, "primary x87 projection must not round a slightly excessive ratio down to binary32 one");
    primary.bounds = {0, 0, 128.25, 128.25};
    Uniform(primary, 0x40, "fractional camera bounds remain supported without requiring tile alignment");
}

void BoundsRejectUnprovenPrecisionAndPackedCoordinates() {
    Fixture fixture;
    fixture.bounds = {0x1p-13, 0, 16384, 128};
    Refused(fixture.Generate(), "TERRAIN_NATIVE_BOUNDS_PRECISION");
    fixture.bounds = {-static_cast<double>(std::numeric_limits<float>::denorm_min()), 0, 128, 128};
    Refused(fixture.Generate(), "TERRAIN_NATIVE_BOUNDS_PRECISION");
    fixture.bounds = {0, 0, 128.00000001, 128};
    Refused(fixture.Generate(), "TERRAIN_NATIVE_BOUNDS_PRECISION");

    fixture = Fixture{};
    fixture.grid.header.offset[0] = 0.25f;
    Refused(fixture.Generate(), "TERRAIN_NATIVE_COORDINATE_DOMAIN");
    fixture.grid.header.offset = {-32768, -32768};
    fixture.bounds = {-32768, -32768, -32640, -32640};
    Uniform(fixture, 0x40, "the minimum decoded native packed vertex is inclusive");
    fixture.grid.header.offset = {32512, 32512};
    fixture.bounds = {32512, 32512, 32640, 32640};
    Uniform(fixture, 0x40, "the maximum decoded native packed vertex is inclusive");
    fixture.grid.header.offset[0] = 32640;
    Refused(fixture.Generate(), "TERRAIN_NATIVE_COORDINATE_DOMAIN");
    fixture.grid.header.offset = {-32896, 0};
    Refused(fixture.Generate(), "TERRAIN_NATIVE_COORDINATE_DOMAIN");
    fixture.grid.header.offset = {0, -128.5f};
    Refused(fixture.Generate(), "TERRAIN_NATIVE_COORDINATE_DOMAIN");
}

void StaticSuppressionIncludesBlightAndBoundaries() {
    Fixture fixture(5, 3); // 128格，逐格覆盖内部selector2的全部七位组合。
    for (auto& vertex : fixture.grid.vertices) vertex.textureAndFlagsRaw = 0xa0;
    fixture.objects.resize(128);
    for (unsigned attributes = 0; attributes < fixture.objects.size(); ++attributes)
        fixture.objects[attributes] = static_cast<std::uint8_t>(attributes);
    const auto generated = Take(fixture.Generate());
    constexpr std::array<unsigned, 6> internal{1, 2, 4, 8, 16, 32};
    constexpr std::array<unsigned, 6> serialized{2, 4, 8, 32, 64, 128};
    for (unsigned attributes = 0; attributes < fixture.objects.size(); ++attributes) {
        unsigned expected = 0;
        for (std::size_t property = 0; property < internal.size(); ++property)
            if ((attributes & internal[property]) || (attributes < 64)) expected += serialized[property];
        Require(std::to_integer<unsigned>(generated[attributes]) == expected, "static layer merges six attributes and suppresses the entire terrain layer");
    }
    Require(generated[64] == std::byte{0} && generated[65] == std::byte{2} && generated[127] == std::byte{0xee},
        "static suppression must not leave terrain blight or boundaries behind");
}

void InvalidPaletteSlotAndZeroSentinelAreDistinct() {
    Fixture missing;
    for (auto& vertex : missing.grid.vertices) vertex.textureAndFlagsRaw = 3;
    Uniform(missing, 0xce, "out-of-range palette slot blocks all remaining modes");
    missing.grid.header.groundTilesets = {std::string(4, '\0')};
    missing.ground.clear();
    for (auto& vertex : missing.grid.vertices) vertex.textureAndFlagsRaw = 0;
    Uniform(missing, 0x40, "zero rawcode is a native material sentinel, not a missing positive record");
    missing.grid.header.groundTilesets = {"Tg01"};
    Refused(missing.Generate(), "TERRAIN_NATIVE_GROUND_RULE_MISSING");
}

void ValidationIsBoundedAndRejectsUnknownRules() {
    Fixture fixture;
    Require(Take(fixture.Generate(16)).size() == 16, "exact output budget succeeds without a second map-sized grid");
    Refused(fixture.Generate(15), "TERRAIN_NATIVE_BUDGET");
    Refused(fixture.Generate(0), "TERRAIN_NATIVE_BUDGET");
    Refused(GenerateTerrainNativePathing({}), "TERRAIN_NATIVE_INPUT");

    auto bad = fixture;
    bad.grid.header.width = std::numeric_limits<std::uint32_t>::max();
    Refused(bad.Generate(), "TERRAIN_NATIVE_DIMENSIONS");
    bad = fixture; bad.grid.vertices.pop_back();
    Refused(bad.Generate(), "TERRAIN_NATIVE_DIMENSIONS");
    bad = fixture; bad.grid.header.version = 12;
    Refused(bad.Generate(), "TERRAIN_NATIVE_VERSION");
    bad = fixture; bad.grid.vertices[1].textureAndFlagsRaw = 0x10;
    Refused(bad.Generate(), "TERRAIN_NATIVE_RAMP_UNSUPPORTED", "(1,0)");
    bad = fixture; bad.grid.vertices[2].waterHeightAndFlagsRaw |= 0x8000;
    Refused(bad.Generate(), "TERRAIN_NATIVE_VERTEX_FLAGS", "(0,1)");
    bad = fixture; bad.grid.vertices[3].groundHeightRaw |= 0x4000;
    Refused(bad.Generate(), "TERRAIN_NATIVE_VERTEX_FLAGS", "(1,1)");
    bad = fixture; bad.specialCount = 20;
    Refused(bad.Generate(), "TERRAIN_NATIVE_SPECIAL_UNSUPPORTED", "20");
    bad = fixture; bad.objects = {0};
    Refused(bad.Generate(), "TERRAIN_NATIVE_STATIC_DIMENSIONS");
    bad = fixture; bad.objects.resize(16); bad.objects[6] = 0x80;
    Refused(bad.Generate(), "TERRAIN_NATIVE_STATIC_FLAGS", "(2,1)");
    bad = fixture; bad.Wet(8193); bad.water.reset();
    Refused(bad.Generate(), "TERRAIN_NATIVE_WATER_RULE_MISSING");
    bad = fixture; bad.Wet(8193); bad.water->deepLevelPathing = -1;
    Refused(bad.Generate(), "TERRAIN_NATIVE_WATER_RULE");
    bad = fixture; bad.Wet(8193); bad.water->height = std::numeric_limits<float>::infinity();
    Refused(bad.Generate(), "TERRAIN_NATIVE_WATER_RULE");
    bad = fixture; bad.bounds.right = std::numeric_limits<double>::quiet_NaN();
    Refused(bad.Generate(), "TERRAIN_NATIVE_BOUNDS");
    bad = fixture; bad.bounds.right = bad.bounds.left;
    Refused(bad.Generate(), "TERRAIN_NATIVE_BOUNDS");
    bad = fixture; bad.grid.header.offset[1] = std::numeric_limits<float>::infinity();
    Refused(bad.Generate(), "TERRAIN_NATIVE_BOUNDS");
    bad = fixture; bad.ground.push_back(bad.ground.front());
    Refused(bad.Generate(), "TERRAIN_NATIVE_GROUND_AMBIGUOUS");
    bad = fixture; bad.grid.header.groundTilesets[0] = "abc";
    Refused(bad.Generate(), "TERRAIN_NATIVE_GROUND_RAWCODE");
    bad = fixture; bad.grid.header.groundTilesets[0][0] = static_cast<char>(0x80);
    Refused(bad.Generate(), "TERRAIN_NATIVE_GROUND_RAWCODE");
}
} // namespace

int main() {
    try {
        SixModesUseTheirDistinctRules();
        NearestVertexDiffersFromTileOwner();
        StrictWaterRequiresAllFourCorners();
        SharedWaterClassifierKeepsNativePrecision();
        OrdinaryCliffsUseLevelsAndRawGroundPalette();
        PlayableBoundsAreWorldCoordinates();
        BoundsRetainNativeFloatLandingsAndReverseProjection();
        BoundsRejectUnprovenPrecisionAndPackedCoordinates();
        StaticSuppressionIncludesBlightAndBoundaries();
        InvalidPaletteSlotAndZeroSentinelAreDistinct();
        ValidationIsBoundedAndRejectsUnknownRules();
        std::cout << "TerrainNativePathingTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
