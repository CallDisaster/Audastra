#pragma once

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/TerrainRampEditing.h"
#include "War3MapCore/MapData/TerrainStrokeJournal.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/TerrainEditService.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 私有真实来源专项；原创地图来自既有real入口，本文件不创建额外进程。
// 完整W3E/WPM expected独立写字节，不消费坡道分类器或通行生成器的输出。
namespace War3Map::TerrainInnerCornerRampRealCases {
using namespace MapData;
using namespace Services;
using Bytes = std::vector<std::byte>;
inline constexpr std::size_t kBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kTerrainHeader = 69, kVertices = 65 * 65, kPixels = 256 * 256;

inline void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
inline void Check(const Status& status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& diagnostic : status.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
    throw std::runtime_error(message.empty() ? "missing real inner corner status" : message);
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) { Check({result.diagnostics}); throw std::runtime_error("missing real inner corner result"); }
    return std::move(*result.value);
}

inline std::byte ActualGroundMask(const Game::GameFileContents& file, const std::string& rawcode) {
    const std::string_view text(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size());
    const auto rows = Take(Text::ParseSlkCells(text, Text::SlkCellMode::CatalogUnique));
    Require(!rows.empty(), "real inner terrain table has a header");
    constexpr std::array<std::string_view, 4> names{"tileid", "walkable", "flyable", "buildable"};
    std::array<int, 4> columns{};
    for (const auto& [column, field] : rows.begin()->second)
        for (std::size_t i = 0; i < names.size(); ++i) if (Text::LowerAscii(field) == names[i]) {
            Require(columns[i] == 0, "real inner terrain permission columns are unique"); columns[i] = column;
        }
    for (const auto column : columns) Require(column != 0, "real inner ground permissions are complete");
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
                "actual inner ground permissions are explicit booleans");
            if (value->second == "0") mask |= restrictions[i];
        }
    }
    Require(matches == 1, "actual inner ground rawcode is present and unambiguous");
    return static_cast<std::byte>(mask);
}

inline Bytes TerrainLiteral(const Bytes& original, bool created) {
    Require(original.size() == kTerrainHeader + kVertices * 7, "inner real W3E retains the original complete classic layout");
    auto output = original;
    for (unsigned y = 0; y < 65; ++y) for (unsigned x = 0; x < 65; ++x) {
        const auto at = kTerrainHeader + (std::size_t{y} * 65 + x) * 7;
        // 原图header、raw B/water/texture/variation逐字保留，只选实际cliff slot0。
        output[at + 6] = (x <= 32 && y <= 32) ? std::byte{2} : std::byte{3};
        if (created && x >= 31 && x <= 33 && y >= 31 && y <= 33) output[at + 4] |= std::byte{0x10};
    }
    return output;
}

inline TerrainNativePlayableBounds ActualPlayable(const MapSettings& settings, const std::array<float, 4>& margins) {
    for (const auto value : margins) Require(std::isfinite(value) && value >= 0 && value <= 16 && std::trunc(value) == value,
        "actual inner CameraMargins are bounded integral source properties");
    // 输入固定为原创图的整数F32八角。这里只解释实际profile属性，不调用产品投影。
    Require(settings.cameraBounds == std::array<float, 8>{-2816, -3328, 2816, 2816, -2816, 2816, 2816, -3328},
        "real inner fixture retains the known independently authored W3I camera coordinates");
    return {-2816.0 - double(margins[0]) * 128.0, -3328.0 - double(margins[2]) * 128.0,
        2816.0 + double(margins[1]) * 128.0, 2816.0 + double(margins[3]) * 128.0};
}

