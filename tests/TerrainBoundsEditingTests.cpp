#include "War3MapCore/MapData/TerrainBoundsEditing.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/NativeMinimapMarkers.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "unexpected refusal";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
template<class T> void Refused(const Result<T>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value, "refused operation cannot return a partial value");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; }), "refusal names its reason");
}

struct Fixture {
    TerrainGridSnapshot terrain;
    MapSettings settings;
    std::array<float, 4> margins{1, 2, 3, 1};
    TerrainBoundsEditIntent intent{{1, 2, 9, 7}};
    Fixture() {
        terrain.header.width = 11; terrain.header.height = 9;
        terrain.header.offset = {-640, -512};
        terrain.header.groundTilesets = {"Tg01"};
        terrain.header.cliffTilesets = {"Cl01"};
        terrain.vertices.resize(99);
        for (std::size_t i = 0; i < terrain.vertices.size(); ++i) {
            auto& vertex = terrain.vertices[i];
            vertex.groundHeightRaw = static_cast<std::uint16_t>(8192 + i);
            vertex.waterHeightAndFlagsRaw = static_cast<std::uint16_t>(0xa000 | (i % 2 ? 0x4000 : 0));
            vertex.textureAndFlagsRaw = static_cast<std::uint8_t>(i % 2 ? 0xc0 : 0x40);
            vertex.variationRaw = static_cast<std::uint8_t>(i);
            vertex.cliffAndLayerRaw = 2;
        }
        settings.formatVersion = 25; settings.custom.emplace();
        settings.mapName = "synthetic bounds"; settings.author = "example";
        settings.description = "unchanged"; settings.recommendedPlayers = "one";
        settings.flags = 0x20000120;
        settings.cameraBounds = {-512, -384, 384, 384, -512, 384, 384, -384};
        settings.cameraMargins = {0, 0, 0, 0};
        settings.playableWidth = 10; settings.playableHeight = 8;
    }
};

void ProjectionPreservesEverythingExceptTypedGeometry() {
    Fixture f;
    const auto original = Take(f.settings.Serialize());
    auto output = Take(ProjectTerrainPlayableBounds(f.terrain, f.settings, f.margins, f.intent));
    Require(output.cameraBounds == std::array<float, 8>{-384, 128, 256, 256, -384, 256, 256, 128},
        "non-square asymmetric margins use SW NE NW SE camera order");
    Require(output.cameraMargins == std::array<std::int32_t, 4>{1, 1, 2, 1}, "W3I border order is left right bottom top");
    Require(output.playableWidth == 8 && output.playableHeight == 5, "playable size measures tiles");
    Require(output.flags == 0x20000520, "only the supplement-border flag is added");
    Require(Take(NativeMinimapPlayableRectangle(f.terrain, output)) == std::array<std::uint32_t, 4>{1, 2, 9, 7},
        "minimap uses the same half-open playable crop");
    const auto native = Take(ComputeTerrainNativePlayableBounds(f.terrain.header, output, f.margins));
    Require(native.left == -512 && native.bottom == -256 && native.right == 512 && native.top == 384,
        "native pathing expands camera using actual asymmetric margins");
    output.cameraBounds = f.settings.cameraBounds; output.cameraMargins = f.settings.cameraMargins;
    output.playableWidth = f.settings.playableWidth; output.playableHeight = f.settings.playableHeight;
    output.flags = f.settings.flags;
    Require(Take(output.Serialize()) == original, "all other W3I fields and bytes remain identical");
    Require(Take(f.settings.Serialize()) == original, "projection does not mutate its input");
}

void EdgesHaveAnIndependentByteOracle() {
    Fixture f;
    const auto before = Take(SerializeMapTerrain(f.terrain));
    auto expected = before;
    // W3E header: magic/version/tileset/custom, two one-entry tables, dimensions/offset.
    constexpr std::size_t records = 45;
    std::size_t expectedChanges = 0;
    for (std::uint32_t y = 0; y < 8; ++y) for (std::uint32_t x = 0; x < 10; ++x) {
        const auto index = static_cast<std::size_t>(y) * 11 + x;
        const auto highByte = records + index * 7 + 3;
        const auto old = std::to_integer<unsigned>(expected[highByte]);
        const auto next = (old & ~0x40u) | ((x < 1 || x >= 9 || y < 2 || y >= 7) ? 0x40u : 0u);
        expectedChanges += old != next;
        expected[highByte] = static_cast<std::byte>(next);
    }
    Require(Take(ApplyTerrainPlayableEdges(f.terrain, f.intent.playable, 80)) == expectedChanges,
        "changed count comes from actual bit differences");
    Require(Take(SerializeMapTerrain(f.terrain)) == expected,
        "only SW map-edge bits change: terminal row/column, boundary, water and unknown bits survive");
    Require(Take(ApplyTerrainPlayableEdges(f.terrain, f.intent.playable, 80)) == 0, "repeated same bounds are a no-op");
}

