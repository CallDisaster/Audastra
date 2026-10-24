#include "War3MapCore/MapData/TerrainNativePathing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

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
    std::optional<std::uint8_t> maximumCliffLevel;

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
        auto input = TerrainNativePathingInput{&grid, bounds, ground, water, objects, specialCount};
        input.maximumCliffLevel = maximumCliffLevel;
        return input;
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

void DryRampCornerRulesHaveLiteralOracles() {
    Fixture fixture(5, 5);
    const auto set = [&](int x, int y, int level, bool ramp = true) {
        auto& vertex = fixture.grid.vertices[static_cast<std::size_t>(y) * 5 + x];
        vertex.cliffAndLayerRaw = static_cast<std::uint8_t>(level);
        vertex.textureAndFlagsRaw = ramp ? 0x10 : 0;
    };
    const auto center = [&] { return Take(ClassifyNativeDryRampCorners(fixture.grid, 14))[12]; };
    set(1, 2, 2); set(2, 2, 2); set(3, 2, 3);
    Require(center() == 'R', "one differing ramp axis produces R");
    set(2, 1, 2); set(2, 3, 3);
    Require(center() == 'S', "two differing axes produce S");
    set(1, 1, 4, false);
    Require(center() == '!', "non-ramp diagonal belongs to rangeAll and rejects an otherwise valid S");
    set(1, 1, 2, false); set(3, 2, 4);
    Require(center() == '!', "ramp range above one rejects before axis classification");
    set(2, 2, 4);
    Require(center() == 'H', "High takes precedence over a low-end range rejection");
    set(2, 2, 4, false);
    Require(center() == 'B', "non-ramp vertex still receives the original High label B");
    set(2, 2, 1, false);
    Require(center() == 'A', "non-ramp low point is A");

    Fixture all(3, 3);
    for (auto& vertex : all.grid.vertices) vertex.textureAndFlagsRaw = 0x10;
    const std::vector<char> ordinary{'L','L','L', 'L','!','L', 'L','L','L'};
    Require(Take(ClassifyNativeDryRampCorners(all.grid, 14)) == ordinary,
        "all-nine equal ramps reject only the interior; missing edge neighbors are not fabricated");
    Require(Take(ClassifyNativeDryRampCorners(all.grid, 1)) == std::vector<char>(9, 'H'),
        "actual M below uniform ramp levels changes High; minimum cannot start at the first sample");
    all.grid.vertices[8].cliffAndLayerRaw = 3;
    Require(Take(ClassifyNativeDryRampCorners(all.grid, 14))[4] == 'S',
        "all-nine ramps with only diagonal variation and no axis diff produce S");
    Refused(ClassifyNativeDryRampCorners(all.grid, 15), "TERRAIN_NATIVE_RAMP_RULE_MISSING");
    Refused(ClassifyNativeDryRampCorners(all.grid, 14, 8), "TERRAIN_NATIVE_BUDGET");

    Fixture edge(3, 3);
    edge.grid.vertices[0].textureAndFlagsRaw = 0x10;
    edge.grid.vertices[3].textureAndFlagsRaw = 0x10;
    edge.grid.vertices[6].textureAndFlagsRaw = 0x10; edge.grid.vertices[6].cliffAndLayerRaw = 3;
    Require(Take(ClassifyNativeDryRampCorners(edge.grid, 14))[3] == 'R',
        "at x boundary the valid NS pair works while the missing EW endpoint remains missing");

    Fixture halo(5, 5);
    for (const auto index : {5, 6, 7}) halo.grid.vertices[index].textureAndFlagsRaw = 0x10;
    halo.grid.vertices[7].cliffAndLayerRaw = 3;
    Require(Take(ClassifyNativeDryRampCorners(halo.grid, 14))[6] == 'R', "corner before outside-cell halo change");
    halo.grid.vertices[0].cliffAndLayerRaw = 4;
    Require(Take(ClassifyNativeDryRampCorners(halo.grid, 14))[6] == '!',
        "q-minus-one diagonal halo influences the SW classification, even outside the queried cell");
    halo.grid.vertices[0].cliffAndLayerRaw = 2;
    halo.grid.vertices[12].cliffAndLayerRaw = 4;
    Require(Take(ClassifyNativeDryRampCorners(halo.grid, 14))[6] == '!',
        "q-plus-two diagonal halo influences the NE classification of cell(0,0)");
}

