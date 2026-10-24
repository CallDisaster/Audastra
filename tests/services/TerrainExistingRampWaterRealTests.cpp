#include "TerrainExistingRampWaterRealTests.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapMinimapMarkers.h"
#include "War3MapCore/MapData/MapShadow.h"
#include "War3MapCore/MapData/TerrainRampEditing.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <cmath>
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
    throw std::runtime_error(message.empty() ? "missing existing ramp water result" : message);
}

ShadowGridSnapshot ShadowFixture(const TerrainGridSnapshot& terrain) {
    ShadowGridSnapshot shadow;
    shadow.width = (terrain.header.width - 1) * 4;
    shadow.height = (terrain.header.height - 1) * 4;
    shadow.intensities.resize(std::size_t{shadow.width} * shadow.height, 0);
    for (std::size_t y = 29; y < 34; ++y) for (std::size_t x = 17; x < 24; ++x)
        shadow.intensities[y * shadow.width + x] = 255;
    shadow.intensities[(shadow.height - 31) * shadow.width + shadow.width - 19] = 255;
    shadow.intensities[(shadow.height / 2) * shadow.width + shadow.width / 3] = 255;
    return shadow;
}
} // namespace

void CheckRealExistingRampWaterSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game) {
    const auto originalTerrain = *Take(ParseMapTerrain(Take(original.ReadMember("war3map.w3e")))).Snapshot();
    Require(originalTerrain.header.width == 65 && originalTerrain.header.height == 65,
        "existing ramp water uses the playable 64-cell original fixture");
    Require(!originalTerrain.header.cliffTilesets.empty(), "existing ramp seed selects a real cliff palette slot");
    const auto doodads = Take(ParseDoodadPlacements(Take(original.ReadMember("war3map.doo"))));
    Require(doodads.specialDoodads.empty(), "water fixture has no unaccounted special deletion");
    const auto initialResources = Take(TerrainPathingSave::CaptureWaterBrushRules(original,
        originalTerrain, originalTerrain, doodads, game, true, kBytes));
    const auto* initial = initialResources.WaterBrushRules();
    Require(initial && initial->deepWaterThreshold >= 2, "actual water threshold supports a quantized shallow seed");
    const auto shadow = ShadowFixture(originalTerrain);
    const auto shadowBytes = Take(SerializeMapShadow(shadow));
    MinimapMarkersSnapshot staleMarkers;
    staleMarkers.records = {{0, {31, 43}, {0x11, 0x44, 0xaa, 0xff}},
        {1, {177, 67}, {0xaa, 0xbb, 0x44, 0xff}}, {2, {91, 213}, {0x44, 0x11, 0xcc, 0xff}}};
    const auto staleMarkerBytes = Take(SerializeMapMinimapMarkers(staleMarkers));
    for (const bool wet : {false, true}) for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
        const bool deep = kind == TerrainWaterBrushKind::Deep;
        const auto scenario = directory / (std::string("playable-") + (wet ? "wet" : "dry")
            + "-existing-ramp-" + (deep ? "deep" : "shallow") + "-water");
        Require(std::filesystem::create_directory(scenario), "exclusive existing ramp water scenario");
        auto before = originalTerrain;
        for (std::uint32_t y = 0; y < 65; ++y) for (std::uint32_t x = 0; x < 65; ++x) {
            auto& vertex = before.vertices[std::size_t{y} * 65 + x];
            Require(!vertex.HasWater() && !vertex.IsRamp(), "existing ramp seed starts with the dry original terrain");
            // 手造既有墙面须选择实际slot0；slot15只代表未选材质的平地。
            vertex.cliffAndLayerRaw = static_cast<std::uint8_t>(x <= 31 ? 3 : 2);
        }
        for (const auto x : {31u, 32u, 33u}) before.vertices[32 * 65 + x].textureAndFlagsRaw |= 0x10;
        const auto center = std::size_t{32} * 65 + 32;
        const auto rawBase = before.vertices[center].groundHeightRaw;
        Require(rawBase <= 0x3fff, "actual fixture base height is classic14bit");
        const double ground = (double(rawBase) - 8192.0) / 4.0 + 64.0;
        // 水面使用实际offset和threshold，使原坡道中点确实浅覆水；不是只置HasWater位。
        const double encoded = std::trunc((ground + double(initial->deepWaterThreshold) * 0.5
            - double(initial->waterLayerOffset) + 2048.0) * 4.0);
        Require(std::isfinite(encoded) && encoded >= 0 && encoded <= 16383, "actual shallow seed water surface is encodable");
        if (wet) for (std::uint32_t y = 29; y <= 35; ++y) for (std::uint32_t x = 29; x <= 35; ++x) {
            auto& vertex = before.vertices[std::size_t{y} * 65 + x];
            Require(vertex.groundHeightRaw == rawBase, "authored wet ramp neighbourhood has one raw base");
            vertex.textureAndFlagsRaw |= 0x40;
            vertex.waterHeightAndFlagsRaw = static_cast<std::uint16_t>(encoded);
        }
        RefreshTerrainDerived(before, 0, 0, 65, 65);
        const auto beforeBytes = Take(SerializeMapTerrain(before));
        const std::array<MemberReplacement, 3> seed{{{"war3map.w3e", beforeBytes},
            {"war3map.shd", shadowBytes}, {"war3map.mmp", staleMarkerBytes}}};
        const auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "seeded.w3x", seed)).path));
        const auto resources = Take(TerrainPathingSave::CaptureWaterBrushRules(seeded, before, before,
            doodads, game, true, kBytes));
        const auto* loaded = resources.WaterBrushRules();
        Require(loaded != nullptr, "existing ramp water captures actual structural rules");
        const auto rulesSha256 = Take(resources.WaterBrushDependencyFingerprint());
        const TerrainStrokePoint point{before.header.offset[0] + 32 * 128, before.header.offset[1] + 32 * 128};
        const auto anchor = Take(CaptureTerrainWaterStrokeAnchor(before, point, loaded->maximumCliffLevel));
        Require(anchor.rampHeightOffset == 64 && anchor.hasWater == wet && anchor.groundHeightRaw == rawBase,
            "literal straight ramp keeps raw B distinct from complete R64 ground");
        const double surface = (double(anchor.waterHeightRaw) - 8192.0) / 4.0 + loaded->waterLayerOffset;
        if (wet) Require(surface > ground && surface < ground + loaded->deepWaterThreshold,
            "actual wet ramp begins in shallow strict coverage before either water brush");
        TerrainWaterStrokeOptions options;
        options.kind = kind; options.shape = TerrainBrushShape::Square; options.size = 2;
        options.cliffTexture = 0; options.randomSeed = 73; options.enforceWaterLimits = true;
        const TerrainWaterBrush brush{options.kind, options.shape, options.size, options.cliffTexture,
            options.randomSeed, anchor, options.algorithm};
        const auto planned = Take(PrepareTerrainWaterStroke(before, *loaded, brush, std::span(&point, 1),
            doodads, resources.WaterSpecialShapes()));
        const auto afterBytes = Take(SerializeMapTerrain(planned.terrain));
        Require(afterBytes != beforeBytes && !planned.result.changed.Empty() && planned.terrain.vertices[center].HasWater()
            && planned.result.removedSpecialSourceRecords.empty(), "each actual ramp water scenario changes terrain with no special deletion");
        const auto native = Take(TerrainPathingSave::CaptureNative(seeded, before, planned.terrain, game, kBytes));
        const auto oldNative = Take(native.GenerateNative(before, kBytes));
        const auto newNative = Take(native.GenerateNative(planned.terrain, kBytes));
        auto oldPathing = Take(original.ReadMember("war3map.wpm"));
        Require(oldPathing.size() == 16 + 256 * 256 && oldNative.size() == 256 * 256
            && oldNative.size() == newNative.size(), "existing ramp water pathing covers the entire authored grid");
        auto expectedPathing = oldPathing;
        // 此专项复用真实资源Core/通行生成器检查公共保存编排；独立字面算法oracle在Core/Services测试。
        for (std::size_t i = 0; i < oldNative.size(); ++i) {
            oldPathing[i + 16] = (oldPathing[i + 16] & std::byte{0x11}) | oldNative[i];
            expectedPathing[i + 16] = (expectedPathing[i + 16] & std::byte{0x11}) | newNative[i];
        }
        Require(oldPathing != expectedPathing, "each existing ramp water stroke changes actual full native pathing");
        const auto source = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "source.w3x",
            std::array<MemberReplacement, 1>{{{"war3map.wpm", oldPathing}}})).path));
        const auto expectedMarkers = Take(TerrainMinimapMarkers::Capture(source, game,
            std::array<MemberReplacement, 1>{{{"war3map.w3e", afterBytes}}}));
        Require(expectedMarkers->Bytes() != staleMarkerBytes, "ordinary save must replace the deliberately stale MMP seed");
        MapEditSaveRequest request;
        request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / "saved.w3x";
        request.toolRoot = tool; request.gameRoot = game; request.expectedSourceSha256 = source.SourceSha256();
        request.expectedTerrainSha256 = Take(Sha256(beforeBytes)); request.minimapChoice = TerrainMinimapSaveChoice::Automatic;
        MapEditTerrainStroke stroke; stroke.points = {point}; stroke.waterBrush = options; request.terrainStrokes = {stroke};
        const auto saved = Take(SaveMapEditCandidate(request));
        const auto readback = Take(MapArchiveSession::Open(saved.candidate));
        const auto actualTerrain = Take(readback.ReadMember("war3map.w3e"));
        const auto actualPathing = Take(readback.ReadMember("war3map.wpm"));
        const auto actualShadow = Take(readback.ReadMember("war3map.shd"));
        const auto actualMarkers = Take(readback.ReadMember("war3map.mmp"));
        Require(actualTerrain == afterBytes && actualPathing == expectedPathing,
            "public dry/wet R64 shallow/deep save reads back complete planned W3E and WPM bytes");
        Require(actualShadow == shadowBytes && Take(ParseMapShadow(actualShadow, shadow.width, shadow.height)).intensities == shadow.intensities
            && std::count(shadow.intensities.begin(), shadow.intensities.end(), std::uint8_t{255}) == 37,
            "public water save preserves the complete asymmetric nonzero prior SHD");
        const auto markers = Take(ParseMapMinimapMarkers(actualMarkers));
        Require(actualMarkers == expectedMarkers->Bytes() && markers.version == 0
            && markers.records.size() == expectedMarkers->MarkerCount() && expectedMarkers->VerifyReadback(readback).Succeeded(),
            "actual MMP is regenerated from final dependencies and independently parsed and verified after archive reopening");
        Require(saved.scriptPreserved && native.VerifyReadback(readback, kBytes).Succeeded()
            && resources.VerifyReadback(source, kBytes).Succeeded(), "actual rules and playable script keep their source identities");
        for (const auto& member : source.Members()) {
            const auto name = Text::LowerAscii(member.name);
            if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp" || name == "war3map.mmp") continue;
            Require(Take(readback.ReadMember(member.name)) == member.data, "ramp water save preserves every unrelated source member");
        }
        Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256()
            && Take(Sha256File(original.SourcePath())) == original.SourceSha256(), "both source copies remain byte-for-byte unchanged");
        std::cout << "playable_existing_ramp_water_save_passed wet=" << wet << " deep=" << deep
            << " source_sha256=" << source.SourceSha256() << " candidate_sha256=" << saved.candidateSha256
            << " w3e_sha256=" << Take(Sha256(actualTerrain)) << " wpm_sha256=" << Take(Sha256(actualPathing))
            << " actual_rules_sha256=" << rulesSha256 << " actual_max_cliff_level=" << unsigned(loaded->maximumCliffLevel)
            << " actual_water_offset=" << loaded->waterLayerOffset << " actual_deep_threshold=" << loaded->deepWaterThreshold
            << " shd_bytes=" << actualShadow.size() << " shd_sha256=" << Take(Sha256(actualShadow)) << " shd_nonzero=37"
            << " mmp_bytes=" << actualMarkers.size() << " mmp_sha256=" << Take(Sha256(actualMarkers))
            << " mmp_records=" << markers.records.size() << " source_unchanged=true"
            << " expected_uses_product_core=true expected_uses_native_generator=true expected_uses_mmp_generator=true"
            << " native_oracle=false native_editor_verified=false game_verified=false\n";
    }
}
void CheckRealCornerRampSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game) {
    const auto originalTerrain = *Take(ParseMapTerrain(Take(original.ReadMember("war3map.w3e")))).Snapshot();
    Require(originalTerrain.header.width == 65 && originalTerrain.header.height == 65,
        "corner ramp uses the playable original 64-cell map");
    Require(!originalTerrain.header.cliffTilesets.empty(), "corner seed selects a real cliff palette slot");
    const auto doodads = Take(ParseDoodadPlacements(Take(original.ReadMember("war3map.doo"))));
    Require(doodads.doodads.empty() && doodads.specialDoodads.empty(), "corner seed has no unaccounted placement contribution");
    const auto shadowBytes = Take(SerializeMapShadow(ShadowFixture(originalTerrain)));
    const auto waterResources = Take(TerrainPathingSave::CaptureWaterBrushRules(original,
        originalTerrain, originalTerrain, doodads, game, true, kBytes));
    Require(waterResources.WaterBrushRules(), "corner scenarios capture actual water and maximum-level rules");
    const double waterOffset = waterResources.WaterBrushRules()->waterLayerOffset;
    for (const bool wet : {false, true}) for (const int dx : {-1, 1}) for (const int dy : {-1, 1}) {
        const auto scenario = directory / (std::string(wet ? "playable-wet-corner-ramp-" : "playable-corner-ramp-")
            + (dx > 0 ? "east-" : "west-") + (dy > 0 ? "north" : "south"));
        Require(std::filesystem::create_directory(scenario), "exclusive actual corner scenario");
        auto before = originalTerrain;
        for (int y = 0; y < 65; ++y) for (int x = 0; x < 65; ++x) {
            auto& vertex = before.vertices[std::size_t(y) * 65 + x];
            Require(!vertex.IsRamp() && !vertex.HasWater() && !vertex.IsBlight(),
                "original corner seed is dry, has no ramp or blight");
            const bool high = (x - 32) * dx <= 0 && (y - 32) * dy <= 0;
            vertex.cliffAndLayerRaw = std::uint8_t(high ? 3 : 2);
        }
        if (wet) {
            const double rawGround = (double(before.vertices[32 * 65 + 32].groundHeightRaw) - 8192.0) * 0.25;
            const double encoded = std::trunc((rawGround + 64.0 - waterOffset) * 4.0 + 8192.0);
            Require(std::isfinite(encoded) && encoded >= 0 && encoded <= 16383,
                "actual water offset permits a bounded authored wet corner surface");
            const double surface = (encoded - 8192.0) * 0.25 + waterOffset;
            Require(surface > rawGround + 32 && surface < rawGround + 96,
                "wet corner uses a measured surface crossing the new S32 and R64 heights");
            for (unsigned y = 24; y <= 40; ++y) for (unsigned x = 24; x <= 40; ++x) {
                auto& vertex = before.vertices[std::size_t(y) * 65 + x];
                vertex.waterHeightAndFlagsRaw = std::uint16_t(encoded);
                vertex.textureAndFlagsRaw |= 0x40;
            }
        }
        RefreshTerrainDerived(before, 0, 0, 65, 65);
        auto after = before;
        for (int x = 0; x < 3; ++x) for (int y = 0; y < 3; ++y)
            after.vertices[std::size_t(32 + y * dy) * 65 + 32 + x * dx].textureAndFlagsRaw |= 0x10;
        RefreshTerrainDerived(after, 0, 0, 65, 65);
        const auto beforeBytes = Take(SerializeMapTerrain(before)), afterBytes = Take(SerializeMapTerrain(after));
        const std::array<MemberReplacement, 2> seed{{{"war3map.w3e", beforeBytes}, {"war3map.shd", shadowBytes}}};
        const auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "seeded.w3x", seed)).path));
        const auto native = Take(TerrainPathingSave::CaptureNative(seeded, before, after, game, kBytes));
        const auto g0 = Take(native.GenerateNative(before, kBytes)), g1 = Take(native.GenerateNative(after, kBytes));
        auto originalWpm = Take(original.ReadMember("war3map.wpm")), expectedWpm = originalWpm;
        Require(g0.size() == 256 * 256 && g1.size() == g0.size() && originalWpm.size() == 16 + g0.size(),
            "actual corner pathing covers every source cell");
        // 此专项验证实际资源和公共编排；完整独立字面WPM另见Core/Services角坡测试。
        for (std::size_t i = 0; i < g0.size(); ++i) {
            originalWpm[i + 16] = (originalWpm[i + 16] & std::byte{0x11}) | g0[i];
            expectedWpm[i + 16] = (expectedWpm[i + 16] & std::byte{0x11}) | g1[i];
        }
        Require(originalWpm != expectedWpm, "corner changes real native pathing rather than only journal state");
        auto current = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "source.w3x",
            std::array<MemberReplacement, 1>{{{"war3map.wpm", originalWpm}}})).path));
        const TerrainStrokePoint point{before.header.offset[0] + 32 * 128 + dx * 32,
            before.header.offset[1] + 32 * 128 + dy * 32};
        for (const bool create : {true, false}) {
            const auto& currentTerrain = create ? before : after;
            const auto rules = Take(TerrainPathingSave::CaptureWaterBrushRules(current,
                currentTerrain, currentTerrain, doodads, game, true, kBytes));
            TerrainBrush brush; brush.kind = TerrainBrushKind::CornerRamp; brush.value = create;
            const auto candidate = Take(PrepareTerrainCornerRampStroke(currentTerrain, *rules.WaterBrushRules(),
                brush, std::span(&point, 1), doodads, rules.WaterSpecialShapes()));
            const auto& expectedTerrain = create ? afterBytes : beforeBytes;
            Require(Take(SerializeMapTerrain(candidate.terrain)) == expectedTerrain,
                "actual rules preserve independent complete nine-bit W3E literal in every direction");
            MapEditSaveRequest request;
            request.sourceMap = current.SourcePath(); request.targetCandidate = scenario / (create ? "created.w3x" : "removed.w3x");
            request.toolRoot = tool; request.gameRoot = game; request.expectedSourceSha256 = current.SourceSha256();
            request.expectedTerrainSha256 = Take(Sha256(create ? beforeBytes : afterBytes));
            request.minimapChoice = TerrainMinimapSaveChoice::Automatic;
            MapEditTerrainStroke stroke; stroke.brush = brush; stroke.points = {point}; request.terrainStrokes = {stroke};
            const auto saved = Take(SaveMapEditCandidate(request));
            auto readback = Take(MapArchiveSession::Open(saved.candidate));
            Require(Take(readback.ReadMember("war3map.w3e")) == expectedTerrain
                && Take(readback.ReadMember("war3map.wpm")) == (create ? expectedWpm : originalWpm)
                && Take(readback.ReadMember("war3map.shd")) == shadowBytes && saved.scriptPreserved,
                "real-toolchain original map corner create/remove saves and reads back complete terrain/pathing/shadow");
            for (const auto& member : current.Members()) {
                const auto name = Text::LowerAscii(member.name);
                if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp" || name == "war3map.mmp") continue;
                Require(Take(readback.ReadMember(member.name)) == member.data, "corner preserves every unrelated source member");
            }
            Require(Take(Sha256File(current.SourcePath())) == current.SourceSha256()
                && Take(Sha256File(original.SourcePath())) == original.SourceSha256(), "corner keeps every source immutable");
            std::cout << "playable_corner_ramp_save_passed wet=" << wet << " dx=" << dx << " dy=" << dy << " create=" << create
                << " source_sha256=" << current.SourceSha256() << " candidate_sha256=" << saved.candidateSha256
                << " w3e_sha256=" << Take(Sha256(expectedTerrain)) << " wpm_sha256=" << Take(Sha256(create ? expectedWpm : originalWpm))
                << " actual_rules_sha256=" << Take(rules.WaterBrushDependencyFingerprint())
                << " expected_w3e_literal=true expected_uses_native_generator=true native_editor_verified=false game_verified=false\n";
            current = std::move(readback);
        }
    }
}
} // namespace War3Map::TerrainSaveTests
