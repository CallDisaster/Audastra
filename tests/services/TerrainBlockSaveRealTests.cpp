#include "TerrainBlockSaveRealTests.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainBlockEditing.h"
#include "War3MapCore/Services/MapEditSaveService.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace War3Map::TerrainSaveTests {
namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    if (result.Succeeded() && result.value) return std::move(*result.value);
    std::string details;
    for (const auto& note : result.diagnostics) details += note.code + ": " + note.message + "\n";
    throw std::runtime_error(details.empty() ? "missing real block save result" : details);
}
}

void CheckRealTerrainBlockSave(const MapArchiveSession& original, const std::filesystem::path& directory,
    const std::filesystem::path& tool, const std::filesystem::path& game) {
    using namespace MapData;
    using namespace Services;
    const auto scenario = directory / "terrain-block-public-save";
    Require(std::filesystem::create_directory(scenario), "exclusive block public-save evidence directory");
    const auto originalSha = Take(Sha256File(original.SourcePath()));
    auto before = *Take(ParseMapTerrain(Take(original.ReadMember("war3map.w3e")))).Snapshot();
    Require(before.header.width == 65 && before.header.height == 65, "real block fixture uses the original classic 64-cell map");
    const TerrainCellRect cells{16, 16, 20, 20};
    // 原创平地只改变源区raw高度和合法地面变体，不改变任何原生通行贡献。
    // expected不调用块Prepare、笔刷或通行生成器。
    for (unsigned y = 0; y <= 4; ++y) for (unsigned x = 0; x <= 4; ++x) {
        auto& vertex = before.vertices[(cells.y0 + y) * 65 + cells.x0 + x];
        Require(!vertex.HasWater() && !vertex.IsRamp() && !vertex.IsBoundary() && !vertex.IsMapEdge()
            && vertex.CliffLevel() == 2 && vertex.groundHeightRaw <= 16000,
            "block fixture is dry playable interior without cliffs or unknown flags");
        vertex.groundHeightRaw = static_cast<std::uint16_t>(vertex.groundHeightRaw + 7 * (1 + x + y * 5));
        vertex.variationRaw = static_cast<std::uint8_t>((vertex.variationRaw & 0xf0) | ((x + y) % 4));
    }
    RefreshTerrainDerived(before, 16, 16, 21, 21);
    const auto sourceTerrain = Take(SerializeMapTerrain(before));
    const std::array replacements{MemberReplacement{"war3map.w3e", sourceTerrain}};
    const auto source = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "source.w3x", replacements)).path));
    auto expected = before;
    for (unsigned y = 0; y <= 4; ++y) for (unsigned x = 0; x <= 4; ++x)
        expected.vertices[(40 + y) * 65 + 40 + x] = before.vertices[(16 + y) * 65 + 16 + x];
    const auto expectedTerrain = Take(SerializeMapTerrain(expected));
    Require(expectedTerrain != sourceTerrain, "independent block literal must change the target");
    MapEditSaveRequest request;
    request.sourceMap = source.SourcePath(); request.toolRoot = tool; request.gameRoot = game;
    request.expectedSourceSha256 = source.SourceSha256(); request.expectedTerrainSha256 = Take(Sha256(sourceTerrain));
    MapEditTerrainStroke stroke;
    stroke.blockPaste = TerrainBlockPasteIntent{1, cells, {40, 40}, {}};
    request.terrainStrokes = {stroke};
    std::vector<std::byte> minimap;
    for (const bool suppliedDigest : {false, true}) {
        if (suppliedDigest) request.terrainStrokes[0].blockPaste->expectedBlockSha256 = Take(CaptureTerrainBlockFingerprint(before, cells));
        request.targetCandidate = scenario / (suppliedDigest ? "bound-digest.w3x" : "captured-digest.w3x");
        const auto saved = Take(SaveMapEditCandidate(request));
        const auto output = Take(MapArchiveSession::Open(request.targetCandidate));
        Require(saved.appliedStrokeCount == 1 && saved.scriptPreserved
            && Take(output.ReadMember("war3map.j")) == Take(source.ReadMember("war3map.j")),
            "public block save keeps the actual toolchain-checked source script without an unrelated rewrite");
        Require(saved.generatedMissingMinimap.has_value(), "public block save rebuilds the automatic minimap from final terrain");
        Require(Take(output.ReadMember("war3map.w3e")) == expectedTerrain,
            "actual public block save matches every independently constructed W3E byte");
        for (const char* name : {"war3map.wpm", "war3map.shd", "war3map.doo", "war3mapUnits.doo"}) {
            Require(output.HasMember(name) == source.HasMember(name), "block save preserves unrelated member presence");
            if (source.HasMember(name)) Require(Take(output.ReadMember(name)) == Take(source.ReadMember(name)),
                "height/variation block preserves complete actual pathing, shadow and placement bytes");
        }
        const auto image = Take(output.ReadMember("war3mapMap.blp"));
        if (suppliedDigest) Require(image == minimap, "actual content noise is independent of who captured the source-block digest");
        else minimap = image;
        Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "public block SaveAs leaves source map unchanged");
        std::cout << "block-public-save supplied_digest=" << suppliedDigest << " source_sha=" << source.SourceSha256()
            << " output_sha=" << output.SourceSha256() << '\n';
    }
    request.terrainStrokes[0].blockPaste->expectedBlockSha256.assign(64, '0');
    request.targetCandidate = scenario / "stale-digest-rejected.w3x";
    const auto refused = SaveMapEditCandidate(request);
    Require(!refused.Succeeded() && !std::filesystem::exists(request.targetCandidate)
        && std::any_of(refused.diagnostics.begin(), refused.diagnostics.end(), [](const auto& note) {
            return note.code == "TERRAIN_BLOCK_SOURCE_CHANGED";
        }), "public block save must reject a stale digest before publishing any candidate");
    Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256()
        && Take(Sha256File(original.SourcePath())) == originalSha, "all real block sources remain read-only");
}
} // namespace War3Map::TerrainSaveTests
