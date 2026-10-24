#include "War3MapCore/MapData/MapShadow.h"
#include "War3MapCore/MapData/TerrainShadowRebuild.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "operation failed";
        for (const auto& item : result.diagnostics) message += " | " + item.code + ": " + item.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}

void Refused(const Result<TerrainShadowProjection>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value, "failure must not publish a partially projected grid");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& item) { return item.code == code; }), "failure must identify its actual boundary");
}

struct Fixture {
    std::array<float, 16> heights{};
    std::array<float, 3> direction{-1, 0, 0.125f};
    TerrainShadowHeightSamples Input() const { return {4, 4, heights, direction}; }
    TerrainShadowProjection Project() const { return Take(ProjectTerrainShadow(Input())); }
};

void CardinalProjectionStopsAtEqualHeight() {
    Fixture fixture;
    for (std::size_t y = 0; y < 4; ++y) fixture.heights[y * 4] = 8;
    const std::vector<std::uint8_t> expected{0,1,0,0, 0,1,0,0, 0,1,0,0, 0,1,0,0};
    const auto before = fixture.heights;
    const auto result = fixture.Project();
    Require(result.width == 4 && result.height == 4, "projection retains the explicit native pixel dimensions");
    Require(result.terrainShadowBits == expected, "8 high ridge projects one pixel at slope -4; equality stops the ray");
    Require(fixture.heights == before, "projection borrows the height field without editing it");
    Require(fixture.Project().terrainShadowBits == expected, "identical captured samples project deterministically");

    fixture.direction = {1, 0, -0.125f};
    fixture.heights.fill(0);
    for (std::size_t y = 0; y < 4; ++y) fixture.heights[y * 4 + 3] = 8;
    const std::vector<std::uint8_t> reverse{0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0};
    Require(fixture.Project().terrainShadowBits == reverse, "negative major axis follows native signed denominator");

    fixture.heights.fill(0);
    fixture.direction = {0, -1, 0.125f};
    for (std::size_t x = 0; x < 4; ++x) fixture.heights[x] = 8;
    const std::vector<std::uint8_t> north{0,0,0,0, 1,1,1,1, 0,0,0,0, 0,0,0,0};
    Require(fixture.Project().terrainShadowBits == north, "zero X component selects Y without division by zero");
}

void SlantedProjectionRetainsTruncationAndAxisTie() {
    Fixture fixture;
    fixture.heights[0] = 8;
    fixture.direction = {-1, -0.5f, 0.125f};
    std::vector<std::uint8_t> expected(16, 0);
    expected[1] = 1;
    Require(fixture.Project().terrainShadowBits == expected, "half-row minor step truncates toward zero");
    fixture.direction = {-1, -1, 0.125f};
    expected[1] = 0; expected[5] = 1;
    Require(fixture.Project().terrainShadowBits == expected, "equal axis magnitudes use Y-major diagonal projection");

    fixture.direction = {-1, 0.5f, 0.125f};
    expected[5] = 0; expected[1] = 1;
    Require(fixture.Project().terrainShadowBits == expected, "minor coordinate -0.5 maps to row zero before bounds check");

    fixture.heights.fill(4);
    fixture.direction = {-1, 0, 0};
    Require(fixture.Project().terrainShadowBits == std::vector<std::uint8_t>(16, 0),
        "flat equal-height samples do not cast onto one another");
}

void RejectsMissingNonfiniteAndUnboundedInputs() {
    Fixture fixture;
    auto input = fixture.Input();
    input.width = 0;
    Refused(ProjectTerrainShadow(input), "TERRAIN_SHADOW_DIMENSIONS");
    input = fixture.Input(); input.height = 3;
    Refused(ProjectTerrainShadow(input), "TERRAIN_SHADOW_DIMENSIONS");
    input = fixture.Input(); input.receiverHeights = input.receiverHeights.first(15);
    Refused(ProjectTerrainShadow(input), "TERRAIN_SHADOW_HEIGHT_COUNT");
    fixture.heights[7] = std::numeric_limits<float>::quiet_NaN();
    Refused(ProjectTerrainShadow(fixture.Input()), "TERRAIN_SHADOW_HEIGHT");
    fixture.heights[7] = 0;
    fixture.direction[1] = std::numeric_limits<float>::infinity();
    Refused(ProjectTerrainShadow(fixture.Input()), "TERRAIN_SHADOW_LIGHT");
    fixture.direction = {0, 0, 1};
    Refused(ProjectTerrainShadow(fixture.Input()), "TERRAIN_SHADOW_LIGHT_VERTICAL");
    fixture.direction = {std::numeric_limits<float>::min(), 0, std::numeric_limits<float>::max()};
    Refused(ProjectTerrainShadow(fixture.Input()), "TERRAIN_SHADOW_LIGHT_STEP");

    fixture.direction = {-1, 0, 0.125f};
    TerrainShadowRebuildLimits limits;
    ++limits.maximumWork;
    Refused(ProjectTerrainShadow(fixture.Input(), limits), "TERRAIN_SHADOW_LIMITS");
    limits = {}; limits.maximumPixels = 15;
    Refused(ProjectTerrainShadow(fixture.Input(), limits), "TERRAIN_SHADOW_BUDGET");
    limits = {}; limits.maximumWork = 20;
    Refused(ProjectTerrainShadow(fixture.Input(), limits), "TERRAIN_SHADOW_BUDGET");
    limits = {}; limits.maximumDimension = 3;
    Refused(ProjectTerrainShadow(fixture.Input(), limits), "TERRAIN_SHADOW_BUDGET");
}

void CancellationAndBudgetDoNotPublishPrefix() {
    Fixture fixture;
    fixture.heights[0] = 16;
    const auto before = fixture.heights;
    std::stop_source source;
    source.request_stop();
    Refused(ProjectTerrainShadow(fixture.Input(), {}, source.get_token()), "TERRAIN_SHADOW_CANCELLED");
    auto limits = TerrainShadowRebuildLimits{};
    limits.maximumWork = 18; // 高度扫描后允许首个投影步，随后必须丢弃整个候选。
    Refused(ProjectTerrainShadow(fixture.Input(), limits), "TERRAIN_SHADOW_BUDGET");
    Require(fixture.heights == before, "cancelled and budget-exhausted candidates leave input intact");
    const auto complete = fixture.Project();
    limits.maximumWork = complete.work;
    Require(Take(ProjectTerrainShadow(fixture.Input(), limits)).terrainShadowBits == complete.terrainShadowBits,
        "an exact work budget succeeds without a hidden extra operation");
    --limits.maximumWork;
    Refused(ProjectTerrainShadow(fixture.Input(), limits), "TERRAIN_SHADOW_BUDGET");
}

void OriginalShadowCodecRemainsLossless() {
    const std::vector<std::byte> source{std::byte{0}, std::byte{1}, std::byte{0x7F}, std::byte{0xFF}};
    auto shadow = Take(ParseMapShadow(source, 2, 2));
    Require(Take(SerializeMapShadow(shadow)) == source,
        "adding terrain projection must not turn source SHD codec into binary normalization");
}

} // namespace

int main() {
    try {
        CardinalProjectionStopsAtEqualHeight();
        SlantedProjectionRetainsTruncationAndAxisTie();
        RejectsMissingNonfiniteAndUnboundedInputs();
        CancellationAndBudgetDoNotPublishPrefix();
        OriginalShadowCodecRemainsLossless();
        std::cout << "Terrain shadow projection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
