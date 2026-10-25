#pragma once

#include "TerrainInnerCornerRampRealCases.h"

#include <charconv>

// 原创地图配本机实际游戏表与最终保存链；expected不读取产品坡道/通行生成结果。
namespace War3Map::TerrainWetInnerCornerRampRealCases {
using namespace MapData;
using namespace Services;
namespace R = TerrainInnerCornerRampRealCases;
using R::Bytes;
using R::Require;
using R::Take;
inline void Check(const Status& status) { R::Check(status); }

struct ActualWater { float height = 0; bool impassable = false; };
inline ActualWater WaterRow(const Game::GameFileContents& file) {
    const auto rows = Take(Text::ParseSlkCells(
        std::string_view(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()),
        Text::SlkCellMode::CatalogUnique));
    Require(!rows.empty(), "actual wet inner Water.slk has a header");
    constexpr std::array<std::string_view, 3> fields{"waterid", "height", "impassable"};
    std::array<int, 3> columns{};
    for (const auto& [column, value] : rows.begin()->second)
        for (std::size_t i = 0; i < fields.size(); ++i) if (Text::LowerAscii(value) == fields[i]) {
            Require(columns[i] == 0, "actual wet inner water columns are unique"); columns[i] = column;
        }
    for (const auto column : columns) Require(column != 0, "actual wet inner water columns are complete");
    ActualWater result; std::size_t found = 0;
    for (auto row = std::next(rows.begin()); row != rows.end(); ++row) {
        const auto id = row->second.find(columns[0]);
        if (id == row->second.end() || id->second != "LSha") continue;
        ++found;
        const auto height = row->second.find(columns[1]), impassable = row->second.find(columns[2]);
        Require(height != row->second.end() && impassable != row->second.end()
            && (impassable->second == "0" || impassable->second == "1"), "actual wet inner water values are explicit");
        const auto parsed = std::from_chars(height->second.data(), height->second.data() + height->second.size(), result.height);
        Require(parsed.ec == std::errc{} && parsed.ptr == height->second.data() + height->second.size()
            && std::isfinite(result.height), "actual wet inner water height is a complete finite float");
        result.impassable = impassable->second == "1";
    }
    Require(found == 1, "actual wet inner LSha row is unique"); return result;
}

inline bool Wet(bool mixed, int x, int y) {
    return !mixed || (x == 0 && y == 0) || (x == 0 && y == -1) || (x == -4 && y == -4);
}
inline int GroundQuarter(bool created, int x, int y) {
    // raw8193=0.25，低层2、高层3；R64与S96严格分开，H0保持原高层。
    int value = 1 + (x <= 0 && y <= 0 ? 0 : 512);
    if (created && ((x == 0 && y == -1) || (x == -1 && y == 0))) value += 256;
    if (created && x == 0 && y == 0) value += 384;
    return value;
}
inline Bytes TerrainLiteral(const Bytes& original, bool created, bool mixed, std::uint16_t water) {
    auto bytes = R::TerrainLiteral(original, created);
    for (unsigned y = 0; y < 65; ++y) for (unsigned x = 0; x < 65; ++x) {
        const auto at = R::kTerrainHeader + (std::size_t{y} * 65 + x) * 7;
        bytes[at] = std::byte{1}; bytes[at + 1] = std::byte{0x20};
        // 原版MapEdge原字只在水位字段的bit14；保留它以及原header/材质/variation。
        bytes[at + 2] = std::byte(water & 255);
        bytes[at + 3] = (bytes[at + 3] & std::byte{0x40}) | std::byte(water >> 8);
        if (Wet(mixed, int(x) - 32, int(y) - 32)) bytes[at + 4] |= std::byte{0x40};
    }
    return bytes;
}

inline Bytes PathingLiteral(const TerrainGridSnapshot& original, const TerrainNativePlayableBounds& bounds,
    std::byte ground, const ActualWater& waterRule, double surface, double deep,
    bool created, bool mixed) {
    // 六种模式的独立常量：depth × cliff × strict。再叠实际地面权限/impassable。
    constexpr unsigned masks[3][2][2] = {
        {{0x40, 0x00}, {0xca, 0x0a}}, {{0x48, 0x08}, {0xca, 0x0a}}, {{0x0a, 0x0a}, {0x0a, 0x0a}}};
    auto bytes = R::PathingLiteral(original, bounds, ground, created);
    for (unsigned py = 0; py < 256; ++py) for (unsigned px = 0; px < 256; ++px) {
        const unsigned cx = px / 4, cy = py / 4;
        const double left = -4096.0 + cx * 128.0, bottom = -4096.0 + cy * 128.0;
        const auto& sw = original.vertices[std::size_t{cy} * 65 + cx];
        const bool playable = left >= bounds.left && bottom >= bounds.bottom
            && left + 128.0 <= bounds.right && bottom + 128.0 <= bounds.top && !sw.IsMapEdge();
        bool blocked = (cx == 32 && cy <= 32) || (cy == 32 && cx <= 32);
        if (created && ((cx == 32 && cy == 31) || (cx == 31 && cy == 32) || (cx == 32 && cy == 32))) blocked = false;
        if (created && ((cx == 31 && cy == 30) || (cx == 30 && cy == 31))) blocked = true;
        const int nearestX = int((px + 2) / 4) - 32, nearestY = int((py + 2) / 4) - 32;
        const double height = double(GroundQuarter(created, nearestX, nearestY)) * 0.25;
        const unsigned depth = !Wet(mixed, nearestX, nearestY) || height > surface ? 0
            : height <= surface - deep ? 2 : 1;
        bool strict = true;
        for (int y = 0; y <= 1; ++y) for (int x = 0; x <= 1; ++x) {
            const int rx = int(cx) + x - 32, ry = int(cy) + y - 32;
            strict = strict && Wet(mixed, rx, ry) && surface > double(GroundQuarter(created, rx, ry)) * 0.25;
        }
        unsigned native = masks[depth][blocked ? 1 : 0][strict ? 1 : 0] | (std::to_integer<unsigned>(ground) & 0x0e);
        if (waterRule.impassable && depth != 0) native |= 0xca;
        if (!playable) native = 0xce;
        const unsigned authored = (px % 7 == 1 ? 1u : 0u) | (py % 11 == 2 ? 0x10u : 0u);
        bytes[16 + std::size_t{py} * 256 + px] = std::byte(native | authored);
    }
    return bytes;
}

inline void Check(const MapArchiveSession& original, const std::filesystem::path& directory,
    const std::filesystem::path& tool, const std::filesystem::path& game) {
    const auto rawOriginal = Take(original.ReadMember("war3map.w3e"));
    const auto flat = *Take(ParseMapTerrain(rawOriginal)).Snapshot();
    Require(flat.header.width == 65 && flat.header.height == 65 && flat.vertices.size() == R::kVertices
        && flat.header.offset == std::array<float, 2>{-4096, -4096} && flat.header.mainTileset == 'L'
        && !flat.header.cliffTilesets.empty(), "wet inner real regression uses the known original 64-cell actual tileset");
    for (const auto& vertex : flat.vertices) Require(vertex.groundHeightRaw == 8192 && vertex.GroundTextureIndex() == 0
        && vertex.CliffLevel() == 2 && !vertex.HasWater() && !vertex.IsRamp() && !vertex.IsBlight() && !vertex.IsBoundary(),
        "wet inner independent literal starts from uniform raw source plus original MapEdge only");
    const auto doodads = Take(ParseDoodadPlacements(Take(original.ReadMember("war3map.doo"))));
    Require(doodads.doodads.empty() && doodads.specialDoodads.empty(), "wet inner real literal has no placement contributions");
    for (const char* name : {"TerrainArt/Terrain.slk", "TerrainArt/CliffTypes.slk", "TerrainArt/Water.slk",
        "UI/MiscData.txt", "UI/WorldEditData.txt", "war3mapMisc.txt"})
        Require(!original.HasMember(name), "wet inner independent rules do not bypass a map table/profile override");
    const auto gameSource = Take(Game::GameDataSource::MountStrict(game, flat.header.mainTileset));
    const auto groundFile = Take(gameSource.ReadFileBoundedWithSource("TerrainArt/Terrain.slk", 2u * 1024u * 1024u));
    const auto waterFile = Take(gameSource.ReadFileBoundedWithSource("TerrainArt/Water.slk", 2u * 1024u * 1024u));
    Require(!groundFile.archiveName.empty() && !waterFile.archiveName.empty(), "wet inner actual tables have game winning containers");
    const auto ground = R::ActualGroundMask(groundFile, flat.header.groundTilesets[0]);
    const auto waterRule = WaterRow(waterFile);
    const auto baseline = Take(TerrainPathingSave::CaptureNative(original, flat, flat, game, R::kBytes));
    Require(baseline.NativeCameraMargins(), "wet inner full expected WPM uses actual source CameraMargins");
    const auto margins = *baseline.NativeCameraMargins();
    const auto bounds = R::ActualPlayable(Take(MapSettings::Parse(Take(original.ReadMember("war3map.w3i")))), margins);
    const auto waterOwner = Take(TerrainPathingSave::CaptureWaterBrushRules(original, flat, flat, doodads, game, true, R::kBytes));
    const auto* rules = waterOwner.WaterBrushRules();
    Require(rules && rules->waterLayerOffset == waterRule.height * 128.0f,
        "actual rule owner agrees with separately decoded winning Water.slk float");
    const double offset = double(waterRule.height * 128.0f), deep = double(rules->deepWaterThreshold);
    Require(std::isfinite(offset) && std::isfinite(deep) && deep > 0 && deep <= 1024,
        "actual water properties admit a finite bounded independently derived oracle");
    for (const bool mixed : {false, true}) for (const bool deepNear : {false, true}) {
        const std::string name = std::string("playable-real-wet-inner-corner-") + (mixed ? "mixed" : "all")
            + (deepNear ? "-deep-near" : "-surface-near");
        const auto scenario = directory / name;
        Require(std::filesystem::create_directory(scenario), "exclusive stable wet inner real scene");
        const double target = 96.25 + (deepNear ? deep : 0.0);
        const auto decodedSurface = [&](int raw) { return (double(raw) - 8192.0) * 0.25 + offset; };
        const double encoded = 8192.0 + (target - offset) * 4.0;
        Require(std::isfinite(encoded) && encoded >= 1 && encoded <= 16382,
            "actual water offset admits two representable raw values surrounding the S96 threshold");
        int lowerRaw = int(std::floor(encoded));
        // 用实际binary64落点复核上下邻值，不能把float属性先强行量化成quarter。
        if (decodedSurface(lowerRaw) > target) --lowerRaw;
        if (decodedSurface(lowerRaw + 1) <= target) ++lowerRaw;
        Require(lowerRaw >= 0 && lowerRaw < 16383 && decodedSurface(lowerRaw) <= target
            && decodedSurface(lowerRaw + 1) > target, "independent actual water neighbours must strictly bracket the threshold");
        const auto rawWater = static_cast<std::uint16_t>(lowerRaw + (mixed ? 1 : 0));
        const double surface = decodedSurface(rawWater);
        const bool exactEquality = surface == target;
        const auto beforeBytes = TerrainLiteral(rawOriginal, false, mixed, rawWater);
        const auto afterBytes = TerrainLiteral(rawOriginal, true, mixed, rawWater);
        const auto before = *Take(ParseMapTerrain(beforeBytes)).Snapshot(), after = *Take(ParseMapTerrain(afterBytes)).Snapshot();
        const auto oldWpm = PathingLiteral(flat, bounds, ground, waterRule, surface, deep, false, mixed);
        const auto newWpm = PathingLiteral(flat, bounds, ground, waterRule, surface, deep, true, mixed);
        Require(oldWpm != newWpm, "full independent wet inner WPM changes shoulders and openings");
        Bytes shadow(R::kPixels, std::byte{37}); shadow[17 * 256 + 31] = std::byte{255}; shadow[211 * 256 + 47] = std::byte{64};
        const Bytes sentinel{std::byte{0}, std::byte{0xff}, std::byte{0x64}, std::byte{0x96}};
        const std::array<MemberReplacement, 4> seed{{{"war3map.w3e", beforeBytes}, {"war3map.wpm", oldWpm},
            {"war3map.shd", shadow}, {"Fixture/WetInnerCornerUnrelated.bin", sentinel}}};
        auto current = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "source.w3x", seed)).path));
        std::vector<std::pair<std::filesystem::path, std::string>> sources{{original.SourcePath(), original.SourceSha256()},
            {current.SourcePath(), current.SourceSha256()}};
        const auto verifySources = [&] {
            for (const auto& [path, sha] : sources) Require(Take(Sha256File(path)) == sha, "wet inner every source stays byte-for-byte unchanged");
        };
        const auto native = Take(TerrainPathingSave::CaptureNative(current, before, after, game, R::kBytes));
        for (const bool created : {false, true}) {
            const auto actual = Take(native.GenerateNative(created ? after : before, R::kBytes));
            const auto& expected = created ? newWpm : oldWpm;
            Require(actual.size() == R::kPixels, "actual wet inner native output covers every WPM pixel");
            for (std::size_t i = 0; i < actual.size(); ++i) Require(actual[i] == (expected[16 + i] & std::byte{0xee}),
                "actual wet inner G0/G1 matches independent full WPM rather than manufacturing expected");
        }
        const TerrainStrokePoint point{32, 32};
        for (const bool create : {true, false}) {
            TerrainBrush brush; brush.kind = TerrainBrushKind::CornerRamp; brush.value = create;
            MapEditSaveRequest request;
            request.sourceMap = current.SourcePath(); request.targetCandidate = scenario / (create ? "public-created.w3x" : "public-removed.w3x");
            request.toolRoot = tool; request.gameRoot = game; request.expectedSourceSha256 = current.SourceSha256();
            request.expectedTerrainSha256 = Take(Sha256(create ? beforeBytes : afterBytes));
            request.minimapChoice = TerrainMinimapSaveChoice::Automatic;
            MapEditTerrainStroke stroke; stroke.brush = brush; stroke.points = {point}; request.terrainStrokes = {stroke};
            const auto saved = Take(SaveMapEditCandidate(request));
            auto readback = Take(MapArchiveSession::Open(saved.candidate));
            Require(Take(readback.ReadMember("war3map.w3e")) == (create ? afterBytes : beforeBytes)
                && Take(readback.ReadMember("war3map.wpm")) == (create ? newWpm : oldWpm) && saved.scriptPreserved,
                "public real-toolchain wet inner create/remove reopens complete literal W3E/WPM and checked script");
            for (const auto& member : current.Members()) {
                const auto memberName = Text::LowerAscii(member.name);
                if (memberName == "war3map.w3e" || memberName == "war3map.wpm" || memberName == "war3mapmap.blp" || memberName == "war3map.mmp") continue;
                Require(Take(readback.ReadMember(member.name)) == member.data, "wet inner public save preserves every unrelated authored member");
            }
            verifySources();
            std::cout << "playable_real_wet_inner_corner_public_save_passed scene=" << name << " create=" << create
                << " raw_at_or_below=" << lowerRaw << " raw_above=" << lowerRaw + 1 << " raw_selected=" << rawWater
                << " actual_surface=" << surface << " target_surface=" << target << " exact_equality=" << exactEquality
                << " source_sha256=" << current.SourceSha256() << " candidate_sha256=" << readback.SourceSha256()
                << " w3e_sha256=" << Take(Sha256(create ? afterBytes : beforeBytes))
                << " wpm_sha256=" << Take(Sha256(create ? newWpm : oldWpm))
                << " expected_complete_w3e_literal=true expected_complete_wpm_literal=true expected_uses_native_generator=false"
                << " script_preserved=true source_unchanged=true native_editor_verified=false game_verified=false\n";
            sources.emplace_back(readback.SourcePath(), readback.SourceSha256()); current = std::move(readback);
        }
        MapDataEditSession warm(MapDocument::FromArchive(current));
        Check(warm.AttachTerrain(beforeBytes)); Check(warm.AttachPathing(oldWpm));
        const auto owner = Take(TerrainPathingSave::CaptureWaterBrushRules(current, before, before, doodads, game, true, R::kBytes));
        const MapDataQueryBinding document{name, 1, 0, current.SourceSha256()};
        const auto binding = Take(CaptureTerrainEditState(warm, document)).binding;
        TerrainBrush brush; brush.kind = TerrainBrushKind::CornerRamp;
        const auto command = Take(PrepareTerrainCornerRampEdit(warm, document, binding, owner, brush, std::span(&point, 1)));
        Require(!warm.IsDirty() && command.Plan().TerrainPreview()
            && Take(SerializeMapTerrain(*command.Plan().TerrainPreview())) == afterBytes,
            "wet inner default4 preview matches independent complete W3E without publishing");
        Take(ApplyTerrainEdit(warm, document, command));
        const auto journal = Take(TerrainPathingSave::CaptureJournal(current, warm));
        const auto wire = Take(EncodeTerrainStrokeJournal(journal));
        const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
        Require(!journal.history && wire.size() > 4 && wire[3] == std::byte{'J'}
            && decoded.strokes.size() == 1 && decoded.strokes[0].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeWetCorners
            && Take(EncodeTerrainStrokeJournal(decoded)) == wire, "actual wet inner cold wire canonically retains explicit algorithm4");
        MapDataEditSession raw(MapDocument::FromArchive(current));
        Check(raw.AttachTerrain(beforeBytes)); Check(raw.AttachPathing(oldWpm)); Check(raw.SetTerrain(afterBytes));
        auto restored = Take(TerrainPathingSave::RestoreJournal(current, MapDocument::FromArchive(current),
            beforeBytes, oldWpm, decoded, raw, game));
        Require(restored.receipts.size() == 1 && restored.waterOwners.size() == 1 && restored.session->MatchesTerrainJournalDraft(raw),
            "actual wet inner cold recovery recreates real owner and exact complete draft");
        auto& edits = *restored.session;
        const auto verifyTerrain = [&](bool created) {
            Require(Take(SerializeMapTerrain(*edits.Terrain())) == (created ? afterBytes : beforeBytes),
                "actual wet inner cold Undo/Redo retains all original water and height bytes");
        };
        verifyTerrain(true);
        Require(edits.Undo(), "actual wet inner cold command Undo"); verifyTerrain(false);
        Require(edits.Redo(), "actual wet inner cold command Redo"); verifyTerrain(true);
        const auto identity = edits.SessionIdentity(); std::size_t saveIndex = 0;
        const auto save = [&](bool created, const char* phase) {
            verifyTerrain(created);
            const auto lease = edits.PrepareTerrainHistoryForSave();
            const auto epochLimited = [](const auto& result) {
                return !result.Succeeded() && !result.value && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [](const auto& diagnostic) { return diagnostic.code == "TERRAIN_HISTORY_EPOCH_LIMIT"; });
            };
            Require(saveIndex == 0 ? lease.Succeeded() && lease.value.has_value() : epochLimited(lease),
                "wet inner first station freezes history; later saved edited epochs keep the explicit cold-history limit");
            const auto plan = Take(TerrainPathingSave::PrepareForSave(current, edits, game)); Check(plan.SaveGate());
            MapSaveTask task; Check(task.FreezeArchive(current)); task.target = scenario / phase;
            task.replacements = edits.Replacements(); task.removals = edits.Removals();
            Check(plan.InstallPathingOutput(edits, task.replacements));
            task.beforePublish = [&] {
                auto guard = plan.VerifyForSave(edits, task.sourceSha256, task.replacements, task.removals);
                return guard.Succeeded() ? plan.VerifySourceInputs(current) : guard;
            };
            MapSaveService::Run(task); Check({task.diagnostics});
            Require(task.executed && task.succeeded && task.nextSession, "actual wet inner formal save publishes and reopens");
            auto readback = Take(MapArchiveSession::Open(task.written)); Check(plan.VerifyReadback(readback));
            Require(Take(readback.ReadMember("war3map.w3e")) == (created ? afterBytes : beforeBytes)
                && Take(readback.ReadMember("war3map.wpm")) == (created ? newWpm : oldWpm),
                "actual wet inner formal create/saved Undo/Redo matches every independent W3E/WPM byte");
            for (const auto& member : current.Members()) {
                const auto memberName = Text::LowerAscii(member.name);
                if (memberName == "war3map.w3e" || memberName == "war3map.wpm") continue;
                Require(Take(readback.ReadMember(member.name)) == member.data, "wet inner Session save preserves every unrelated member and asymmetric nonzero SHD");
            }
            const auto revision = edits.Revision(), sourceRevision = edits.TerrainSourceRevision();
            Check(plan.CommitSaved(edits, MapDocument::FromArchive(readback)));
            Require(!edits.IsDirty() && edits.SessionIdentity() == identity && edits.Revision() == revision
                && edits.TerrainSourceRevision() == sourceRevision + 1, "wet inner successful save rebases source without losing live Undo/Redo");
            if (saveIndex == 0) Check(edits.CommitTerrainHistorySave(*lease.value));
            std::cout << "playable_real_wet_inner_corner_save_passed scene=" << name << " phase=" << phase << " created=" << created
                << " source_sha256=" << current.SourceSha256() << " candidate_sha256=" << readback.SourceSha256()
                << " w3e_sha256=" << Take(Sha256(created ? afterBytes : beforeBytes)) << " wpm_sha256=" << Take(Sha256(created ? newWpm : oldWpm))
                << " actual_rules_sha256=" << Take(owner.WaterBrushDependencyFingerprint())
                << " actual_water_container=" << waterFile.archiveName << " actual_water_sha256=" << Take(Sha256(waterFile.bytes))
                << " actual_ground_container=" << groundFile.archiveName << " actual_ground_sha256=" << Take(Sha256(groundFile.bytes))
                << " actual_water_offset=" << rules->waterLayerOffset << " actual_deep_threshold=" << rules->deepWaterThreshold
                << " raw_at_or_below=" << lowerRaw << " raw_above=" << lowerRaw + 1 << " raw_selected=" << rawWater
                << " actual_surface=" << surface << " target_surface=" << target << " exact_equality=" << exactEquality
                << " expected_complete_w3e_literal=true expected_complete_wpm_literal=true expected_uses_native_generator=false"
                << " source_unchanged=true native_editor_verified=false game_verified=false\n";
            verifySources(); sources.emplace_back(readback.SourcePath(), readback.SourceSha256()); current = std::move(readback); ++saveIndex;
        };
        save(true, "session-created.w3x");
        Require(edits.Undo() && !edits.CanUndo(), "wet inner Undo survives actual save"); save(false, "session-undone.w3x");
        Require(edits.Redo() && !edits.CanRedo(), "wet inner Redo survives saved Undo"); save(true, "session-redone.w3x"); verifySources();
    }
}
} // namespace War3Map::TerrainWetInnerCornerRampRealCases

namespace War3Map::TerrainSaveTests {
inline void CheckRealWetInnerCornerRampSave(const MapArchiveSession& original, const std::filesystem::path& directory,
    const std::filesystem::path& tool, const std::filesystem::path& game) {
    TerrainWetInnerCornerRampRealCases::Check(original, directory, tool, game);
}
} // namespace War3Map::TerrainSaveTests
