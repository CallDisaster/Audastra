#include "TerrainShadowRealTests.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/MapData/MapMinimapMarkers.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/MapShadow.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/MapData/TerrainShadowResources.h"
#include "War3MapCore/Script/MapScriptBuild.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/TerrainEditService.h"
#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Services/TerrainShadowReceiverResources.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace War3Map::TerrainSaveTests {
namespace {
using namespace MapData;
using namespace Services;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kBytes = 64u * 1024u * 1024u;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    if (result.Succeeded() && result.value) return std::move(*result.value);
    std::string message;
    for (const auto& item : result.diagnostics) message += item.code + ": " + item.message + "\n";
    throw std::runtime_error(message.empty() ? "missing actual shadow save result" : message);
}
void Check(const Status& status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& item : status.diagnostics) message += item.code + ": " + item.message + "\n";
    throw std::runtime_error(message);
}
} // namespace

#include "TerrainShadowReceiverRealCases.h"

void CheckRealTerrainShadowSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game) {
    const auto scenario = directory / "playable-explicit-shadow";
    Require(std::filesystem::create_directory(scenario), "exclusive actual shadow scenario");
    const auto terrainBytes = Take(original.ReadMember("war3map.w3e"));
    const auto terrain = *Take(ParseMapTerrain(terrainBytes)).Snapshot();
    Require(terrain.header.width == 65 && terrain.header.height == 65 && terrain.vertices.size() == 65 * 65,
        "actual shadow fixture has the complete original 64-cell grid");
    const auto& first = terrain.vertices.front();
    Require(std::all_of(terrain.vertices.begin(), terrain.vertices.end(), [&](const auto& vertex) {
        return vertex.groundHeightRaw == first.groundHeightRaw && vertex.CliffLevel() == first.CliffLevel()
            && !vertex.IsRamp() && !vertex.HasWater();
    }), "literal zero-shadow oracle requires the entire actual source to be ordinary dry flat terrain");
    const auto doodads = Take(ParseDoodadPlacements(Take(original.ReadMember("war3map.doo"))));
    Require(doodads.doodads.empty() && doodads.specialDoodads.empty(), "actual shadow fixture has no omitted doodad caster");

    // 从本机真实完整profile链取方向并复核，不由测试传入一个已验证布尔值或默认向量。
    const auto light = Take(TerrainShadowResources::Capture(game,
        Game::NativeEditorStartupRules::Classic127aExpansion, kBytes));
    const auto direction = light.LightDirection();
    Require(std::all_of(direction.begin(), direction.end(), [](float value) { return std::isfinite(value); })
        && direction[2] > 0 && (direction[0] != 0 || direction[1] != 0),
        "actual light must have an above-terrain nonvertical direction for the literal flat zero-shadow case");
    Require(light.SourceBytes() > 0 && light.CaptureBudgetBytes() > 0 && !light.DependencyFingerprint().empty(),
        "actual direction retains its source profile identity and capture budget");
    Check(light.VerifySources(kBytes));

    const auto width = (terrain.header.width - 1) * 4, height = (terrain.header.height - 1) * 4;
    const auto pixels = std::size_t{width} * height;
    // 使用非binary旧SHD哨兵，证明显式计算是完整00/FF编码而非保留旧高位。
    const Bytes oldShadow(pixels, std::byte{37});
    const Bytes sentinel{std::byte{0}, std::byte{'S'}, std::byte{0xff}, std::byte{0x7d}, std::byte{0x80}};
    const auto emptyUnits = Take(UnitPlacements{}.Serialize());
    const std::array<MemberReplacement, 3> seed{{{"war3map.shd", oldShadow},
        {"war3mapUnits.doo", emptyUnits}, {"Fixture/ShadowUnrelated.bin", sentinel}}};
    const auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "seeded.w3x", seed)).path));
    // 原创模板默认有sloc；本独立副本明确换为空DOO单位集合。已编译脚本里的
    // CreateUnit是运行时代码，不冒充DOO摆放，也不因这次显式SHD改写作者脚本。
    Require(Take(ParseUnitPlacements(Take(seeded.ReadMember("war3mapUnits.doo")))).units.empty(),
        "actual shadow scene explicitly contains no DOO units");
    // 仅MMP基线复用产品生成器，使后续保存的无关成员都可逐字核对。
    // SHD预期仍是独立字面全零，不使用阴影采样器或投影器制造expected。
    const auto markers = Take(TerrainMinimapMarkers::Capture(seeded, game));
    Require(markers->MarkerCount() == 0, "empty authored DOO unit scene has no minimap markers");
    const std::array<MemberReplacement, 1> markerSeed{{{"war3map.mmp", markers->Bytes()}}};
    const auto source = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "source.w3x", markerSeed)).path));
    Check(markers->VerifyReadback(source));
    const auto terrainSha = Take(Sha256(terrainBytes));
    Require(Take(source.ReadMember("war3map.w3e")) == terrainBytes && Take(source.ReadMember("war3map.shd")) == oldShadow,
        "shadow source keeps the full original terrain and nonbinary prior SHD");

    MapEditSaveRequest request;
    request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / "saved.w3x";
    request.toolRoot = tool; request.gameRoot = game;
    request.expectedSourceSha256 = source.SourceSha256(); request.expectedTerrainSha256 = terrainSha;
    request.rebuildTerrainShadow = true;
    Require(request.terrainStrokes.empty() && request.fieldEdits.empty(), "shadow-only request does not add a dummy terrain edit");

    auto stale = request;
    stale.targetCandidate = scenario / "stale-w3e-must-not-exist.w3x";
    stale.expectedTerrainSha256[0] = stale.expectedTerrainSha256[0] == '0' ? '1' : '0';
    bool progressed = false;
    const auto denied = SaveMapEditCandidate(stale, [&](std::string_view) { progressed = true; });
    Require(!denied.Succeeded() && !denied.value && !progressed && !std::filesystem::exists(stale.targetCandidate)
        && std::any_of(denied.diagnostics.begin(), denied.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "SAVE_TERRAIN_STALE"; }),
        "wrong actual W3E SHA rejects before script execution, shadow calculation or candidate publication");
    Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256()
        && Take(Sha256File(original.SourcePath())) == original.SourceSha256(), "stale request preserves both original source archives");

    const auto saved = Take(SaveMapEditCandidate(request));
    const auto readback = Take(MapArchiveSession::Open(saved.candidate));
    const auto shadowBytes = Take(readback.ReadMember("war3map.shd"));
    const auto parsed = Take(ParseMapShadow(shadowBytes, width, height));
    Require(shadowBytes.size() == pixels && shadowBytes == Bytes(pixels, std::byte{0})
        && std::all_of(parsed.intensities.begin(), parsed.intensities.end(), [](auto value) { return value == 0; }),
        "entire flat empty-caster actual shadow is literal all-zero bytes, without logical 0/1 or old high bits");
    Require(saved.scriptPreserved && saved.appliedStrokeCount == 0 && saved.appliedFieldEditCount == 0
        && saved.appliedPlacementCount == 0 && saved.editedMembers.empty() && !saved.generatedMissingMinimap,
        "upper shadow-only save keeps the checked authored script without inventing edits or rebuilding unrelated minimap imagery");
    Require(saved.sourceSha256 == source.SourceSha256() && saved.candidateSha256 == readback.SourceSha256()
        && saved.candidateSha256 == Take(Sha256File(saved.candidate))
        && saved.originalTerrainSha256 == terrainSha && saved.savedTerrainSha256 == terrainSha,
        "upper receipt binds actual source, candidate and unchanged W3E digests");
    for (const auto& member : source.Members()) {
        if (Text::LowerAscii(member.name) == "war3map.shd") continue;
        Require(Take(readback.ReadMember(member.name)) == member.data,
            "explicit shadow save preserves every unrelated original source member byte-for-byte");
    }
    Require(Take(readback.ReadMember("Fixture/ShadowUnrelated.bin")) == sentinel, "unrelated binary sentinel remains intact");
    Check(markers->VerifyReadback(readback)); Check(light.VerifySources(kBytes));
    Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256()
        && Take(Sha256File(original.SourcePath())) == original.SourceSha256(), "successful shadow candidate leaves both source archives unchanged");
    std::cout << "playable_explicit_shadow_save_passed source_sha256=" << source.SourceSha256()
        << " candidate_sha256=" << saved.candidateSha256 << " w3e_sha256=" << terrainSha
        << " shd_bytes=" << shadowBytes.size() << " shd_sha256=" << Take(Sha256(shadowBytes))
        << " prior_shd_fill=37 shd_nonzero=0 caster_count=0 doo_unit_count=0 source_unchanged=true"
        << " direction_x=" << direction[0] << " direction_y=" << direction[1] << " direction_z=" << direction[2]
        << " actual_direction_source_sha256=" << light.DependencyFingerprint()
        << " actual_direction_source_bytes=" << light.SourceBytes()
        << " actual_direction_capture_budget=" << light.CaptureBudgetBytes()
        << " expected_shadow_is_literal=true mmp_baseline_uses_product_generator=true"
        << " stale_w3e_rejected_before_execution=true native_oracle=false native_editor_verified=false game_verified=false\n";

    // 原版独立阴影计算不读取WPM；缺MapBLP也不能间接强迫这个请求创建WPM。
    // 只在原创副本上准确删除实际存在的成员，保留完整DOO/脚本/profile来源。
    const std::array<std::string, 3> absentNames{"war3map.wpm", "war3mapMap.blp", "war3mapMap.tga"};
    const auto originalImages = Take(source.DescribeExactMemberIdentities(source.SourceSha256(), absentNames));
    std::vector<std::string> removals;
    for (std::size_t i = 0; i < absentNames.size(); ++i)
        if (originalImages.members[i].present) removals.push_back(absentNames[i]);
    const auto withoutPathing = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
        scenario / "source-without-pathing-or-minimap.w3x", {}, removals)).path));
    const auto verifyAbsent = [&](const MapArchiveSession& archive) {
        const auto state = Take(archive.DescribeExactMemberIdentities(archive.SourceSha256(), absentNames));
        Require(std::all_of(state.members.begin(), state.members.end(), [](const auto& member) { return !member.present; }),
            "actual exact identities prove WPM, BLP and TGA absent rather than unlisted or unreadable");
    };
    verifyAbsent(withoutPathing);
    auto absentRequest = request;
    absentRequest.sourceMap = withoutPathing.SourcePath();
    absentRequest.expectedSourceSha256 = withoutPathing.SourceSha256();
    absentRequest.targetCandidate = scenario / "saved-without-pathing-or-minimap.w3x";
    Require(absentRequest.minimapChoice == TerrainMinimapSaveChoice::Automatic,
        "missing-WPM shadow regression uses the actual public default automatic minimap choice");
    const auto absentResult = SaveMapEditCandidate(absentRequest);
    const auto absentSaved = Take(absentResult);
    const auto absentReadback = Take(MapArchiveSession::Open(absentSaved.candidate));
    verifyAbsent(absentReadback);
    const auto absentShadow = Take(absentReadback.ReadMember("war3map.shd"));
    const auto absentParsed = Take(ParseMapShadow(absentShadow, width, height));
    Require(absentShadow == Bytes(pixels, std::byte{0}) && absentParsed.intensities.size() == pixels
        && std::all_of(absentParsed.intensities.begin(), absentParsed.intensities.end(), [](auto value) { return value == 0; }),
        "missing unrelated pathing/minimap still writes and independently reads every literal zero SHD pixel");
    Require(absentSaved.scriptPreserved && !absentSaved.generatedMissingMinimap
        && absentSaved.originalTerrainSha256 == terrainSha && absentSaved.savedTerrainSha256 == terrainSha
        && absentSaved.appliedStrokeCount == 0 && absentSaved.editedMembers.empty(),
        "shadow-only missing-WPM result does not invent terrain/pathing/image edits or replace the authored script");
    const auto warning = [&](std::string_view code) {
        return std::any_of(absentResult.diagnostics.begin(), absentResult.diagnostics.end(), [&](const auto& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Warning && diagnostic.code == code;
        });
    };
    Require(warning("CLASSIC_MINIMAP_GENERATION_UNSUPPORTED") && warning("MINIMAP_GENERATION_SKIPPED"),
        "known missing WPM is a visible bounded minimap capability warning, never a silent fallback or generic baseline waiver");
    for (const auto& member : withoutPathing.Members()) {
        if (Text::LowerAscii(member.name) == "war3map.shd") continue;
        Require(Take(absentReadback.ReadMember(member.name)) == member.data,
            "shadow-only missing-WPM save preserves all unrelated original members byte-for-byte");
    }
    Check(markers->VerifyReadback(absentReadback)); Check(light.VerifySources(kBytes));
    Require(absentSaved.sourceSha256 == withoutPathing.SourceSha256()
        && absentSaved.candidateSha256 == absentReadback.SourceSha256()
        && absentSaved.candidateSha256 == Take(Sha256File(absentSaved.candidate))
        && Take(Sha256File(withoutPathing.SourcePath())) == withoutPathing.SourceSha256()
        && Take(Sha256File(source.SourcePath())) == source.SourceSha256()
        && Take(Sha256File(original.SourcePath())) == original.SourceSha256(),
        "missing-WPM save binds the actual candidate identity and leaves every source archive unchanged");
    std::cout << "playable_explicit_shadow_without_pathing_minimap_passed source_sha256=" << withoutPathing.SourceSha256()
        << " candidate_sha256=" << absentSaved.candidateSha256 << " w3e_sha256=" << terrainSha
        << " shd_bytes=" << absentShadow.size() << " shd_sha256=" << Take(Sha256(absentShadow))
        << " wpm_absent=true map_blp_absent=true map_tga_absent=true minimap_capability_warning=true"
        << " expected_shadow_is_literal=true source_unchanged=true native_oracle=false native_editor_verified=false game_verified=false\n";
    CheckRealTerrainShadowReceiversSave(original, directory, tool, game);
}
} // namespace War3Map::TerrainSaveTests
