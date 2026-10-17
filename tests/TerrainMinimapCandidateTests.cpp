#include "War3MapCore/Render/TerrainMinimapCandidate.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

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
    input = original; input.currentW3e[Record(input, 0)] = std::byte{1}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_NOT_FLAT");
    input = original; input.currentW3e[Record(input, 0) + 6] = std::byte{3}; Rebind(input); Rejected(input, "TERRAIN_MINIMAP_NOT_FLAT");
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

} // namespace

int main() {
    try {
        TestDirectionAndFit(); TestGroupedDrawAndAllLayers(); TestBilinearByteSpace(); TestOriginalTrilinearAndCellClamp();
        TestSafeMipCapsAndOddCells(); TestIdentityAndHonestProvenance(); TestUnsupportedDomainAndResources(); TestBudgetsBeforeFullMesh();
        std::cout << "TerrainMinimapCandidateTests: all tests passed\n"; return 0;
    } catch (const std::exception& exception) {
        std::cerr << "TerrainMinimapCandidateTests: " << exception.what() << '\n'; return 1;
    }
}