inline Bytes PathingLiteral(const TerrainGridSnapshot& original, const TerrainNativePlayableBounds& bounds,
    std::byte ground, bool created) {
    Bytes output{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
    for (const auto value : {0u, 256u, 256u})
        for (unsigned shift = 0; shift < 32; shift += 8) output.push_back(std::byte((value >> shift) & 255));
    output.resize(16 + kPixels);
    for (unsigned y = 0; y < 256; ++y) for (unsigned x = 0; x < 256; ++x) {
        const unsigned cx = x / 4, cy = y / 4;
        const double left = -4096.0 + cx * 128.0, bottom = -4096.0 + cy * 128.0;
        const auto& sw = original.vertices[std::size_t{cy} * 65 + cx];
        const bool playable = left >= bounds.left && bottom >= bounds.bottom
            && left + 128.0 <= bounds.right && bottom + 128.0 <= bounds.top && !sw.IsMapEdge();
        // 手推原L形边；S96内角权重打开三格，两个只接一个R的肩部仍阻断。
        bool blocked = (cx == 32 && cy <= 32) || (cy == 32 && cx <= 32);
        if (created && ((cx == 32 && cy == 31) || (cx == 31 && cy == 32) || (cx == 32 && cy == 32))) blocked = false;
        if (created && ((cx == 31 && cy == 30) || (cx == 30 && cy == 31))) blocked = true;
        const auto native = playable ? ground | (blocked ? std::byte{0x8a} : std::byte{0}) : std::byte{0xce};
        const auto authored = std::byte((x % 7 == 1 ? 1 : 0) | (y % 11 == 2 ? 0x10 : 0));
        output[16 + std::size_t{y} * 256 + x] = native | authored;
    }
    return output;
}

inline void Check(const MapArchiveSession& original, const std::filesystem::path& directory,
    const std::filesystem::path& tool, const std::filesystem::path& game) {
    const auto scenario = directory / "playable-real-inner-corner-ramp";
    Require(std::filesystem::create_directory(scenario), "exclusive real inner corner scenario");
    const auto rawOriginal = Take(original.ReadMember("war3map.w3e"));
    const auto flat = *Take(ParseMapTerrain(rawOriginal)).Snapshot();
    Require(flat.header.width == 65 && flat.header.height == 65 && flat.vertices.size() == kVertices
        && flat.header.offset == std::array<float, 2>{-4096, -4096} && flat.header.mainTileset == 'L'
        && !flat.header.cliffTilesets.empty(), "inner real regression uses the original 64-cell actual tileset fixture");
    for (const auto& vertex : flat.vertices) Require(vertex.groundHeightRaw == 8192 && vertex.GroundTextureIndex() == 0
        && vertex.CliffLevel() == 2 && !vertex.HasWater() && !vertex.IsRamp() && !vertex.IsBlight() && !vertex.IsBoundary(),
        "full inner independent literal starts from dry uniform ground and source MapEdge only");
    const auto doodads = Take(ParseDoodadPlacements(Take(original.ReadMember("war3map.doo"))));
    Require(doodads.doodads.empty() && doodads.specialDoodads.empty(), "real inner literal excludes static and special object footprints");
    Require(!original.HasMember("TerrainArt/Terrain.slk") && !original.HasMember("TerrainArt/CliffTypes.slk"),
        "inner test does not override actual game terrain or cliff tables");
    const auto mounted = Take(Game::GameDataSource::MountStrict(game, flat.header.mainTileset));
    const auto table = Take(mounted.ReadFileBoundedWithSource("TerrainArt/Terrain.slk", 2u * 1024u * 1024u));
    Require(!table.archiveName.empty() && !table.bytes.empty(), "inner ground permissions have a real game winning container");
    const auto ground = ActualGroundMask(table, flat.header.groundTilesets[0]);
    const auto beforeBytes = TerrainLiteral(rawOriginal, false), afterBytes = TerrainLiteral(rawOriginal, true);
    const auto before = *Take(ParseMapTerrain(beforeBytes)).Snapshot();
    const auto after = *Take(ParseMapTerrain(afterBytes)).Snapshot();
    const auto baselineRules = Take(TerrainPathingSave::CaptureNative(original, flat, flat, game, kBytes));
    Require(baselineRules.NativeCameraMargins(), "inner independent WPM uses actual captured CameraMargins attributes");
    const auto margins = *baselineRules.NativeCameraMargins();
    const auto bounds = ActualPlayable(Take(MapSettings::Parse(Take(original.ReadMember("war3map.w3i")))), margins);
    const auto oldWpm = PathingLiteral(flat, bounds, ground, false), newWpm = PathingLiteral(flat, bounds, ground, true);
    Require(oldWpm != newWpm, "independent inner WPM changes three openings and two shoulders");
    Bytes shadow(kPixels, std::byte{37}); shadow[17 * 256 + 31] = std::byte{255}; shadow[211 * 256 + 47] = std::byte{64};
    const Bytes sentinel{std::byte{0}, std::byte{0xff}, std::byte{0x95}, std::byte{0x1b}};
    const std::array<MemberReplacement, 4> seed{{{"war3map.w3e", beforeBytes}, {"war3map.wpm", oldWpm},
        {"war3map.shd", shadow}, {"Fixture/InnerCornerUnrelated.bin", sentinel}}};
    auto current = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "source.w3x", seed)).path));
    std::vector<std::pair<std::filesystem::path, std::string>> sources{{original.SourcePath(), original.SourceSha256()},
        {current.SourcePath(), current.SourceSha256()}};
    const auto verifySources = [&] {
        for (const auto& [path, sha] : sources) Require(Take(Sha256File(path)) == sha, "inner every real source archive stays byte-for-byte unchanged");
    };
    const auto native = Take(TerrainPathingSave::CaptureNative(current, before, after, game, kBytes));
    for (const bool created : {false, true}) {
        const auto actual = Take(native.GenerateNative(created ? after : before, kBytes));
        const auto& expected = created ? newWpm : oldWpm;
        Require(actual.size() == kPixels, "actual inner native generation covers the full WPM body");
        for (std::size_t i = 0; i < actual.size(); ++i) Require(actual[i] == (expected[16 + i] & std::byte{0xee}),
            "actual native G0/G1 is tested against independent full inner literal rather than manufacturing expected");
    }
    const TerrainStrokePoint point{32, 32}; // 原点-4096加32格，canonical每轴再加32。
    TerrainBrush brush; brush.kind = TerrainBrushKind::CornerRamp;
    MapEditSaveRequest request;
    request.sourceMap = current.SourcePath(); request.targetCandidate = scenario / "public-created.w3x";
    request.toolRoot = tool; request.gameRoot = game; request.expectedSourceSha256 = current.SourceSha256();
    request.expectedTerrainSha256 = Take(Sha256(beforeBytes)); request.minimapChoice = TerrainMinimapSaveChoice::Automatic;
    MapEditTerrainStroke stroke; stroke.brush = brush; stroke.points = {point}; request.terrainStrokes = {stroke};
    const auto publicSaved = Take(SaveMapEditCandidate(request));
    const auto publicReadback = Take(MapArchiveSession::Open(publicSaved.candidate));
    Require(Take(publicReadback.ReadMember("war3map.w3e")) == afterBytes
        && Take(publicReadback.ReadMember("war3map.wpm")) == newWpm && publicSaved.scriptPreserved,
        "real toolchain public inner save reads back complete independent W3E/WPM and preserves the checked script");
    for (const auto& member : current.Members()) {
        const auto name = Text::LowerAscii(member.name);
        if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp" || name == "war3map.mmp") continue;
        Require(Take(publicReadback.ReadMember(member.name)) == member.data, "public inner save preserves every unrelated authored member");
    }
    verifySources();
    std::cout << "playable_real_inner_corner_public_save_passed source_sha256=" << current.SourceSha256()
        << " candidate_sha256=" << publicReadback.SourceSha256() << " w3e_sha256=" << Take(Sha256(afterBytes))
        << " wpm_sha256=" << Take(Sha256(newWpm))
        << " expected_complete_w3e_literal=true expected_complete_wpm_literal=true expected_uses_native_generator=false"
        << " script_preserved=true source_unchanged=true native_editor_verified=false game_verified=false\n";

    MapDataEditSession edits(MapDocument::FromArchive(current));
    Check(edits.AttachTerrain(beforeBytes)); Check(edits.AttachPathing(oldWpm));
    const auto rules = Take(TerrainPathingSave::CaptureWaterBrushRules(current, before, before, doodads, game, true, kBytes));
    const MapDataQueryBinding document{"real-inner-corner", 1, 0, current.SourceSha256()};
    const auto binding = Take(CaptureTerrainEditState(edits, document)).binding;
    const auto command = Take(PrepareTerrainCornerRampEdit(edits, document, binding, rules, brush,
        std::span(&point, 1), false, {}, TerrainCornerRampAlgorithm::NativeCorners));
    Require(!edits.IsDirty() && command.Plan().TerrainPreview()
        && Take(SerializeMapTerrain(*command.Plan().TerrainPreview())) == afterBytes,
        "actual rules prepare an independent complete inner preview without publishing a session edit");
    Take(ApplyTerrainEdit(edits, document, command));
    const auto verifyTerrain = [&](bool created) {
        Require(Take(SerializeMapTerrain(*edits.Terrain())) == (created ? afterBytes : beforeBytes),
            "actual inner Session Undo/Redo retains every independent W3E byte");
    };
    verifyTerrain(true);
    Require(edits.Undo(), "real inner command supports Undo before saving"); verifyTerrain(false);
    Require(edits.Redo(), "real inner command supports Redo before saving"); verifyTerrain(true);
    const auto identity = edits.SessionIdentity();
    const auto save = [&](bool created, const char* name) {
        verifyTerrain(created);
        const auto plan = Take(TerrainPathingSave::PrepareForSave(current, edits, game)); Check(plan.SaveGate());
        MapSaveTask task; Check(task.FreezeArchive(current)); task.target = scenario / name;
        task.replacements = edits.Replacements(); task.removals = edits.Removals();
        Check(plan.InstallPathingOutput(edits, task.replacements));
        task.beforePublish = [&] {
            auto guard = plan.VerifyForSave(edits, task.sourceSha256, task.replacements, task.removals);
            return guard.Succeeded() ? plan.VerifySourceInputs(current) : guard;
        };
        MapSaveService::Run(task); Check({task.diagnostics});
        Require(task.executed && task.succeeded && task.nextSession, "real inner formal save publishes and reopens the archive");
        auto readback = Take(MapArchiveSession::Open(task.written)); Check(plan.VerifyReadback(readback));
        Require(Take(readback.ReadMember("war3map.w3e")) == (created ? afterBytes : beforeBytes)
            && Take(readback.ReadMember("war3map.wpm")) == (created ? newWpm : oldWpm),
            "real inner create and saved Undo/Redo read back every independent W3E/WPM byte");
        for (const auto& member : current.Members()) {
            const auto memberName = Text::LowerAscii(member.name);
            if (memberName == "war3map.w3e" || memberName == "war3map.wpm") continue;
            Require(Take(readback.ReadMember(member.name)) == member.data, "Session inner save preserves all unrelated source members including prior nonzero SHD");
        }
        const auto revision = edits.Revision(), sourceRevision = edits.TerrainSourceRevision();
        Check(plan.CommitSaved(edits, MapDocument::FromArchive(readback)));
        Require(!edits.IsDirty() && edits.SessionIdentity() == identity && edits.Revision() == revision
            && edits.TerrainSourceRevision() == sourceRevision + 1, "real inner saving rebases physical source without discarding Undo/Redo or consuming edit revision");
        verifySources();
        const auto journal = Take(TerrainPathingSave::CaptureJournal(readback, edits));
        const auto wire = Take(EncodeTerrainStrokeJournal(journal));
        Require(journal.history && wire[3] == std::byte{'E'} && journal.strokes.size() == 1
            && journal.strokes[0].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeCorners
            && Take(EncodeTerrainStrokeJournal(Take(DecodeTerrainStrokeJournal(wire)))) == wire,
            "real inner saved history keeps FTJE with a canonical new NativeCorners3 typed command");
        std::cout << "playable_real_inner_corner_save_passed phase=" << name << " created=" << created
            << " source_sha256=" << current.SourceSha256() << " candidate_sha256=" << readback.SourceSha256()
            << " w3e_sha256=" << Take(Sha256(Take(readback.ReadMember("war3map.w3e"))))
            << " wpm_sha256=" << Take(Sha256(Take(readback.ReadMember("war3map.wpm"))))
            << " actual_rules_sha256=" << Take(rules.WaterBrushDependencyFingerprint())
            << " actual_terrain_container=" << table.archiveName << " actual_terrain_sha256=" << Take(Sha256(table.bytes))
            << " actual_camera_margins=" << margins[0] << ',' << margins[1] << ',' << margins[2] << ',' << margins[3]
            << " expected_complete_w3e_literal=true expected_complete_wpm_literal=true expected_uses_native_generator=false"
            << " source_unchanged=true native_editor_verified=false game_verified=false\n";
        sources.emplace_back(readback.SourcePath(), readback.SourceSha256()); current = std::move(readback);
    };
    save(true, "session-created.w3x");
    Require(edits.Undo() && !edits.CanUndo(), "real inner single Undo survives successful actual save"); save(false, "session-undone.w3x");
    Require(edits.Redo() && !edits.CanRedo(), "real inner single Redo survives a saved Undo"); save(true, "session-redone.w3x");
    verifySources();
}
} // namespace War3Map::TerrainInnerCornerRampRealCases

namespace War3Map::TerrainSaveTests {
inline void CheckRealInnerCornerRampSave(const MapArchiveSession& original, const std::filesystem::path& directory,
    const std::filesystem::path& tool, const std::filesystem::path& game) {
    TerrainInnerCornerRampRealCases::Check(original, directory, tool, game);
}
} // namespace War3Map::TerrainSaveTests
