#pragma once

#include "War3MapCore/Render/TerrainGroundMesh.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>

// 在主驱动全局scope、CaptureFrame/ReadView/SaveBmp之后包含（同RenderTerrainGridChecks.h）。
// 只走生产builder和provider；以下CPU数学只求原输入的像素中心oracle，不生成参考渲染图。
namespace TerrainTransitionTest {
using Json = nlohmann::ordered_json;
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Render;
constexpr std::uint32_t kWidth = 512, kHeight = 384;
constexpr std::array<int, 3> kA{32, 72, 112}, kB{208, 168, 48}, kC{224, 32, 200};
// 首次执行前固定：8-bit双线性fraction误差<=2/512，经最大176色差后<0.69，
// 加UNORM目标量化0.5及float余量，允许2 code；不在运行后调阈值。
constexpr double kResidualLimit = 2.0, kRmsLimit = 0.85;
constexpr double kTLow = .15, kTHigh = .85, kSeamBand = .25, kNearBand = .04;
// 事前排除共享投影线两侧<半像素的中心，避免CPU double覆盖与GPU subpixel snap归属分歧。
constexpr double kSharedEdgeExclusionPixels = .5;
constexpr std::size_t kMinimumSide = 512, kMinimumNearSide = 128, kMinimumControlMismatch = 128;
enum class Case { Continuous, UnusedPoison, SourceEdge, Variation };
constexpr std::array<const char*, 4> kNames{"continuous", "unused-poison", "source-edge-negative", "variation-negative"};

template<class T> T Checked(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string text = "terrain transition production API refused";
        for (const auto& d : result.diagnostics) text += "|" + d.code + ":" + d.message;
        throw std::runtime_error(text);
    }
    return std::move(*result.value);
}
std::string Digest(std::span<const std::byte> bytes) { return Checked(Sha256(bytes)); }
void Check(bool value, const char* message) { Require(value, message); }

