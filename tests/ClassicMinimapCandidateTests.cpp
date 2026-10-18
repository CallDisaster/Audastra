#include "War3MapCore/Hash.h"
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

} // namespace

int main() {
    try {
        BasicGroundBoundaryAndOutput();
        ActualUnresolvedLayersReject();
        ObjectDeclarationAndAtlasIdentityReject();
        PostprocessRandomSourceMustBeExplicit();
        std::cout << "Classic minimap candidate tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
