#include "TerrainPlayableBoundsRealTests.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapMinimapMarkers.h"
#include "War3MapCore/MapData/MapShadow.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>

namespace War3Map::TerrainSaveTests {
namespace {
using namespace MapData;
using namespace Services;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kBytes = 64u * 1024u * 1024u;
constexpr std::size_t kTerrainHeader = 69, kVertices = 65 * 65, kPathingPixels = 256 * 256;
constexpr TerrainPlayableRectangle kOriginal{6, 4, 58, 56}, kInset{10, 9, 54, 55};

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Check(Status status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& item : status.diagnostics) message += item.code + ": " + item.message + "\n";
    throw std::runtime_error(message.empty() ? "missing playable bounds status" : message);
}
template<class T> T Take(Result<T> result) {
    if (result.Succeeded() && result.value) return std::move(*result.value);
    Check({result.diagnostics});
    throw std::runtime_error("missing playable bounds result");
}

// 读取本次实际winning表中的三个布尔字段；不借用产品通行生成器计算预期。
std::byte DryGroundMask(const Game::GameFileContents& file, const std::string& rawcode) {
    const std::string_view text(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size());
    const auto rows = Take(Text::ParseSlkCells(text, Text::SlkCellMode::CatalogUnique));
    Require(!rows.empty(), "actual bounds terrain table has a header");
    constexpr std::array<const char*, 4> names{"tileid", "walkable", "flyable", "buildable"};
    std::array<int, 4> columns{};
    for (const auto& [column, field] : rows.begin()->second)
        for (std::size_t i = 0; i < names.size(); ++i) if (Text::LowerAscii(field) == names[i]) {
            Require(columns[i] == 0, "actual bounds terrain fields have unique columns"); columns[i] = column;
        }
    for (const auto column : columns) Require(column != 0, "actual bounds terrain fields are complete");
    unsigned mask = 0x40;
    std::size_t matches = 0;
    for (auto row = std::next(rows.begin()); row != rows.end(); ++row) {
        const auto id = row->second.find(columns[0]);
        if (id == row->second.end() || id->second != rawcode) continue;
        ++matches;
        constexpr std::array<unsigned, 3> restrictions{0x02, 0x04, 0x08};
        for (std::size_t i = 0; i < restrictions.size(); ++i) {
            const auto value = row->second.find(columns[i + 1]);
            Require(value != row->second.end() && (value->second == "0" || value->second == "1"),
                "actual bounds terrain permissions are explicit booleans");
            if (value->second == "0") mask |= restrictions[i];
        }
    }
    Require(matches == 1, "actual bounds ground rawcode is unique and present");
    return static_cast<std::byte>(mask);
}

bool Outside(const TerrainPlayableRectangle& rectangle, unsigned x, unsigned y) {
    return x < rectangle.west || x >= rectangle.east || y < rectangle.south || y >= rectangle.north;
}

Bytes ExpectedTerrain(const Bytes& source, const TerrainPlayableRectangle& rectangle) {
    Require(source.size() == kTerrainHeader + kVertices * 7, "authored classic W3E has the literal palette/header layout");
    auto output = source;
    // 每地格只改SW顶点的water word bit14；北末行、东末列及其它原字节逐字保留。
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
        auto& high = output[kTerrainHeader + (std::size_t{y} * 65 + x) * 7 + 3];
        high = (high & std::byte{0xbf}) | (Outside(rectangle, x, y) ? std::byte{0x40} : std::byte{0});
    }
    return output;
}

Bytes ExpectedPathing(const Bytes& source, const TerrainPlayableRectangle& rectangle, std::byte ground) {
    Require(source.size() == 16 + kPathingPixels, "authored WPM has the complete 256-square body");
    auto output = source;
    for (unsigned y = 0; y < 256; ++y) for (unsigned x = 0; x < 256; ++x) {
        const auto index = 16 + std::size_t{y} * 256 + x;
        output[index] = (source[index] & std::byte{0x11})
            | (Outside(rectangle, x / 4, y / 4) ? std::byte{0xce} : ground);
    }
    return output;
}

