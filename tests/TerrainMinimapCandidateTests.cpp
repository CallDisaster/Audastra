#include "War3MapCore/Render/TerrainMinimapCandidate.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Render/TerrainGroundMesh.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace {

using namespace War3Map;
using namespace War3Map::Render;
using Rgba = std::array<unsigned, 4>;

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

void Near(double actual, double expected, const char* message) {
    Require(std::isfinite(actual) && std::abs(actual - expected) < 1e-9, message);
}

void U32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>((value >> shift) & 255));
}

void Raw(std::vector<std::byte>& bytes, std::string_view text) {
    for (const auto c : text) bytes.push_back(static_cast<std::byte>(c));
}

std::vector<std::byte> W3i(std::array<std::int32_t, 4> margins = {1, 3, 2, 1},
    std::int32_t width = 4, std::int32_t height = 3, std::uint32_t version = 25) {
    // 独立v25字节，不调用MapSettings writer产生期望；四字符串为空，八bounds故意不等于crop。
    std::vector<std::byte> bytes;
    U32(bytes, version); U32(bytes, 0); U32(bytes, 6061);
    if (version >= 28) bytes.insert(bytes.end(), 16, std::byte{0});
    bytes.insert(bytes.end(), 4, std::byte{0});
    for (unsigned i = 0; i < 8; ++i) U32(bytes, std::bit_cast<std::uint32_t>(static_cast<float>(i + 1)));
    for (const auto margin : margins) U32(bytes, static_cast<std::uint32_t>(margin));
    U32(bytes, static_cast<std::uint32_t>(width)); U32(bytes, static_cast<std::uint32_t>(height));
    U32(bytes, 0); bytes.push_back(std::byte{'L'}); U32(bytes, 0);
    bytes.insert(bytes.end(), 4, std::byte{0}); U32(bytes, 0); bytes.insert(bytes.end(), 4, std::byte{0});
    bytes.insert(bytes.end(), 16 + 4 + 4 + 1 + 1 + 4, std::byte{0}); // fog、weather、sound NUL、light、water。
    if (version >= 28) U32(bytes, 0);
    if (version >= 31) { U32(bytes, 0); U32(bytes, 0); }
    for (unsigned i = 0; i < 6; ++i) U32(bytes, 0); // players/forces及四组custom表。
    return bytes;
}

void Rebind(TerrainMinimapInput& input) {
    const auto hash = Sha256(input.currentW3e);
    Require(hash.Succeeded(), "fixture SHA 必须成功"); input.binding.terrainSha256 = *hash.value;
}

Image::DecodedImage Solid(std::uint32_t width, std::uint32_t height, Rgba rgba) {
    Image::DecodedImage image{width, height, std::vector<std::byte>(static_cast<std::size_t>(width) * height * 4)};
    for (std::size_t i = 0; i < image.rgba.size(); ++i) image.rgba[i] = static_cast<std::byte>(rgba[i % 4]);
    return image;
}

std::shared_ptr<TerrainAtlasSnapshot> Atlas(std::uint32_t cellSize = 4, Rgba color = {90, 130, 170, 255},
    std::uint32_t columns = 4) {
    auto atlas = std::make_shared<TerrainAtlasSnapshot>();
    atlas->source = {"fixture/ground.blp", Resources::ResourceLayer::MapImport, 500, "fixture-map", "fixture/ground.blp"};
    atlas->sourceSha256 = std::string(64, 'a'); atlas->sourceBytes = 1024;
    atlas->columns = columns; atlas->sourceWidth = cellSize * columns; atlas->sourceHeight = cellSize * 4;
    atlas->image = Solid(atlas->sourceWidth, atlas->sourceHeight, color); atlas->validatedMipCount = 1;
    for (std::uint32_t y = 0; y < 4; ++y) for (std::uint32_t x = 0; x < columns; ++x)
        atlas->cells.push_back({x, y, x * cellSize, y * cellSize, cellSize, cellSize});
    return atlas;
}

TerrainMinimapInput Fixture(std::uint32_t width = 2, std::uint32_t height = 2, std::uint32_t palettes = 1) {
    TerrainMinimapInput input;
    input.binding.documentId = "minimap-fixture"; input.binding.documentGeneration = 3; input.binding.revision = 5;
    input.binding.sourceSha256 = std::string(64, 'b');
    auto& bytes = input.currentW3e;
    Raw(bytes, "W3E!"); U32(bytes, 11); bytes.push_back(std::byte{'L'}); U32(bytes, 0);
    const std::array<std::string_view, 4> ids{"Ldrt", "Lgrs", "Lrok", "Ldro"};
    U32(bytes, palettes);
    for (std::uint32_t i = 0; i < palettes; ++i) { Raw(bytes, ids[i]); input.groundAtlases.push_back(Atlas()); }
    U32(bytes, 1); Raw(bytes, "CLdi"); U32(bytes, width); U32(bytes, height);
    U32(bytes, std::bit_cast<std::uint32_t>(10.0f)); U32(bytes, std::bit_cast<std::uint32_t>(20.0f));
    // 直接构造独立 v11 字节，不调用生产 Parse/Serialize 来生成测试网格。
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        bytes.insert(bytes.end(), {std::byte{0}, std::byte{32}, std::byte{0}, std::byte{32},
            std::byte{0}, std::byte{0}, std::byte{2}});
    }
    Rebind(input); return input;
}

std::size_t Record(const TerrainMinimapInput& input, std::size_t index) {
    return 41 + 4 * input.groundAtlases.size() + index * 7;
}

constexpr std::size_t CreateRecord(std::size_t x, std::size_t y) { return 69 + (y * 65 + x) * 7; }

std::vector<std::byte> CreateW3i() {
    auto bytes = W3i({6, 6, 4, 8}, 52, 52);
    // 原创CreateMapCandidate的八cameraBounds；它们有意不等于声明边距的世界范围。
    const std::array<float, 8> bounds{-2816, -3328, 2816, 2816, -2816, 2816, 2816, -3328};
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        const auto raw = std::bit_cast<std::uint32_t>(bounds[i]);
        for (unsigned b = 0; b < 4; ++b) bytes[16 + i * 4 + b] = static_cast<std::byte>((raw >> (b * 8)) & 255);
    }
    return bytes;
}

TerrainMinimapInput CreateTerrainFixture() {
    // 独立编码标准原创种子，保留全部MapEdge原位；不调用Create/Parse/Serialize生成期望字节。
    auto input = Fixture(); input.currentW3e.clear(); input.groundAtlases.clear();
    auto& bytes = input.currentW3e;
    Raw(bytes, "W3E!"); U32(bytes, 11); bytes.push_back(std::byte{'L'}); U32(bytes, 0);
    U32(bytes, 6); Raw(bytes, "LdrtLdroLdrgLrokLgrsLgrd"); U32(bytes, 2); Raw(bytes, "CLdiCLgr");
    U32(bytes, 65); U32(bytes, 65);
    U32(bytes, std::bit_cast<std::uint32_t>(-4096.0f)); U32(bytes, std::bit_cast<std::uint32_t>(-4096.0f));
    for (std::size_t y = 0; y < 65; ++y) for (std::size_t x = 0; x < 65; ++x) {
        const bool edge = x < 64 && y < 64 && (x < 6 || x >= 58 || y < 4 || y >= 56);
        bytes.insert(bytes.end(), {std::byte{0}, std::byte{32}, std::byte{0}, edge ? std::byte{96} : std::byte{32},
            std::byte{0}, std::byte{0}, std::byte{0xf2}});
    }
    const auto atlas = Atlas(); input.groundAtlases.assign(6, atlas);
    input.rangePolicy = TerrainMinimapRangePolicy::DeclaredW3iMarginsExteriorMapEdge;
    input.sourceW3i = CreateW3i(); Rebind(input); return input;
}

void CreateMapEdge(TerrainMinimapInput& input, std::size_t x, std::size_t y, bool edge) {
    input.currentW3e[CreateRecord(x, y) + 3] = edge ? std::byte{96} : std::byte{32}; Rebind(input);
}

