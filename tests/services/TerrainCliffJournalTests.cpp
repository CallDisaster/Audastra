#include "TerrainCliffJournalTests.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/Services/TerrainPathingSave.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>

namespace War3Map::TerrainSaveTests {
namespace {
using namespace MapData;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kResourceBudget = 8u * 1024u * 1024u;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result, const std::source_location& location = std::source_location::current()) {
    if (!result.Succeeded() || !result.value) {
        std::string message = std::string(location.function_name()) + ":" + std::to_string(location.line());
        for (const auto& diagnostic : result.diagnostics) message += "\n" + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
template<class T> bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
Bytes Text(std::string_view value) {
    const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
    return {bytes.begin(), bytes.end()};
}
void SetMember(std::vector<MapArchiveMember>& members, std::string_view name, Bytes bytes) {
    const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
    if (found == members.end()) members.push_back({std::string(name), std::move(bytes)});
    else found->data = std::move(bytes);
}

// 与Core事务测试使用同一原创数值形态；本层另外读取真实来源DOO建立私有依赖身份。
struct Fixture {
    std::filesystem::path directory, game;
    TerrainGridSnapshot grid;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> source;
    std::unique_ptr<MapDataEditSession> edits;
    TerrainStrokePoint point{2048, 2048};
    explicit Fixture(bool placements = true) {
        static std::atomic<unsigned> next{0};
        directory = std::filesystem::temp_directory_path() / ("Audastra-cliff-journal-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        Require(std::filesystem::create_directory(directory), "private journal directory");
        game = directory / "game"; Require(std::filesystem::create_directory(game), "private game-source directory");
        const std::vector<MapArchiveMember> gameMembers{{"fixture.txt", Text("original journal fixture")}};
        Take(WriteVerifiedArchive(game / "war3.mpq", gameMembers));
        grid.header.width = grid.header.height = 33; grid.header.mainTileset = 'L';
        grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"Cl01", "Cl02"};
        grid.vertices.resize(33 * 33);
        for (auto& vertex : grid.vertices) {
            vertex.groundHeightRaw = 8320; vertex.waterHeightAndFlagsRaw = 7936;
            vertex.variationRaw = 3; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(grid, 0, 0, 33, 33);
        Bytes pathing{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
        for (const auto value : {0u, 128u, 128u}) for (unsigned shift = 0; shift < 32; shift += 8)
            pathing.push_back(static_cast<std::byte>((value >> shift) & 255));
        pathing.resize(16 + 128 * 128, std::byte{0x11});
        members = {{"war3map.w3e", Take(SerializeMapTerrain(grid))}, {"war3map.wpm", std::move(pathing)},
            {"TerrainArt/Terrain.slk", Text("ID;PWXL;N;E\nB;X4;Y2;D0\n"
                "C;X1;Y1;K\"tileID\"\nC;X2;K\"walkable\"\nC;X3;K\"flyable\"\nC;X4;K\"buildable\"\n"
                "C;X1;Y2;K\"Tg01\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\nE\n")},
            {"TerrainArt/Water.slk", Text("ID;PWXL;N;E\nB;X2;Y2;D0\n"
                "C;X1;Y1;K\"waterID\"\nC;X2;K\"height\"\nC;X1;Y2;K\"LSha\"\nC;X2;K0\nE\n")},
            {"TerrainArt/CliffTypes.slk", Text("ID;PWXL;N;E\nB;X1;Y3;D0\n"
                "C;X1;Y1;K\"cliffID\"\nC;X1;Y2;K\"Cl01\"\nC;X1;Y3;K\"Cl02\"\nE\n")},
            {"UI/MiscData.txt", Text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n[Water]\nDeepLevelPathing=128\n")}};
        if (placements) {
            UnitPlacements units; UnitPlacement unit;
            unit.rawcode = "u001"; unit.creationId = 10; unit.position = {2048, 2048, 42}; unit.scale = {1, 1, 1};
            units.units.push_back(unit); unit.creationId = 11; unit.position = {0, 0, 52}; units.units.push_back(unit);
            DoodadPlacements doodads; DoodadPlacement doodad;
            doodad.rawcode = "d001"; doodad.creationId = 12; doodad.position = {2048, 2048, 37}; doodad.scale = {1, 1, 1};
            doodads.doodads.push_back(doodad);
            members.push_back({"war3mapUnits.doo", Take(units.Serialize())});
            members.push_back({"war3map.doo", Take(doodads.Serialize())});
        }
        Reopen("source.w3x");
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(directory, error); }
    Bytes Member(std::string_view name) const {
        const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
        Require(found != members.end(), "journal fixture member exists"); return found->data;
    }
    void Reopen(const char* name) {
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(directory / name, members)).path)));
        edits = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(members));
        Require(edits->AttachTerrain(Member("war3map.w3e")).Succeeded()
            && edits->AttachPathing(Member("war3map.wpm")).Succeeded(), "attach complete original journal inputs");
    }
    TerrainPathingResources Rules() const {
        const auto current = Take(edits->Snapshot(MapDataKind::Doodads, kResourceBudget));
        Require(current.Doodads() || current.Member(MapDataKind::Doodads).state == MapDataState::Missing,
            "journal owner sees actual ready or missing DOO");
        const DoodadPlacements empty;
        return Take(Services::TerrainPathingSave::CaptureWaterBrushRules(*source, grid, *edits->Terrain(),
            current.Doodads() ? *current.Doodads() : empty, game, true, kResourceBudget));
    }
};

Bytes HistoricalV1DependencyWire(const Fixture& f) {
    // 固定旧v1的61项字面清单，不读取生产profile数组或owner的依赖集合。
    constexpr std::array<std::string_view, 61> names{
        "war3map.w3e", "terrainart/terrain.slk", "terrainart/water.slk", "terrainart/clifftypes.slk",
        "units/miscdata.txt", "units/miscgame.txt", "ui/miscdata.txt", "ui/worldeditdata.txt",
        "ui/worldeditstrings.txt", "ui/worldeditgamestrings.txt", "ui/uniteditordata.txt", "ui/aieditordata.txt",
        "ui/worldeditlayout.txt", "units/commandfunc.txt", "units/commandstrings.txt", "ui/triggerdata.txt",
        "ui/triggerstrings.txt", "units/humanunitstrings.txt", "units/neutralunitstrings.txt", "units/nightelfunitstrings.txt",
        "units/orcunitstrings.txt", "units/undeadunitstrings.txt", "units/campaignunitstrings.txt", "units/unitglobalstrings.txt",
        "units/humanunitfunc.txt", "units/neutralunitfunc.txt", "units/nightelfunitfunc.txt", "units/orcunitfunc.txt",
        "units/undeadunitfunc.txt", "units/campaignunitfunc.txt", "units/itemstrings.txt", "units/itemfunc.txt",
        "units/humanabilityfunc.txt", "units/neutralabilityfunc.txt", "units/nightelfabilityfunc.txt", "units/orcabilityfunc.txt",
        "units/undeadabilityfunc.txt", "units/commonabilityfunc.txt", "units/itemabilityfunc.txt", "units/campaignabilityfunc.txt",
        "units/humanabilitystrings.txt", "units/neutralabilitystrings.txt", "units/nightelfabilitystrings.txt", "units/orcabilitystrings.txt",
        "units/undeadabilitystrings.txt", "units/commonabilitystrings.txt", "units/itemabilitystrings.txt", "units/campaignabilitystrings.txt",
        "units/humanupgradefunc.txt", "units/orcupgradefunc.txt", "units/undeadupgradefunc.txt", "units/nightelfupgradefunc.txt",
        "units/campaignupgradefunc.txt", "units/neutralupgradefunc.txt", "units/humanupgradestrings.txt", "units/orcupgradestrings.txt",
        "units/undeadupgradestrings.txt", "units/nightelfupgradestrings.txt", "units/campaignupgradestrings.txt", "units/neutralupgradestrings.txt",
        "war3mapmisc.txt"};
    constexpr std::string_view emptySha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const Bytes emptyDoo{
        std::byte{'W'}, std::byte{'3'}, std::byte{'d'}, std::byte{'o'},
        std::byte{8}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{11}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    Require(Take(DoodadPlacements{}.Serialize()) == emptyDoo, "legacy digest fixture uses the complete literal empty classic DOO");
    Bytes wire;
    const auto u64 = [&](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) wire.push_back(std::byte((value >> shift) & 255));
    };
    const auto string = [&](std::string_view value) {
        u64(value.size()); const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
        wire.insert(wire.end(), bytes.begin(), bytes.end());
    };
    const auto terrain = f.Member("war3map.w3e");
    string("terrain-water-dependencies-v1"); string(Take(Sha256(terrain))); u64(terrain.size());
    string(Take(Sha256(emptyDoo))); u64(1); u64(61);
    for (std::size_t i = 0; i < names.size(); ++i) {
        Bytes bytes;
        switch (i) {
        case 0: bytes = terrain; break;
        case 1: bytes = f.Member("TerrainArt/Terrain.slk"); break;
        case 2: bytes = f.Member("TerrainArt/Water.slk"); break;
        case 3: bytes = f.Member("TerrainArt/CliffTypes.slk"); break;
        case 6: bytes = f.Member("UI/MiscData.txt"); break;
        default: break;
        }
        const bool present = i <= 3 || i == 6;
        string(names[i]); string(present ? Take(Sha256(bytes)) : std::string(emptySha)); string("");
        u64(bytes.size()); u64(present); u64(i == 0); u64(!present);
    }
    return wire;
}

void HistoricalV1DigestKeepsIndependentMapMiscOpaque() {
    Fixture f(false);
    const auto wire = HistoricalV1DependencyWire(f);
    const auto expected = Take(Sha256(wire));
    const auto rules = f.Rules();
    Require(wire.size() <= rules.WaterBrushDependencyBudgetBytes()
        && Take(rules.WaterBrushDependencyFingerprint()) == expected,
        "new capture preserves the complete independent historical v1 wire, including its fixed last absence tuple");
    const auto sourceBytes = rules.SourceBytes();
    SetMember(f.members, "war3mapMisc.txt", Text("[Terrain]\nMaxSlope=0\nMinHeight=2000\nMaxHeight=-256\n[Water]\nDeepLevelPathing=0\n"));
    f.Reopen("map-misc.w3x");
    const auto present = f.Rules();
    Require(present.SourceBytes() == sourceBytes && Take(present.WaterBrushDependencyFingerprint()) == expected,
        "the compatibility tuple neither claims real absence nor hashes conflicting map Misc as a rule source");
    Require(rules.VerifyReadback(*f.source, kResourceBudget).Succeeded(),
        "an added opaque map Misc does not invalidate an independent resource owner");
    SetMember(f.members, "war3mapMisc.txt", Text("changed opaque map Misc")); f.Reopen("changed-map-misc.w3x");
    Require(Take(f.Rules().WaterBrushDependencyFingerprint()) == expected,
        "changed map Misc still cannot enter the reserved v1 slot");
    SetMember(f.members, "UI/MiscData.txt", Text("[Terrain]\nMaxSlope=89\nMinHeight=-256\nMaxHeight=1536\n[Water]\nDeepLevelPathing=128\n"));
    f.Reopen("changed-startup.w3x");
    Require(!rules.VerifyReadback(*f.source, kResourceBudget).Succeeded()
        && Take(f.Rules().WaterBrushDependencyFingerprint()) != expected,
        "actual startup source bytes remain both fingerprinted and independently rechecked");
}

TerrainBrush CliffBrush() {
    TerrainBrush brush; brush.kind = TerrainBrushKind::CliffLevel; brush.size = 1;
    brush.cliffTexture = 1; brush.levelDelta = 1; brush.seed = 1; return brush;
}
void HistoricalV1DigestRestoresAllJournalVersions() {
    for (const unsigned version : {1u, 2u, 3u}) {
        Fixture f(false); const auto rules = f.Rules();
        const auto digest = Take(Sha256(HistoricalV1DependencyWire(f)));
        const auto terrain = f.Member("war3map.w3e"), pathing = f.Member("war3map.wpm");
        auto prepared = [&]() {
            if (version == 2) return f.edits->PrepareTerrainCliffEdit(rules, CliffBrush(), std::span(&f.point, 1), false);
            TerrainWaterBrush brush{TerrainWaterBrushKind::Shallow, TerrainBrushShape::Square, 1, 1, 1,
                Take(CaptureTerrainWaterStrokeAnchor(*f.edits->Terrain(), f.point, rules.WaterBrushRules()->maximumCliffLevel))};
            brush.algorithm = version == 1 ? TerrainWaterAlgorithm::LegacyFlat : TerrainWaterAlgorithm::NativeRamps;
            return f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1), false);
        }();
        const auto plan = Take(std::move(prepared));
        Take(f.edits->ApplyPreparedTerrainEdit(plan));
        const auto after = Take(SerializeMapTerrain(*f.edits->Terrain()));
        auto journal = Take(Services::TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
        Require(journal.strokes.size() == 1 && journal.strokes.front().rulesSha256 == digest,
            "each journal version keeps the independent historical rule digest");
        // 以独立旧wire摘要作为恢复输入，不能用当前owner摘要替代旧记录资格。
        journal.strokes.front().rulesSha256 = digest;
        const auto encoded = Take(EncodeTerrainStrokeJournal(journal));
        Require(encoded.size() > 4 && encoded[0] == std::byte{'F'} && encoded[1] == std::byte{'T'}
            && encoded[2] == std::byte{'J'} && encoded[3] == static_cast<std::byte>('0' + version), "journal retains its explicit FTJ1/2/3 wire version");
        const auto decoded = Take(DecodeTerrainStrokeJournal(encoded));
        Require(Take(EncodeTerrainStrokeJournal(decoded)) == encoded && decoded.strokes.front().rulesSha256 == digest,
            "journal codec roundtrips the old digest without upgrading the water algorithm");
        auto restored = Take(Services::TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
            terrain, pathing, decoded, *f.edits, f.game));
        Require(restored.session && restored.receipts.size() == 1 && restored.waterOwners.size() == 1
            && Take(SerializeMapTerrain(*restored.session->Terrain())) == after,
            "owned replay restores the complete historical command rather than falling back to raw data");
        Require(restored.session->Undo() && Take(SerializeMapTerrain(*restored.session->Terrain())) == terrain
            && restored.session->Redo() && Take(SerializeMapTerrain(*restored.session->Terrain())) == after,
            "all historical journal versions retain complete Undo and Redo");
    }
}
void OrdinaryCliffUsesActualJournalOwner() {
    Fixture f; const auto before = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto rules = f.Rules(); const auto brush = CliffBrush();
    const auto plan = Take(f.edits->PrepareTerrainCliffEdit(rules, brush, std::span(&f.point, 1), true));
    Require(!plan.WaterBrush() && plan.Brush().kind == TerrainBrushKind::CliffLevel && plan.WaterResult(),
        "ordinary cliff retains its own public intent and common native preparation");
    Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == before, "journal preparation leaves original terrain intact");
    Take(f.edits->ApplyPreparedTerrainEdit(plan)); const auto after = Take(SerializeMapTerrain(*f.edits->Terrain()));
    Require(f.edits->Snapshot().Units()->units.front().position[2] == 170
        && f.edits->Snapshot().Doodads()->doodads.front().position[2] == 165,
        "complete DOO groups follow the literal128-unit ordinary cliff delta");
    const auto journal = Take(f.edits->ExportTerrainStrokeJournal());
    Require(journal.strokes.size() == 1 && !journal.strokes.front().intent.waterBrush
        && journal.strokes.front().intent.brush.kind == TerrainBrushKind::CliffLevel
        && journal.strokes.front().cliffEnforceWaterLimits == true && journal.strokes.front().rulesSha256.size() == 64
        && journal.strokes.front().rulesSha256 == Take(rules.WaterBrushDependencyFingerprint()),
        "project recovery retains ordinary cliff, actual complete rule identity and water-limit setting");
    Require(f.edits->Undo() && Take(SerializeMapTerrain(*f.edits->Terrain())) == before, "journal cliff one Undo restores complete native bytes");
    Require(f.edits->Redo() && Take(SerializeMapTerrain(*f.edits->Terrain())) == after, "journal cliff one Redo restores exact native bytes");
    Require(!f.edits->ApplyPreparedTerrainEdit(plan).Succeeded(), "journal owner cannot revive a stale native candidate");
}
void NativeWaterRecordsExistingCornerAndCompleteUndo() {
    for (const bool wet : {false, true}) {
        Fixture f;
        for (std::size_t y = 15; y <= 17; ++y) for (std::size_t x = 15; x <= 17; ++x) {
            auto& vertex = f.grid.vertices[y * 33 + x]; vertex.textureAndFlagsRaw |= 0x10;
            if (x == 17 && y == 17) vertex.cliffAndLayerRaw = 3;
        }
        if (wet) {
            auto& center = f.grid.vertices[16 * 33 + 16]; center.textureAndFlagsRaw |= 0x40;
            center.waterHeightAndFlagsRaw = 8064;
        }
        RefreshTerrainDerived(f.grid, 0, 0, 33, 33); SetMember(f.members, "war3map.w3e", Take(SerializeMapTerrain(f.grid)));
        f.Reopen("water-corner.w3x"); const auto rules = f.Rules();
        const TerrainWaterBrush brush{TerrainWaterBrushKind::Shallow, TerrainBrushShape::Square, 1, 1, 1,
            Take(CaptureTerrainWaterStrokeAnchor(*f.edits->Terrain(), f.point, rules.WaterBrushRules()->maximumCliffLevel))};
        const auto before = Take(SerializeMapTerrain(*f.edits->Terrain()));
        const auto beforeUnits = f.Member("war3mapUnits.doo"), beforeDoodads = f.Member("war3map.doo");
        const auto prepared = Take(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1), true));
        Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == before && !f.edits->IsDirty(), "journal v2 preparation preserves source state");
        Take(f.edits->ApplyPreparedTerrainEdit(prepared)); const auto after = Take(SerializeMapTerrain(*f.edits->Terrain()));
        const auto snapshot = f.edits->Snapshot();
        Require(snapshot.Units()->units.front().position[2] == -118 && snapshot.Doodads()->doodads.front().position[2] == -123
            && snapshot.Units()->units.back().position[2] == 52, "both complete DOO groups retain literal S32 ground64 to ground-96 offsets");
        const auto journal = Take(f.edits->ExportTerrainStrokeJournal());
        Require(journal.strokes.size() == 1 && journal.strokes.front().intent.waterBrush
            && journal.strokes.front().intent.waterBrush->algorithm == TerrainWaterAlgorithm::NativeRamps
            && journal.strokes.front().rulesSha256 == Take(rules.WaterBrushDependencyFingerprint()),
            "the actual v2 command records its algorithm with the complete rule and DOO identity");
        Require(f.edits->Undo() && Take(SerializeMapTerrain(*f.edits->Terrain())) == before
            && Take(f.edits->Snapshot().Units()->Serialize()) == beforeUnits
            && Take(f.edits->Snapshot().Doodads()->Serialize()) == beforeDoodads,
            "one Undo restores the complete old ramp and both original placement members");
        Require(f.edits->Redo() && Take(SerializeMapTerrain(*f.edits->Terrain())) == after
            && f.edits->Snapshot().Doodads()->doodads.front().position[2] == -123, "one Redo restores the same complete v2 command");
        Require(!f.edits->ApplyPreparedTerrainEdit(prepared).Succeeded(), "verified v2 journal anchor cannot bypass revision CAS");
    }
}
void NativeCliffWithoutRuleDigestCannotBecomeLegacyJournal() {
    Fixture f(false); const auto rules = f.Rules(); auto brush = CliffBrush();
    brush.anchor = Take(CaptureTerrainStrokeAnchor(*f.edits->Terrain(), f.point));
    const auto before = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto full = Take(f.edits->PrepareTerrainCliffEdit(rules, brush, std::span(&f.point, 1), false));
    const auto digestWork = rules.WaterBrushDependencyBudgetBytes() * 3;
    Require(digestWork && full.WaterPreparationWork() > digestWork, "native rule digest has a separate actual work allowance");
    TerrainWaterBrushLimits limits; limits.maximumWork = full.WaterPreparationWork() - digestWork;
    const auto bounded = Take(f.edits->PrepareTerrainCliffEdit(rules, brush, std::span(&f.point, 1), false, limits));
    Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == before && !f.edits->IsDirty(), "bounded journal preparation remains atomic");
    Require(bounded.HasChanges() && bounded.WaterPreparationWork() == limits.maximumWork,
        "native cliff remains a complete draft when only extra journal work is unavailable");
    Take(f.edits->ApplyPreparedTerrainEdit(bounded));
    Require(f.edits->IsDirty() && HasCode(f.edits->ExportTerrainStrokeJournal(), "TERRAIN_JOURNAL_HISTORY_UNEXPRESSIBLE"),
        "a caller anchor cannot disguise a native cliff without its rule digest as legacy replay provenance");
    Require(f.edits->Undo() && !f.edits->IsDirty(), "unrecordable native draft retains complete Undo");
}
void NativeFollowChargesActualSamplesAndOptionalDigest() {
    std::array<std::size_t, 2> costs{};
    for (std::size_t nearby = 0; nearby < costs.size(); ++nearby) {
        Fixture f; auto doodads = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
        DoodadPlacement distant = doodads.doodads.front(); distant.position = {0, 0, 61};
        for (std::uint32_t i = 0; i < 4096; ++i) { distant.creationId = 100 + i; doodads.doodads.push_back(distant); }
        if (nearby) doodads.doodads.back().position = {float(f.point.x), float(f.point.y), 61};
        SetMember(f.members, "war3map.doo", Take(doodads.Serialize())); f.Reopen("many-placements.w3x");
        const auto rules = f.Rules(); const auto brush = CliffBrush();
        const auto before = Take(SerializeMapTerrain(*f.edits->Terrain()));
        const auto plan = Take(f.edits->PrepareTerrainCliffEdit(rules, brush, std::span(&f.point, 1), true));
        costs[nearby] = plan.WaterPreparationWork(); const auto digestWork = rules.WaterBrushDependencyBudgetBytes() * 3;
        Require(digestWork && costs[nearby] > digestWork, "follow accounting and optional journal work stay separately bounded");
        TerrainWaterBrushLimits exact; exact.maximumWork = costs[nearby] - digestWork;
        const auto bounded = Take(f.edits->PrepareTerrainCliffEdit(rules, brush, std::span(&f.point, 1), true, exact));
        Require(bounded.WaterPreparationWork() == exact.maximumWork, "actual follow cost repeats at its exact non-journal boundary");
        --exact.maximumWork;
        Require(HasCode(f.edits->PrepareTerrainCliffEdit(rules, brush, std::span(&f.point, 1), true, exact), "TERRAIN_WATER_SESSION_BUDGET"),
            "one unit below actual follow cost rejects before the final height query");
        Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == before && !f.edits->IsDirty(), "budget failures leave the original session intact");
        Take(f.edits->ApplyPreparedTerrainEdit(plan)); const auto actual = f.edits->Snapshot().Doodads();
        Require(actual->doodads.size() == 4097 && actual->doodads[1].position[2] == 61
            && actual->doodads.back().position[2] == (nearby ? 189 : 61), "thousands of distant placements remain complete while nearby placements follow");
        Require(Take(f.edits->ExportTerrainStrokeJournal()).strokes.size() == 1, "full budget records the complete actual follow command");
    }
    Require(costs[1] == costs[0] + 8 * 512, "one extra actual placement adds exactly one bounded before-and-after query charge");
}

