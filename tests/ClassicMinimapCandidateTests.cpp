#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Render/ClassicMinimapCandidate.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace War3Map;
using namespace War3Map::Render;
using Bytes = std::vector<std::byte>;

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

template<class T> T Take(Result<T> result) {
    Require(result.Succeeded() && result.value, "fixture dependency failed");
    return std::move(*result.value);
}

void Put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::byte>((value >> (i * 8)) & 255);
}

void Put16(Bytes& bytes, std::size_t at, std::uint16_t value) {
    bytes[at] = static_cast<std::byte>(value & 255); bytes[at + 1] = static_cast<std::byte>(value >> 8);
}

Bytes W3e(bool boundary = false, bool blight = false, bool variation = false,
    bool water = false, bool cliff = false, bool mapEdge = false) {
    constexpr std::uint32_t width = 3, height = 3;
    Bytes bytes(45 + width * height * 7);
    const auto put = [&](std::size_t at, std::string_view text) {
        for (std::size_t i = 0; i < text.size(); ++i) bytes[at + i] = static_cast<std::byte>(text[i]);
    };
    put(0, "W3E!"); Put32(bytes, 4, 11); bytes[8] = std::byte{'L'};
    Put32(bytes, 13, 1); put(17, "Ldrt"); Put32(bytes, 21, 1); put(25, "CLdi");
    Put32(bytes, 29, width); Put32(bytes, 33, height);
    for (std::size_t at = 45; at < bytes.size(); at += 7) {
        Put16(bytes, at, 8192); Put16(bytes, at + 2, mapEdge ? 0x4000 : 8192);
        bytes[at + 4] = static_cast<std::byte>((boundary ? 0x80 : 0) | (blight ? 0x20 : 0));
        bytes[at + 5] = static_cast<std::byte>(variation ? 17 : 0);
        bytes[at + 6] = static_cast<std::byte>(cliff && at == 45 ? 0x13 : 0xf2);
    }
    if (water) bytes[45 + 4] = std::byte{0x40};
    return bytes;
}

Bytes TwoGroundW3e() {
    // 顺序写header，不沿用单槽fixture的固定偏移。2 ground + 1 cliff -> 49字节。
    Bytes bytes;
    const auto raw = [&](std::string_view text) {
        for (const auto c : text) bytes.push_back(static_cast<std::byte>(c));
    };
    const auto u32 = [&](std::uint32_t value) {
        const auto at = bytes.size(); bytes.resize(at + 4); Put32(bytes, at, value);
    };
    raw("W3E!"); u32(11); bytes.push_back(std::byte{'L'}); u32(0);
    u32(2); raw("LdrtLgrs"); u32(1); raw("CLdi"); u32(3); u32(3); u32(0); u32(0);
    Require(bytes.size() == 49, "双ground fixture必须为实际header长度");
    for (std::uint32_t y = 0; y < 3; ++y) for (std::uint32_t x = 0; x < 3; ++x) {
        const auto at = bytes.size(); bytes.resize(at + 7);
        Put16(bytes, at, 8192); Put16(bytes, at + 2, 8192);
        // 四SW锚点按南行0/1、北行1/0。terminal东/北顶点槽15是负控：
        // 它不在两个ground槽中；若误作逐格锚点就应失败或得到错误颜色。
        const auto slot = x == 2 || y == 2 ? 15u : (x + y) % 2;
        bytes[at + 4] = static_cast<std::byte>(slot | (x == 0 && y == 0 ? 0x80u : 0u));
        bytes[at + 5] = std::byte{0}; bytes[at + 6] = std::byte{0xf2};
    }
    return bytes;
}