void HeightRaw(TerrainMinimapInput& input, std::size_t vertex, std::uint16_t value) {
    const auto offset = Record(input, vertex);
    input.currentW3e[offset] = static_cast<std::byte>(value & 255);
    input.currentW3e[offset + 1] = static_cast<std::byte>(value >> 8);
    Rebind(input);
}

TerrainGroundMesh Mesh(const TerrainMinimapInput& input) {
    auto parsed = MapData::ParseMapTerrain(input.currentW3e);
    Require(parsed.Succeeded(), "独立mesh对照需要可解析W3E");
    auto mesh = BuildTerrainGroundMesh(*parsed.value->Snapshot(), input.groundAtlases, {}, TerrainLighting{false},
        nullptr, {}, {}, {}, TerrainGroundBudgetPolicy::Strict);
    Require(mesh.Succeeded(), "独立mesh对照需要完整Strict unlit网格"); return std::move(*mesh.value);
}

void Ground(TerrainMinimapInput& input, std::size_t vertex, unsigned texture) {
    input.currentW3e[Record(input, vertex) + 4] = static_cast<std::byte>(texture);
}

TerrainMinimapCandidate Build(const TerrainMinimapInput& input, const TerrainMinimapLimits& limits = {}) {
    auto result = BuildTerrainMinimapCandidate(input, limits);
    if (!result.Succeeded()) throw std::runtime_error("候选意外失败: " + result.diagnostics.front().code);
    return std::move(*result.value);
}

void Rejected(const TerrainMinimapInput& input, std::string_view code = {}, const TerrainMinimapLimits& limits = {}) {
    const auto result = BuildTerrainMinimapCandidate(input, limits);
    Require(!result.Succeeded() && !result.value && !result.diagnostics.empty(), "拒绝必须没有部分候选");
    if (!code.empty()) Require(result.diagnostics.front().code == code, "候选拒绝类别错误");
}

Rgba Pixel(const TerrainMinimapCandidate& result, std::uint32_t x, std::uint32_t y) {
    Rgba rgba{};
    for (std::size_t c = 0; c < 4; ++c) rgba[c] = std::to_integer<unsigned>(result.image.rgba[(static_cast<std::size_t>(y) * 256 + x) * 4 + c]);
    return rgba;
}

void SetCell(Image::DecodedImage& image, std::uint32_t cell, std::uint32_t column, std::uint32_t row, Rgba rgba) {
    for (std::uint32_t y = row * cell; y < (row + 1) * cell; ++y)
        for (std::uint32_t x = column * cell; x < (column + 1) * cell; ++x)
            for (std::size_t c = 0; c < 4; ++c) image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4 + c] = static_cast<std::byte>(rgba[c]);
}

void TestDirectionAndFit() {
    auto atlas = Atlas();
    // 每个 4x4 cell 的四象限同色，采样点避开中线；手算期望与生产 UV 实现无关。
    const std::array<Rgba, 4> quadrant{{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}}};
    for (std::uint32_t y = 0; y < 16; ++y) for (std::uint32_t x = 0; x < 16; ++x) {
        const auto& color = quadrant[(y % 4 >= 2 ? 2u : 0u) + (x % 4 >= 2 ? 1u : 0u)];
        for (std::size_t c = 0; c < 4; ++c) atlas->image.rgba[(static_cast<std::size_t>(y) * 16 + x) * 4 + c] = static_cast<std::byte>(color[c]);
    }
    auto input = Fixture(); input.groundAtlases[0] = atlas;
    const auto square = Build(input);
    Require(Pixel(square, 32, 32) == quadrant[0] && Pixel(square, 223, 32) == quadrant[1]
        && Pixel(square, 32, 223) == quadrant[2] && Pixel(square, 223, 223) == quadrant[3], "输出必须北上南下、X 向东且只翻一次 Y");
    Require(square.manifest.worldExtent == std::array<double, 4>{10, 20, 138, 148}, "范围必须含原始非居中 offset");
    input = Fixture(3, 2); input.groundAtlases[0] = atlas;
    const auto wide = Build(input);
    Require(wide.manifest.worldExtent == std::array<double, 4>{10, 20, 266, 148}
        && wide.manifest.contentRectangle == std::array<double, 4>{0, 64, 256, 128}, "横向地图应完整 fit 并上下各 padding64");
    Require(Pixel(wide, 0, 64) == quadrant[0] && Pixel(wide, 255, 191) == quadrant[3], "fit 内首末像素中心方向错误");
    for (const auto y : {63u, 192u}) for (const auto x : {0u, 255u})
        Require(Pixel(wide, x, y) == Rgba{0, 0, 0, 255}, "横向 padding 必须 opaque black");
    input = Fixture(2, 3); input.groundAtlases[0] = atlas;
    const auto tall = Build(input);
    Require(tall.manifest.contentRectangle == std::array<double, 4>{64, 0, 128, 256}
        && tall.manifest.worldExtent == std::array<double, 4>{10, 20, 138, 276}, "纵向地图应等比 fit 并保留完整范围");
    Require(Pixel(tall, 64, 0) == quadrant[0] && Pixel(tall, 191, 255) == quadrant[3], "纵向地图方向错误");
    for (const auto x : {63u, 192u}) for (const auto y : {0u, 255u})
        Require(Pixel(tall, x, y) == Rgba{0, 0, 0, 255}, "纵向 padding 必须 opaque black");
}

void TestGroupedDrawAndAllLayers() {
    auto input = Fixture(4, 2, 2);
    input.groundAtlases = {Atlas(4, {255, 0, 0, 255}), Atlas(4, {0, 255, 0, 128})};
    for (std::size_t vertex = 0; vertex < 8; ++vertex) Ground(input, vertex, vertex % 4 < 2 ? 1u : 0u);
    Rebind(input);
    const auto grouped = Build(input);
    // texture0 的 base indices 在前，但原 tile0 属 texture1；drawIndex/6 会把左右弄反。
    Require(Pixel(grouped, 42, 128) == Rgba{0, 255, 0, 255} && Pixel(grouped, 213, 128) == Rgba{255, 0, 0, 255},
        "按纹理归组的 indices 必须经 quad 原 tile 映射恢复空间位置，base alpha 必须忽略");
    Require(Pixel(grouped, 127, 128) == Rgba{127, 128, 0, 255}, "混合 tile 不能省去完整 overlay");
    input = Fixture(2, 2, 2);
    input.groundAtlases = {Atlas(4, {0, 0, 255, 0}), Atlas(4, {255, 0, 0, 128})};
    Ground(input, 1, 1); Rebind(input);
    const auto diagonal = Build(input);
    for (const auto x : {126u, 127u, 128u}) Require(Pixel(diagonal, x, 127) == Rgba{128, 0, 127, 255},
        "共享对角每层只能 blend 一次；blue alpha0 base 仍然不透明");
    input = Fixture(2, 2, 4);
    input.groundAtlases = {Atlas(4, {0, 0, 255, 0}), Atlas(4, {255, 0, 0, 128}),
        Atlas(4, {0, 255, 0, 128}), Atlas(4, {255, 255, 255, 0})};
    for (std::size_t vertex = 0; vertex < 4; ++vertex) Ground(input, vertex, static_cast<unsigned>(vertex));
    Rebind(input);
    const auto layered = Build(input);
    Require(layered.manifest.meshQuads == 4 && Pixel(layered, 127, 127) == Rgba{64, 128, 63, 255},
        "完整四层必须按实际 batch 先 red128 再 green128，最终才 round，不再次预乘");
}