TerrainGridSnapshot SourceGrid(Case mode) {
    TerrainGridSnapshot grid;
    grid.header.width = 3; grid.header.height = 2; grid.header.mainTileset = 'L';
    grid.header.offset = {-128, -64}; grid.header.groundTilesets = {"TstA", "TstB"}; grid.header.cliffTilesets = {"TstC"};
    constexpr std::array<std::uint8_t, 6> materials{0, 0, 1, 0, 1, 1}; // south A A B / north A B B
    for (std::uint32_t y = 0; y < 2; ++y) for (std::uint32_t x = 0; x < 3; ++x) {
        TerrainVertex v; v.groundHeightRaw = 8192; v.waterHeightAndFlagsRaw = 7936; v.cliffAndLayerRaw = 2;
        v.textureAndFlagsRaw = materials[y * 3 + x];
        v.variationRaw = mode == Case::Variation && x == 1 && y == 0 ? 1 : 0;
        v.position = {-128.f + x * 128.f, -64.f + y * 128.f, 0}; v.waterBaseHeight = -64;
        grid.vertices.push_back(v);
    }
    grid.tiles = {{{0, 1, 3, 4}}, {{1, 2, 4, 5}}};
    grid.minimum = {-128, -64, 0}; grid.maximum = {128, 64, 0};
    return grid;
}
std::shared_ptr<TerrainAtlasSnapshot> Atlas(bool overlay, Case mode) {
    auto atlas = std::make_shared<TerrainAtlasSnapshot>();
    atlas->columns = 4; atlas->sourceWidth = atlas->sourceHeight = atlas->image.width = atlas->image.height = 8;
    atlas->validatedMipCount = 1; atlas->image.rgba.resize(8 * 8 * 4);
    for (std::uint32_t row = 0; row < 4; ++row) for (std::uint32_t col = 0; col < 4; ++col) {
        atlas->cells.push_back({col, row, col * 2, row * 2, 2, 2});
        const bool used = overlay ? (col == 0 && row == 1) || (col == 1 && row == 3) : col == 0 && row == 0;
        for (std::uint32_t py = 0; py < 2; ++py) for (std::uint32_t px = 0; px < 2; ++px) {
            const auto at = (static_cast<std::size_t>(row * 2 + py) * 8 + col * 2 + px) * 4;
            auto color = used ? (overlay ? kB : kA) : kC;
            if (!used && mode == Case::UnusedPoison) color = {8, 232, 248};
            for (std::size_t c = 0; c < 3; ++c) atlas->image.rgba[at + c] = static_cast<std::byte>(color[c]);
            unsigned alpha = 255;
            if (overlay && col == 0 && row == 1) alpha = py == 0 && px == 1 ? 255 : 0; // mask4: NE
            if (overlay && col == 1 && row == 3) alpha = py == 1 && px == 0 ? 0 : 255; // mask13: all except SW
            if (overlay && mode == Case::SourceEdge && col == 1 && row == 3 && py == 1 && px == 0) alpha = 255;
            atlas->image.rgba[at + 3] = static_cast<std::byte>(alpha);
        }
    }
    atlas->sourceBytes = atlas->image.rgba.size(); atlas->sourceSha256 = Digest(atlas->image.rgba);
    return atlas;
}
double Alpha(Case mode, unsigned tile, double s, double t) {
    if (mode == Case::SourceEdge && tile == 1) return 1;
    return tile == 0 ? s * t : s + t - s * t;
}
std::array<double, 3> Expected(Case mode, unsigned tile, double s, double t) {
    const auto& base = mode == Case::Variation && tile == 1 ? kC : kA;
    const double alpha = Alpha(mode, tile, s, t); std::array<double, 3> result{};
    for (std::size_t c = 0; c < 3; ++c) result[c] = (1 - alpha) * base[c] + alpha * kB[c];
    return result; // neutral vertex tint=1, fog/shadow disabled, linear UNORM, no gamma transform.
}
Json SourceIdentity(const TerrainGridSnapshot& source) {
    Json j{{"width", source.header.width}, {"height", source.header.height}, {"offset", source.header.offset},
        {"version", source.header.version}, {"main_tileset", source.header.mainTileset}, {"custom_tileset", source.header.usesCustomTileset},
        {"ground_tilesets", source.header.groundTilesets}, {"cliff_tilesets", source.header.cliffTilesets},
        {"minimum", source.minimum}, {"maximum", source.maximum}, {"unresolved_ground", source.unresolvedGroundTextures},
        {"unresolved_cliff", source.unresolvedCliffTextures}, {"vertices", Json::array()}, {"tiles", Json::array()}};
    for (const auto& v : source.vertices) j["vertices"].push_back({v.groundHeightRaw, v.waterHeightAndFlagsRaw,
        v.textureAndFlagsRaw, v.variationRaw, v.cliffAndLayerRaw, v.position[0], v.position[1], v.position[2], v.waterBaseHeight,
        v.textureFlagsExtensionRaw, v.extendedTextureLayout});
    for (const auto& tile : source.tiles) j["tiles"].push_back(tile.vertices);
    return j;
}
Json MeshIdentity(const TerrainGroundMesh& mesh) {
    Json j{{"grid", Json::array()}, {"base_quads", Json::array()}, {"transition_vertices", Json::array()},
        {"base_indices", mesh.baseIndices}, {"transition_indices", mesh.indices}, {"overlay_tiles", mesh.overlayTiles},
        {"fallback", mesh.fallback.totalTiles}, {"hidden", mesh.hiddenTileCount}, {"mixed", mesh.mixedGroundTextureTiles},
        {"transitions_requested", mesh.budget.transitions.requested}, {"transitions_displayed", mesh.budget.transitions.displayed}};
    for (const auto& v : mesh.groundGrid) j["grid"].push_back({v.x, v.y, v.groundZ, v.textureR, v.textureG, v.textureB});
    for (const auto& q : mesh.baseQuads) j["base_quads"].push_back({q.sourceTile, q.uvLeft, q.uvRight, q.uvTop, q.uvBottom});
    for (const auto& v : mesh.vertices) j["transition_vertices"].push_back({v.x, v.y, v.z, v.red, v.green, v.blue, v.u, v.v});
    return j;
}
struct Fixture {
    Case mode;
    TerrainGridSnapshot terrain;
    std::array<std::shared_ptr<TerrainAtlasSnapshot>, 2> atlases;
    TerrainGroundMesh mesh;
    explicit Fixture(Case value) : mode(value), terrain(SourceGrid(value)), atlases{Atlas(false, value), Atlas(true, value)} {
        const std::array<std::shared_ptr<const TerrainAtlasSnapshot>, 2> inputs{atlases[0], atlases[1]};
        mesh = Checked(BuildTerrainGroundMesh(terrain, inputs, {}, TerrainLighting{false}, nullptr, {}, {}, {},
            TerrainGroundBudgetPolicy::Strict, TerrainGroundBlightPolicy::SharedBase, TerrainGroundRepresentation::SharedGrid));
        const auto valid = ValidateTerrainGroundGrid(mesh, terrain);
        Check(valid.Succeeded(), "transition production grid source validation failed");
        Check(mesh.representation == TerrainGroundRepresentation::SharedGrid && !mesh.lit && !mesh.hiddenTileCount
            && !mesh.fallback.totalTiles && mesh.texturedTiles == 2 && mesh.mixedGroundTextureTiles == 2
            && mesh.groundGrid.size() == 6 && mesh.baseQuads.size() == 2 && mesh.baseIndices.size() == 12
            && mesh.vertices.size() == 8 && mesh.indices.size() == 12 && mesh.overlayTiles == std::vector<std::uint32_t>{0, 1}
            && mesh.textures.size() == 2 && mesh.baseBatches.size() == 1 && mesh.batches.size() == 1
            && mesh.budget.transitions.requested == 2 && mesh.budget.transitions.displayed == 2
            && !mesh.budget.transitions.omitted && !mesh.budget.reduced
            && mesh.waterGrid.empty() && mesh.waterQuads.empty() && mesh.blightIndices.empty(), "transition builder did not retain exact two-cell domain");
        Check(mesh.textures[0] == atlases[0] && mesh.textures[1] == atlases[1]
            && mesh.baseBatches[0].textureIndex == 0 && !mesh.baseBatches[0].alphaBlend
            && mesh.batches[0].textureIndex == 1 && mesh.batches[0].alphaBlend && !mesh.batches[0].translucent,
            "transition material/alpha order differs from production expected domain");
        constexpr std::array<std::uint32_t, 12> topology{0, 1, 2, 1, 3, 2, 4, 5, 6, 5, 7, 6};
        Check(std::equal(mesh.baseIndices.begin(), mesh.baseIndices.end(), topology.begin())
            && std::equal(mesh.indices.begin(), mesh.indices.end(), topology.begin()), "transition production topology differs");
        for (std::size_t i = 0; i < 6; ++i) {
            const auto& g = mesh.groundGrid[i]; const auto& v = terrain.vertices[i];
            Check(g.x == v.position[0] && g.y == v.position[1] && g.groundZ == 0
                && g.textureR == 1 && g.textureG == 1 && g.textureB == 1, "transition grid geometry/tint is not original neutral source");
        }
        constexpr std::array<std::array<unsigned, 4>, 2> corners{{{0, 1, 3, 4}, {1, 2, 4, 5}}};
        for (unsigned tile = 0; tile < 2; ++tile) {
            const auto& q = mesh.baseQuads[tile]; const bool variant = mode == Case::Variation && tile == 1;
            const float left = variant ? 6.5f / 8 : .5f / 8, right = variant ? 7.5f / 8 : 1.5f / 8;
            Check(q.sourceTile == tile && q.uvLeft == left && q.uvRight == right && q.uvTop == left && q.uvBottom == right,
                "transition base variation did not select declared original cell");
            for (unsigned corner = 0; corner < 4; ++corner) {
                const auto& v = mesh.vertices[tile * 4 + corner]; const auto& p = terrain.vertices[corners[tile][corner]].position;
                const unsigned col = tile == 0 ? 0 : 1, row = tile == 0 ? 1 : 3;
                const float u = (col * 2 + (corner % 2 ? 1.5f : .5f)) / 8;
                const float uv = (row * 2 + (corner < 2 ? 1.5f : .5f)) / 8;
                Check(v.x == p[0] && v.y == p[1] && v.z == p[2] && v.red == 1 && v.green == 1 && v.blue == 1
                    && v.u == u && v.v == uv, "transition actual mask4/13 UV or source corner mismatch");
            }
        }
    }
};

