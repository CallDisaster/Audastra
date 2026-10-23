#include "War3MapCore/MapData/TerrainSpecialDoodadPathing.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string text;
        for (const auto& d : result.diagnostics) text += d.code + ": " + d.message + "\n";
        throw std::runtime_error(text);
    }
    return std::move(*result.value);
}
template<class T> void Refused(const Result<T>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& d) { return d.code == code; }), "refusal must identify its precise cause without partial output");
}
TerrainGridSnapshot Terrain() {
    TerrainGridSnapshot grid;
    grid.header.width = grid.header.height = 5; grid.header.groundTilesets = {"Tg01"}; grid.vertices.resize(25);
    for (auto& vertex : grid.vertices) { vertex.groundHeightRaw = 8192; vertex.cliffAndLayerRaw = 2; }
    return grid;
}
SpecialDoodadPlacement Placement(int x = 1, int y = 1) {
    SpecialDoodadPlacement p; p.rawcode = "Sp00"; p.position = {x, y}; return p;
}
using Pixel = std::array<unsigned, 4>;
Bytes Texture(const std::array<Pixel, 16>& pixels, bool top = true, bool alpha = true) {
    Bytes bytes(18); bytes[2] = std::byte{2}; bytes[12] = bytes[14] = std::byte{4};
    bytes[16] = alpha ? std::byte{32} : std::byte{24};
    bytes[17] = static_cast<std::byte>((top ? 32 : 0) | (alpha ? 8 : 0));
    for (unsigned row = 0; row < 4; ++row) for (unsigned x = 0; x < 4; ++x) {
        const auto& pixel = pixels[(top ? row : 3 - row) * 4 + x];
        for (unsigned c = 0; c < (alpha ? 4u : 3u); ++c) bytes.push_back(static_cast<std::byte>(pixel[c]));
    }
    return bytes;
}
void ChannelsAndOriginsHaveLiteralExpectations() {
    const std::array<Pixel, 16> pixels{{
        {0,0,255,0}, {0,255,0,0}, {255,0,0,0}, {255,255,255,0},
        {0,0,254,0}, {0,254,0,0}, {254,0,0,0}, {0,0,0,0},
        {0,0,1,0}, {0,1,0,0}, {1,0,0,0}, {127,127,127,0},
        {255,0,255,0}, {255,255,0,0}, {0,255,255,0}, {0,0,0,0}}};
    constexpr std::array<std::uint8_t, 16> masks{0xcb,0x0d,0xc7,1, 1,1,1,1, 1,1,1,1, 0xc3,5,9,0xcf};
    const auto terrain = Terrain(); const auto placed = Placement();
    for (const bool top : {false, true}) for (const bool alpha : {false, true}) {
        const auto bytes = Texture(pixels, top, alpha); const auto original = bytes;
        const TerrainSpecialDoodadPathingInput item{&placed, Take(DecodeDoodadFootprintTga(bytes))};
        const auto layer = Take(BuildTerrainSpecialDoodadPathing(terrain, terrain, std::span(&item, 1), 14, 4096));
        std::vector<std::uint8_t> expected(256);
        for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) expected[(4+y)*16+4+x] = masks[y*4+x];
        Require(layer == expected && bytes == original, "all special pixels and untouched cells match literal native channel/origin rules");
        TerrainNativeGroundRule rule{"Tg01", false, false, false};
        TerrainNativePathingInput input; input.terrain = &terrain; input.playableBounds = {0,0,512,512};
        input.groundRules = std::span(&rule, 1); input.specialDoodadCount = 1; input.specialAttributes = layer;
        const auto actual = Take(GenerateTerrainNativePathing(input));
        Bytes native(256, std::byte{0x4e});
        for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
            native[(4+y)*16+4+x] = static_cast<std::byte>(masks[y*4+x] & 0xce);
        Require(actual == native, "special pixels replace ordinary ground and sea/shore rules, without pretending to be static objects");
    }
}
void FirstRegistrationWinsAndWaterStillApplies() {
    const auto black = Texture({});
    std::array<Pixel, 16> whitePixels; whitePixels.fill({255,255,255,0});
    const auto white = Texture(whitePixels);
    const auto a = Placement(), b = Placement(1,1);
    std::array<TerrainSpecialDoodadPathingInput, 2> records{{{&a, Take(DecodeDoodadFootprintTga(black))},
        {&b, Take(DecodeDoodadFootprintTga(white))}}};
    auto terrain = Terrain();
    const auto first = Take(BuildTerrainSpecialDoodadPathing(terrain, terrain, records, 14, 4096));
    Require(first[4*16+4] == 1, "black first pixel is still a covering record");
    std::swap(records[0], records[1]);
    const auto reverse = Take(BuildTerrainSpecialDoodadPathing(terrain, terrain, records, 14, 4096));
    Require(reverse[4*16+4] == 0xcf, "first record, not OR union or sorted rawcode, wins overlaps");
    TerrainNativeGroundRule rule{"Tg01", true, true, true};
    TerrainNativePathingInput input; input.terrain = &terrain; input.playableBounds = {0,0,512,512};
    input.groundRules = std::span(&rule, 1); input.specialDoodadCount = 2; input.specialAttributes = first;
    input.water = TerrainNativeWaterRules{0,64,false};
    for (const auto& [depth, expected] : std::array<std::pair<unsigned, unsigned>, 3>{{{0,0},{32,8},{80,10}}}) {
        for (auto& vertex : terrain.vertices) {
            vertex.textureAndFlagsRaw = depth ? 0x40 : 0;
            vertex.waterHeightAndFlagsRaw = static_cast<std::uint16_t>(8192+depth*4);
        }
        Require(std::to_integer<unsigned>(Take(GenerateTerrainNativePathing(input))[4*16+4]) == expected,
            "ordinary water veto occurs before special channel query");
    }
    input.water->impassable = true;
    Require(Take(GenerateTerrainNativePathing(input))[4*16+4] == std::byte{0xca}, "impassable water still vetoes special pixels except flight");
    terrain.vertices[6].textureAndFlagsRaw |= 0x80;
    Require(Take(GenerateTerrainNativePathing(input))[4*16+4] == std::byte{0xce}, "boundary veto occurs before special pixel query");
    input.specialDoodadCount = 0;
    Refused(GenerateTerrainNativePathing(input), "TERRAIN_NATIVE_SPECIAL_UNSUPPORTED");
    input.specialDoodadCount = 2; auto invalid = first; invalid[0] = 2; input.specialAttributes = invalid;
    Refused(GenerateTerrainNativePathing(input), "TERRAIN_NATIVE_SPECIAL_FLAGS");
}
void AlphaShapesAndFailureBoundaries() {
    auto before = Terrain(), after = before;
    std::array<Pixel, 16> pixels{};
    pixels[0][3] = pixels[15][3] = 127; pixels[3][3] = 255;
    before.vertices[7].cliffAndLayerRaw = before.vertices[11].cliffAndLayerRaw = 3;
    before.vertices[12].cliffAndLayerRaw = 4; after = before;
    auto bytes = Texture(pixels); auto placed = Placement();
    TerrainSpecialDoodadPathingInput input{&placed, Take(DecodeDoodadFootprintTga(bytes))};
    Take(BuildTerrainSpecialDoodadPathing(before, after, std::span(&input,1), 14, 256));
    Refused(BuildTerrainSpecialDoodadPathing(before, after, std::span(&input,1), 14, 255), "TERRAIN_SPECIAL_BUDGET");
    Refused(BuildTerrainSpecialDoodadPathing(before, after, std::span(&input,1), 3, 4096), "TERRAIN_SPECIAL_HEIGHT_LIMIT");
    after.vertices[12].cliffAndLayerRaw = 3;
    Refused(BuildTerrainSpecialDoodadPathing(before, after, std::span(&input,1), 14, 4096), "TERRAIN_SPECIAL_CLIFF_CHANGED");
    after = before;
    for (const auto alpha : {11u,119u,131u,249u}) {
        auto bad = bytes; bad[21] = static_cast<std::byte>(alpha);
        input.footprint = Take(DecodeDoodadFootprintTga(bad));
        Refused(BuildTerrainSpecialDoodadPathing(before, after, std::span(&input,1), 14, 4096), "TERRAIN_SPECIAL_ALPHA");
    }
    input.footprint = Take(DecodeDoodadFootprintTga(bytes));
    placed.position = {4,1};
    Refused(BuildTerrainSpecialDoodadPathing(before, after, std::span(&input,1), 14, 4096), "TERRAIN_SPECIAL_BOUNDS");
    placed.position = {-1,1};
    Refused(BuildTerrainSpecialDoodadPathing(before, after, std::span(&input,1), 14, 4096), "TERRAIN_SPECIAL_LAYOUT");
    placed = Placement(); placed.variation = 1;
    Refused(BuildTerrainSpecialDoodadPathing(before, after, std::span(&input,1), 14, 4096), "TERRAIN_SPECIAL_LAYOUT");
    placed = Placement(); input.footprint->width = 3;
    Refused(BuildTerrainSpecialDoodadPathing(before, after, std::span(&input,1), 14, 4096), "TERRAIN_SPECIAL_LAYOUT");
    input.footprint.reset();
    const auto zero = Take(BuildTerrainSpecialDoodadPathing(before, after, std::span(&input,1), 14, 4096));
    Require(zero == std::vector<std::uint8_t>(256,0), "actual disabled path contributes no special coverage");
}
} // namespace

int main() {
    try {
        ChannelsAndOriginsHaveLiteralExpectations(); FirstRegistrationWinsAndWaterStillApplies(); AlphaShapesAndFailureBoundaries();
        std::cout << "Special doodad pathing passed independent matrices, ordering, water and alpha constraints\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