void TestBilinearByteSpace() {
    auto atlas = Atlas();
    for (std::uint32_t y = 0; y < 16; ++y) for (std::uint32_t x = 0; x < 16; ++x) {
        const Rgba color{40 + 40 * (x % 4), 20 + 40 * (y % 4), 60, 255};
        for (std::size_t c = 0; c < 4; ++c) atlas->image.rgba[(static_cast<std::size_t>(y) * 16 + x) * 4 + c] = static_cast<std::byte>(color[c]);
    }
    auto input = Fixture(); input.groundAtlases[0] = atlas;
    const auto result = Build(input);
    // texel coordinate = 3*(pixel+0.5)/256。pixel(95,159) 给出
    // (1.119140625,1.869140625)，RGB=(84.765625,94.765625,60)。
    Require(Pixel(result, 95, 159) == Rgba{85, 95, 60, 255}
        && Pixel(result, 127, 127) == Rgba{100, 80, 60, 255}, "双线性必须在 UNORM 字节数值空间计算，最终才 round");
}

void TestOriginalTrilinearAndCellClamp() {
    // 独立手算 oracle：3×1 tiles，cell128，rho=127*3/256=381/256。
    // pixel(170,128) 的 local fx=511/512，mip1 x=63.1259765625，必须夹到63。
    // lambda=log2(381/256)，blue200→green200 三线性后最终 (0,115,85)。
    auto atlas = Atlas(128, {255, 0, 255, 255});
    SetCell(atlas->image, 128, 0, 0, {0, 0, 200, 255});
    atlas->additionalMips.push_back(Solid(256, 256, {255, 0, 255, 255}));
    SetCell(atlas->additionalMips[0], 64, 0, 0, {0, 200, 0, 255}); atlas->validatedMipCount = 2;
    auto input = Fixture(4, 2); input.groundAtlases[0] = atlas;
    const auto result = Build(input);
    Require(Pixel(result, 170, 128) == Rgba{0, 115, 85, 255}, "原链三线性必须用真实导数，并在每个 mip 内夹紧 cell，不能漏入 magenta 邻格");
    Require(Pixel(result, 170, 84) == Rgba{0, 0, 0, 255} && Pixel(result, 170, 171) == Rgba{0, 0, 0, 255}
        && Pixel(result, 170, 85) == Rgba{0, 115, 85, 255} && Pixel(result, 170, 170) == Rgba{0, 115, 85, 255},
        "非整数 fit padding 不能预先取整");
    const auto& facts = result.manifest.atlases.front();
    Near(facts.maximumRequestedLod, std::log2(381.0 / 256.0), "LOD 必须来自实际 UV 足迹");
    Near(facts.maximumSampledLod, std::log2(381.0 / 256.0), "可用原始 mip 必须参与采样");
    Require(facts.originalMaxMip == 1 && facts.cellSafeMaxMip == 7 && facts.usableMaxMip == 1
        && !facts.missingOriginalMipsLimitedSampling && !facts.cellBoundaryLimitedSampling, "安全原链 cap facts 错误");
    auto slopeInput = input; HeightRaw(slopeInput, 1, 8192 + 4096);
    const auto slope = Build(slopeInput);
    Require(Pixel(slope, 170, 128) == Rgba{0, 115, 85, 255}
        && Pixel(slope, 170, 84) == Rgba{0, 0, 0, 255} && Pixel(slope, 170, 171) == Rgba{0, 0, 0, 255}
        && Pixel(slope, 170, 85) == Rgba{0, 115, 85, 255} && Pixel(slope, 170, 170) == Rgba{0, 115, 85, 255},
        "陡普通坡面必须保持手算三线性与非整数clip像素，不能改用3D边长LOD");
    Near(slope.manifest.atlases[0].maximumRequestedLod, std::log2(381.0 / 256.0), "坡度不改变顶视UV足迹");
    Near(slope.manifest.atlases[0].maximumSampledLod, std::log2(381.0 / 256.0), "坡度不改变原链取样LOD");
    Require(slope.image.rgba == result.image.rgba && slope.encoded.bytes == result.encoded.bytes
        && slope.manifest.meshSha256 != result.manifest.meshSha256, "相同顶视像素/BLP不能掩盖真实mesh高程身份");
    atlas->additionalMips.clear(); atlas->validatedMipCount = 1;
    const auto topOnly = Build(input);
    Require(Pixel(topOnly, 170, 128) == Rgba{0, 0, 200, 255}, "原图缺 lower 时只能 clamp 到 mip0，不能伪造 chain");
    Require(topOnly.manifest.atlases[0].missingOriginalMipsLimitedSampling
        && topOnly.manifest.atlases[0].maximumSampledLod == 0 && topOnly.manifest.atlases[0].mips.size() == 1,
        "缺 lower 必须显式记录且实际解码 facts 只含原级");
}

void TestSafeMipCapsAndOddCells() {
    auto atlas = Atlas(4, {0, 0, 200, 255});
    atlas->additionalMips = {Solid(8, 8, {0, 200, 0, 255}), Solid(4, 4, {20, 40, 60, 255}),
        Solid(2, 2, {250, 250, 0, 255}), Solid(1, 1, {255, 0, 255, 255})};
    atlas->validatedMipCount = 5;
    auto input = Fixture(513, 3); input.groundAtlases[0] = atlas;
    const auto result = Build(input);
    Require(Pixel(result, 127, 127) == Rgba{20, 40, 60, 255}, "每 cell 退化为 1 texel 后不能继续采全 atlas 小 mip");
    const auto& facts = result.manifest.atlases[0];
    Require(facts.originalMaxMip == 4 && facts.cellSafeMaxMip == 2 && facts.usableMaxMip == 2
        && facts.cellBoundaryLimitedSampling && !facts.missingOriginalMipsLimitedSampling, "cell 几何 cap 与缺原链必须分开");
    Near(facts.maximumRequestedLod, std::log2(6.0), "cell4 与512 tiles 的足迹为6 texels");
    Near(facts.maximumSampledLod, 2, "采样末级必须 cap2");
    atlas = Atlas(3, {0, 0, 200, 255});
    atlas->additionalMips = {Solid(6, 6, {200, 0, 0, 255}), Solid(3, 3, {0, 200, 0, 255}), Solid(1, 1, {255, 0, 255, 255})};
    atlas->validatedMipCount = 4;
    input = Fixture(257, 2); input.groundAtlases[0] = atlas;
    const auto odd = Build(input);
    Require(Pixel(odd, 127, 127) == Rgba{0, 0, 200, 255} && odd.manifest.atlases[0].cellSafeMaxMip == 0
        && odd.manifest.atlases[0].cellBoundaryLimitedSampling, "奇数 cell 的下级不再有安全整数边界，不能跨 cell 借 texel");
    input = Fixture(); input.groundAtlases[0] = Atlas(1, {1, 2, 3, 0}, 8);
    const auto single = Build(input);
    Require(Pixel(single, 127, 127) == Rgba{1, 2, 3, 255} && single.manifest.atlases[0].cellSafeMaxMip == 0,
        "8×4 布局的1texel cell 应安全常色采样且不读取不存在 mip");
}