void CpuSourceChecks(const Fixture& baseline, const Fixture& poison, const Fixture& edge, const Fixture& variation) {
    Check(baseline.atlases[0]->image.Valid() && baseline.atlases[1]->image.Valid(), "transition original atlas invalid");
    Check(SourceIdentity(baseline.terrain) == SourceIdentity(poison.terrain)
        && SourceIdentity(baseline.terrain) == SourceIdentity(edge.terrain), "atlas controls changed original source grid");
    for (unsigned atlas = 0; atlas < 2; ++atlas) for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
        const unsigned col = x / 2, row = y / 2;
        const bool used = atlas ? (col == 0 && row == 1) || (col == 1 && row == 3) : col == 0 && row == 0;
        const auto color = used ? (atlas ? kB : kA) : kC;
        const auto at = (y * 8 + x) * 4;
        for (unsigned channel = 0; channel < 3; ++channel) {
            const auto actual = std::to_integer<int>(baseline.atlases[atlas]->image.rgba[at + channel]);
            Check(actual == color[channel], "original atlas RGB does not match declared source cell");
            const auto poisoned = std::to_integer<int>(poison.atlases[atlas]->image.rgba[at + channel]);
            constexpr std::array<int, 3> poisonRgb{8, 232, 248};
            Check(poisoned == (used ? actual : poisonRgb[channel]), "unused-cell poison changed a selected cell or missed an unused cell");
        }
        const unsigned alpha = !atlas ? 255 : col == 0 && row == 1 ? (y % 2 == 0 && x % 2 == 1 ? 255 : 0)
            : col == 1 && row == 3 ? (y % 2 == 1 && x % 2 == 0 ? 0 : 255) : 255;
        Check(std::to_integer<unsigned>(baseline.atlases[atlas]->image.rgba[at + 3]) == alpha
            && poison.atlases[atlas]->image.rgba[at + 3] == baseline.atlases[atlas]->image.rgba[at + 3],
            "physical source mask4/13 alpha or poison alpha differs");
    }
    std::size_t edgeBytes = 0;
    for (std::size_t i = 0; i < 256; ++i) if (baseline.atlases[1]->image.rgba[i] != edge.atlases[1]->image.rgba[i]) {
        ++edgeBytes; Check(i == ((7 * 8 + 2) * 4 + 3), "source-edge control changed more than mask13 SW alpha");
    }
    Check(edgeBytes == 1 && baseline.atlases[0]->image.rgba == edge.atlases[0]->image.rgba, "source-edge control has no single-byte cause");
    Check(baseline.atlases[0]->image.rgba == variation.atlases[0]->image.rgba
        && baseline.atlases[1]->image.rgba == variation.atlases[1]->image.rgba, "variation control changed source atlas");
    auto restoredVariation = variation.terrain; restoredVariation.vertices[1].variationRaw = 0;
    Check(SourceIdentity(restoredVariation) == SourceIdentity(baseline.terrain), "variation control changed more than one variation field");
    for (std::size_t i = 0; i < 6; ++i) Check(variation.terrain.vertices[i].variationRaw == (i == 1 ? 1 : 0), "variation control changed another source vertex");
    Check(MeshIdentity(baseline.mesh) == MeshIdentity(poison.mesh), "unused poison changed geometry/material selection");
    // 两侧仅C0；normal导数t与1-t不同。精确边界、单边极限和物理角均以原创输入自检。
    for (const double t : {0., .125, .5, .875, 1.}) {
        Check(Alpha(Case::Continuous, 0, 1, t) == t && Alpha(Case::Continuous, 1, 0, t) == t,
            "continuous source does not share exact alpha edge limit");
        const auto a = Expected(Case::Continuous, 0, 1, t), b = Expected(Case::Continuous, 1, 0, t);
        Check(a == b, "continuous source color limits differ");
    }
    Check(Alpha(Case::Continuous, 0, 0, 0) == 0 && Alpha(Case::Continuous, 0, 1, 1) == 1
        && Alpha(Case::Continuous, 1, 0, 0) == 0 && Alpha(Case::Continuous, 1, 1, 0) == 1
        && Alpha(Case::Continuous, 1, 0, 1) == 1, "source mask physical corner directions differ");
    for (const auto mode : {Case::SourceEdge, Case::Variation}) {
        const auto left = Expected(mode, 0, 1, .5), right = Expected(mode, 1, 0, .5);
        Check(std::abs(left[0] - right[0]) >= 80, "negative source does not contain a known edge discontinuity");
    }
}

