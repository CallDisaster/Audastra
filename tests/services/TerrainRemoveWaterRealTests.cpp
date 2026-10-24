#include "TerrainGroundBrushRealTests.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/TextUtil.h"

#include <array>
#include <iostream>
#include <stdexcept>

namespace War3Map::TerrainSaveTests {
namespace {
using namespace MapData;
using namespace Services;
constexpr std::size_t kBytes = 64u * 1024u * 1024u;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    if (result.Succeeded() && result.value) return std::move(*result.value);
    std::string message;
    for (const auto& item : result.diagnostics) message += item.code + ": " + item.message + "\n";
    throw std::runtime_error(message.empty() ? "missing real remove-water result" : message);
}
} // namespace

void CheckRealRemoveWaterSave(const MapArchiveSession& shallow, const MapArchiveSession& deep,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game) {
    for (const bool deepWater : {false, true}) {
        // 输入是先前浅/深水专项经公共保存并重新打开的原创图，不另造水刷输出。
        const auto& source = deepWater ? deep : shallow;
        const auto scenario = directory / (deepWater ? "playable-deep-remove-water" : "playable-shallow-remove-water");
        Require(std::filesystem::create_directory(scenario), "exclusive actual remove-water scenario");
        const auto sourceSha = Take(Sha256File(source.SourcePath()));
        Require(sourceSha == source.SourceSha256(), "actual water readback keeps its source archive identity");
        const auto beforeBytes = Take(source.ReadMember("war3map.w3e"));
        const auto before = *Take(ParseMapTerrain(beforeBytes)).Snapshot();
        Require(before.header.width == 65 && before.header.height == 65,
            "remove-water uses the playable original 64-cell map");
        Require(beforeBytes.size() >= before.vertices.size() * 7,
            "actual W3E contains the complete seven-byte vertex array");
        const auto vertexStart = beforeBytes.size() - before.vertices.size() * 7;
        auto expectedTerrain = beforeBytes;
        std::size_t wetCount = 0;
        for (std::size_t i = 0; i < before.vertices.size(); ++i) {
            const auto flags = vertexStart + i * 7 + 4;
            Require(beforeBytes[flags] == static_cast<std::byte>(before.vertices[i].textureAndFlagsRaw),
                "literal W3E vertex offset agrees with parsed source flags");
            if (!before.vertices[i].HasWater()) continue;
            const auto x = i % 65, y = i / 65;
            Require(x >= 16 && x <= 48 && y >= 16 && y <= 48,
                "one bounded square covers all water created by the preceding public save");
            expectedTerrain[flags] &= std::byte{0xbf};
            ++wetCount;
        }
        Require(wetCount != 0 && expectedTerrain != beforeBytes, "actual source has water to remove");
        const auto after = *Take(ParseMapTerrain(expectedTerrain)).Snapshot();
        for (std::size_t i = 0; i < before.vertices.size(); ++i) {
            const auto& old = before.vertices[i]; const auto& current = after.vertices[i];
            Require(!current.HasWater() && current.groundHeightRaw == old.groundHeightRaw
                && current.waterHeightAndFlagsRaw == old.waterHeightAndFlagsRaw
                && current.cliffAndLayerRaw == old.cliffAndLayerRaw && current.variationRaw == old.variationRaw
                && current.textureAndFlagsRaw == static_cast<std::uint8_t>(old.textureAndFlagsRaw & 0xbf),
                "literal remove-water expectation preserves all raw heights, layers, textures and other flags");
        }

        const auto resources = Take(TerrainPathingSave::CaptureNative(source, before, after, game, kBytes));
        Require(resources.VerifySourceInputs(source, kBytes).Succeeded(),
            "remove-water binds actual old water and terrain resource winners before saving");
        const auto oldNative = Take(resources.GenerateNative(before, kBytes));
        const auto newNative = Take(resources.GenerateNative(after, kBytes));
        const auto oldPathing = Take(source.ReadMember("war3map.wpm"));
        Require(oldPathing.size() == 16 + 256 * 256 && oldNative.size() == 256 * 256
            && newNative.size() == oldNative.size() && oldNative != newNative,
            "removing actual shallow or deep water changes the complete native pathing contribution");
        auto expectedPathing = oldPathing;
        // 此专项复用实际资源生成器核对公共编排；独立字面水位/WPM oracle在CPU回归中。
        for (std::size_t i = 0; i < oldNative.size(); ++i) {
            Require(oldPathing[16 + i] == ((oldPathing[16 + i] & std::byte{0x11}) | oldNative[i]),
                "actual public water source has a completely explained old native WPM");
            expectedPathing[16 + i] = (oldPathing[16 + i] & std::byte{0x11}) | newNative[i];
        }
        const std::array<MemberReplacement, 1> finalInputs{{{"war3map.w3e", expectedTerrain}}};
        const auto markers = Take(TerrainMinimapMarkers::Capture(source, game, finalInputs));
        Require(!source.HasMember("war3mapMap.tga") && source.HasMember("war3mapMap.blp"),
            "actual remove-water fixture uses the preceding automatic BLP instead of an authored TGA");
        std::vector<std::byte> firstBlp;
        std::string firstDecodedSha;
        for (const bool repeat : {false, true}) {
            MapEditSaveRequest request;
            request.sourceMap = source.SourcePath();
            request.targetCandidate = scenario / (repeat ? "saved-repeat.w3x" : "saved.w3x");
            request.toolRoot = tool; request.gameRoot = game;
            request.expectedSourceSha256 = sourceSha; request.expectedTerrainSha256 = Take(Sha256(beforeBytes));
            MapEditTerrainStroke stroke;
            stroke.brush.kind = TerrainBrushKind::RemoveWater; stroke.brush.shape = TerrainBrushShape::Square;
            stroke.brush.radius = 16; stroke.pointSpace = MapEditPointSpace::TerrainVertex; stroke.points = {{32, 32}};
            request.terrainStrokes = {stroke};
            const auto saved = Take(SaveMapEditCandidate(request));
            const auto readback = Take(MapArchiveSession::Open(saved.candidate));
            Require(saved.appliedStrokeCount == 1 && saved.scriptPreserved
                && saved.sourceSha256 == sourceSha && saved.originalTerrainSha256 == Take(Sha256(beforeBytes))
                && Take(readback.ReadMember("war3map.w3e")) == expectedTerrain
                && Take(readback.ReadMember("war3map.wpm")) == expectedPathing,
                "actual remove-water public save reads back the full literal W3E and actual-resource WPM");
            Require(resources.VerifyReadback(readback, kBytes).Succeeded()
                && markers->VerifyReadback(readback).Succeeded()
                && Take(readback.ReadMember("war3map.mmp")) == markers->Bytes(),
                "actual resource identities and final marker dependencies verify after reopening");
            Require(saved.generatedMissingMinimap.has_value(),
                "each actual remove-water save generates and verifies the final automatic BLP");
            const auto blp = Take(readback.ReadMember("war3mapMap.blp"));
            const auto& minimap = *saved.generatedMissingMinimap;
            Require(!blp.empty() && minimap.encodedBlpSha256 == Take(Sha256(blp))
                && minimap.encodedBlpBytes == blp.size() && minimap.encodedMipCount == 1,
                "actual generated BLP matches the verified single-mip receipt");
            if (repeat) {
                Require(blp == firstBlp && minimap.decodedMip0RgbaSha256 == firstDecodedSha,
                    "same actual source and remove-water stroke generate byte-identical BLP including content-seeded noise");
            } else {
                firstBlp = blp; firstDecodedSha = minimap.decodedMip0RgbaSha256;
            }
            Require(readback.Members().size() == source.Members().size(),
                "remove-water does not introduce or remove unrelated archive members");
            for (const auto& member : source.Members()) {
                const auto name = Text::LowerAscii(member.name);
                if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3map.mmp" || name == "war3mapmap.blp") continue;
                Require(Take(readback.ReadMember(member.name)) == member.data,
                    "remove-water preserves every unrelated script, settings, placement, object and shadow byte");
            }
            Require(Take(Sha256File(source.SourcePath())) == sourceSha
                && resources.VerifySourceInputs(source, kBytes).Succeeded(),
                "both remove-water saves retain the full actual source archive and resource identities");
            std::cout << "playable_remove_water_save_passed deep=" << deepWater << " repeat=" << repeat
                << " source=" << IO::PathUtf8(source.SourcePath()) << " source_sha256=" << sourceSha
                << " candidate=" << IO::PathUtf8(saved.candidate) << " candidate_sha256=" << saved.candidateSha256
                << " removed_wet_vertices=" << wetCount << " w3e_sha256=" << Take(Sha256(expectedTerrain))
                << " wpm_sha256=" << Take(Sha256(expectedPathing)) << " blp_sha256=" << minimap.encodedBlpSha256
                << " captured_bytes=" << resources.SourceBytes() << " cumulative_capture_budget=" << resources.CaptureBudgetBytes()
                << " source_unchanged=true expected_w3e_literal=true expected_wpm_uses_native_generator=true"
                << " native_oracle=false native_editor_verified=false game_verified=false\n";
        }
    }
}
} // namespace War3Map::TerrainSaveTests