void TestIdentityAndHonestProvenance() {
    auto input = Fixture();
    const auto first = Build(input);
    Require(first.image.Valid() && first.image.width == 256 && first.image.height == 256, "固定输出图像尺寸无效");
    Require(first.manifest.currentW3eSha256 == input.binding.terrainSha256
        && first.manifest.reserializedW3eSha256 == input.binding.terrainSha256
        && first.manifest.outputRgbaSha256 == *Sha256(first.image.rgba).value, "当前字节、内部重序列化及实际输出 SHA 必须各自核对");
    Require(!first.manifest.archiveSourceIndependentlyVerified && !first.manifest.encodedAtlasSourcesIndependentlyVerified
        && !first.manifest.resourceWinnersIndependentlyVerified && !first.manifest.nativeEditorOrGameVerified
        && !first.manifest.formalSaveAuthorized, "CPU 候选不能自授来源、原版或保存验收资格");
    auto changed = input;
    changed.currentW3e[Record(changed, 0) + 5] = std::byte{1};
    Rejected(changed, "TERRAIN_MINIMAP_IDENTITY");
    changed = input; changed.binding.terrainSha256[0] = 'A'; Rejected(changed, "TERRAIN_MINIMAP_BINDING");
    changed = input; changed.binding.documentId.assign(129, 'x'); Rejected(changed, "TERRAIN_MINIMAP_BINDING");
    changed = input; changed.binding.documentId = "another-document"; ++changed.binding.revision;
    const auto revision = Build(changed);
    Require(revision.image.rgba == first.image.rgba && revision.manifest.manifestSha256 != first.manifest.manifestSha256,
        "相同像素也必须保留不同文档/修订身份");
    changed = input; changed.binding.sourceSha256.assign(64, 'c');
    const auto hostSource = Build(changed);
    Require(hostSource.image.rgba == first.image.rgba && hostSource.manifest.manifestSha256 != first.manifest.manifestSha256
        && !hostSource.manifest.archiveSourceIndependentlyVerified, "宿主 archive 身份是声明，不可冒充归档实际重读");
    auto mutableAtlas = std::make_shared<TerrainAtlasSnapshot>(*input.groundAtlases[0]);
    mutableAtlas->sourceSha256.assign(64, 'd'); changed = input; changed.groundAtlases[0] = mutableAtlas;
    const auto declared = Build(changed);
    Require(declared.manifest.atlases[0].declaredEncodedSha256 == std::string(64, 'd')
        && declared.manifest.atlases[0].mips[0].decodedRgbaSha256 == first.manifest.atlases[0].mips[0].decodedRgbaSha256
        && declared.manifest.manifestSha256 != first.manifest.manifestSha256, "声明编码身份与实际解码像素 SHA 不可混同");
    mutableAtlas->image.rgba.back() = std::byte{0}; // 未被 variation0 选中的 cell
    const auto pixelChange = Build(changed);
    Require(pixelChange.image.rgba == declared.image.rgba
        && pixelChange.manifest.atlases[0].mips[0].decodedRgbaSha256 != declared.manifest.atlases[0].mips[0].decodedRgbaSha256
        && pixelChange.manifest.manifestSha256 != declared.manifest.manifestSha256, "实际完整 mip SHA 必须捕获未采样 texel 的变化");
}

void TestUnsupportedDomainAndResources() {
    const auto original = Fixture();
    for (const auto [flag, code] : {std::pair{0x80u, "TERRAIN_MINIMAP_BOUNDARY"}, {0x10u, "TERRAIN_MINIMAP_RAMP"},
        {0x40u, "TERRAIN_MINIMAP_WATER"}, {0x20u, "TERRAIN_MINIMAP_BLIGHT"}}) {
        auto input = original; input.currentW3e[Record(input, 0) + 4] = static_cast<std::byte>(flag); Rebind(input); Rejected(input, code);
    }
    auto input = original;
    input.currentW3e[Record(input, 0) + 3] = std::byte{0x60}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_MAP_EDGE");
    input = original; input.currentW3e[Record(input, 0) + 3] = std::byte{0xa0}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_UNKNOWN_FLAGS");
    input = original; input.currentW3e[Record(input, 0) + 6] = std::byte{3}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_NOT_FLAT");
    input = original; HeightRaw(input, 0, 0x8000); Rejected(input, "TERRAIN_GROUND_HEIGHT_AMBIGUOUS");
    input = original; input.currentW3e[Record(input, 0) + 5] = std::byte{18}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_VARIATION");
    input = original; input.currentW3e[Record(input, 0) + 6] = std::byte{0x12}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_PALETTE");
    // 合法 cliff index0 已由全部正常 fixture 覆盖；sentinel15 同样允许，不能把任一合法索引当实悬崖。
    input = original;
    for (std::size_t i = 0; i < 4; ++i) input.currentW3e[Record(input, i) + 6] = std::byte{0xf2};
    Rebind(input); Require(Pixel(Build(input), 127, 127) == Rgba{90, 130, 170, 255}, "flat topology 上合法 cliff sentinel 应允许");
    input = original; Ground(input, 0, 1); Rebind(input); Rejected(input, "TERRAIN_MINIMAP_PALETTE");
    input = original; input.groundAtlases[0].reset(); Rejected(input, "TERRAIN_MINIMAP_ATLAS");
    input = original; input.groundAtlases.push_back(Atlas()); Rejected(input, "TERRAIN_MINIMAP_ATLAS");
    auto bad = Atlas(); input = original; input.groundAtlases[0] = bad;
    bad->sourceSha256 = "caller-label"; Rejected(input, "TERRAIN_MINIMAP_ATLAS_IDENTITY");
    bad->sourceSha256.assign(64, 'a'); bad->cells[0].x = 1; Rejected(input, "TERRAIN_MINIMAP_ATLAS");
    bad->cells[0].x = 0; bad->additionalMips.push_back(Solid(3, 3, {1, 2, 3, 255})); bad->validatedMipCount = 2;
    Rejected(input, "TERRAIN_MINIMAP_ATLAS");
    input = original; input.currentW3e.push_back(std::byte{0}); Rebind(input); Rejected(input);
    input = original; input.currentW3e.resize(8); Rebind(input); Rejected(input);
    input = original; input.currentW3e[4] = std::byte{13}; Rebind(input); Rejected(input);
}

void TestBudgetsBeforeFullMesh() {
    const auto input = Fixture(3, 2);
    const auto baseline = Build(input);
    const auto& memory = baseline.manifest.memory;
    Require(memory.estimatedPeakBytes == memory.retainedInputCapacityBytes + memory.estimatedExtraPeakBytes + memory.estimatedOutputBytes
        && memory.observedContainerCapacityBytes <= memory.estimatedPeakBytes
        && memory.outputCapacityBytes >= 256 * 256 * 4 && memory.parsedTerrainCapacityBytes && memory.meshCapacityBytes,
        "内存账必须区分输入保活、额外工作、输出及估算/实际capacity");
    Sha256Stream independentHash(1);
    Require(independentHash.Update({}).Succeeded(), "独立 Hash 观察必须触发惰性 CNG 初始化");
    Require(memory.maximumObservedHashScratchBytes >= independentHash.ScratchBytes()
        && memory.maximumObservedHashScratchBytes <= 1024 * 1024,
        "实际 Hash scratch 必须包括第一次 Update 后才分配的 CNG object capacity");
    TerrainMinimapLimits limits; limits.maxTiles = 1; Rejected(input, {}, limits);
    limits = {}; limits.maxW3eBytes = input.currentW3e.size() - 1; Rejected(input, "TERRAIN_MINIMAP_BUDGET", limits);
    limits = {}; limits.maxRetainedInputBytes = memory.retainedInputCapacityBytes - 1; Rejected(input, "TERRAIN_MINIMAP_BUDGET", limits);
    limits = {}; limits.maxWorkingBytes = 1024; Rejected(input, "TERRAIN_MINIMAP_BUDGET", limits);
    limits = {}; limits.maxWorkingBytes = memory.estimatedPeakBytes - 1; Rejected(input, "TERRAIN_MINIMAP_BUDGET", limits);
    limits.maxWorkingBytes = memory.estimatedPeakBytes;
    Require(Build(input, limits).image.rgba == baseline.image.rgba, "恰好满足构建前保守预算的输入应允许");
    limits = {}; ++limits.maxTiles; Rejected(input, "TERRAIN_MINIMAP_LIMITS", limits);
    limits = {}; ++limits.maxWorkingBytes; Rejected(input, "TERRAIN_MINIMAP_LIMITS", limits);
    limits = {}; ++limits.maxW3eBytes; Rejected(input, "TERRAIN_MINIMAP_LIMITS", limits);
    limits = {}; ++limits.maxRetainedInputBytes; Rejected(input, "TERRAIN_MINIMAP_LIMITS", limits);
    limits = {}; limits.maxTiles = 0; Rejected(input, "TERRAIN_MINIMAP_LIMITS", limits);
    auto capacity = input;
    auto atlas = std::make_shared<TerrainAtlasSnapshot>(*input.groundAtlases[0]);
    atlas->image.rgba.reserve(atlas->image.rgba.size() + 1024 * 1024); capacity.groundAtlases[0] = atlas;
    limits = {}; limits.maxRetainedInputBytes = memory.retainedInputCapacityBytes + 512 * 1024;
    Rejected(capacity, "TERRAIN_MINIMAP_BUDGET", limits);
    // 同一个 frozen atlas 多槽复用只计算一份 RGBA 保活，不偷偷复制 16/32 个 cell。
    auto shared = Fixture(2, 2, 2); shared.groundAtlases[1] = shared.groundAtlases[0];
    Ground(shared, 1, 1); Rebind(shared);
    const auto aliased = Build(shared);
    Require(aliased.manifest.atlases.size() == 1 && aliased.manifest.meshQuads == 2, "共享 atlas 槽身份与完整层数应分别保留");
}