void DryRampCellsUseCornerWeightsAndExistingLayers() {
    Fixture fixture(5, 5); fixture.maximumCliffLevel = 14;
    for (std::size_t y : {1u, 2u}) for (std::size_t x = 0; x < 5; ++x) {
        auto& vertex = fixture.grid.vertices[y * 5 + x];
        vertex.textureAndFlagsRaw = 0x10; vertex.cliffAndLayerRaw = x < 2 ? 2 : 3;
    }
    const std::string labels = "AAAAA" "LRHLL" "LRHLL" "AAAAA" "AAAAA";
    Require(Take(ClassifyNativeDryRampCorners(fixture.grid, 14)) == std::vector<char>(labels.begin(), labels.end()),
        "asymmetric literal rows fix output order and full-neighborhood classification");
    const auto generated = Take(fixture.Generate());
    // 每项是整个4x4通行地格的字面六mode预期；R单角阻断、双角放行，
    // 无R/S的层级差仍阻断。此矩阵不是由产品分类计算。
    constexpr std::array<unsigned, 16> expected{
        0xca,0xca,0xca,0xca, 0x40,0x40,0x40,0x40,
        0xca,0xca,0xca,0xca, 0x40,0x40,0x40,0x40};
    for (std::size_t y = 0; y < 16; ++y) for (std::size_t x = 0; x < 16; ++x)
        Require(generated[y * 16 + x] == static_cast<std::byte>(expected[(y / 4) * 4 + x / 4]),
            "dry ramp full WPM has independent literal cell masks");
    const auto required = generated.size() + fixture.grid.vertices.size();
    Require(Take(fixture.Generate(required)) == generated, "exact output-plus-label budget succeeds");
    Refused(fixture.Generate(required - 1), "TERRAIN_NATIVE_BUDGET");
    auto input = fixture.Input(); std::vector<std::uint8_t> special(generated.size()); special[0] = 1;
    input.specialDoodadCount = 1; input.specialAttributes = special;
    auto layered = Take(GenerateTerrainNativePathing(input));
    Require(layered[0] == std::byte{0} && layered[1] == std::byte{0xca},
        "complete special layer overrides ramp C only where covered");
    fixture.objects.assign(generated.size(), 0); fixture.objects[1] = 0x41;
    layered = Take(fixture.Generate());
    Require(layered[1] == std::byte{2} && layered[0] == std::byte{0xca},
        "ordinary static suppression and selector2 attributes still compose with ramps");
    fixture.maximumCliffLevel.reset();
    Refused(fixture.Generate(), "TERRAIN_NATIVE_RAMP_RULE_MISSING");
    fixture.maximumCliffLevel = 14; fixture.grid.vertices.back().textureAndFlagsRaw |= 0x40;
    fixture.objects.clear(); fixture.water = TerrainNativeWaterRules{0, 64, false};
    const auto mixed = Take(fixture.Generate());
    for (std::size_t y = 0; y < 16; ++y) for (std::size_t x = 0; x < 16; ++x) {
        const auto mask = x >= 14 && y >= 14 ? 0x48u : expected[(y / 4) * 4 + x / 4];
        Require(mixed[y * 16 + x] == static_cast<std::byte>(mask),
            "remote water uses its own nearest vertex without rejecting the complete dry ramp map");
    }
    Refused(ClassifyNativeDryRampCorners(fixture.grid, 14), "TERRAIN_NATIVE_WATER_RAMP_UNSUPPORTED");

    Fixture singleS(5, 5); singleS.maximumCliffLevel = 14;
    for (const auto index : {7, 11, 12, 13, 17}) singleS.grid.vertices[index].textureAndFlagsRaw = 0x10;
    singleS.grid.vertices[13].cliffAndLayerRaw = singleS.grid.vertices[17].cliffAndLayerRaw = 3;
    const auto s = Take(singleS.Generate());
    Require(s[8 * 16 + 8] == std::byte{0x40}, "one S corner has weight two and opens its differing-level cell");
}

