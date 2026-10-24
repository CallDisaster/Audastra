#include "TerrainCliffJournalTests.h"

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

TerrainBrush CliffBrush() {
    TerrainBrush brush; brush.kind = TerrainBrushKind::CliffLevel; brush.size = 1;
    brush.cliffTexture = 1; brush.levelDelta = 1; brush.seed = 1; return brush;
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
}

void CheckTerrainCliffJournalContracts() {
    OrdinaryCliffUsesActualJournalOwner();
    NativeWaterRecordsExistingCornerAndCompleteUndo();
    NativeCliffWithoutRuleDigestCannotBecomeLegacyJournal();
    NativeFollowChargesActualSamplesAndOptionalDigest();
}
}