MapSettings ExpectedSettings(const MapSettings& source, const std::array<float, 4>& actual,
    const TerrainPlayableRectangle& rectangle) {
    for (const auto margin : actual) Require(std::isfinite(margin) && margin >= 0 && margin <= 65536
        && std::trunc(margin) == margin, "actual CameraMargins are in the supported native integer domain");
    Require(rectangle.east - rectangle.west > double(actual[0]) + actual[1]
        && rectangle.north - rectangle.south > double(actual[2]) + actual[3],
        "authored playable rectangle leaves a nonempty actual camera domain");
    auto output = source;
    // 夹具原点是(-4096,-4096)；独立写八个坐标，不调用产品的bounds投影或crop。
    const float west = static_cast<float>(-4096.0 + (rectangle.west + double(actual[0])) * 128.0);
    const float south = static_cast<float>(-4096.0 + (rectangle.south + double(actual[2])) * 128.0);
    const float east = static_cast<float>(-4096.0 + (rectangle.east - double(actual[1])) * 128.0);
    const float north = static_cast<float>(-4096.0 + (rectangle.north - double(actual[3])) * 128.0);
    output.cameraBounds = {west, south, east, north, west, north, east, south};
    output.cameraMargins = {static_cast<std::int32_t>(rectangle.west), static_cast<std::int32_t>(64 - rectangle.east),
        static_cast<std::int32_t>(rectangle.south), static_cast<std::int32_t>(64 - rectangle.north)};
    output.playableWidth = static_cast<std::int32_t>(rectangle.east - rectangle.west);
    output.playableHeight = static_cast<std::int32_t>(rectangle.north - rectangle.south);
    output.flags |= 0x400u;
    return output;
}

std::string CameraCall(const TerrainPlayableRectangle& rectangle) {
    const auto coordinate = [](unsigned cell) { return std::to_string(-4096 + static_cast<int>(cell) * 128) + ".0"; };
    const auto left = [&](unsigned x) { return coordinate(x) + " + GetCameraMargin(CAMERA_MARGIN_LEFT)"; };
    const auto right = [&](unsigned x) { return coordinate(x) + " - GetCameraMargin(CAMERA_MARGIN_RIGHT)"; };
    const auto bottom = [&](unsigned y) { return coordinate(y) + " + GetCameraMargin(CAMERA_MARGIN_BOTTOM)"; };
    const auto top = [&](unsigned y) { return coordinate(y) + " - GetCameraMargin(CAMERA_MARGIN_TOP)"; };
    // 保持既有脚本生成器契约：final W3I margins定位地格边，运行时再施GetCameraMargin。
    return "call SetCameraBounds( " + left(rectangle.west) + ", " + bottom(rectangle.south) + ", "
        + right(rectangle.east) + ", " + top(rectangle.north) + ", " + left(rectangle.west) + ", "
        + top(rectangle.north) + ", " + right(rectangle.east) + ", " + bottom(rectangle.south) + " )";
}
} // namespace

void CheckRealPlayableBoundsSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game) {
    const auto scenario = directory / "playable-bounds-inset-restore";
    Require(std::filesystem::create_directory(scenario), "exclusive playable bounds save scenario");
    const auto originalTerrainBytes = Take(original.ReadMember("war3map.w3e"));
    const auto originalTerrain = *Take(ParseMapTerrain(originalTerrainBytes)).Snapshot();
    Require(originalTerrain.header.width == 65 && originalTerrain.header.height == 65
        && originalTerrain.header.offset == std::array<float, 2>{-4096, -4096},
        "bounds real save uses the original 65-square centered flat fixture");
    for (const auto& vertex : originalTerrain.vertices) Require(vertex.groundHeightRaw == 8192
        && vertex.GroundTextureIndex() == 0 && vertex.CliffLevel() == 2 && !vertex.HasWater()
        && !vertex.IsRamp() && !vertex.IsBoundary() && !vertex.IsBlight(),
        "independent bounds WPM oracle has only uniform dry flat terrain and MapEdge");
    Require(ExpectedTerrain(originalTerrainBytes, kOriginal) == originalTerrainBytes,
        "original authored MapEdge matches the literal original rectangle");
    const auto originalSettings = Take(MapSettings::Parse(Take(original.ReadMember("war3map.w3i"))));
    Require(originalSettings.cameraMargins == std::array<std::int32_t, 4>{6, 6, 4, 8}
        && originalSettings.playableWidth == 52 && originalSettings.playableHeight == 52,
        "original fixture has the known non-noop starting playable rectangle");
    const auto originalScript = Take(original.ReadMember("war3map.j"));
    const std::string_view originalText(reinterpret_cast<const char*>(originalScript.data()), originalScript.size());
    Require(!Take(original.ReadMember("war3map.wtg")).empty() && !Take(original.ReadMember("war3map.wct")).empty()
        && originalText.find(CameraCall(kOriginal)) != std::string_view::npos,
        "CreateMapCandidate supplied actual WTG/WCT and a compiled camera initializer");
    Require(!original.HasMember("war3mapMap.tga") && !original.HasMember("TerrainArt/Terrain.slk"),
        "bounds fixture uses automatic BLP and the actual game-winning ground table");
    auto mounted = Take(Game::GameDataSource::MountStrict(game, originalTerrain.header.mainTileset));
    const auto table = Take(mounted.ReadFileBoundedWithSource("TerrainArt/Terrain.slk", 2u * 1024u * 1024u));
    const auto ground = DryGroundMask(table, originalTerrain.header.groundTilesets[0]);
    auto pathingSeed = Take(original.ReadMember("war3map.wpm"));
    Require(pathingSeed.size() == 16 + kPathingPixels, "original full WPM fixture is present");
    for (unsigned y = 0; y < 256; ++y) for (unsigned x = 0; x < 256; ++x)
        pathingSeed[16 + std::size_t{y} * 256 + x] = static_cast<std::byte>((x % 7 == 1 ? 1 : 0) | (y % 11 == 2 ? 0x10 : 0));
    pathingSeed = ExpectedPathing(pathingSeed, kOriginal, ground);
    Bytes shadow(kPathingPixels, std::byte{0});
    for (std::size_t y = 17; y < 24; ++y) for (std::size_t x = 31; x < 36; ++x) shadow[y * 256 + x] = std::byte{255};
    shadow[211 * 256 + 47] = std::byte{37}; shadow[103 * 256 + 192] = std::byte{64};
    MinimapMarkersSnapshot staleMarkers;
    staleMarkers.records = {{0, {31, 43}, {0x11, 0x44, 0xaa, 0xff}}, {1, {177, 67}, {0xaa, 0xbb, 0x44, 0xff}}};
    const auto markerSeed = Take(SerializeMapMinimapMarkers(staleMarkers));
    const Bytes sentinel{std::byte{0}, std::byte{0xff}, std::byte{0x93}, std::byte{0x1b}};
    const std::array<MemberReplacement, 4> seed{{{"war3map.wpm", pathingSeed}, {"war3map.shd", shadow},
        {"war3map.mmp", markerSeed}, {"Fixture/BoundsUnrelated.bin", sentinel}}};
    auto source = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "source.w3x", seed)).path));
    std::vector<std::pair<std::filesystem::path, std::string>> sources{{original.SourcePath(), original.SourceSha256()},
        {source.SourcePath(), source.SourceSha256()}};
    const auto verifySources = [&] {
        for (const auto& [path, sha] : sources) Require(Take(Sha256File(path)) == sha,
            "bounds save preserves every actual input archive byte-for-byte");
    };

    for (const bool restore : {false, true}) {
        const auto rectangle = restore ? kOriginal : kInset;
        const auto beforeBytes = Take(source.ReadMember("war3map.w3e"));
        const auto before = *Take(ParseMapTerrain(beforeBytes)).Snapshot();
        const auto resources = Take(TerrainPathingSave::CaptureNative(source, before, before, game, kBytes));
        Require(resources.NativeCameraMargins() != nullptr, "bounds edit owns actual native CameraMargins");
        const auto actual = *resources.NativeCameraMargins();
        const auto expectedTerrain = ExpectedTerrain(originalTerrainBytes, rectangle);
        const auto expectedPathing = ExpectedPathing(pathingSeed, rectangle, ground);
        const auto expectedSettings = ExpectedSettings(Take(MapSettings::Parse(Take(source.ReadMember("war3map.w3i")))), actual, rectangle);
        const auto expectedW3i = Take(expectedSettings.Serialize());
        const auto expectedW3iSha = Take(Sha256(expectedW3i));
        Require(expectedTerrain != beforeBytes, "both inset and restore commands change actual MapEdge bytes");
        const auto actualG0 = Take(resources.GenerateNative(before, kBytes));
        const auto beforeWpm = Take(source.ReadMember("war3map.wpm"));
        Require(actualG0.size() == kPathingPixels, "actual G0 covers the complete source WPM body");
        for (std::size_t i = 0; i < kPathingPixels; ++i) Require(actualG0[i] == (beforeWpm[16 + i] & std::byte{0xee}),
            "literal complete old pathing independently agrees with the actual winning native rules");

        MapDataEditSession edits(MapDocument::FromArchive(source));
        Check(edits.AttachTerrain(beforeBytes)); Check(edits.AttachPathing(beforeWpm));
        const auto command = Take(edits.PrepareTerrainBoundsEdit(resources, {rectangle}));
        Require(command.HasChanges(), "actual bounds command is not a no-op");
        Check(edits.ApplyPreparedTerrainBoundsEdit(command));
        Require(Take(SerializeMapTerrain(*edits.Terrain())) == expectedTerrain
            && Take(edits.Snapshot().Settings()->Serialize()) == expectedW3i,
            "typed command matches independent full W3E and final W3I expectations before ordinary save");
        auto plan = std::make_shared<TerrainSavePlan>(Take(TerrainPathingSave::PrepareForSave(source, edits, game)));
        Check(plan->SaveGate()); Require(plan->RebuiltPathing(), "bounds ordinary save uses complete G0/G1 regeneration");
        MapSaveTask expectedTask;
        Check(expectedTask.FreezeArchive(source)); expectedTask.target = scenario / (restore ? "restored.w3x" : "inset.w3x");
        expectedTask.replacements = edits.Replacements(); Check(plan->InstallPathingOutput(edits, expectedTask.replacements));
        const auto markerOwner = Take(TerrainMinimapMarkers::Capture(source, game, expectedTask.replacements));
        Require(std::any_of(markerOwner->Dependencies().begin(), markerOwner->Dependencies().end(), [&](const auto& dependency) {
            return Text::LowerAscii(dependency.name) == "war3map.w3i" && dependency.source == TerrainMinimapMarkerSource::Map
                && dependency.sha256 == expectedW3iSha && dependency.bytes == expectedW3i.size();
        }), "MMP owner captured the final W3I bytes and digest, rather than the old camera rectangle");
        Check(markerOwner->AdmitToPlan(*plan, edits));
        expectedTask.replacements.push_back({"war3map.mmp", markerOwner->Bytes()});
        const auto minimap = Take(TerrainMinimapSave::PrepareAutomaticOrPreserve(expectedTask, edits, game, {}, plan));
        Require(minimap.generated && !minimap.preserved && minimap.generated->ClassicManifest()
            && minimap.generated->ClassicManifest()->sourceRectangle
                == std::array<std::uint32_t, 4>{rectangle.west, rectangle.south, rectangle.east, rectangle.north},
            "automatic BLP owner consumes the final typed W3I playable rectangle without fallback");
        expectedTask.replacements.push_back(minimap.generated->MinimapReplacement());

        MapEditSaveRequest request;
        request.sourceMap = source.SourcePath(); request.targetCandidate = expectedTask.target;
        request.toolRoot = tool; request.gameRoot = game; request.expectedSourceSha256 = source.SourceSha256();
        request.expectedTerrainSha256 = Take(Sha256(beforeBytes)); request.minimapChoice = TerrainMinimapSaveChoice::Automatic;
        MapEditTerrainStroke stroke; stroke.bounds = TerrainBoundsEditIntent{rectangle}; request.terrainStrokes = {stroke};
        const auto saved = Take(SaveMapEditCandidate(request));
        auto readback = Take(MapArchiveSession::Open(saved.candidate));
        Require(Take(readback.ReadMember("war3map.w3e")) == expectedTerrain
            && Take(readback.ReadMember("war3map.w3i")) == expectedW3i
            && Take(readback.ReadMember("war3map.wpm")) == expectedPathing,
            "ordinary bounds save reopens with complete independently expected W3E, W3I and WPM bytes");
        const auto actualShadow = Take(readback.ReadMember("war3map.shd"));
        const auto parsedShadow = Take(ParseMapShadow(actualShadow, 256, 256));
        Require(actualShadow == shadow && parsedShadow.intensities.size() == kPathingPixels
            && std::count_if(parsedShadow.intensities.begin(), parsedShadow.intensities.end(), [](auto value) { return value != 0; }) == 37,
            "same-size playable bounds preserve every byte of asymmetric nonzero source SHD");
        const auto scriptBytes = Take(readback.ReadMember("war3map.j"));
        const std::string_view script(reinterpret_cast<const char*>(scriptBytes.data()), scriptBytes.size());
        const auto expectedCall = CameraCall(rectangle);
        const auto cameraPosition = script.find(expectedCall);
        Require(!saved.scriptPreserved && scriptBytes != Take(source.ReadMember("war3map.j"))
            && cameraPosition != std::string_view::npos
            && script.find("call SetCameraBounds(") == cameraPosition
            && script.find("call SetCameraBounds(", cameraPosition + expectedCall.size()) == std::string_view::npos
            && script.find(CameraCall(restore ? kInset : kOriginal)) == std::string_view::npos,
            "actual script rebuild and compiler path write exactly the final eight camera expressions instead of preserving the old script");
        const auto blp = Take(readback.ReadMember("war3mapMap.blp"));
        Require(saved.generatedMissingMinimap && blp == minimap.generated->MinimapReplacement().data
            && saved.generatedMissingMinimap->encodedBlpSha256 == Take(Sha256(blp))
            && saved.generatedMissingMinimap->rangePolicy == Render::TerrainMinimapRangePolicy::ClassicPlayable
            && saved.generatedMissingMinimap->encodedMipCount == 1
            && saved.generatedMissingMinimap->width == 256 && saved.generatedMissingMinimap->height == 256,
            "ordinary final BLP is byte-identical to the final-W3I automatic owner and independently reopened receipt");
        const auto markers = Take(readback.ReadMember("war3map.mmp"));
        Require(markers == markerOwner->Bytes() && markers != markerSeed
            && Take(ParseMapMinimapMarkers(markers)).records.size() == markerOwner->MarkerCount(),
            "ordinary MMP replaces the stale source seed and matches the final-W3I dependency owner");
        Check(markerOwner->VerifyReadback(readback)); Check(minimap.generated->VerifyReadback(expectedTask, readback));
        Check(resources.VerifyReadback(source, kBytes));
        Require(saved.sourceSha256 == source.SourceSha256() && saved.candidateSha256 == readback.SourceSha256()
            && saved.candidateSha256 == Take(Sha256File(saved.candidate)) && saved.appliedStrokeCount == 1
            && saved.originalTerrainSha256 == Take(Sha256(beforeBytes))
            && saved.savedTerrainSha256 == Take(Sha256(expectedTerrain))
            && saved.savedPathingSha256 == Take(Sha256(expectedPathing))
            && saved.appliedFieldEditCount == 0 && saved.appliedPlacementCount == 0,
            "actual bounds receipt binds the complete source, candidate and one typed operation");
        for (const auto& member : source.Members()) {
            const auto name = Text::LowerAscii(member.name);
            if (name == "war3map.w3e" || name == "war3map.w3i" || name == "war3map.wpm" || name == "war3map.j"
                || name == "war3mapmap.blp" || name == "war3map.mmp") continue;
            Require(Take(readback.ReadMember(member.name)) == member.data, "bounds save preserves every unrelated actual source member");
        }
        verifySources();
        std::cout << "playable_bounds_save_passed restore=" << restore << " source_sha256=" << source.SourceSha256()
            << " candidate_sha256=" << saved.candidateSha256 << " w3e_sha256=" << Take(Sha256(expectedTerrain))
            << " w3i_sha256=" << expectedW3iSha << " wpm_sha256=" << Take(Sha256(expectedPathing))
            << " shd_sha256=" << Take(Sha256(shadow)) << " blp_sha256=" << Take(Sha256(blp))
            << " mmp_sha256=" << Take(Sha256(markers)) << " actual_camera_margins="
            << actual[0] << ',' << actual[1] << ',' << actual[2] << ',' << actual[3]
            << " actual_ground_table_winner=" << table.archiveName << " actual_ground_table_sha256=" << Take(Sha256(table.bytes))
            << " shd_nonzero=37 script_preserved=false script_seconds=" << saved.scriptSeconds
            << " source_unchanged=true expected_geometry_uses_product_projection=false expected_wpm_uses_product_generator=false"
            << " expected_blp_uses_product_generator=true expected_mmp_uses_product_generator=true"
            << " native_oracle=false native_editor_verified=false game_verified=false\n";
        sources.emplace_back(readback.SourcePath(), readback.SourceSha256());
        source = std::move(readback);
    }
    Require(Take(source.ReadMember("war3map.w3e")) == originalTerrainBytes
        && Take(source.ReadMember("war3map.wpm")) == pathingSeed
        && Take(source.ReadMember("Fixture/BoundsUnrelated.bin")) == sentinel,
        "second ordinary save restores complete original W3E/WPM and keeps unrelated binary content");
    verifySources();
}
} // namespace War3Map::TerrainSaveTests
