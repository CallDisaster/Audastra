#include "TerrainCliffTransportSaveTests.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <iterator>
#include <set>
#include <span>
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
    throw std::runtime_error(message.empty() ? "missing real cliff texture result" : message);
}

std::set<std::string> CliffIds(const Game::GameFileContents& source) {
    const auto text = std::string_view(reinterpret_cast<const char*>(source.bytes.data()), source.bytes.size());
    const auto cells = Take(Text::ParseSlkCells(text, Text::SlkCellMode::CatalogUnique));
    Require(!cells.empty(), "actual CliffTypes has a header");
    int column = -1;
    for (const auto& [index, name] : cells.begin()->second)
        if (Text::LowerAscii(name) == "cliffid") column = index;
    Require(column >= 0, "actual CliffTypes exposes cliffID");
    std::set<std::string> ids;
    for (auto row = std::next(cells.begin()); row != cells.end(); ++row) {
        const auto id = row->second.find(column);
        if (id != row->second.end()) Require(id->second.size() == 4 && ids.insert(id->second).second,
            "actual CliffTypes rawcodes are unique four-byte values");
    }
    return ids;
}

std::vector<std::byte> WithoutActualCliffRow(const Game::GameFileContents& source, const std::string& id) {
    std::string text(reinterpret_cast<const char*>(source.bytes.data()), source.bytes.size());
    const auto needle = '"' + id + '"';
    const auto offset = text.find(needle);
    Require(offset != text.npos && text.find(needle, offset + needle.size()) == text.npos,
        "missing-row negative test replaces exactly the real cliffID cell");
    Require(text.find("\"Zz00\"") == text.npos, "negative cliff row ID is absent from the actual table");
    text.replace(offset, needle.size(), "\"Zz00\"");
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    return {bytes.begin(), bytes.end()};
}
} // namespace

void CheckRealCliffTextureSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game, const Game::GameDataSource& mounted) {
    Require(!original.HasMember("TerrainArt/CliffTypes.slk"), "positive cliff fixture uses the actual game table winner");
    const auto table = Take(mounted.ReadFileBoundedWithSource("TerrainArt/CliffTypes.slk", 2u * 1024u * 1024u));
    const auto ids = CliffIds(table);
    auto before = *Take(ParseMapTerrain(Take(original.ReadMember("war3map.w3e")))).Snapshot();
    Require(before.header.width == 65 && before.header.height == 65 && !before.header.cliffTilesets.empty(),
        "pure cliff texture fixture uses the existing authored 64-cell source");
    const auto oldId = before.header.cliffTilesets.front();
    Require(ids.contains(oldId), "source palette row exists in actual CliffTypes");
    const auto alternate = std::find_if(ids.begin(), ids.end(), [&](const auto& id) { return id != oldId; });
    Require(alternate != ids.end(), "actual CliffTypes contains a distinct target palette row");
    before.header.cliffTilesets = {oldId, *alternate};
    // 构造已有的干地悬崖；纯换材质不能借CliffLevel(0)把起点高度/层级传播到邻点。
    for (std::uint32_t y = 0; y < 65; ++y) for (std::uint32_t x = 0; x < 65; ++x) {
        auto& vertex = before.vertices[std::size_t{y} * 65 + x];
        Require(!vertex.HasWater() && !vertex.IsRamp(), "pure cliff texture seed is dry and has no ramps");
        vertex.cliffAndLayerRaw = static_cast<std::uint8_t>(x <= 31 ? 3 : 2);
        vertex.variationRaw = static_cast<std::uint8_t>((vertex.variationRaw & 0x1f) | ((x + y) % 3) << 5);
    }
    before.vertices[32 * 65 + 31].groundHeightRaw = static_cast<std::uint16_t>(before.vertices[32 * 65 + 31].groundHeightRaw + 8);
    before.vertices[32 * 65 + 33].groundHeightRaw = static_cast<std::uint16_t>(before.vertices[32 * 65 + 33].groundHeightRaw + 20);
    RefreshTerrainDerived(before, 0, 0, 65, 65);
    auto after = before;
    for (std::uint32_t y = 31; y <= 33; ++y) for (std::uint32_t x = 31; x <= 33; ++x) {
        auto& vertex = after.vertices[std::size_t{y} * 65 + x];
        vertex.cliffAndLayerRaw = static_cast<std::uint8_t>((vertex.cliffAndLayerRaw & 0x0f) | 0x10);
    }
    const auto beforeBytes = Take(SerializeMapTerrain(before)), afterBytes = Take(SerializeMapTerrain(after));
    Require(beforeBytes != afterBytes, "independent nine-point palette-only byte expectation changes terrain");
    const auto scenario = directory / "playable-pure-cliff-texture";
    Require(std::filesystem::create_directory(scenario), "exclusive pure cliff texture scenario");
    const auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "seeded.w3x",
        std::array<MemberReplacement, 1>{{{"war3map.w3e", beforeBytes}}})).path));
    // 实际资源用于重现原WPM；纯材质笔划的新WPM预期严格等于来源，不取产品输出作oracle。
    const auto resources = Take(TerrainPathingSave::CaptureNative(seeded, before, after, game, kBytes));
    const auto oldNative = Take(resources.GenerateNative(before, kBytes));
    const auto newNative = Take(resources.GenerateNative(after, kBytes));
    Require(oldNative == newNative, "palette-only edit preserves actual native pathing on this seeded geometry");
    auto sourcePathing = Take(seeded.ReadMember("war3map.wpm"));
    Require(sourcePathing.size() == 16 + oldNative.size(), "source native pathing dimensions match WPM");
    for (std::size_t i = 0; i < oldNative.size(); ++i)
        sourcePathing[i + 16] = (sourcePathing[i + 16] & std::byte{0x11}) | oldNative[i];
    const auto source = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "source.w3x",
        std::array<MemberReplacement, 1>{{{"war3map.wpm", sourcePathing}}})).path));
    MapEditTerrainStroke stroke;
    stroke.pointSpace = MapEditPointSpace::TerrainVertex; stroke.points = {{32, 32}};
    stroke.brush.kind = TerrainBrushKind::CliffTexture; stroke.brush.shape = TerrainBrushShape::Square;
    stroke.brush.size = 2; stroke.brush.cliffTexture = 1;
    MapEditSaveRequest request;
    request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / "saved.w3x";
    request.expectedSourceSha256 = source.SourceSha256(); request.expectedTerrainSha256 = Take(Sha256(beforeBytes));
    request.toolRoot = tool; request.gameRoot = game; request.terrainStrokes = {stroke};
    const std::array<MemberReplacement, 2> expectedMembers{{{"war3map.w3e", afterBytes}, {"war3map.wpm", sourcePathing}}};
    const auto markers = Take(TerrainMinimapMarkers::Capture(source, game, expectedMembers));
    const auto saved = Take(SaveMapEditCandidate(request));
    const auto readback = Take(MapArchiveSession::Open(saved.candidate));
    Require(Take(readback.ReadMember("war3map.w3e")) == afterBytes,
        "pure cliff texture public save changes only nine palette indices, preserving all heights, levels and variations");
    Require(Take(readback.ReadMember("war3map.wpm")) == sourcePathing,
        "pure cliff texture save reproduces the complete original WPM");
    Require(Take(readback.ReadMember("war3map.mmp")) == markers->Bytes()
        && markers->VerifyReadback(readback).Succeeded(), "pure cliff texture save generates exact actual-source MMP");
    for (const auto& member : source.Members()) {
        const auto name = Text::LowerAscii(member.name);
        if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp" || name == "war3map.mmp") continue;
        Require(Take(readback.ReadMember(member.name)) == member.data,
            "pure cliff texture public save preserves source SHD, objects, placements and unrelated members");
    }
    const auto reject = [&](MapEditSaveRequest invalid, const char* code) {
        const auto sourceSha = Take(Sha256File(invalid.sourceMap));
        const auto result = SaveMapEditCandidate(invalid);
        const auto label = invalid.targetCandidate.filename().string();
        if (result.Succeeded() || result.value)
            throw std::runtime_error("pure cliff texture negative " + label + " must reject with " + code);
        if (std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; })) {
            std::string message = "pure cliff texture negative " + label + " expected " + code + ":\n";
            for (const auto& item : result.diagnostics) message += item.code + ": " + item.message + "\n";
            throw std::runtime_error(message);
        }
        Require(!std::filesystem::exists(invalid.targetCandidate) && Take(Sha256File(invalid.sourceMap)) == sourceSha,
            "rejected pure cliff texture operation preserves source bytes and leaves no output map");
    };
    for (const auto& id : {oldId, *alternate}) {
        const auto missing = Take(MapArchiveSession::Open(Take(source.SaveCandidate(scenario / ("missing-" + id + ".w3x"),
            std::array<MemberReplacement, 1>{{{"TerrainArt/CliffTypes.slk", WithoutActualCliffRow(table, id)}}})).path));
        auto invalid = request; invalid.sourceMap = missing.SourcePath(); invalid.expectedSourceSha256 = missing.SourceSha256();
        invalid.targetCandidate = scenario / ("missing-" + id + "-must-not-exist.w3x");
        reject(std::move(invalid), "TERRAIN_PATHING_PROPERTY_MISSING");
    }
    auto unknownVariation = before;
    unknownVariation.vertices[32 * 65 + 32].variationRaw = static_cast<std::uint8_t>(
        (unknownVariation.vertices[32 * 65 + 32].variationRaw & 0x1f) | (3 << 5));
    const auto unknownBytes = Take(SerializeMapTerrain(unknownVariation));
    const auto unknown = Take(MapArchiveSession::Open(Take(source.SaveCandidate(scenario / "unknown-old-variation.w3x",
        std::array<MemberReplacement, 1>{{{"war3map.w3e", unknownBytes}}})).path));
    auto invalid = request; invalid.sourceMap = unknown.SourcePath(); invalid.expectedSourceSha256 = unknown.SourceSha256();
    invalid.expectedTerrainSha256 = Take(Sha256(unknownBytes)); invalid.targetCandidate = scenario / "unknown-variation-must-not-exist.w3x";
    reject(std::move(invalid), "MAP_TERRAIN_PATHING_UNVERIFIED");
    invalid = request; invalid.terrainStrokes.front().brush.cliffTexture = 2;
    invalid.targetCandidate = scenario / "outside-palette-must-not-exist.w3x";
    reject(std::move(invalid), "TERRAIN_BRUSH_CLIFF_TEXTURE");
    auto wrongPathing = sourcePathing; wrongPathing[16 + 32 * 4 * 256 + 32 * 4] ^= std::byte{0x02};
    const auto mismatched = Take(MapArchiveSession::Open(Take(source.SaveCandidate(scenario / "unexplained-wpm.w3x",
        std::array<MemberReplacement, 1>{{{"war3map.wpm", wrongPathing}}})).path));
    // 实际旧/新CliffTypes行与不变几何能证明纯材质不影响通行；历史WPM限制应逐字保留，
    // 不能仅因它不等于完整原生重算而拒绝本次不涉及通行的保存。
    auto preserved = request; preserved.sourceMap = mismatched.SourcePath();
    preserved.expectedSourceSha256 = mismatched.SourceSha256();
    preserved.targetCandidate = scenario / "preserved-custom-wpm.w3x";
    const std::array<MemberReplacement, 2> preservedMembers{{{"war3map.w3e", afterBytes}, {"war3map.wpm", wrongPathing}}};
    const auto preservedMarkers = Take(TerrainMinimapMarkers::Capture(mismatched, game, preservedMembers));
    const auto preservedSave = Take(SaveMapEditCandidate(preserved));
    const auto preservedReadback = Take(MapArchiveSession::Open(preservedSave.candidate));
    Require(Take(preservedReadback.ReadMember("war3map.w3e")) == afterBytes
        && Take(preservedReadback.ReadMember("war3map.wpm")) == wrongPathing,
        "verified pure cliff material change preserves all authored WPM bytes and exact independent W3E");
    Require(Take(preservedReadback.ReadMember("war3map.mmp")) == preservedMarkers->Bytes()
        && preservedMarkers->VerifyReadback(preservedReadback).Succeeded(),
        "preserved custom WPM save generates exact MMP bound to its actual source");
    for (const auto& member : mismatched.Members()) {
        const auto name = Text::LowerAscii(member.name);
        if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp" || name == "war3map.mmp") continue;
        Require(Take(preservedReadback.ReadMember(member.name)) == member.data,
            "pure cliff material change with custom WPM preserves every unrelated member");
    }

    // 同一来源若再修改远处的边界，必须回到完整G0/G1并解释旧WPM；
    // 纯材质证明不能替该通行变化放行。独立构造Boundary的单点原始字段预期。
    auto boundaryAfter = after;
    auto& boundaryVertex = boundaryAfter.vertices[48 * 65 + 48];
    Require(!boundaryVertex.IsBoundary(), "boundary negative starts from an ordinary interior vertex");
    boundaryVertex.textureAndFlagsRaw |= std::uint8_t{0x80};
    const auto boundaryResources = Take(TerrainPathingSave::CaptureNative(mismatched, before, boundaryAfter, game, kBytes));
    Require(Take(boundaryResources.GenerateNative(before, kBytes)) == oldNative
        && Take(boundaryResources.GenerateNative(boundaryAfter, kBytes)) != oldNative,
        "actual native rules prove the added boundary changes pathing while the source geometry is unchanged");
    MapEditTerrainStroke boundary;
    boundary.pointSpace = MapEditPointSpace::TerrainVertex; boundary.points = {{48, 48}};
    boundary.brush.kind = TerrainBrushKind::Boundary; boundary.brush.shape = TerrainBrushShape::Square;
    boundary.brush.size = 1; boundary.brush.value = true;
    invalid = preserved; invalid.terrainStrokes.push_back(boundary);
    invalid.targetCandidate = scenario / "unexplained-wpm-boundary-must-not-exist.w3x";
    reject(std::move(invalid), "TERRAIN_PATHING_SOURCE_UNEXPLAINED");
    Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256()
        && Take(Sha256File(original.SourcePath())) == original.SourceSha256()
        && Take(Sha256File(mismatched.SourcePath())) == mismatched.SourceSha256(),
        "pure cliff tests preserve all three input maps");
    std::cout << "cliff_texture_actual_table winner=" << table.archiveName << " sha256=" << Take(Sha256(table.bytes))
        << " old=" << oldId << " new=" << *alternate << " source_sha256=" << source.SourceSha256()
        << " candidate_sha256=" << readback.SourceSha256() << '\n';
}
} // namespace War3Map::TerrainSaveTests