struct ScreenVertex { double x, y, inverseW, s, t; };
struct Triangle { std::array<ScreenVertex, 3> v; unsigned tile; };
double Edge(const ScreenVertex& a, const ScreenVertex& b, double x, double y) { return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x); }
ScreenVertex Project(const War3RenderViewInfoV2& view, double x, double y, double s, double t) {
    const auto* m = view.viewProjection;
    const double w = x * m[3] + y * m[7] + m[15], cx = x * m[0] + y * m[4] + m[12];
    const double cy = x * m[1] + y * m[5] + m[13], z = x * m[2] + y * m[6] + m[14];
    Check(std::isfinite(w) && w > 0 && std::abs(cx) < w && std::abs(cy) < w && z > 0 && z < w,
        "transition oracle does not support clipped or behind-eye original triangles");
    return {(cx / w + 1) * view.width / 2, (1 - cy / w) * view.height / 2, 1 / w, s, t};
}
std::array<Triangle, 4> Triangles(const War3RenderViewInfoV2& view) {
    std::array<Triangle, 4> output{};
    constexpr std::array<std::array<unsigned, 3>, 2> topology{{{0, 1, 2}, {1, 3, 2}}};
    for (unsigned tile = 0; tile < 2; ++tile) {
        std::array<ScreenVertex, 4> vertices{};
        for (unsigned corner = 0; corner < 4; ++corner) vertices[corner] = Project(view,
            -128 + tile * 128.0 + (corner % 2) * 128, -64 + (corner / 2) * 128.0, corner % 2, corner / 2);
        for (unsigned face = 0; face < 2; ++face) {
            auto& triangle = output[tile * 2 + face]; triangle.tile = tile;
            for (unsigned i = 0; i < 3; ++i) triangle.v[i] = vertices[topology[face][i]];
            if (Edge(triangle.v[0], triangle.v[1], triangle.v[2].x, triangle.v[2].y) < 0) std::swap(triangle.v[1], triangle.v[2]);
        }
    }
    return output;
}
struct Sample { unsigned tile; double s, t, edgeDistance; };
std::optional<Sample> Pixel(const std::array<Triangle, 4>& triangles, double x, double y) {
    for (const auto& triangle : triangles) {
        const auto& v = triangle.v;
        const double area = Edge(v[0], v[1], v[2].x, v[2].y); Check(area > 0, "transition projected triangle is degenerate");
        std::array<double, 3> e{Edge(v[1], v[2], x, y), Edge(v[2], v[0], x, y), Edge(v[0], v[1], x, y)};
        bool inside = true;
        for (unsigned i = 0; i < 3; ++i) {
            const auto& a = v[(i + 1) % 3]; const auto& b = v[(i + 2) % 3];
            const bool topLeft = b.y < a.y || (b.y == a.y && b.x > a.x);
            inside &= e[i] > 0 || (e[i] == 0 && topLeft);
        }
        if (!inside) continue;
        double denominator = 0, s = 0, t = 0;
        for (unsigned i = 0; i < 3; ++i) { const double weight = e[i] / area * v[i].inverseW; denominator += weight; s += weight * v[i].s; t += weight * v[i].t; }
        Check(denominator > 0, "transition pixel has nonpositive perspective weight"); s /= denominator; t /= denominator;
        return Sample{triangle.tile, s, t, triangle.tile == 0 ? 1 - s : s};
    }
    return {};
}
struct Observation {
    CapturedFrame frame;
    War3RenderTerrainGridCaptureFactsV2 facts{sizeof(facts)};
    std::array<std::size_t, 2> samples{}, near{}, baselineMismatch{};
    std::array<std::array<std::size_t, 4>, 2> tBins{};
    double maximum = 0, sumSquares = 0, baselineMaximum = 0;
    std::size_t residualValues = 0, overLimitPixels = 0;
    Json residualRows = Json::array();
};
void Analyze(Observation& result, Case mode) {
    const auto triangles = Triangles(result.frame.receipt.view);
    const auto south = Project(result.frame.receipt.view, 0, -64, 0, 0), north = Project(result.frame.receipt.view, 0, 64, 0, 1);
    const auto sharedLength = std::hypot(north.x - south.x, north.y - south.y);
    Check(sharedLength > 0, "transition projected shared edge is degenerate");
    for (std::uint32_t y = 0; y < kHeight; ++y) for (std::uint32_t x = 0; x < kWidth; ++x) {
        if (std::abs(Edge(south, north, x + .5, y + .5)) / sharedLength < kSharedEdgeExclusionPixels) continue;
        const auto sample = Pixel(triangles, x + .5, y + .5); if (!sample) continue;
        const auto& p = *sample;
        if (p.t < kTLow || p.t > kTHigh || p.edgeDistance < 0 || p.edgeDistance > kSeamBand) continue;
        const auto at = (static_cast<std::size_t>(y) * kWidth + x) * 4;
        Check(result.frame.pixels[at + 3] == 255, "transition ROI output alpha is not opaque");
        ++result.samples[p.tile]; if (p.edgeDistance <= kNearBand) ++result.near[p.tile];
        const auto bin = std::min(3u, static_cast<unsigned>((p.t - kTLow) * 4 / (kTHigh - kTLow))); ++result.tBins[p.tile][bin];
        const auto expected = Expected(mode, p.tile, p.s, p.t), baseline = Expected(Case::Continuous, p.tile, p.s, p.t);
        double maximum = 0, wrong = 0;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const double actual = result.frame.pixels[at + channel], residual = std::abs(actual - expected[channel]);
            maximum = std::max(maximum, residual); wrong = std::max(wrong, std::abs(actual - baseline[channel]));
            result.sumSquares += residual * residual; ++result.residualValues;
        }
        result.maximum = std::max(result.maximum, maximum); result.baselineMaximum = std::max(result.baselineMaximum, wrong);
        result.overLimitPixels += maximum > kResidualLimit;
        if (wrong > 8) ++result.baselineMismatch[p.tile];
        // 全ROI residual保存在可选工件中；不是第二renderer，也不输出base64图片。
        result.residualRows.push_back({x, y, p.tile, p.s, p.t, maximum, wrong});
    }
}
Observation CaptureProduction(const RendererOwner& renderer, const Fixture& fixture, std::uint64_t revision,
    const War3RenderCameraV2& camera) {
    const auto& mesh = fixture.mesh;
    std::array<War3RenderMipRgbaV2, 2> levels{}; std::array<War3RenderTextureMipChainV2, 2> textures{};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& source = *mesh.textures[i];
        Check(source.validatedMipCount == 1 && source.additionalMips.empty() && source.columns == 4
            && source.image.width == 8 && source.image.height == 8, "transition source must retain exact single original mip");
        levels[i] = {sizeof(War3RenderMipRgbaV2), 8, 8, source.image.rgba.size(), reinterpret_cast<const std::uint8_t*>(source.image.rgba.data())};
        textures[i] = {sizeof(War3RenderTextureMipChainV2), 1, 0, 4, 4, &levels[i]};
    }
    const auto batch = [](const TerrainGroundBatch& source) {
        return War3RenderTerrainBatchV2{source.firstIndex, source.indexCount,
            source.textureIndex | (source.alphaBlend ? WAR3_RENDER_TERRAIN_ALPHA_BLEND : 0u)};
    };
    const std::array<War3RenderTerrainBatchV2, 1> bases{batch(mesh.baseBatches[0])}, transitions{batch(mesh.batches[0])};
    std::array<War3RenderTerrainVertexV2, 8> vertices{};
    for (std::size_t i = 0; i < vertices.size(); ++i) { const auto& v = mesh.vertices[i]; vertices[i] = {v.x, v.y, v.z, v.red, v.green, v.blue, v.u, v.v}; }
    War3RenderTerrainGridFrameV2 input{}; input.structSize = sizeof(input); input.gridFormat = 1; input.width = 3; input.height = 2;
    input.gridVertexCount = 6; input.baseQuadCount = 2; input.baseIndexCount = 12; input.baseBatchCount = 1;
    input.groundScene = {sizeof(War3RenderTerrainMipSceneV2), 8, 12, 2, 1, revision, vertices.data(), mesh.indices.data(), textures.data(), transitions.data()};
    input.groundGrid = mesh.groundGrid.data(); input.baseQuads = mesh.baseQuads.data(); input.baseIndices = mesh.baseIndices.data(); input.baseBatches = bases.data();
    Check(renderer.api->setCamera(renderer.handle, &camera) == 0 && renderer.api->submitTerrainGridFrame(renderer.handle, &input) == 0,
        "transition production SharedGrid submission rejected");
    Observation result; result.frame = CaptureFrame(renderer, kWidth, kHeight, revision, camera);
    Check(SameView(ReadView(renderer), result.frame.receipt.view), "transition latest completed view differs from atomic capture");
    Check(renderer.api->getTerrainGridCaptureFacts(renderer.handle, &result.frame.receipt.view, &result.facts) == 0
        && SameView(result.facts.view, result.frame.receipt.view) && result.facts.gridFormat == 1
        && result.facts.gridVertexCount == 6 && result.facts.baseQuadCount == 2 && result.facts.transitionVertexCount == 8
        && result.facts.groundTextureCount == 2 && !result.facts.blightIndexCount && !result.facts.blightBatchCount
        && !result.facts.waterQuadCount && !result.facts.passBits, "transition completed grid facts lost submitted production domain");
    War3RenderStatusV2 status{sizeof(status)};
    Check(renderer.api->getStatus(renderer.handle, &status) == 0 && !status.result && status.deviceReady && status.documentRevision == revision,
        "transition completed device/status revision invalid");
    Analyze(result, fixture.mode); return result;
}