std::shared_ptr<const TerrainAtlasSnapshot> Atlas(std::array<std::uint8_t, 4> color,
    bool malformed = false) {
    auto result = std::make_shared<TerrainAtlasSnapshot>();
    result->source = {"TerrainArt\\\\Ldrt.blp", Resources::ResourceLayer::Game,
        Resources::ResourcePriority::GameBase, "classic-test-game", "TerrainArt\\\\Ldrt.blp"};
    result->sourceSha256 = malformed ? "bad" : std::string(64, 'a');
    result->sourceBytes = 16 * 16 * 4; result->sourceWidth = result->sourceHeight = 16;
    result->validatedMipCount = 1; result->columns = 4;
    result->image.width = result->image.height = 16;
    result->image.rgba.resize(16 * 16 * 4);
    for (std::size_t pixel = 0; pixel < 16 * 16; ++pixel)
        for (std::size_t channel = 0; channel < 4; ++channel)
            result->image.rgba[pixel * 4 + channel] = static_cast<std::byte>(color[channel]);
    for (std::uint32_t row = 0; row < 4; ++row) for (std::uint32_t column = 0; column < 4; ++column)
        result->cells.push_back({column, row, column * 4, row * 4, 4, 4});
    return result;
}

ClassicMinimapCandidateInput Input(Bytes terrain) {
    ClassicMinimapCandidateInput input;
    input.terrain.currentW3e = terrain;
    input.terrain.binding.documentId = "classic-test-document";
    input.terrain.binding.documentGeneration = 1;
    input.terrain.binding.revision = 4;
    input.terrain.binding.sourceSha256 = std::string(64, 'b');
    input.terrain.binding.terrainSha256 = Take(Sha256(terrain));
    input.terrain.groundAtlases = {Atlas({100, 120, 140, 0})};
    input.objectDomain.sourceSha256 = input.terrain.binding.sourceSha256;
    input.objectDomain.documentId = input.terrain.binding.documentId;
    input.objectDomain.documentGeneration = 1;
    input.objectDomain.revision = 4;
    input.objectDomain.terrainSourceRevision = 7;
    input.objectDomain.unitsState = ClassicMinimapMemberState::Missing;
    input.objectDomain.doodadsState = ClassicMinimapMemberState::Missing;
    input.terrainSourceRevision = 7;
    input.postprocess.medianRadius = 0;
    input.postprocess.blurRadius = 0;
    input.postprocess.noisePercent = 0;
    return input;
}

void RequireRejected(const Result<ClassicMinimapCandidate>& result, const char* code) {
    Require(!result.Succeeded() && !result.value, "失败不得返回部分候选");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == code;
    }), "缺少预期Classic拒绝诊断");
}

std::array<unsigned, 4> RgbaAt(const Image::DecodedImage& image, std::uint32_t x, std::uint32_t y) {
    std::array<unsigned, 4> result{};
    const auto at = (static_cast<std::size_t>(y) * image.width + x) * 4;
    for (unsigned c = 0; c < 4; ++c) result[c] = std::to_integer<unsigned>(image.rgba[at + c]);
    return result;
}

void DistinctSwAnchorsTerminalVerticesAndFlip() {
    auto input = Input(TwoGroundW3e());
    input.terrain.groundAtlases.push_back(Atlas({20, 40, 60, 17}));
    const auto result = Take(BuildClassicMinimapCandidate(input));
    Require(result.manifest.sourceTiles == 4 && result.manifest.boundaryTiles == 1,
        "四tile仅一个SW边界，不能用整图solid取样自证");
    Require(RgbaAt(result.raw, 0, 0) == std::array<unsigned, 4>{9, 11, 13, 255}, "南西SW槽0需做边界混黑");
    Require(RgbaAt(result.raw, 1, 0) == std::array<unsigned, 4>{20, 40, 60, 255}, "南东SW槽1代表色");
    Require(RgbaAt(result.raw, 0, 1) == std::array<unsigned, 4>{20, 40, 60, 255}, "北西SW槽1代表色");
    Require(RgbaAt(result.raw, 1, 1) == std::array<unsigned, 4>{100, 120, 140, 255}, "北东SW槽0不得取terminal无效槽15");
    Require(RgbaAt(result.preNoise, 0, 0) == RgbaAt(result.raw, 0, 1), "最终preNoise首行必须来自北行");
    Require(RgbaAt(result.preNoise, 255, 0) == RgbaAt(result.raw, 1, 1), "最终preNoise北东正确");
    Require(RgbaAt(result.preNoise, 0, 255) == RgbaAt(result.raw, 0, 0), "最终preNoise末行必须来自南行");
    Require(RgbaAt(result.preNoise, 255, 255) == RgbaAt(result.raw, 1, 0), "最终preNoise南东正确");
    Require(result.preNoise.rgba == result.finalImage.rgba, "禁噪时两张256图必须精确相同");
}