void TestEncodedCandidateUsesCurrentImage() {
    auto input = Fixture();
    const auto first = Build(input);
    Require(first.encoded.mipCount == 9 && first.encoded.totalMipPixels == 87381
        && first.encoded.bytes.size() == 88561 && first.encoded.paletteColorCount == 1
        && !first.encoded.quantized && first.encoded.method == Image::BlpOpaquePaletteMethod::Exact,
        "固定256小地图必须生成九级共享调色板BLP，而非仅返回RGBA");
    const auto decoded = Image::DecodeBlp(first.encoded.bytes);
    Require(decoded.Succeeded() && decoded.value->rgba == first.image.rgba,
        "单色小地图BLP的实际像素必须来自本次RGBA");
    const auto hash = Sha256(first.encoded.bytes);
    Require(hash.Succeeded() && first.manifest.outputBlpSha256 == *hash.value
        && first.manifest.encodedBlpBytes == first.encoded.bytes.size()
        && first.manifest.encodedMipCount == first.encoded.mipCount
        && first.manifest.encodedTotalMipPixels == first.encoded.totalMipPixels
        && first.manifest.paletteColorCount == first.encoded.paletteColorCount
        && !first.manifest.formalSaveAuthorized && !first.manifest.nativeEditorOrGameVerified,
        "编码候选事实和SHA必须绑定实际字节，不能授予正式保存或原版验收");
    auto other = Atlas(4, {210, 40, 10, 255});
    input.groundAtlases[0] = other;
    const auto second = Build(input);
    const auto secondDecoded = Image::DecodeBlp(second.encoded.bytes);
    Require(secondDecoded.Succeeded() && secondDecoded.value->rgba == second.image.rgba
        && first.encoded.bytes != second.encoded.bytes
        && first.manifest.outputBlpSha256 != second.manifest.outputBlpSha256
        && first.manifest.manifestSha256 != second.manifest.manifestSha256,
        "更换当前材质后不能复用旧小地图图像或旧编码身份");
}

void TestDryHeightfieldKeepsPixelsButChangesGeometryIdentity() {
    auto input = Fixture(3, 3); input.groundAtlases[0] = Atlas(4, {12, 34, 56, 0});
    const auto flat = Build(input); const auto flatMesh = Mesh(input);
    const auto expected = Solid(256, 256, {12, 34, 56, 255});
    Require(flat.image.rgba == expected.rgba, "固定单色顶视oracle必须覆盖全部256平方像素，忽略base alpha");
    static_assert(TerrainMinimapManifest::algorithmVersion == 4);
    static_assert(TerrainMinimapManifest::rangePolicyVersion == 2);
    static_assert(TerrainMinimapMapEdgeFacts::policyVersion == 1);
    Require(std::string_view(TerrainMinimapManifest::samplingPolicy) == "original-mips-cell-clamped-trilinear-unorm-v1"
        && std::string_view(TerrainMinimapManifest::encodingPolicy) == "opaque-full-chain-shared-adaptive-palette-v1",
        "声明边距v3不能暗改默认采样/编码v1策略");
    // 156是正式保存限制；157和陡坡也属于本生成器的普通干地heightfield域。
    for (const int delta : {156, 157, 4096, -156, -157}) {
        auto changed = input; HeightRaw(changed, 4, static_cast<std::uint16_t>(8192 + delta));
        const auto result = Build(changed); const auto mesh = Mesh(changed);
        Require(result.image.rgba == expected.rgba && result.image.rgba == flat.image.rgba
            && result.encoded.bytes == flat.encoded.bytes && result.manifest.outputRgbaSha256 == flat.manifest.outputRgbaSha256
            && result.manifest.outputBlpSha256 == flat.manifest.outputBlpSha256, "普通高程变化不能改变固定顶视RGBA或确定性BLP");
        Require(result.manifest.currentW3eSha256 != flat.manifest.currentW3eSha256
            && result.manifest.reserializedW3eSha256 == result.manifest.currentW3eSha256
            && result.manifest.meshSha256 != flat.manifest.meshSha256 && result.manifest.manifestSha256 != flat.manifest.manifestSha256,
            "相同像素不能复用W3E、真实mesh或manifest身份");
        Require(mesh.vertices.size() == flatMesh.vertices.size() && mesh.indices == flatMesh.indices
            && mesh.batches.size() == flatMesh.batches.size() && mesh.blocks.size() == flatMesh.blocks.size(),
            "高度不能丢格、重排三角或改变材质批次");
        std::size_t changedCorners = 0;
        for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
            const auto& before = flatMesh.vertices[index]; const auto& after = mesh.vertices[index];
            Require(before.x == after.x && before.y == after.y && before.u == after.u && before.v == after.v
                && before.red == after.red && before.green == after.green && before.blue == after.blue,
                "只改高程时XY、UV、unlit RGB必须逐顶点不变");
            const auto center = after.x == 138 && after.y == 148; // 手写header offset(10,20)+中心(128,128)。
            Require(after.z == (center ? delta / 4.0f : 0.0f), "真实mesh必须保留手算Z，禁止为了像素相同而flatten");
            changedCorners += center;
        }
        Require(changedCorners == 4, "2乘2 tiles中心顶点应在四份quad中各出现一次");
        for (std::size_t index = 0; index < mesh.batches.size(); ++index) {
            const auto& before = flatMesh.batches[index]; const auto& after = mesh.batches[index];
            Require(before.firstIndex == after.firstIndex && before.indexCount == after.indexCount
                && before.textureIndex == after.textureIndex && before.alphaBlend == after.alphaBlend && before.translucent == after.translucent,
                "高度不改变实际draw批次布局");
        }
        Require(mesh.blocks[0].minimum[2] == std::min(0.0f, delta / 4.0f)
            && mesh.blocks[0].maximum[2] == std::max(0.0f, delta / 4.0f), "block真实Z bounds也必须变化");
        Near(result.manifest.atlases[0].maximumRequestedLod, flat.manifest.atlases[0].maximumRequestedLod, "普通坡度不改变UV LOD");
        Require(!result.manifest.formalSaveAuthorized && !result.manifest.nativeEditorOrGameVerified, "更宽生成域不能取得保存或原版资格");
    }
}