void InvalidAndBudgetInputsAreAtomic() {
    Fixture f;
    const auto before = Take(SerializeMapTerrain(f.terrain));
    Refused(ApplyTerrainPlayableEdges(f.terrain, f.intent.playable, 79), "TERRAIN_BOUNDS_BUDGET");
    Require(Take(SerializeMapTerrain(f.terrain)) == before, "budget failure leaves every byte intact");
    Refused(ApplyTerrainPlayableEdges(f.terrain, {0, 0, 11, 8}), "TERRAIN_BOUNDS_LAYOUT");
    f.terrain.vertices.back().extendedTextureLayout = true;
    Refused(ApplyTerrainPlayableEdges(f.terrain, f.intent.playable), "TERRAIN_BOUNDS_LAYOUT");
    f.terrain.vertices.back().extendedTextureLayout = false;
    Require(Take(SerializeMapTerrain(f.terrain)) == before, "layout failure cannot leave a modified prefix");
    const auto wireSize = Take(f.settings.Serialize()).size();
    Require(ProjectTerrainPlayableBounds(f.terrain, f.settings, f.margins, f.intent, wireSize).Succeeded(),
        "exact setting byte budget succeeds");
    Refused(ProjectTerrainPlayableBounds(f.terrain, f.settings, f.margins, f.intent, wireSize - 1), "TERRAIN_BOUNDS_BUDGET");
    Refused(ProjectTerrainPlayableBounds(f.terrain, f.settings, f.margins, {{1, 2, 4, 7}}), "TERRAIN_BOUNDS_CAMERA_EMPTY");
    f.intent.algorithm = 2;
    Refused(ProjectTerrainPlayableBounds(f.terrain, f.settings, f.margins, f.intent), "TERRAIN_BOUNDS_LAYOUT");
    f.intent.algorithm = 1;
    f.margins[0] = 0.5f;
    Refused(ProjectTerrainPlayableBounds(f.terrain, f.settings, f.margins, f.intent), "TERRAIN_BOUNDS_CAMERA_CONFIG");
    f.margins[0] = std::numeric_limits<float>::quiet_NaN();
    Refused(ProjectTerrainPlayableBounds(f.terrain, f.settings, f.margins, f.intent), "TERRAIN_BOUNDS_CAMERA_CONFIG");
    f.margins[0] = 1;
    f.terrain.header.offset[0] = -639;
    Refused(ProjectTerrainPlayableBounds(f.terrain, f.settings, f.margins, f.intent), "TERRAIN_BOUNDS_COORDINATE");
}

void CustomCameraKeepsFloatWorldCoordinates() {
    Fixture f;
    f.settings.cameraBounds = {-383.75f, 128.5f, 256.25f, 255.75f, -383.75f, 255.75f, 256.25f, 128.5f};
    const auto bounds = Take(ComputeTerrainNativePlayableBounds(f.terrain.header, f.settings, f.margins));
    Require(bounds.left == -511.75 && bounds.bottom == -255.5 && bounds.right == 512.25 && bounds.top == 383.75,
        "custom camera expansion preserves sub-tile binary32 values");
    f.settings.cameraBounds[2] = std::numeric_limits<float>::infinity();
    Refused(ComputeTerrainNativePlayableBounds(f.terrain.header, f.settings, f.margins), "TERRAIN_NATIVE_PLAYABLE_BOUNDS");
}
}

int main() {
    try {
        ProjectionPreservesEverythingExceptTypedGeometry();
        EdgesHaveAnIndependentByteOracle();
        InvalidAndBudgetInputsAreAtomic();
        CustomCameraKeepsFloatWorldCoordinates();
        std::cout << "terrain bounds editing tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n'; return 1;
    }
}