void ObjectEnumEmptyIdentityAndCumulativeBudgetReject() {
    auto invalid = Input(W3e());
    invalid.objectDomain.unitsState = static_cast<ClassicMinimapMemberState>(999);
    RequireRejected(BuildClassicMinimapCandidate(invalid), "CLASSIC_MINIMAP_OBJECT_DOMAIN_INVALID");
    auto empty = Input(W3e());
    empty.objectDomain.unitsState = ClassicMinimapMemberState::Empty;
    empty.objectDomain.unitsMemberSha256 = std::string(64, 'e');
    RequireRejected(BuildClassicMinimapCandidate(empty), "CLASSIC_MINIMAP_OBJECT_DOMAIN_IDENTITY");
    empty.objectDomain.unitsMemberSha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    empty.objectDomain.unitsMemberBytes = 1;
    RequireRejected(BuildClassicMinimapCandidate(empty), "CLASSIC_MINIMAP_OBJECT_DOMAIN_IDENTITY");

    // 2MiB本可保留此微型输入，但解析前的完整后处理/JPEG预收使总帽不足。
    ClassicMinimapCandidateLimits tight;
    tight.maxWorkingBytes = 2u * 1024u * 1024u;
    RequireRejected(BuildClassicMinimapCandidate(Input(W3e()), tight), "CLASSIC_MINIMAP_BUDGET");
    // 样本size保持合法65536且noise=0；必须仍对额外capacity计费，不能以未消费为由忽略。
    auto retainedSamples = Input(W3e());
    retainedSamples.postprocess.noiseSamples = std::vector<std::uint16_t>(65536, 0);
    retainedSamples.postprocess.noiseSamples->reserve(4u * 1024u * 1024u);
    tight.maxWorkingBytes = 8u * 1024u * 1024u;
    RequireRejected(BuildClassicMinimapCandidate(retainedSamples, tight), "CLASSIC_MINIMAP_BUDGET");
    // SHA故意不匹配：若在统一预算预收前调用SHA，会先返回STALE并复制整份4MiB。
    // 此case要求在任何SHA/Parse之前命中累计预算门。
    auto hashPrecharge = Input(W3e());
    hashPrecharge.terrain.currentW3e.resize(4u * 1024u * 1024u);
    hashPrecharge.terrain.binding.terrainSha256 = std::string(64, 'd');
    tight.maxWorkingBytes = 4u * 1024u * 1024u + 128u * 1024u;
    RequireRejected(BuildClassicMinimapCandidate(hashPrecharge, tight), "CLASSIC_MINIMAP_BUDGET");
}

void BasicGroundBoundaryAndOutput() {
    auto input = Input(W3e(true, true, true));
    const auto result = Take(BuildClassicMinimapCandidate(input));
    Require(result.raw.width == 2 && result.raw.height == 2 && result.raw.Valid(), "原图必须按W3E tile逐格输出");
    Require(result.raw.rgba[0] == std::byte{9} && result.raw.rgba[1] == std::byte{11}
        && result.raw.rgba[2] == std::byte{13} && result.raw.rgba[3] == std::byte{255}, "Boundary A230黑混合应截断");
    Require(result.manifest.boundaryTiles == 4 && result.manifest.ignoredBlightSeen && result.manifest.ignoredVariationSeen,
        "blight/variation忽略但必须留在manifest事实");
    Require(!result.manifest.rawRgbaSha256.empty() && !result.manifest.preNoiseRgbaSha256.empty()
        && !result.manifest.finalRgbaSha256.empty() && !result.manifest.blpSha256.empty(), "manifest必须保留四阶段摘要");
    Require(result.preNoise.Valid() && result.finalImage.Valid() && !result.encoded.bytes.empty(), "必须接通postprocess与JPEG");
    Require(result.encoded.quality == 40 && result.encoded.jpegMipBytes > 0, "JPEG必须是q40单mip");
    auto legalCliffTexture = W3e();
    for (std::size_t at = 45; at < legalCliffTexture.size(); at += 7) legalCliffTexture[at + 6] = std::byte{0x02};
    const auto legal = Take(BuildClassicMinimapCandidate(Input(legalCliffTexture)));
    Require(legal.raw.rgba[0] == std::byte{100}, "合法cliff palette索引不等于悬崖，平层必须成功");
}