std::vector<MapArchiveMember> PaletteGameMembers(bool changedRow = false) {
    return {{"fixture.txt", Text("original palette journal resources")},
        {"TerrainArt/Terrain.slk", Text(std::string("ID;PWXL;N;E\nB;X4;Y3;D0\n")
            + "C;X1;Y1;K\"tileID\"\nC;X2;K\"walkable\"\nC;X3;K\"flyable\"\nC;X4;K\"buildable\"\n"
              "C;X1;Y2;K\"Tg01\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\n"
              "C;X1;Y3;K\"Tg02\"\nC;X2;K1\nC;X3;K1\nC;X4;K" + (changedRow ? "0\nE\n" : "1\nE\n"))},
        {"TerrainArt/CliffTypes.slk", Text("ID;PWXL;N;E\nB;X1;Y4;D0\nC;X1;Y1;K\"cliffID\"\n"
            "C;X1;Y2;K\"Cl01\"\nC;X1;Y3;K\"Cl02\"\nC;X1;Y4;K\"Cl03\"\nE\n")}};
}

void PaletteJournalUsesActualOwnerAndOrderedWaterReplay() {
    Fixture f(false);
    std::erase_if(f.members, [](const auto& member) {
        return member.name == "TerrainArt/Terrain.slk" || member.name == "TerrainArt/CliffTypes.slk";
    });
    Require(std::filesystem::remove(f.game / "war3.mpq"), "replace only owned synthetic game archive");
    Take(WriteVerifiedArchive(f.game / "war3.mpq", PaletteGameMembers())); f.Reopen("palette-source.w3x");
    const auto terrain = f.Member("war3map.w3e"), pathing = f.Member("war3map.wpm");
    const auto sourceSha = f.source->SourceSha256();
    Take(Services::TerrainPathingSave::EditTextureSlot(*f.source, *f.edits, TerrainTextureTable::Ground, 0, "Tg02", f.game));
    const auto first = Take(f.edits->ExportTerrainStrokeJournal());
    Require(first.strokes.size() == 1 && first.strokes[0].intent.palette && first.strokes[0].intent.points.empty()
        && first.strokes[0].intent.palette->expectedRawcode == "Tg01" && first.strokes[0].rulesSha256.size() == 64,
        "actual map plus game winning table creates a typed zero-point existing-slot record");
    const auto revision = f.edits->Revision();
    Take(Services::TerrainPathingSave::EditTextureSlot(*f.source, *f.edits, TerrainTextureTable::Ground, 0, "Tg02", f.game));
    Require(f.edits->Revision() == revision && Take(f.edits->ExportTerrainStrokeJournal()).strokes.size() == 1,
        "same-slot same-value replacement is a true no-op without duplicate history");
    Require(!Services::TerrainPathingSave::EditTextureSlot(*f.source, *f.edits, TerrainTextureTable::Ground, 1, "Tg02", f.game).Succeeded()
        && f.edits->Revision() == revision, "ground append cannot duplicate a rawcode already present in another slot");
    const auto rules = f.Rules();
    TerrainWaterBrush water{TerrainWaterBrushKind::Shallow, TerrainBrushShape::Square, 1, 0, 1,
        Take(CaptureTerrainWaterStrokeAnchor(*f.edits->Terrain(), f.point, rules.WaterBrushRules()->maximumCliffLevel))};
    const auto prepared = Take(f.edits->PrepareTerrainWaterEdit(rules, water, std::span(&f.point, 1), false));
    Take(f.edits->ApplyPreparedTerrainEdit(prepared));
    Take(Services::TerrainPathingSave::EditTextureSlot(*f.source, *f.edits, TerrainTextureTable::Cliff, 1, "Cl03", f.game));
    const auto after = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto journal = Take(Services::TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
    const auto encoded = Take(EncodeTerrainStrokeJournal(journal));
    Require(encoded[3] == std::byte{'6'} && journal.strokes.size() == 3 && journal.strokes[0].intent.palette
        && journal.strokes[1].intent.waterBrush && journal.strokes[2].intent.palette,
        "FTJ6 keeps ground replacement, real water brush and cliff replacement in exact order");
    auto restored = Take(Services::TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        terrain, pathing, Take(DecodeTerrainStrokeJournal(encoded)), *f.edits, f.game));
    Require(restored.receipts.size() == 3 && restored.waterOwners.size() == 3
        && restored.session->MatchesTerrainJournalDraft(*f.edits), "owned ordered replay restores complete raw draft and all three actual owners");
    for (int i = 0; i < 3; ++i) Require(restored.session->Undo(), "all three palette/water commands undo");
    Require(Take(SerializeMapTerrain(*restored.session->Terrain())) == terrain, "mixed Undo restores original header and vertices exactly");
    for (int i = 0; i < 3; ++i) Require(restored.session->Redo(), "all three palette/water commands redo");
    Require(Take(SerializeMapTerrain(*restored.session->Terrain())) == after, "mixed Redo restores the entire output exactly");
    auto wrongOld = journal; wrongOld.strokes[0].intent.palette->expectedRawcode = "Tg03";
    Require(HasCode(Services::TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        terrain, pathing, wrongOld, *f.edits, f.game), "TERRAIN_JOURNAL_PALETTE_DRIFT"), "slot expected-old drift rejects before publication");
    Require(std::filesystem::remove(f.game / "war3.mpq"), "change only owned synthetic table source");
    Take(WriteVerifiedArchive(f.game / "war3.mpq", PaletteGameMembers(true)));
    Require(f.source->SourceSha256() == sourceSha && HasCode(Services::TerrainPathingSave::RestoreJournal(*f.source,
        MapDocument::FromMembers(f.members), terrain, pathing, journal, *f.edits, f.game), "TERRAIN_JOURNAL_RESOURCE_DRIFT"),
        "changed winning game Terrain row fails closed without changing the actual map identity");
    Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == after, "failed drift replay never mutates the active draft");
}