void TestDeclaredMarginsFixedPixelsAndLod() {
    auto input = Fixture(9, 7);
    auto gradient = Atlas();
    for (std::uint32_t y = 0; y < 16; ++y) for (std::uint32_t x = 0; x < 16; ++x) {
        const Rgba color{40 + 40 * (x % 4), 20 + 40 * (y % 4), 60, 255};
        for (std::size_t c = 0; c < 4; ++c) gradient->image.rgba[(y * 16 + x) * 4 + c] = static_cast<std::byte>(color[c]);
    }
    input.groundAtlases[0] = gradient;
    const auto full = Build(input);
    input.sourceW3i = W3i(); // full不消费，默认像素与BLP保持逐字相同。
    const auto ignored = Build(input);
    Require(ignored.image.rgba == full.image.rgba && ignored.encoded.bytes == full.encoded.bytes
        && ignored.manifest.w3i.state == "NotRequested" && ignored.manifest.w3i.sha256.empty(), "默认full不能隐式消费W3I");
    input.sourceW3i = std::vector<std::byte>{std::byte{99}};
    Require(Build(input).image.rgba == full.image.rgba, "未消费的损坏W3I也不能改变默认生成域");
    input.sourceW3i = W3i();
    input.rangePolicy = TerrainMinimapRangePolicy::DeclaredW3iMargins;
    const auto cropped = Build(input);
    Require(cropped.manifest.worldExtent == std::array<double, 4>{10, 20, 1034, 788}
        && cropped.manifest.renderExtent == std::array<double, 4>{138, 276, 650, 660}
        && cropped.manifest.contentRectangle == std::array<double, 4>{0, 32, 256, 192}, "非对称边距必须保留全域与声明选区独立事实");
    Require(cropped.manifest.w3i.sha256 == *Sha256(*input.sourceW3i).value && cropped.manifest.w3i.stage == "source-archive"
        && cropped.manifest.w3i.cameraBounds[0] == 1 && cropped.manifest.w3i.cameraBounds[7] == 8,
        "实际W3I bytes必须自产SHA且原cameraBounds不应强制等于crop");
    Require(Pixel(cropped, 23, 63) == Rgba{84, 79, 60, 255}
        && Pixel(cropped, 0, 32) == Rgba{41, 21, 60, 255}
        && Pixel(cropped, 255, 223) == Rgba{159, 139, 60, 255}, "手算原tile局部UV固定像素不能被重编号或二次翻Y");
    for (const auto y : {31u, 224u}) for (std::uint32_t x = 0; x < 256; ++x)
        Require(Pixel(cropped, x, y) == Rgba{0, 0, 0, 255}, "半开padding必须全行opaque黑");
    Require(cropped.manifest.meshSha256 == full.manifest.meshSha256 && cropped.manifest.sourceTiles == 48
        && !cropped.manifest.archiveSourceIndependentlyVerified && !cropped.manifest.nativeEditorOrGameVerified
        && !cropped.manifest.formalSaveAuthorized, "pure范围数学不能授予来源、native或保存资格，也不缩原mesh");
    auto atlas = Atlas(128, {255, 0, 255, 255});
    SetCell(atlas->image, 128, 0, 0, {0, 0, 200, 255});
    atlas->additionalMips.push_back(Solid(256, 256, {255, 0, 255, 255}));
    atlas->additionalMips.push_back(Solid(128, 128, {255, 0, 255, 255}));
    SetCell(atlas->additionalMips[0], 64, 0, 0, {0, 200, 0, 255});
    SetCell(atlas->additionalMips[1], 32, 0, 0, {200, 0, 0, 255}); atlas->validatedMipCount = 3;
    input.groundAtlases[0] = atlas;
    const auto cropMip = Build(input);
    input.rangePolicy = TerrainMinimapRangePolicy::FullW3e;
    const auto fullMip = Build(input);
    Require(Pixel(cropMip, 23, 63) == Rgba{0, 198, 2, 255} && Pixel(fullMip, 23, 63) == Rgba{198, 2, 0, 255},
        "原三mip固定颜色应区分crop/full足迹，不能只改反查或最后裁图");
    Near(cropMip.manifest.atlases[0].maximumRequestedLod, std::log2(127.0 / 64), "crop足迹=127乘4/256");
    Near(fullMip.manifest.atlases[0].maximumSampledLod, std::log2(127.0 / 32), "full足迹=127乘8/256");
    Require(cropMip.manifest.memory.estimatedSettingsScratchBytes >= cropMip.manifest.memory.parsedSettingsCapacityBytes
        && cropMip.manifest.memory.parsedSettingsCapacityBytes > input.sourceW3i->size(), "解析临时容量必须进入旧working账");
}

void TestDeclaredMarginsPolicyAndRefusals() {
    auto input = Fixture(9, 7); input.rangePolicy = TerrainMinimapRangePolicy::DeclaredW3iMargins;
    Rejected(input, "TERRAIN_MINIMAP_W3I_MISSING");
    input.sourceW3i.emplace(); Rejected(input, "TERRAIN_MINIMAP_W3I_EMPTY");
    input.sourceW3i = W3i(); const auto original = Build(input);
    for (unsigned side = 0; side < 4; ++side) {
        std::array<std::int32_t, 4> margins{1, 3, 2, 1}; ++margins[side];
        input.sourceW3i = W3i(margins, side < 2 ? 3 : 4, side >= 2 ? 2 : 3);
        const auto changed = Build(input); auto expected = original.manifest.renderExtent;
        expected[side == 0 ? 0 : side == 1 ? 2 : side == 2 ? 1 : 3] += side == 0 || side == 2 ? 128 : -128;
        Require(changed.manifest.renderExtent == expected && changed.manifest.w3i.sha256 != original.manifest.w3i.sha256,
            "只改一边必须只移动对应世界边，不从playable猜居中原点");
    }
    input.sourceW3i = W3i({-1, 3, 2, 1}); Rejected(input, "TERRAIN_MINIMAP_W3I_MARGINS");
    input.sourceW3i = W3i({std::numeric_limits<std::int32_t>::max(), 3, 2, 1}); Rejected(input, "TERRAIN_MINIMAP_W3I_MARGINS");
    input.sourceW3i = W3i({5, 3, 2, 1}, 0, 3); Rejected(input, "TERRAIN_MINIMAP_W3I_MARGINS");
    input.sourceW3i = W3i({1, 3, 5, 1}, 4, 0); Rejected(input, "TERRAIN_MINIMAP_W3I_MARGINS");
    input.sourceW3i = W3i({1, 3, 2, 1}, 5, 3); Rejected(input, "TERRAIN_MINIMAP_W3I_MARGINS");
    input.sourceW3i = W3i(); (*input.sourceW3i)[0] = std::byte{99}; Rejected(input, "W3I_UNSUPPORTED_VERSION");
    input.sourceW3i = W3i({1, 3, 2, 1}, 4, 3, 28); Rejected(input, "TERRAIN_MINIMAP_W3I_POLICY_LAYOUT");
    input.sourceW3i = W3i(); (*input.sourceW3i)[18] = std::byte{192}; (*input.sourceW3i)[19] = std::byte{127}; Rejected(input, "W3I_NONFINITE");
    input.sourceW3i = W3i(); input.sourceW3i->push_back(std::byte{0}); Rejected(input, "TERRAIN_MINIMAP_W3I_PARTIAL");
    input.sourceW3i = W3i(); const auto baseline = Build(input);
    // 只改原name字符串：语义选区/像素相同，整字SHA与manifest仍必须改变。
    input.sourceW3i->insert(input.sourceW3i->begin() + 12, std::byte{'N'});
    const auto renamed = Build(input);
    Require(renamed.image.rgba == baseline.image.rgba && renamed.encoded.bytes == baseline.encoded.bytes
        && renamed.manifest.w3i.sha256 != baseline.manifest.w3i.sha256 && renamed.manifest.manifestSha256 != baseline.manifest.manifestSha256,
        "同像素不能掩盖W3I实际整字依赖变化");
    input.sourceW3i = W3i(); const auto dry = input;
    // SW位于crop外，仍逐一校验全源拒绝域，不能因选区变小而开放复杂内容。
    for (const auto [flag, code] : {std::pair{0x80u, "TERRAIN_MINIMAP_BOUNDARY"}, {0x10u, "TERRAIN_MINIMAP_RAMP"},
        {0x40u, "TERRAIN_MINIMAP_WATER"}, {0x20u, "TERRAIN_MINIMAP_BLIGHT"}}) {
        input = dry; input.currentW3e[Record(input, 0) + 4] = static_cast<std::byte>(flag); Rebind(input); Rejected(input, code);
    }
    input = dry; input.currentW3e[Record(input, 0) + 3] = std::byte{0x60}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_MAP_EDGE");
    input = dry; input.currentW3e[Record(input, 0) + 3] = std::byte{0xa0}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_UNKNOWN_FLAGS");
    input = dry; input.currentW3e[Record(input, 0) + 6] = std::byte{3}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_NOT_FLAT");
    input = Fixture(9, 7); input.rangePolicy = TerrainMinimapRangePolicy::DeclaredW3iMargins; input.sourceW3i = W3i();
    auto limits = TerrainMinimapLimits{}; limits.maxRetainedInputBytes = Build(input).manifest.memory.retainedInputCapacityBytes - 1;
    Rejected(input, "TERRAIN_MINIMAP_BUDGET", limits);
    limits = {}; limits.maxWorkingBytes = Build(input).manifest.memory.estimatedPeakBytes - 1;
    Rejected(input, "TERRAIN_MINIMAP_BUDGET", limits);
    input.rangePolicy = static_cast<TerrainMinimapRangePolicy>(42); Rejected(input, "TERRAIN_MINIMAP_RANGE_POLICY");
}