void ActualUnresolvedLayersReject() {
    RequireRejected(BuildClassicMinimapCandidate(Input(W3e(false, false, false, true))), "CLASSIC_MINIMAP_WATER");
    RequireRejected(BuildClassicMinimapCandidate(Input(W3e(false, false, false, false, true))), "CLASSIC_MINIMAP_CLIFF_UNRESOLVED");
    const auto mapEdge = Take(BuildClassicMinimapCandidate(Input(W3e(false, false, false, false, false, true))));
    Require(mapEdge.raw.rgba[0] == std::byte{9} && mapEdge.raw.rgba[1] == std::byte{11}
        && mapEdge.raw.rgba[2] == std::byte{13}, "MapEdge必须沿0xC0语义应用A230黑混合");
    auto crop = Input(W3e()); crop.terrain.rangePolicy = TerrainMinimapRangePolicy::DeclaredW3iMargins;
    RequireRejected(BuildClassicMinimapCandidate(crop), "CLASSIC_MINIMAP_PLAYABLE_RECT_UNRESOLVED");
    auto generation = Input(W3e()); generation.terrain.generation = TerrainMinimapGenerationConfig{};
    RequireRejected(BuildClassicMinimapCandidate(generation), "CLASSIC_MINIMAP_GENERATION_UNSUPPORTED");
    auto unexpectedW3i = Input(W3e()); unexpectedW3i.terrain.sourceW3i = Bytes{std::byte{1}};
    RequireRejected(BuildClassicMinimapCandidate(unexpectedW3i), "CLASSIC_MINIMAP_W3I_UNEXPECTED");
    auto oversized = Input(W3e());
    RequireRejected(BuildClassicMinimapCandidate(oversized, {.maxW3eBytes = 1}), "CLASSIC_MINIMAP_W3E_BUDGET");
}

void ObjectDeclarationAndAtlasIdentityReject() {
    auto unknown = Input(W3e());
    unknown.objectDomain.unitsState = ClassicMinimapMemberState::Unknown;
    RequireRejected(BuildClassicMinimapCandidate(unknown), "CLASSIC_MINIMAP_OBJECT_DOMAIN_UNKNOWN");
    auto stale = Input(W3e());
    stale.objectDomain.sourceSha256 = std::string(64, 'c');
    RequireRejected(BuildClassicMinimapCandidate(stale), "CLASSIC_MINIMAP_OBJECT_DOMAIN_STALE");
    auto badAtlas = Input(W3e());
    badAtlas.terrain.groundAtlases = {Atlas({100, 120, 140, 255}, true)};
    RequireRejected(BuildClassicMinimapCandidate(badAtlas), "CLASSIC_MINIMAP_ATLAS");
    auto noSource = Input(W3e());
    noSource.terrain.binding.terrainSha256 = std::string(64, 'd');
    RequireRejected(BuildClassicMinimapCandidate(noSource), "CLASSIC_MINIMAP_TERRAIN_STALE");
}

void PostprocessRandomSourceMustBeExplicit() {
    auto input = Input(W3e());
    input.postprocess.noisePercent = 10;
    RequireRejected(BuildClassicMinimapCandidate(input), "TERRAIN_MINIMAP_POSTPROCESS_NOISE_SOURCE_REQUIRED");
}

