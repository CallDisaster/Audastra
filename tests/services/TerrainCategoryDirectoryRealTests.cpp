#include "TerrainCategoryDirectoryRealTests.h"

#include "War3MapCore/Game/NativeDoodadCategoryDirectory.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainDoodadFootprint.h"
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

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class T> T Take(Result<T> result) {
    if (result.Succeeded() && result.value) return std::move(*result.value);
    std::string message;
    for (const auto& item : result.diagnostics) message += item.code + ": " + item.message + "\n";
    throw std::runtime_error(message.empty() ? "missing category source result" : message);
}

void Check(const Status& status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& item : status.diagnostics) message += item.code + ": " + item.message + "\n";
    throw std::runtime_error(message);
}
} // namespace

void CheckRealCategoryDirectorySave(const MapArchiveSession& source,
    const MapEditSaveRequest& request, const DoodadPlacements& placements,
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    std::span<const std::byte> footprintBytes) {
    const auto directory = request.targetCandidate.parent_path();
    const auto categories = Take(Game::NativeDoodadCategoryDirectory::Capture(request.gameRoot,
        Game::NativeEditorStartupRules::Classic127aExpansion, kBytes));
    const auto snap = categories.CategoryCSnapCells();
    Require(snap == 2 || snap == 4, "actual startup source determines a supported C grid");
    auto shifted = placements;
    Require(shifted.doodads.size() >= 2, "category fixture has a final ordinary C placement");
    shifted.doodads.back().position[0] += 64;
    const auto footprint = Take(DecodeDoodadFootprintTga(footprintBytes));
    Require(footprint.uniform && footprint.width == footprint.height,
        "actual category fixture uses an unambiguous uniform square");
    const auto cells = std::size_t{before.header.width - 1} * (before.header.height - 1) * 16;
    std::vector<std::uint8_t> two(cells), four(cells), unshifted(cells);
    Check(AddDoodadFootprint(two, before.header, shifted.doodads.back(), footprint, 2, std::nullopt));
    Check(AddDoodadFootprint(four, before.header, shifted.doodads.back(), footprint, 4, std::nullopt));
    Check(AddDoodadFootprint(unshifted, before.header, placements.doodads.back(), footprint, snap, std::nullopt));
    Require(two != four && unshifted != (snap == 2 ? two : four),
        "shifted actual C placement distinguishes both grids and changes its previous footprint");
    // 原场景为整图深水，可能遮住对象的同类OR位。C区改为干地，只留
    // 本次水位笔刷的中心邻域有水，确保旧占地位移确实造成可测的WPM差异。
    auto dryBefore = before, dryAfter = after;
    for (std::size_t y = 0; y < before.header.height; ++y) for (std::size_t x = 0; x < before.header.width; ++x) {
        if (x >= 31 && x <= 32 && y >= 31 && y <= 32) continue;
        const auto i = y * before.header.width + x;
        dryBefore.vertices[i].textureAndFlagsRaw &= static_cast<std::uint8_t>(~0x40u);
        dryAfter.vertices[i].textureAndFlagsRaw &= static_cast<std::uint8_t>(~0x40u);
    }
    RefreshTerrainDerived(dryBefore, 0, 0, before.header.width, before.header.height);
    RefreshTerrainDerived(dryAfter, 0, 0, after.header.width, after.header.height);
    const auto dryBeforeBytes = Take(SerializeMapTerrain(dryBefore));
    const std::array<MemberReplacement, 1> dryTerrain{{{"war3map.w3e", dryBeforeBytes}}};
    const auto drySeed = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
        directory / "grid-dry-seeded.w3x", dryTerrain)).path));
    const auto dryRules = Take(TerrainPathingSave::CaptureNative(drySeed, dryBefore, dryAfter, request.gameRoot, kBytes));
    const auto baselineNative = Take(dryRules.GenerateNative(dryBefore, kBytes));
    auto originalWpm = Take(source.ReadMember("war3map.wpm"));
    Require(baselineNative.size() == cells && originalWpm.size() == cells + 16, "dry category fixture pathing dimensions");
    for (std::size_t i = 0; i < cells; ++i)
        originalWpm[i + 16] = (originalWpm[i + 16] & std::byte{0x11}) | baselineNative[i];
    const std::array<MemberReplacement, 1> dryPathing{{{"war3map.wpm", originalWpm}}};
    const auto baseline = Take(MapArchiveSession::Open(Take(drySeed.SaveCandidate(
        directory / "grid-dry-source.w3x", dryPathing)).path));
    const auto shiftedDoo = Take(shifted.Serialize());
    const std::array<MemberReplacement, 1> changed{{{"war3map.doo", shiftedDoo}}};
    const auto mismatched = Take(MapArchiveSession::Open(Take(baseline.SaveCandidate(
        directory / "shifted-grid-source.w3x", changed)).path));
    Require(Take(mismatched.ReadMember("war3map.wpm")) == originalWpm,
        "shifted source intentionally retains all previous WPM bytes");
    const auto rules = Take(TerrainPathingSave::CaptureNative(mismatched, dryBefore, dryAfter, request.gameRoot, kBytes));
    const auto oldNative = Take(rules.GenerateNative(dryBefore, kBytes));
    const auto newNative = Take(rules.GenerateNative(dryAfter, kBytes));
    Require(oldNative.size() == cells && newNative.size() == cells,
        "category save keeps complete native pathing dimensions");
    auto correctOldWpm = originalWpm, expectedWpm = originalWpm;
    for (std::size_t i = 0; i < cells; ++i) {
        correctOldWpm[i + 16] = (originalWpm[i + 16] & std::byte{0x11}) | oldNative[i];
        expectedWpm[i + 16] = (originalWpm[i + 16] & std::byte{0x11}) | newNative[i];
    }
    Require(correctOldWpm != originalWpm && expectedWpm != correctOldWpm,
        "real source C relocation and subsequent water edit both change pathing");
    auto blocked = request;
    blocked.sourceMap = mismatched.SourcePath(); blocked.expectedSourceSha256 = mismatched.SourceSha256();
    blocked.expectedTerrainSha256 = Take(Sha256(dryBeforeBytes));
    blocked.targetCandidate = directory / "grid-must-not-exist.w3x";
    const auto rejected = SaveMapEditCandidate(blocked);
    Require(!rejected.Succeeded() && !rejected.value
        && std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "TERRAIN_PATHING_SOURCE_UNEXPLAINED";
        }), "known C grid must still reject a source with unexplained old WPM");
    Require(!std::filesystem::exists(blocked.targetCandidate)
        && Take(Sha256File(mismatched.SourcePath())) == mismatched.SourceSha256()
        && Take(Sha256File(source.SourcePath())) == source.SourceSha256(),
        "refused C save preserves both sources and publishes nothing");

    // 与既有实际资源专项一致，完整WPM预期复用生成器；字面网格预期另在Core/Qt覆盖。
    const std::array<MemberReplacement, 1> repaired{{{"war3map.wpm", correctOldWpm}}};
    const auto consistent = Take(MapArchiveSession::Open(Take(mismatched.SaveCandidate(
        directory / "shifted-grid-consistent-source.w3x", repaired)).path));
    auto accepted = request;
    accepted.sourceMap = consistent.SourcePath(); accepted.expectedSourceSha256 = consistent.SourceSha256();
    accepted.expectedTerrainSha256 = blocked.expectedTerrainSha256;
    accepted.targetCandidate = directory / "shifted-grid-saved.w3x";
    const auto saved = Take(SaveMapEditCandidate(accepted));
    const auto readback = Take(MapArchiveSession::Open(saved.candidate));
    Require(saved.scriptPreserved && Take(readback.ReadMember("war3map.wpm")) == expectedWpm
        && Take(readback.ReadMember("war3map.w3e")) == Take(SerializeMapTerrain(dryAfter)),
        "known C grid permits public water save with complete W3E and WPM readback");
    for (const auto& member : consistent.Members()) {
        const auto name = Text::LowerAscii(member.name);
        if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp") continue;
        Require(Take(readback.ReadMember(member.name)) == member.data,
            "category save preserves every unrelated member, including shifted DOO and script");
    }
    Check(categories.VerifySources(kBytes));
    Check(rules.VerifyReadback(readback, kBytes));
    Require(Take(Sha256File(consistent.SourcePath())) == consistent.SourceSha256()
        && Take(Sha256File(mismatched.SourcePath())) == mismatched.SourceSha256()
        && Take(Sha256File(baseline.SourcePath())) == baseline.SourceSha256()
        && Take(Sha256File(drySeed.SourcePath())) == drySeed.SourceSha256()
        && Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "all actual C sources remain untouched");
    std::cout << "category_c_grid_source_mismatch_refused source_sha256=" << mismatched.SourceSha256() << '\n';
    std::cout << "category_c_grid_save_passed snap_cells=" << unsigned(snap)
        << " source_sha256=" << consistent.SourceSha256() << " candidate_sha256=" << saved.candidateSha256
        << " native_editor_verified=false game_verified=false\n";
}
} // namespace War3Map::TerrainSaveTests