void WetRampOffsetsAndStrictWaterHaveLiteralOracles() {
    const auto checkThresholds = [](Fixture& fixture, std::size_t vertex, int offset, std::size_t pixel) {
        fixture.maximumCliffLevel = 14; fixture.water = TerrainNativeWaterRules{0, 64, false};
        auto& wet = fixture.grid.vertices[vertex]; wet.textureAndFlagsRaw |= 0x40;
        // 单个湿角不构成全格strict覆盖。64世界单位是实际deep阈值，比较包含等号。
        for (const auto [quarterHeight, mask] : std::array<std::pair<int, unsigned>, 4>{
            std::pair{offset * 4 - 1, 0x40u}, {offset * 4, 0x48u},
            {(offset + 64) * 4 - 1, 0x48u}, {(offset + 64) * 4, 0x0au}}) {
            wet.waterHeightAndFlagsRaw = static_cast<std::uint16_t>(8192 + quarterHeight);
            Require(Take(fixture.Generate())[pixel] == static_cast<std::byte>(mask),
                "literal effective ramp offset preserves dry, equality-shallow and equality-deep thresholds");
        }
        fixture.water->height = 0.25f;
        wet.waterHeightAndFlagsRaw = static_cast<std::uint16_t>(8192 + (offset - 32) * 4);
        Require(Take(fixture.Generate())[pixel] == std::byte{0x48},
            "actual water layer height is added after raw water decoding and compared with the ramp ground");
    };
    Fixture r(5, 5);
    for (std::size_t y : {1u, 2u}) for (std::size_t x = 0; x < 5; ++x) {
        auto& vertex = r.grid.vertices[y * 5 + x];
        vertex.textureAndFlagsRaw = 0x10; vertex.cliffAndLayerRaw = x < 2 ? 2 : 3;
    }
    checkThresholds(r, 6, 64, 4 * 16 + 4);
    const auto queriedR = Take(QueryNativeTerrainRamp(r.grid, 14, 1, 1));
    Require(queriedR.corner == 'R' && queriedR.heightOffset == 64 && queriedR.belongsToRampCell,
        "bounded live R query has the independent64-unit offset and a containing ramp cell");
    Require(Take(r.Generate(281)) == Take(r.Generate()), "wet ramp output and full labels fit the exact combined budget");
    Refused(r.Generate(280), "TERRAIN_NATIVE_BUDGET");
    for (const int offset : {32, 64, 96}) {
        Fixture s(5, 5);
        for (std::size_t y = 1; y <= 3; ++y) for (std::size_t x = 1; x <= 3; ++x) {
            auto& vertex = s.grid.vertices[y * 5 + x]; vertex.textureAndFlagsRaw = 0x10;
            if ((offset == 32 && x == 3 && y == 3)
                || (offset == 64 && ((x == 1 && y == 1) || (x == 3 && y == 3)))
                || (offset == 96 && (x == 3 || y == 3))) vertex.cliffAndLayerRaw = 3;
        }
        const std::string literal = offset == 32 ? "AAAAA" "ALLLA" "ALSRA" "ALRHA" "AAAAA"
            : offset == 64 ? "AAAAA" "AHRLA" "ARSRA" "ALRHA" "AAAAA"
            : "AAAAA" "ALRHA" "ARSHA" "AHHHA" "AAAAA";
        Require(Take(ClassifyNativeDryRampCorners(s.grid, 14)) == std::vector<char>(literal.begin(), literal.end()),
            "independent complete corner labels distinguish the three S height cases");
        const auto queried = Take(QueryNativeTerrainRamp(s.grid, 14, 2, 2));
        Require(queried.corner == 'S' && queried.heightOffset == offset && queried.belongsToRampCell,
            "bounded S query matches independently stated32/64/96 height and cell membership");
        checkThresholds(s, 12, offset, 8 * 16 + 8);
        if (offset == 32) {
            s.water = TerrainNativeWaterRules{0, 200, false};
            for (auto& vertex : s.grid.vertices) {
                vertex.textureAndFlagsRaw |= 0x40; vertex.waterHeightAndFlagsRaw = 8192 + 128 * 4;
            }
            Require(Take(s.Generate())[8 * 16 + 8] == std::byte{0x48},
                "H at exactly its water surface is shallow but prevents all-four strict coverage");
            ++s.grid.vertices[18].waterHeightAndFlagsRaw;
            Require(Take(s.Generate())[8 * 16 + 8] == std::byte{0x08},
                "one raw quarter over H completes strict coverage using the same R/S/H ground rules");
            s.water = TerrainNativeWaterRules{0, 64, false};
            for (auto& vertex : s.grid.vertices) vertex.textureAndFlagsRaw &= ~0x40;
            s.grid.vertices[12].textureAndFlagsRaw |= 0x40;
            s.grid.vertices[12].waterHeightAndFlagsRaw = 8192 + 64 * 4;
            Require(Take(s.Generate())[8 * 16 + 8] == std::byte{0x48},
                "unmodified full snapshot gives center S the quarter-layer offset");
            const auto outer = s.grid.vertices[5];
            s.grid.vertices[5].textureAndFlagsRaw |= 0x10;
            s.grid.vertices[5].cliffAndLayerRaw = 3;
            Require(Take(QueryNativeTerrainRamp(s.grid, 14, 2, 2)).heightOffset == 0,
                "bounded point query reads second-halo corner inputs from the current revision");
            Require(Take(s.Generate())[8 * 16 + 8] == std::byte{0x0a},
                "a second-halo change alters a corner classification and invalidates S height without changing the center raw nine");
            s.grid.vertices[5] = outer;
            s.grid.vertices[0].waterHeightAndFlagsRaw |= 0x8000;
            Refused(QueryNativeTerrainRamp(s.grid, 14, 2, 2), "TERRAIN_NATIVE_VERTEX_FLAGS");
            s.grid.vertices[0].waterHeightAndFlagsRaw &= 0x7fff;
            Require(Take(s.Generate())[8 * 16 + 8] == std::byte{0x48},
                "restored snapshot rebuilds its own labels without leaking the prior revision's S subtype");
        }
    }
    Fixture invalid(5, 5);
    for (auto& vertex : invalid.grid.vertices) vertex.textureAndFlagsRaw = 0x10;
    invalid.grid.vertices[18].cliffAndLayerRaw = 3;
    Require(Take(ClassifyNativeDryRampCorners(invalid.grid, 14))[12] == 'S',
        "all-nine ramps admit S even when surrounding corner labels do not admit a height subtype");
    checkThresholds(invalid, 12, 0, 8 * 16 + 8);

    Fixture edge(3, 3); edge.maximumCliffLevel = 14; edge.water = TerrainNativeWaterRules{0, 64, false};
    for (const auto at : {0u, 3u, 6u}) edge.grid.vertices[at].textureAndFlagsRaw = 0x10;
    edge.grid.vertices[6].cliffAndLayerRaw = 3;
    edge.grid.vertices[3].textureAndFlagsRaw |= 0x40;
    edge.grid.vertices[3].waterHeightAndFlagsRaw = 8192 + 64 * 4;
    Require(Take(edge.Generate())[4 * 8] == std::byte{0xca},
        "missing EW neighbors do not erase a valid NS R offset of64 at the map edge");
    edge.grid.vertices[3].waterHeightAndFlagsRaw = 8192 + 128 * 4;
    Require(Take(edge.Generate())[4 * 8] == std::byte{0x0a},
        "edge R still reaches deep water at exactly its effective ground plus64");
    const auto queriedEdge = Take(QueryNativeTerrainRamp(edge.grid, 14, 0, 1));
    Require(queriedEdge.corner == 'R' && queriedEdge.heightOffset == 64 && queriedEdge.belongsToRampCell,
        "bounded query retains original missing-neighbor rules at the map edge");
    Refused(QueryNativeTerrainRamp(edge.grid, 15, 0, 1), "TERRAIN_NATIVE_RAMP_RULE_MISSING");
    Refused(QueryNativeTerrainRamp(edge.grid, 14, 3, 1), "TERRAIN_NATIVE_DIMENSIONS");
}