std::filesystem::path OutputDirectory() {
    constexpr auto key = L"WAR3_RENDER_TERRAIN_TRANSITION_AUDIT_OUTPUT";
    SetLastError(ERROR_SUCCESS); const auto count = GetEnvironmentVariableW(key, nullptr, 0);
    if (!count) { Check(GetLastError() == ERROR_ENVVAR_NOT_FOUND || GetLastError() == ERROR_SUCCESS, "transition output environment unavailable"); return {}; }
    Check(count < 32768, "transition output path too long"); std::vector<wchar_t> buffer(count);
    Check(GetEnvironmentVariableW(key, buffer.data(), count) == count - 1, "transition output environment changed");
    const std::filesystem::path path(buffer.data());
    Check(path.is_absolute() && !std::filesystem::exists(path) && std::filesystem::create_directory(path), "transition audit output must be a new absolute directory");
    return path;
}
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    Check(!std::filesystem::exists(path), "transition audit output exists");
    std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close(); Check(file.good(), "transition audit artifact write failed");
}
void Export(const std::filesystem::path& directory, const Fixture& fixture, const Observation& result, const char* name) {
    Json j{{"schema", "terrain-transition-original-two-cell-v1"}, {"case", name}, {"source_materials_south", {"A", "A", "B"}},
        {"source_materials_north", {"A", "B", "B"}}, {"left_mask", 4}, {"right_mask", 13},
        {"color_A", kA}, {"color_B", kB}, {"unused_variation_C", kC}, {"alpha_left", "s*t"},
        {"alpha_right", fixture.mode == Case::SourceEdge ? "1" : "s+t-s*t"}, {"continuity_contract", "C0 only; normal derivatives may differ"},
        {"neutral_vertex_tint", 1}, {"fog", false}, {"shadow", false}, {"UNORM", true}, {"MSAA_samples", 1},
        {"submitted_mip_count", 1}, {"submitted_max_sample_mip", 0}, {"gpu_sampled_lod_telemetry", false},
        {"mip_evidence", "actual immutable single-mip descriptor; existing SampleLevel clamped to level0"},
        {"source", SourceIdentity(fixture.terrain)}, {"mesh", MeshIdentity(fixture.mesh)},
        {"atlas_sha256", {fixture.atlases[0]->sourceSha256, fixture.atlases[1]->sourceSha256}},
        {"frame_rgba_sha256", Digest(std::as_bytes(std::span(result.frame.pixels)))}, {"MVP", Json::array()},
        {"width", kWidth}, {"height", kHeight}, {"frame", result.frame.receipt.view.frameCount}, {"ticket", result.frame.receipt.capture.ticket},
        {"scene_epoch", result.frame.receipt.view.sceneEpoch}, {"document_revision", result.frame.receipt.view.documentRevision},
        {"completed_grid", {{"vertices", result.facts.gridVertexCount}, {"base_quads", result.facts.baseQuadCount},
            {"transition_vertices", result.facts.transitionVertexCount}, {"textures", result.facts.groundTextureCount}, {"pass_bits", result.facts.passBits}}},
        {"ROI", {{"t", {kTLow, kTHigh}}, {"distance_from_shared_edge_in_tile_units", {0., kSeamBand}}, {"near_band", kNearBand},
            {"shared_projected_edge_exclusion_pixels", kSharedEdgeExclusionPixels}, {"pixel_centers", true},
            {"perspective_correct_triangle_barycentrics", true}, {"top_left_edge_rule", true}}},
        {"minimum_samples_per_side", kMinimumSide}, {"minimum_near_samples_per_side", kMinimumNearSide},
        {"minimum_samples_per_t_bin", 32}, {"minimum_negative_baseline_mismatch_right", kMinimumControlMismatch},
        {"residual_limit_codes", kResidualLimit}, {"rms_limit_codes", kRmsLimit}, {"samples", result.samples}, {"near_samples", result.near},
        {"t_bins", result.tBins}, {"max_residual", result.maximum}, {"over_limit_pixels", result.overLimitPixels},
        {"rms_residual", result.residualValues ? std::sqrt(result.sumSquares / result.residualValues) : -1},
        {"baseline_oracle_max_residual", result.baselineMaximum}, {"baseline_oracle_over8_pixels", result.baselineMismatch},
        {"original_editor_validated", false}, {"runtime_validated", false}};
    for (const auto v : result.frame.receipt.view.viewProjection) j["MVP"].push_back(v);
    std::cout << "terrain transition " << name << " residual=" << result.maximum << " over=" << result.overLimitPixels
        << " samples=" << result.samples[0] << '/' << result.samples[1] << " near=" << result.near[0] << '/' << result.near[1] << '\n';
    if (directory.empty()) { std::cout << j.dump() << '\n'; return; }
    for (unsigned i = 0; i < 2; ++i) Write(directory / (std::string(name) + (i ? "-B.rgba" : "-A.rgba")), fixture.atlases[i]->image.rgba);
    Write(directory / (std::string(name) + ".rgba"), std::as_bytes(std::span(result.frame.pixels)));
    SaveBmp(directory / (std::string(name) + ".bmp"), result.frame.pixels, kWidth, kHeight);
    const auto receipt = j.dump(2), residuals = result.residualRows.dump();
    Write(directory / (std::string(name) + ".json"), std::as_bytes(std::span(receipt.data(), receipt.size())));
    Write(directory / (std::string(name) + "-residuals.json"), std::as_bytes(std::span(residuals.data(), residuals.size())));
}
void RequireObservation(const Observation& value) {
    Check(value.overLimitPixels == 0 && value.maximum <= kResidualLimit && value.residualValues
        && std::sqrt(value.sumSquares / value.residualValues) <= kRmsLimit, "transition pixel residual exceeds predeclared analytic bound");
    for (unsigned tile = 0; tile < 2; ++tile) {
        Check(value.samples[tile] >= kMinimumSide && value.near[tile] >= kMinimumNearSide, "transition shared-edge ROI has insufficient two-sided coverage");
        for (const auto count : value.tBins[tile]) Check(count >= 32, "transition shared-edge ROI does not span declared t range");
    }
}
void Run(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& originalInfo) {
    Fixture baseline(Case::Continuous), poison(Case::UnusedPoison), edge(Case::SourceEdge), variation(Case::Variation);
    CpuSourceChecks(baseline, poison, edge, variation);
    Check(api && api->structSize >= offsetof(War3RenderPluginApiV2, getTerrainGridCaptureFacts) + sizeof(api->getTerrainGridCaptureFacts)
        && api->submitTerrainGridFrame && api->getTerrainGridCapabilities && api->getTerrainGridCaptureFacts && api->setEnvironment,
        "transition actual provider SharedGrid capabilities missing");
    auto info = originalInfo; info.width = kWidth; info.height = kHeight;
    RendererOwner renderer{api, api->createRenderer(&info)}; Check(renderer.handle != nullptr, "transition renderer unavailable");
    War3RenderTerrainGridCapabilitiesV2 capabilities{sizeof(capabilities)};
    Check(api->getTerrainGridCapabilities(renderer.handle, &capabilities) == 0 && capabilities.structSize == sizeof(capabilities)
        && capabilities.gridFormat == WAR3_RENDER_TERRAIN_GRID_FETCH_V1
        && capabilities.maxGroundTextures == WAR3_RENDER_TERRAIN_GRID_MAX_GROUND_TEXTURES
        && capabilities.maxBlightBatches == WAR3_RENDER_TERRAIN_GRID_MAX_BLIGHT_BATCHES
        && capabilities.maxWaterTextures == WAR3_RENDER_TERRAIN_GRID_MAX_WATER_TEXTURES && !capabilities.reserved,
        "transition provider did not advertise production grid contract");
    const War3RenderEnvironmentV2 neutral{sizeof(neutral), {0, 0, 1}, {0, 0, 0}, {1, 1, 1},
        WAR3_RENDER_FOG_NONE, 0, 1, 0, {0, 0, 0}, 0, 0, 0, 0, 1, 0, nullptr};
    Check(api->setEnvironment(renderer.handle, &neutral) == 0, "transition neutral environment rejected");
    const War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 420, -1.5707963267948966f, 1.35f};
    const auto directory = OutputDirectory();
    const std::array<const Fixture*, 4> fixtures{&baseline, &poison, &edge, &variation};
    std::array<Observation, 4> result;
    for (unsigned i = 0; i < 4; ++i) {
        result[i] = CaptureProduction(renderer, *fixtures[i], 9970 + i, camera);
        Export(directory, *fixtures[i], result[i], kNames[i]); // 保失败之前的真实工件，不能改阈值重写。
    }
    for (const auto& observation : result) RequireObservation(observation);
    Check(result[0].frame.pixels == result[1].frame.pixels, "transition unused poison cell leaked into actual completed frame");
    for (unsigned i = 1; i < 4; ++i) Check(std::memcmp(result[0].frame.receipt.view.viewProjection,
        result[i].frame.receipt.view.viewProjection, sizeof(result[0].frame.receipt.view.viewProjection)) == 0,
        "transition control changed actual MVP");
    for (unsigned i = 2; i < 4; ++i) Check(result[i].baselineMismatch[0] == 0
        && result[i].baselineMismatch[1] >= kMinimumControlMismatch && result[i].baselineMaximum >= 32,
        "transition negative source was not distinguished from continuous baseline oracle on the declared right ROI");
    std::cout << "terrain transition: production builder/shared-grid, exact completed neutral frame, analytic C0 source limits, "
        "two-sided pixel residuals and independent unused/source-edge/variation controls passed\n";
}
} // namespace TerrainTransitionTest