void PaletteDirectoryPreparationUsesFrozenRevision() {
    Fixture f(false);
    const auto rows = PaletteGameMembers();
    for (const auto& member : rows) if (member.name != "fixture.txt") SetMember(f.members, member.name, member.data);
    SetMember(f.members, "war3map.wts", Text("STRING 1\n{\n" + std::string(128u * 1024u, 'x') + "\n}\n"));
    f.Reopen("palette-directory-base.w3x");
    const auto directoryRoot = f.directory / "workspace";
    Require(std::filesystem::create_directory(directoryRoot), "private directory palette source");
    for (const auto& member : f.members) {
        const auto path = directoryRoot / member.name; std::filesystem::create_directories(path.parent_path());
        Require(IO::WriteFileBytes(path, member.data).Succeeded(), "write owned source member");
    }
    const auto directory = Take(DirectoryWorkspace::Open(directoryRoot));
    const auto original = Take(MapDocument::FromDirectory(directory));
    MapDataEditSession edits(original);
    Require(edits.AttachTerrain(f.Member("war3map.w3e")).Succeeded()
        && edits.AttachPathing(f.Member("war3map.wpm")).Succeeded(), "directory palette session has exact original inputs");
    Require(!edits.FreezeForSave(64u * 1024u).Succeeded(), "the broad save snapshot includes unrelated large WTS");
    Require(!edits.FreezeForTerrainTextureSlot(1).Succeeded(), "palette snapshot checks its minimum allocation before copying");
    const auto frozen = Take(edits.FreezeForTerrainTextureSlot(64u * 1024u));
    Require(frozen->Terrain() == edits.Terrain() && frozen->SessionIdentity() == edits.SessionIdentity(),
        "palette snapshot shares the immutable W3E and keeps the exact public CAS identity");
    const auto plan = Take(Services::TerrainPathingSave::PrepareTextureSlot(directory.Snapshot(), *frozen,
        TerrainTextureTable::Ground, 0, "Tg02", f.game));
    Require(!edits.IsDirty() && plan.HasChanges(), "directory preparation on the frozen revision does not edit live state");
    Take(edits.ApplyPreparedTerrainEdit(plan));
    Require(!edits.ApplyPreparedTerrainEdit(plan).Succeeded(), "the same frozen palette plan cannot revive after a revision change");
    const auto journal = Take(Services::TerrainPathingSave::CaptureJournal(directory.Snapshot(), edits));
    const auto restored = Take(Services::TerrainPathingSave::RestoreJournal(directory.Snapshot(), original,
        f.Member("war3map.w3e"), f.Member("war3map.wpm"), journal, edits, f.game));
    Require(restored.session->MatchesTerrainJournalDraft(edits) && restored.receipts.size() == 1,
        "directory source restores via the identical actual-owner palette command");
    const auto snapshot = Take(edits.FreezeForTerrainTextureSlot(64u * 1024u));
    const auto stale = Take(Services::TerrainPathingSave::PrepareTextureSlot(directory.Snapshot(), *snapshot,
        TerrainTextureTable::Ground, 0, "Tg01", f.game));
    TerrainBrush raise; raise.kind = TerrainBrushKind::Raise; raise.radius = 0; raise.strength = 1;
    Take(edits.EditTerrain(raise, std::span(&f.point, 1), false));
    const auto after = Take(SerializeMapTerrain(*edits.Terrain()));
    Require(!edits.ApplyPreparedTerrainEdit(stale).Succeeded() && Take(SerializeMapTerrain(*edits.Terrain())) == after,
        "a late ordinary brush invalidates the pending palette without losing that successful edit");
    const auto source = f.Member("war3map.w3e"); auto changed = source; changed.back() ^= std::byte{1};
    Require(IO::WriteFileBytes(directoryRoot / "war3map.w3e", changed).Succeeded(), "mutate only original synthetic directory source");
    Require(!Services::TerrainPathingSave::PrepareTextureSlot(directory.Snapshot(), edits,
        TerrainTextureTable::Ground, 0, "Tg01", f.game).Succeeded(), "source drift cannot establish a new actual-owner palette command");
}
}

void CheckTerrainCliffJournalContracts() {
    HistoricalV1DigestKeepsIndependentMapMiscOpaque();
    HistoricalV1DigestRestoresAllJournalVersions();
    OrdinaryCliffUsesActualJournalOwner();
    NativeWaterRecordsExistingCornerAndCompleteUndo();
    NativeCliffWithoutRuleDigestCannotBecomeLegacyJournal();
    NativeFollowChargesActualSamplesAndOptionalDigest();
    PaletteJournalUsesActualOwnerAndOrderedWaterReplay();
    PaletteDirectoryPreparationUsesFrozenRevision();
}
}