void TestDeclaredExteriorMapEdgeCreateBytesAndSharedCorners() {
    auto input = CreateTerrainFixture(); const auto original = input.currentW3e;
    const auto flat = Build(input); const auto flatMesh = Mesh(input);
    Require(input.currentW3e == original && flat.manifest.currentW3eSha256 == *Sha256(original).value
        && flat.manifest.reserializedW3eSha256 == flat.manifest.currentW3eSha256,
        "标准Create原字与MapEdge不得为生成而清位或改写，内部重序列化须逐字一致");
    Require(flat.manifest.rangePolicy == TerrainMinimapRangePolicy::DeclaredW3iMarginsExteriorMapEdge
        && flat.manifest.worldExtent == std::array<double, 4>{-4096, -4096, 4096, 4096}
        && flat.manifest.renderExtent == std::array<double, 4>{-3328, -3584, 3328, 3072}
        && flat.manifest.contentRectangle == std::array<double, 4>{0, 0, 256, 256},
        "标准Create须由真实非对称W3I边距取52乘52半开域，不能改用cameraBounds");
    std::vector<std::byte> expectedMask;
    for (std::size_t y = 0; y < 65; ++y) for (std::size_t x = 0; x < 65; ++x)
        expectedMask.push_back(x < 64 && y < 64 && (x < 6 || x >= 58 || y < 4 || y >= 56) ? std::byte{1} : std::byte{0});
    const auto& edge = flat.manifest.mapEdge;
    Require(edge.state == "Validated" && edge.maskSha256 == *Sha256(expectedMask).value
        && edge.sourceTileAnchors == 4096 && edge.declaredTileAnchors == 2704
        && edge.exteriorMapEdgeAnchors == 1392 && edge.terminalClearVertices == 129,
        "mask事实须绑定完整row-major逐vertex单byte，SW外域和末行列分别计数");
    Require(std::string_view(TerrainMinimapMapEdgeFacts::maskPolicy)
        == "declared-w3i-margins-sw-anchor-exterior-terminal-clear-v1", "精确mask政策必须版本化");
    const auto parsed = MapData::ParseMapTerrain(original);
    Require(parsed.Succeeded(), "标准原字必须可解析");
    const auto grid = parsed.value->Snapshot();
    Require(!grid->vertices[55 * 65 + 57].IsMapEdge() && grid->vertices[55 * 65 + 58].IsMapEdge()
        && grid->vertices[56 * 65 + 57].IsMapEdge() && grid->vertices[56 * 65 + 58].IsMapEdge(),
        "最后内部quad允许SE/NW/NE三共享角为MapEdge，不能误做quad四角clear政策");
    Require(flat.manifest.sourceTiles == 4096 && flat.manifest.meshQuads == 4096
        && flatMesh.vertices.size() == 4096 * 4 && flatMesh.indices.size() == 4096 * 6
        && flatMesh.budget.base.displayed == 4096 && !flatMesh.budget.reduced && !flatMesh.hiddenTileCount,
        "声明采样域不能缩完整Strict网格、原tile索引或基础层预算");
    const auto shared = static_cast<std::size_t>(55 * 64 + 57) * 4;
    Require(flatMesh.vertices[shared].x == 3200 && flatMesh.vertices[shared].y == 2944
        && flatMesh.vertices[shared + 1].x == 3328 && flatMesh.vertices[shared + 2].y == 3072
        && flatMesh.vertices[shared + 3].x == 3328 && flatMesh.vertices[shared + 3].y == 3072,
        "共享边缘quad须追溯原tile57,55与原XY，不可重编号或裁断");
    auto legacy = input; legacy.rangePolicy = TerrainMinimapRangePolicy::FullW3e; Rejected(legacy, "TERRAIN_MINIMAP_MAP_EDGE");
    legacy.rangePolicy = TerrainMinimapRangePolicy::DeclaredW3iMargins; Rejected(legacy, "TERRAIN_MINIMAP_MAP_EDGE");
    // 原保存门范围内的非空中心高程；仅验证Render保留真实Z，不自行宣称SaveGate授权。
    const auto center = CreateRecord(32, 32); input.currentW3e[center] = std::byte{156}; Rebind(input);
    const auto raised = Build(input); const auto raisedMesh = Mesh(input);
    Require(input.currentW3e[center + 3] == original[center + 3]
        && raised.image.rgba == flat.image.rgba && raised.encoded.bytes == flat.encoded.bytes
        && raised.manifest.meshSha256 != flat.manifest.meshSha256
        && raised.manifest.mapEdge.maskSha256 == edge.maskSha256
        && raised.manifest.manifestSha256 != flat.manifest.manifestSha256,
        "中心raw156应保持原MapEdge与顶视像素，同时保留非空高程/mesh/manifest身份");
    std::size_t centerCopies = 0;
    for (std::size_t i = 0; i < flatMesh.vertices.size(); ++i) {
        const auto& before = flatMesh.vertices[i]; const auto& after = raisedMesh.vertices[i];
        Require(before.x == after.x && before.y == after.y && before.u == after.u && before.v == after.v,
            "中心高度改动不能改变任何原XY/UV");
        const bool centerVertex = after.x == 0 && after.y == 0;
        Require(after.z == (centerVertex ? 39.0f : 0.0f), "中心真实Z必须是raw156除4而非flatten"); centerCopies += centerVertex;
    }
    Require(centerCopies == 4 && raisedMesh.indices == flatMesh.indices, "完整原格中心应在四quad各保留一次");
    auto renamed = input; renamed.sourceW3i->insert(renamed.sourceW3i->begin() + 12, std::byte{'N'});
    const auto samePixels = Build(renamed);
    Require(samePixels.image.rgba == raised.image.rgba && samePixels.manifest.mapEdge.maskSha256 == edge.maskSha256
        && samePixels.manifest.w3i.sha256 != raised.manifest.w3i.sha256
        && samePixels.manifest.manifestSha256 != raised.manifest.manifestSha256,
        "同mask同像素仍须绑定W3I整字来源，不可只hash边距语义");
    Require(!raised.manifest.archiveSourceIndependentlyVerified && !raised.manifest.nativeEditorOrGameVerified
        && !raised.manifest.formalSaveAuthorized, "新显式mask政策不能自授来源/native/保存权限");
    const auto unrequested = Build(Fixture()).manifest.mapEdge;
    Require(unrequested.state == "NotRequested" && unrequested.maskSha256.empty()
        && !unrequested.sourceTileAnchors && !unrequested.declaredTileAnchors
        && !unrequested.exteriorMapEdgeAnchors && !unrequested.terminalClearVertices, "旧Full无mask请求不能伪造Validated事实");
}

