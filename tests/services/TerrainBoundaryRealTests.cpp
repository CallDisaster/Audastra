#include "TerrainBoundaryRealTests.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace War3Map::TerrainSaveTests {
namespace {
using namespace MapData;
using namespace Services;
constexpr std::size_t kBytes = 64u * 1024u * 1024u;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (result.Succeeded() && result.value) return std::move(*result.value);
    std::string message;
    for (const auto& item : result.diagnostics) message += item.code + ": " + item.message + "\n";
    throw std::runtime_error(message.empty() ? "missing actual boundary save result" : message);
}
}

void CheckRealTerrainBoundarySave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game) {
    const auto scenario = directory / "playable-boundary-add-erase";
    Require(std::filesystem::create_directory(scenario), "exclusive real boundary save scenario");
    const auto originalTerrain = Take(original.ReadMember("war3map.w3e"));
    const auto before = *Take(ParseMapTerrain(originalTerrain)).Snapshot();
    Require(before.header.width == 65 && before.header.height == 65, "actual boundary save uses the original 64-cell map");
    const auto rules = Take(TerrainPathingSave::CaptureNative(original, before, before, game, kBytes));
    const auto native = Take(rules.GenerateNative(before, kBytes));
    auto baselineWpm = Take(original.ReadMember("war3map.wpm"));
    Require(baselineWpm.size() == native.size() + 16, "actual boundary source has the complete pathing dimensions");
    for (std::size_t i = 0; i < native.size(); ++i)
        baselineWpm[16 + i] = (baselineWpm[16 + i] & std::byte{0x11}) | native[i];
    auto boundaryWpm = baselineWpm;
    // 独立原版字面期望：只有Q/SW(32,32)所属的4×4像素取0xCE；相邻格不变。
    for (std::size_t y = 128; y < 132; ++y) for (std::size_t x = 128; x < 132; ++x)
        boundaryWpm[16 + y * 256 + x] = (baselineWpm[16 + y * 256 + x] & std::byte{0x11}) | std::byte{0xce};
    auto boundaryTerrain = before;
    boundaryTerrain.vertices[32 * 65 + 32].textureAndFlagsRaw |= 0x80;
    const auto boundaryWire = Take(SerializeMapTerrain(boundaryTerrain));
    const std::array<MemberReplacement, 1> pathing{{{"war3map.wpm", baselineWpm}}};
    auto source = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "source.w3x", pathing)).path));
    const auto sourceSha = source.SourceSha256();
    for (const bool erase : {false, true}) {
        const auto& expectedTerrain = erase ? originalTerrain : boundaryWire;
        const auto& expectedWpm = erase ? baselineWpm : boundaryWpm;
        MapEditSaveRequest request;
        request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / (erase ? "erased.w3x" : "saved.w3x");
        request.toolRoot = tool; request.gameRoot = game; request.expectedSourceSha256 = source.SourceSha256();
        request.expectedTerrainSha256 = Take(Sha256(Take(source.ReadMember("war3map.w3e"))));
        MapEditTerrainStroke stroke;
        stroke.brush.kind = TerrainBrushKind::Boundary; stroke.brush.shape = TerrainBrushShape::Square;
        stroke.brush.radius = 0; stroke.brush.value = !erase;
        stroke.pointSpace = MapEditPointSpace::TerrainVertex; stroke.points = {{32, 32}};
        request.terrainStrokes = {stroke};
        const std::array<MemberReplacement, 1> finalInputs{{{"war3map.w3e", expectedTerrain}}};
        const auto markers = Take(TerrainMinimapMarkers::Capture(source, game, finalInputs));
        const auto saved = Take(SaveMapEditCandidate(request));
        auto readback = Take(MapArchiveSession::Open(saved.candidate));
        Require(saved.appliedStrokeCount == 1 && saved.scriptPreserved
            && Take(readback.ReadMember("war3map.w3e")) == expectedTerrain
            && Take(readback.ReadMember("war3map.wpm")) == expectedWpm,
            "actual boundary add/erase public save has the complete independent W3E and WPM result");
        Require(Take(readback.ReadMember("war3map.mmp")) == markers->Bytes()
            && markers->VerifyReadback(readback).Succeeded(), "boundary MMP retains final typed sources and playable range");
        Require(saved.generatedMissingMinimap.has_value(), "ordinary boundary save generates the final automatic BLP");
        for (const auto& member : source.Members()) {
            const auto name = Text::LowerAscii(member.name);
            if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3map.mmp" || name == "war3mapmap.blp") continue;
            Require(Take(readback.ReadMember(member.name)) == member.data, "boundary save preserves all unrelated original bytes");
        }
        Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "boundary save never changes its input copy");
        std::cout << "boundary_save_passed erase=" << erase << " source_sha256=" << source.SourceSha256()
            << " candidate_sha256=" << saved.candidateSha256 << " wpm_sha256=" << Take(Sha256(expectedWpm))
            << " native_editor_verified=false game_verified=false\n";
        source = std::move(readback);
    }
    Require(Take(Sha256File(scenario / "source.w3x")) == sourceSha, "both saves preserve the original baseline map");
}
} // namespace War3Map::TerrainSaveTests