void CliffSentinelTransitionsRequireFlatKnownNeighborhoods() {
    Fixture fixture(5, 5);
    for (auto& vertex : fixture.grid.vertices) vertex.cliffAndLayerRaw = 0xf2;
    auto changed = fixture.grid;
    constexpr std::size_t center = 2 * 5 + 2;
    for (std::size_t y = 1; y <= 3; ++y) for (std::size_t x = 1; x <= 3; ++x) {
        auto& vertex = changed.vertices[y * 5 + x];
        vertex.cliffAndLayerRaw = x == 2 && y == 2 ? 1 : 2;
        vertex.variationRaw = static_cast<std::uint8_t>(((x + y) % 3) << 5);
        Require(IsNativeTerrainCliffSentinelTransition(fixture.grid, changed, y * 5 + x)
            && IsNativeTerrainCliffSentinelTransition(changed, fixture.grid, y * 5 + x),
            "flat unselected material can be resolved and restored while the concrete side gains ordinary cliffs");
    }
    Uniform(fixture, 0x40, "flat slot15 remains dry ground in the independent native baseline");
    auto input = fixture.Input(); input.terrain = &changed;
    const auto generated = Take(GenerateTerrainNativePathing(input));
    for (std::size_t y = 0; y < 16; ++y) for (std::size_t x = 0; x < 16; ++x) {
        const bool cliffCell = x / 4 >= 1 && x / 4 <= 2 && y / 4 >= 1 && y / 4 <= 2;
        Require(generated[y * 16 + x] == (cliffCell ? std::byte{0xca} : std::byte{0x40}),
            "resolving a sentinel still generates every new cliff restriction instead of preserving dry G0");
    }
    for (const bool sentinelSide : {false, true}) for (unsigned mode = 0; mode < 11; ++mode) {
        auto before = fixture.grid, after = changed;
        auto& bad = (sentinelSide ? before : after).vertices[1 * 5 + 1];
        switch (mode) {
        case 0: bad.textureAndFlagsRaw |= 0x10; break;
        case 1: bad.textureAndFlagsRaw |= 0x80; break;
        case 2: bad.waterHeightAndFlagsRaw |= 0x4000; break;
        case 3: bad.waterHeightAndFlagsRaw |= 0x8000; break;
        case 4: bad.groundHeightRaw |= 0x4000; break;
        case 5: bad.groundHeightRaw |= 0x8000; break;
        case 6: bad.extendedTextureLayout = true; break;
        case 7: bad.textureFlagsExtensionRaw = 1; break;
        case 8: bad.variationRaw = 3 << 5; break;
        case 9: bad.cliffAndLayerRaw = 0xe2; break;
        case 10: bad.cliffAndLayerRaw = static_cast<std::uint8_t>((bad.cliffAndLayerRaw & 0xf0) | 15); break;
        }
        Require(!IsNativeTerrainCliffSentinelTransition(before, after, center)
            && !IsNativeTerrainCliffSentinelTransition(after, before, center),
            "neighborhood unknown flags, invalid slots, variations, levels and boundaries reject in both directions");
    }
    for (std::size_t y = 1; y <= 3; ++y) for (std::size_t x = 1; x <= 3; ++x) {
        auto nonflat = fixture.grid; nonflat.vertices[y * 5 + x].cliffAndLayerRaw = 0xf3;
        Require(!IsNativeTerrainCliffSentinelTransition(nonflat, changed, center)
            && !IsNativeTerrainCliffSentinelTransition(changed, nonflat, center),
            "every corner of all four sentinel-side cells must be flat, including diagonals");
    }
    for (const std::size_t edge : {0u, 2u, 10u, 14u, 22u, 24u}) {
        auto atEdge = changed; atEdge.vertices[edge].cliffAndLayerRaw = 2;
        Require(!IsNativeTerrainCliffSentinelTransition(fixture.grid, atEdge, edge),
            "an incomplete outer-edge neighborhood does not qualify");
    }
    auto invalid = changed; invalid.vertices[center].cliffAndLayerRaw = 0xe1;
    Require(!IsNativeTerrainCliffSentinelTransition(fixture.grid, invalid, center), "the concrete slot must actually exist");
    invalid = changed; invalid.vertices[center].cliffAndLayerRaw = 0xf1;
    Require(!IsNativeTerrainCliffSentinelTransition(fixture.grid, invalid, center), "slot15-to-slot15 is not material resolution");
    invalid = changed; invalid.vertices.pop_back();
    Require(!IsNativeTerrainCliffSentinelTransition(fixture.grid, invalid, center), "truncated grid is rejected before indexing");
    auto replaced = changed; replaced.header.cliffTilesets[0] = "Cl02";
    Require(IsNativeTerrainCliffSentinelTransition(fixture.grid, replaced, center)
        && IsNativeTerrainCliffSentinelTransition(replaced, fixture.grid, center),
        "same-count cliff replacement is geometrically valid; actual rows are checked by the resource owner");
    auto appended = changed; appended.header.cliffTilesets.push_back("Cl02");
    appended.vertices[center].cliffAndLayerRaw = 0x11;
    Require(IsNativeTerrainCliffSentinelTransition(fixture.grid, appended, center)
        && IsNativeTerrainCliffSentinelTransition(appended, fixture.grid, center),
        "a complete tail append may resolve slot15 to the actual newly available concrete slot");
    input.terrain = &appended;
    Require(Take(GenerateTerrainNativePathing(input)) == generated,
        "valid cliff table changes preserve every independently generated terrain pathing cell");
    for (unsigned mode = 0; mode < 4; ++mode) {
        invalid = appended;
        if (mode == 0) invalid.header.cliffTilesets.front() = "ClXX";
        if (mode == 1) invalid.header.cliffTilesets.clear();
        if (mode == 2) invalid.header.cliffTilesets.resize(16, "ClXX");
        if (mode == 3) invalid.header.cliffTilesets.pop_back();
        Require(!IsNativeTerrainCliffSentinelTransition(fixture.grid, invalid, center)
            && !IsNativeTerrainCliffSentinelTransition(invalid, fixture.grid, center),
            "non-prefix growth, empty or oversized tables and missing concrete slots reject in both directions");
    }
    Require(!IsNativeTerrainCliffSentinelTransition(fixture.grid, changed, fixture.grid.vertices.size()),
        "out-of-range changed vertex is rejected before indexing");
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
    Refused(bad.Generate(), "TERRAIN_NATIVE_RAMP_RULE_MISSING");
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
        DryRampCornerRulesHaveLiteralOracles();
        DryRampCellsUseCornerWeightsAndExistingLayers();
        WetRampOffsetsAndStrictWaterHaveLiteralOracles();
        CliffSentinelTransitionsRequireFlatKnownNeighborhoods();
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