void ClassicWaterDepthAndBoundaryOrdering() {
    auto input = Input(W3e(false, false, false, true));
    auto& colors = input.terrain.classicColors.emplace();
    colors.cliffColors = {{10, 20, 30, 255}};
    colors.waterConfigured = true; colors.waterColor = {20, 40, 80, 128}; colors.deepLevelPathing = 64;
    const auto shallow = Take(BuildClassicMinimapCandidate(input));
    Require(RgbaAt(shallow.raw, 0, 0) == std::array<unsigned, 4>{59, 79, 109, 255}, "浅水按mm色混合一次");
    Require(shallow.manifest.shallowWaterTiles == 1 && shallow.manifest.deepWaterTiles == 0, "浅水逐SW锚点统计");
    colors.waterHeightOffset = 64;
    const auto deep = Take(BuildClassicMinimapCandidate(input));
    Require(RgbaAt(deep.raw, 0, 0) == std::array<unsigned, 4>{39, 59, 94, 255}, "深水含等阈值并逐次截断混合两次");
    Require(deep.manifest.deepWaterTiles == 1 && deep.manifest.shallowWaterTiles == 0, "深水阈值等号不能当浅水");
    colors.waterHeightOffset = -1;
    const auto dry = Take(BuildClassicMinimapCandidate(input));
    Require(RgbaAt(dry.raw, 0, 0) == std::array<unsigned, 4>{100, 120, 140, 255}, "地面高于水面不混水色");
    colors.waterHeightOffset = 64;
    input.terrain.currentW3e[49] |= std::byte{0x80};
    input.terrain.binding.terrainSha256 = Take(Sha256(input.terrain.currentW3e));
    const auto edge = Take(BuildClassicMinimapCandidate(input));
    Require(RgbaAt(edge.raw, 0, 0) == std::array<unsigned, 4>{3, 5, 9, 255}, "边界应在两次水色后混黑");
}

void NativePlayableRectangleUsesW3iAndClamps() {
    auto input = Input(TwoGroundW3e());
    input.terrain.groundAtlases.push_back(Atlas({20, 40, 60, 255}));
    MapData::MapSettings settings;
    settings.formatVersion = 25; settings.custom.emplace(); settings.flags = 0x400;
    settings.cameraMargins = {1, 0, 0, 0}; settings.playableWidth = 1; settings.playableHeight = 2;
    input.terrain.sourceW3i = Take(settings.Serialize());
    input.terrain.rangePolicy = TerrainMinimapRangePolicy::ClassicPlayable;
    const auto cropped = Take(BuildClassicMinimapCandidate(input));
    Require(cropped.manifest.sourceRectangle == std::array<std::uint32_t, 4>{1, 0, 2, 2}
        && cropped.raw.width == 1 && cropped.raw.height == 2, "补足边距模式应精确裁剪真实W3I");
    Require(RgbaAt(cropped.raw, 0, 0) == std::array<unsigned, 4>{20, 40, 60, 255}
        && RgbaAt(cropped.raw, 0, 1) == std::array<unsigned, 4>{100, 120, 140, 255}, "裁剪保持原网格锚点");
    settings.flags = 0;
    settings.cameraBounds = {127.9f, -500, 128.1f, 500, -9000, 9999, 9000, -9999};
    auto& colors = input.terrain.classicColors.emplace();
    colors.cameraMarginsConfigured = true; colors.cameraMargins = {0, 1, 0, 0};
    input.terrain.sourceW3i = Take(settings.Serialize());
    const auto camera = Take(BuildClassicMinimapCandidate(input));
    Require(camera.manifest.sourceRectangle == std::array<std::uint32_t, 4>{1, 0, 2, 2},
        "自定义相机应加回配置边距、限制全图后按最近格截断");
    settings.flags = 0x400; settings.cameraMargins = {1, 1, 0, 0};
    input.terrain.sourceW3i = Take(settings.Serialize());
    RequireRejected(BuildClassicMinimapCandidate(input), "CLASSIC_MINIMAP_PLAYABLE_EMPTY");
}

} // namespace

int main() {
    try {
        BasicGroundBoundaryAndOutput();
        DistinctSwAnchorsTerminalVerticesAndFlip();
        ObjectEnumEmptyIdentityAndCumulativeBudgetReject();
        ActualUnresolvedLayersReject();
        ObjectDeclarationAndAtlasIdentityReject();
        PostprocessRandomSourceMustBeExplicit();
        ClassicWaterDepthAndBoundaryOrdering();
        NativePlayableRectangleUsesW3iAndClamps();
        std::cout << "Classic minimap candidate tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