void TestDeclaredExteriorMapEdgeRefusals() {
    const auto original = CreateTerrainFixture();
    for (const auto [x, y, edge] : {std::tuple{6u, 4u, true}, std::tuple{57u, 55u, true},
        std::tuple{0u, 32u, false}, std::tuple{60u, 32u, false}, std::tuple{32u, 0u, false}, std::tuple{32u, 60u, false},
        std::tuple{64u, 0u, true}, std::tuple{0u, 64u, true}, std::tuple{64u, 64u, true}}) {
        auto input = original; CreateMapEdge(input, x, y, edge); Rejected(input, "TERRAIN_MINIMAP_MAP_EDGE_MASK");
    }
    for (unsigned side = 0; side < 4; ++side) {
        auto input = original; std::array<std::int32_t, 4> margins{6, 6, 4, 8}; ++margins[side];
        input.sourceW3i = W3i(margins, side < 2 ? 51 : 52, side >= 2 ? 51 : 52);
        Rejected(input, "TERRAIN_MINIMAP_MAP_EDGE_MASK");
    }
    auto input = original; input.sourceW3i = W3i({6, 6, 4, 8}, 51, 52); Rejected(input, "TERRAIN_MINIMAP_W3I_MARGINS");
    input = original; input.sourceW3i.reset(); Rejected(input, "TERRAIN_MINIMAP_W3I_MISSING");
    input = original; input.sourceW3i.emplace(); Rejected(input, "TERRAIN_MINIMAP_W3I_EMPTY");
    input = original; input.sourceW3i->push_back(std::byte{0}); Rejected(input, "TERRAIN_MINIMAP_W3I_PARTIAL");
    input = original; input.sourceW3i = W3i({6, 6, 4, 8}, 52, 52, 28); Rejected(input, "TERRAIN_MINIMAP_W3I_POLICY_LAYOUT");
    // 每个点仍在完整源拒域：0,0为外域且本来就带MapEdge，32,32为内部，64,64为终端。
    for (const auto [x, y] : {std::pair{0u, 0u}, std::pair{32u, 32u}, std::pair{64u, 64u}}) {
        for (const auto [flag, code] : {std::pair{0x80u, "TERRAIN_MINIMAP_BOUNDARY"}, {0x10u, "TERRAIN_MINIMAP_RAMP"},
            {0x40u, "TERRAIN_MINIMAP_WATER"}, {0x20u, "TERRAIN_MINIMAP_BLIGHT"}}) {
            input = original; input.currentW3e[CreateRecord(x, y) + 4] = static_cast<std::byte>(flag); Rebind(input); Rejected(input, code);
        }
        input = original; input.currentW3e[CreateRecord(x, y) + 3] |= std::byte{0x80}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_UNKNOWN_FLAGS");
        input = original; input.currentW3e[CreateRecord(x, y) + 6] = std::byte{0xf3}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_NOT_FLAT");
    }
    input = original; input.currentW3e[CreateRecord(0, 0) + 4] = std::byte{6}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_PALETTE");
    input = original; input.currentW3e[CreateRecord(0, 0) + 6] = std::byte{0x22}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_PALETTE");
    input = original; input.currentW3e[CreateRecord(0, 0) + 5] = std::byte{18}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_VARIATION");
}

void TestDeclaredExteriorMapEdgeRealFootprintAndBudgets() {
    auto input = CreateTerrainFixture();
    auto atlas = Atlas(8, {255, 0, 255, 255}); SetCell(atlas->image, 8, 0, 0, {0, 0, 200, 255});
    atlas->additionalMips.push_back(Solid(16, 16, {255, 0, 255, 255}));
    atlas->additionalMips.push_back(Solid(8, 8, {255, 0, 255, 255}));
    SetCell(atlas->additionalMips[0], 4, 0, 0, {0, 200, 0, 255});
    SetCell(atlas->additionalMips[1], 2, 0, 0, {200, 0, 0, 255}); atlas->validatedMipCount = 3;
    input.groundAtlases.assign(6, atlas);
    const auto crop = Build(input);
    // rho=(8-1)*52/256=91/64，lambda=log2(91/64)，byte空间最终(0,102,98)。
    Require(Pixel(crop, 127, 127) == Rgba{0, 102, 98, 255}, "声明52tile原足迹必须参与mip混合，不能先Full256再裁缩");
    Near(crop.manifest.atlases[0].maximumRequestedLod, std::log2(91.0 / 64), "真实声明域足迹须独立手算");
    Near(crop.manifest.atlases[0].maximumSampledLod, std::log2(91.0 / 64), "新mask不改变原链cell安全采样");
    // 单独的无MapEdge Full对照只证明采样数学，不作为标准源准入路径。
    auto fullInput = input;
    for (std::size_t y = 0; y < 65; ++y) for (std::size_t x = 0; x < 65; ++x)
        fullInput.currentW3e[CreateRecord(x, y) + 3] = std::byte{32};
    Rebind(fullInput); fullInput.rangePolicy = TerrainMinimapRangePolicy::FullW3e;
    const auto full = Build(fullInput);
    Require(Pixel(full, 127, 127) == Rgba{0, 161, 39, 255}
        && crop.manifest.meshSha256 == full.manifest.meshSha256 && crop.image.rgba != full.image.rgba,
        "独立Full足迹7/4颜色不同，而mesh始终保留同一4096原格");
    const auto& memory = crop.manifest.memory;
    Require(memory.estimatedSettingsScratchBytes == TerrainMinimapMemoryFacts::settingsScratchFixedBytes
        + TerrainMinimapMemoryFacts::settingsScratchBytesPerSourceByte * input.sourceW3i->size()
        && memory.parsedSettingsCapacityBytes > input.sourceW3i->size()
        && memory.parsedSettingsCapacityBytes <= memory.estimatedSettingsScratchBytes
        && memory.outputCapacityBytes >= sizeof(TerrainMinimapCandidate) + crop.manifest.mapEdge.maskSha256.capacity() + 1
        && memory.observedContainerCapacityBytes <= memory.estimatedPeakBytes, "新mask字符串与Settings解析必须进入capacity/working账");
    TerrainMinimapLimits limits; limits.maxTiles = 2704; Rejected(input, "TERRAIN_TILE_LIMIT", limits);
    limits = {}; limits.maxRetainedInputBytes = memory.retainedInputCapacityBytes - 1; Rejected(input, "TERRAIN_MINIMAP_BUDGET", limits);
    limits = {}; limits.maxWorkingBytes = memory.estimatedPeakBytes - 1; Rejected(input, "TERRAIN_MINIMAP_BUDGET", limits);
    limits.maxWorkingBytes = memory.estimatedPeakBytes;
    Require(Build(input, limits).encoded.bytes == crop.encoded.bytes, "新策略精确working边界应可容纳完整网格和mask事实");
    auto oversized = input; oversized.sourceW3i->reserve(input.sourceW3i->capacity() + 1024 * 1024);
    limits = {}; limits.maxRetainedInputBytes = memory.retainedInputCapacityBytes + 512 * 1024;
    Rejected(oversized, "TERRAIN_MINIMAP_BUDGET", limits);
}

} // namespace

int main() {
    try {
        TestDirectionAndFit(); TestGroupedDrawAndAllLayers(); TestBilinearByteSpace(); TestOriginalTrilinearAndCellClamp();
        TestSafeMipCapsAndOddCells(); TestIdentityAndHonestProvenance(); TestUnsupportedDomainAndResources(); TestBudgetsBeforeFullMesh();
        TestEncodedCandidateUsesCurrentImage();
        TestDryHeightfieldKeepsPixelsButChangesGeometryIdentity();
        TestDeclaredMarginsFixedPixelsAndLod(); TestDeclaredMarginsPolicyAndRefusals();
        TestDeclaredExteriorMapEdgeCreateBytesAndSharedCorners(); TestDeclaredExteriorMapEdgeRefusals();
        TestDeclaredExteriorMapEdgeRealFootprintAndBudgets();
        std::cout << "TerrainMinimapCandidateTests: all tests passed\n"; return 0;
    } catch (const std::exception& exception) {
        std::cerr << "TerrainMinimapCandidateTests: " << exception.what() << '\n'; return 1;
    }
}
