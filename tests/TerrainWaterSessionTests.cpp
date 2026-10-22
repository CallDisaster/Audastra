#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/PlacementElevationInvariant.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kResourceBudget = 8u * 1024u * 1024u;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Check(Status status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& diagnostic : status.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
    throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        Check({std::move(result.diagnostics)});
        throw std::runtime_error("missing result");
    }
    return std::move(*result.value);
}
template<class T> bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
Bytes Text(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data() + value.size())};
}
Bytes Member(const std::vector<MapArchiveMember>& members, std::string_view name) {
    const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
    Require(found != members.end(), "fixture member exists"); return found->data;
}
void SetMember(std::vector<MapArchiveMember>& members, std::string_view name, Bytes bytes) {
    const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
    if (found == members.end()) members.push_back({std::string(name), std::move(bytes)});
    else found->data = std::move(bytes);
}

struct Fixture {
    std::filesystem::path directory, game;
    TerrainGridSnapshot grid;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> source;
    std::unique_ptr<MapDataEditSession> edits;
    TerrainStrokePoint point;

    explicit Fixture(bool blight = true, bool placements = true, std::uint32_t size = 33) {
        static std::atomic<unsigned> next{0};
        directory = std::filesystem::temp_directory_path() / ("Audastra-water-session-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        Require(std::filesystem::create_directory(directory), "private test directory");
        game = directory / "game"; Require(std::filesystem::create_directory(game), "private game-source directory");
        const std::vector<MapArchiveMember> gameMembers{{"fixture.txt", Text("original test fixture")}};
        Take(WriteVerifiedArchive(game / "war3.mpq", gameMembers));
        grid.header.width = grid.header.height = size;
        grid.header.mainTileset = 'L'; grid.header.groundTilesets = {"Tg01"};
        grid.header.cliffTilesets = {"Cl01", "Cl02"}; grid.vertices.resize(std::size_t(size) * size);
        for (auto& vertex : grid.vertices) {
            vertex.groundHeightRaw = 8320; vertex.waterHeightAndFlagsRaw = 7936;
            vertex.textureAndFlagsRaw = blight ? 0x20 : 0;
            vertex.variationRaw = 3; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(grid, 0, 0, size, size);
        point = {double(size / 2) * 128, double(size / 2) * 128};
        Bytes wpm{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
        const auto cells = (size - 1) * 4;
        for (const auto value : {0u, cells, cells}) for (unsigned shift = 0; shift != 32; shift += 8)
            wpm.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        wpm.resize(16 + std::size_t(cells) * cells, blight ? std::byte{0x31} : std::byte{0x11});
        members = {{"war3map.w3e", Take(SerializeMapTerrain(grid))}, {"war3map.wpm", std::move(wpm)},
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
            unit.rawcode = "u001"; unit.creationId = 10;
            unit.position = {float(point.x), float(point.y), 42}; unit.scale = {1, 1, 1};
            units.units.push_back(unit); unit.creationId = 11; unit.position = {0, 0, 52}; units.units.push_back(unit);
            DoodadPlacements doodads; DoodadPlacement doodad;
            doodad.rawcode = "d001"; doodad.creationId = 12;
            doodad.position = {float(point.x), float(point.y), 37}; doodad.scale = {1, 1, 1}; doodads.doodads.push_back(doodad);
            members.push_back({"war3mapUnits.doo", Take(units.Serialize())});
            members.push_back({"war3map.doo", Take(doodads.Serialize())});
        }
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(directory / "source.w3x", members)).path)));
        edits = Session();
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(directory, error); }
    std::unique_ptr<MapDataEditSession> Session(bool pathing = true, const std::vector<MapArchiveMember>* from = nullptr) const {
        const auto& data = from ? *from : members;
        auto result = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(data));
        Check(result->AttachTerrain(Member(data, "war3map.w3e")));
        if (pathing) Check(result->AttachPathing(Member(data, "war3map.wpm")));
        return result;
    }
    TerrainPathingResources Rules(const MapDataEditSession& session) const {
        return Take(TerrainPathingResources::CaptureWaterBrushRules(*source, grid, *session.Terrain(), game, true, kResourceBudget));
    }
    TerrainWaterBrush Brush(TerrainWaterBrushKind kind = TerrainWaterBrushKind::Shallow) const {
        return {kind, TerrainBrushShape::Square, 1, 1, 1, Take(CaptureTerrainWaterStrokeAnchor(*edits->Terrain(), point))};
    }
    Bytes Current(std::string_view name, const MapDataEditSession& session) const {
        const auto replacements = session.Replacements();
        const auto found = std::find_if(replacements.begin(), replacements.end(), [&](const auto& value) { return value.name == name; });
        return found == replacements.end() ? Member(members, name) : found->data;
    }
};

struct Unchanged {
    std::uint64_t revision, sourceRevision, command;
    std::shared_ptr<const TerrainGridSnapshot> terrain;
    bool undo, redo;
    std::vector<std::pair<std::string, Bytes>> output;
    explicit Unchanged(const MapDataEditSession& session) : revision(session.Revision()), sourceRevision(session.TerrainSourceRevision()),
        command(session.LastEdit().CommandIdentity()), terrain(session.Terrain()), undo(session.CanUndo()), redo(session.CanRedo()) {
        for (const auto& member : session.Replacements()) output.emplace_back(member.name, member.data);
    }
    void Verify(const MapDataEditSession& session) const {
        std::vector<std::pair<std::string, Bytes>> now;
        for (const auto& member : session.Replacements()) now.emplace_back(member.name, member.data);
        Require(session.Revision() == revision && session.TerrainSourceRevision() == sourceRevision
            && session.LastEdit().CommandIdentity() == command && session.Terrain() == terrain
            && session.CanUndo() == undo && session.CanRedo() == redo && now == output,
            "failed or prepared operation leaves all session state untouched");
    }
};

void WaterBlightAndBothDooShareOneUndo() {
    for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
        Fixture f;
        const auto rules = f.Rules(*f.edits); const auto brush = f.Brush(kind); const Unchanged initial(*f.edits);
        const auto plan = Take(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1)));
        initial.Verify(*f.edits);
        Require(plan.HasChanges() && plan.WaterBrush() && plan.WaterResult()
            && plan.WaterBrush()->kind == kind && plan.WaterResult()->requiresPathingRebuild
            && HasCode(plan.TerrainPathingGate(), "TERRAIN_SAVE_GATE_NOT_INSPECTED"),
            "water has its own operation identity and never calls an uninspected native save gate successful");
        Take(f.edits->ApplyPreparedTerrainEdit(plan));
        Require(f.edits->Revision() == 1 && f.edits->CanUndo(), "one water stroke commits one revision");
        const auto receipt = f.edits->LastEdit();
        Require(receipt.PlacementLineage().size() == 2, "both placement members carry stable row lineage");
        for (const auto& lineage : receipt.PlacementLineage()) {
            Require(lineage.beforeCount == lineage.afterCount, "water never deletes placement rows");
            for (std::uint32_t row = 0; row < lineage.beforeCount; ++row)
                Require(lineage.beforeToAfter[row] == row, "water preserves exact row identity");
        }
        const auto expectedDelta = kind == TerrainWaterBrushKind::Deep ? -256.0f : -128.0f;
        const auto current = f.edits->Snapshot();
        Require(current.Units()->units[0].position[2] == 42 + expectedDelta
            && current.Units()->units[1].position[2] == 52
            && current.Doodads()->doodads[0].position[2] == 37 + expectedDelta,
            "objects keep their ground-relative height and distant objects remain unchanged");
        Check(VerifyUnitPlacementElevationOnly(Member(f.members, "war3mapUnits.doo"), f.Current("war3mapUnits.doo", *f.edits)));
        Check(VerifyDoodadPlacementElevationOnly(Member(f.members, "war3map.doo"), f.Current("war3map.doo", *f.edits)));
        const auto finalWpm = f.Current("war3map.wpm", *f.edits);
        const auto originalWpm = Member(f.members, "war3map.wpm");
        const auto grid = f.edits->Terrain(); const auto width = (grid->header.width - 1) * 4;
        Require(std::equal(finalWpm.begin(), finalWpm.begin() + 16, originalWpm.begin()), "WPM header retained");
        std::size_t cleared = 0;
        for (std::size_t i = 16; i < finalWpm.size(); ++i) {
            const auto x = (i - 16) % width, y = (i - 16) / width;
            const bool blight = grid->vertices[((y + 2) / 4) * grid->header.width + (x + 2) / 4].IsBlight();
            Require((finalWpm[i] & std::byte{0xdf}) == (originalWpm[i] & std::byte{0xdf}), "water draft only changes WPM bit5");
            Require(((finalWpm[i] & std::byte{0x20}) != std::byte{0}) == blight, "every WPM bit5 follows actual final W3E blight");
            cleared += finalWpm[i] != originalWpm[i];
        }
        Require(cleared > 0 && cleared < finalWpm.size() - 16, "local CleanBlight retains distant original blight");
        Require(Take(f.edits->QueryPathingRestrictions()).totalCells == 0, "water adds no user restrictions");
        const auto finalTerrain = Take(SerializeMapTerrain(*grid));
        Require(f.edits->Undo() && !f.edits->IsDirty() && !f.edits->CanUndo(), "one Undo restores terrain, WPM and both DOOs");
        Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == Member(f.members, "war3map.w3e"), "Undo restores exact W3E bytes");
        Require(f.edits->Redo() && f.Current("war3map.wpm", *f.edits) == finalWpm
            && Take(SerializeMapTerrain(*f.edits->Terrain())) == finalTerrain, "Redo replays stored bytes, not another random stroke");
        for (const auto& member : f.members) Require(Take(f.source->ReadMember(member.name)) == member.data, "original archive stays read-only");
    }
}

void StaleForeignAndSourceGenerationReject() {
    Fixture f;
    const auto rules = f.Rules(*f.edits); const auto brush = f.Brush();
    const auto plan = Take(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1)));
    auto foreign = f.Session(); const Unchanged foreignBefore(*foreign);
    Require(HasCode(foreign->ApplyPreparedTerrainEdit(plan), "TERRAIN_EDIT_STALE"), "foreign session refuses a water candidate");
    foreignBefore.Verify(*foreign);
    TerrainBrush raise; raise.kind = TerrainBrushKind::Raise; raise.radius = 0; raise.strength = 1;
    Take(f.edits->EditTerrain(raise, std::span(&f.point, 1), false));
    const Unchanged afterRaise(*f.edits);
    Require(!f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1)).Succeeded(), "stale current W3E cannot borrow captured rules");
    afterRaise.Verify(*f.edits);
    Require(f.edits->Undo(), "undo intervening edit"); const Unchanged afterUndo(*f.edits);
    Require(HasCode(f.edits->ApplyPreparedTerrainEdit(plan), "TERRAIN_EDIT_STALE"), "ABA Undo does not revive a stale prepared revision");
    afterUndo.Verify(*f.edits);
    const auto fresh = Take(f.edits->PrepareTerrainWaterEdit(f.Rules(*f.edits), brush, std::span(&f.point, 1)));
    const auto revision = f.edits->Revision(); f.edits->CommitSaved(f.edits->Snapshot());
    Require(f.edits->Revision() == revision, "baseline commit is independent of editing revision"); const Unchanged rebased(*f.edits);
    Require(HasCode(f.edits->ApplyPreparedTerrainEdit(fresh), "TERRAIN_EDIT_STALE"), "source-generation change invalidates an otherwise identical candidate");
    rebased.Verify(*f.edits);
}

void RuleOwnerAndOriginalIdentityAreRequired() {
    Fixture f; const auto brush = f.Brush(); const Unchanged before(*f.edits);
    const auto saveRules = Take(TerrainPathingResources::Capture(*f.source, f.grid, f.grid, f.game, kResourceBudget));
    Require(HasCode(f.edits->PrepareTerrainWaterEdit(saveRules, brush, std::span(&f.point, 1)), "TERRAIN_WATER_RULE_OWNER"),
        "save-equivalence rules cannot masquerade as captured water editing rules");
    auto alternate = f.grid; alternate.vertices[0].variationRaw = 2;
    auto otherMembers = f.members; SetMember(otherMembers, "war3map.w3e", Take(SerializeMapTerrain(alternate)));
    const auto otherSource = Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(f.directory / "other.w3x", otherMembers)).path));
    const auto wrongSource = Take(TerrainPathingResources::CaptureWaterBrushRules(otherSource, alternate, *f.edits->Terrain(), f.game, true, kResourceBudget));
    Require(!f.edits->PrepareTerrainWaterEdit(wrongSource, brush, std::span(&f.point, 1)).Succeeded(),
        "same current W3E does not replace the original source identity");
    before.Verify(*f.edits);
}

void FailuresAndRejectedPublicationStayAtomic() {
    Fixture f; const auto rules = f.Rules(*f.edits); const auto brush = f.Brush(); const Unchanged initial(*f.edits);
    const auto plan = Take(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1)));
    const auto rejected = f.edits->ApplyPreparedTerrainEdit(plan, [](const auto&) {
        return Status::Failure("Fixture", "FIXTURE_PUBLICATION_REJECTED", "original test rejects publication");
    });
    Require(HasCode(rejected, "FIXTURE_PUBLICATION_REJECTED"), "unified outer transaction can reject water publication");
    initial.Verify(*f.edits);
    TerrainWaterBrushLimits limits;
    limits.maximumCandidateBytes = 64;
    Require(HasCode(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1), true, limits), "TERRAIN_WATER_SESSION_BUDGET"),
        "budget includes transaction state before the algorithm allocates");
    limits = {}; limits.maximumWork = 1;
    Require(!f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1), true, limits).Succeeded(), "bounded transaction work");
    limits = {}; limits.maximumPoints = 1;
    const std::array points{f.point, f.point};
    Require(HasCode(f.edits->PrepareTerrainWaterEdit(rules, brush, points, true, limits), "TERRAIN_WATER_POINTS"), "bounded point count");
    limits = {}; ++limits.maximumCandidateBytes;
    Require(HasCode(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1), true, limits), "TERRAIN_WATER_LIMITS"), "cannot raise hard memory limit");
    initial.Verify(*f.edits);

    auto missingPathing = f.Session(false); const Unchanged missingBefore(*missingPathing);
    Require(HasCode(missingPathing->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1)), "MAP_PATHING_UNAVAILABLE"),
        "CleanBlight cannot commit without WPM");
    missingBefore.Verify(*missingPathing);
    auto mismatchedMembers = f.members; auto pathing = Member(f.members, "war3map.wpm");
    const auto width = (f.grid.header.width - 1) * 4;
    pathing[16 + std::size_t(f.grid.header.height / 2 * 4) * width + f.grid.header.width / 2 * 4] &= std::byte{0xdf};
    SetMember(mismatchedMembers, "war3map.wpm", std::move(pathing));
    auto mismatched = f.Session(true, &mismatchedMembers); const Unchanged mismatchBefore(*mismatched);
    Require(HasCode(mismatched->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1)), "MAP_PATHING_BLIGHT_MISMATCH"),
        "mismatched source blight fails without a partially changed W3E");
    mismatchBefore.Verify(*mismatched);
}

void ActualDoodadStateAndFollowOptionAreRespected() {
    Fixture f; const auto rules = f.Rules(*f.edits); const auto brush = f.Brush();
    auto specials = *f.edits->Snapshot().Doodads(); specials.specialDoodads.push_back({"D001", 0, {0, 0}});
    Check(f.edits->EditDoodads(specials)); const Unchanged withSpecial(*f.edits);
    Require(HasCode(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1)), "TERRAIN_WATER_SPECIAL_DOODAD_FACTS"),
        "actual special records require actual shape resources, even when not ordinary following");
    withSpecial.Verify(*f.edits);
    auto corruptMembers = f.members; SetMember(corruptMembers, "war3map.doo", Text("broken"));
    auto corrupt = f.Session(true, &corruptMembers); const Unchanged corruptBefore(*corrupt);
    Require(HasCode(corrupt->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1), false), "MAP_DOODADS_UNSAFE"),
        "turning following off cannot treat unreadable DOO as empty");
    corruptBefore.Verify(*corrupt);
    auto noFollow = f.Session();
    const auto plan = Take(noFollow->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1), false));
    Take(noFollow->ApplyPreparedTerrainEdit(plan));
    Require(!noFollow->LastEdit().ChangesPlacements()
        && f.Current("war3map.doo", *noFollow) == Member(f.members, "war3map.doo")
        && f.Current("war3mapUnits.doo", *noFollow) == Member(f.members, "war3mapUnits.doo"), "explicit no-follow preserves both DOOs");
    Fixture empty(false, false);
    const auto emptyRules = empty.Rules(*empty.edits); const auto emptyBrush = empty.Brush();
    auto noWpm = empty.Session(false);
    const auto emptyPlan = Take(noWpm->PrepareTerrainWaterEdit(emptyRules, emptyBrush, std::span(&empty.point, 1)));
    Take(noWpm->ApplyPreparedTerrainEdit(emptyPlan));
    Require(noWpm->IsDirty() && !noWpm->HasTerrainPathing(), "known absent DOO is allowed, while an absent WPM is not fabricated");
    // 来源缺失后由用户新增的普通记录也是实际草稿，不能误认成来源损坏。
    DoodadPlacements added; DoodadPlacement doodad;
    doodad.rawcode = "d001"; doodad.position = {float(empty.point.x), float(empty.point.y), 37}; doodad.scale = {1, 1, 1};
    added.doodads.push_back(doodad); Check(empty.edits->EditDoodads(added));
    const auto addedPlan = Take(empty.edits->PrepareTerrainWaterEdit(emptyRules, emptyBrush, std::span(&empty.point, 1)));
    Take(empty.edits->ApplyPreparedTerrainEdit(addedPlan));
    Require(empty.edits->Snapshot().Doodads()->doodads[0].position[2] == -91, "new explicit ordinary doodads follow Z in the same transaction");
}

void UserRestrictionsAndSeedRemainIndependent() {
    Fixture f;
    TerrainBrush restriction; restriction.kind = TerrainBrushKind::PathingRestriction;
    restriction.pathingRestrictions = 0x06; restriction.size = 1;
    Take(f.edits->EditTerrain(restriction, std::span(&f.point, 1)));
    const auto beforeWpm = f.Current("war3map.wpm", *f.edits);
    const auto beforeCount = Take(f.edits->QueryPathingRestrictions()).totalCells;
    const auto rules = f.Rules(*f.edits); const auto brush = f.Brush();
    const auto first = Take(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1)));
    const auto second = Take(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1)));
    Require(first.WaterResult()->finalRandomState == second.WaterResult()->finalRandomState, "same full stroke keeps the same random result");
    Take(f.edits->ApplyPreparedTerrainEdit(first)); const auto firstBytes = Take(SerializeMapTerrain(*f.edits->Terrain()));
    Require(Take(f.edits->QueryPathingRestrictions()).totalCells == beforeCount, "water does not add or erase existing user intent");
    const auto waterWpm = f.Current("war3map.wpm", *f.edits);
    for (std::size_t i = 16; i < waterWpm.size(); ++i)
        Require((waterWpm[i] & std::byte{0xdf}) == (beforeWpm[i] & std::byte{0xdf}), "preexisting user intent survives CleanBlight");
    Require(f.edits->Undo() && f.Current("war3map.wpm", *f.edits) == beforeWpm, "Undo water preserves prior restriction command");
    const auto repeated = Take(f.edits->PrepareTerrainWaterEdit(f.Rules(*f.edits), brush, std::span(&f.point, 1)));
    Take(f.edits->ApplyPreparedTerrainEdit(repeated));
    Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == firstBytes, "same anchor, seed and content regenerate identical W3E");
}

void LargerOriginalTerrainRecordsPreparationCost() {
    Fixture f(false, false, 257); const auto rules = f.Rules(*f.edits); const auto brush = f.Brush();
    const Unchanged before(*f.edits); const auto started = std::chrono::steady_clock::now();
    const auto plan = Take(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1)));
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    before.Verify(*f.edits); Require(plan.HasChanges(), "larger valid original fixture prepares real changes");
    std::cout << "water-session prepare vertices=" << f.grid.vertices.size()
        << " source_w3e_bytes=" << Member(f.members, "war3map.w3e").size()
        << " source_wpm_bytes=" << Member(f.members, "war3map.wpm").size()
        << " changed_vertices=" << plan.WaterResult()->changedVertices
        << " algorithm_work=" << plan.WaterResult()->work << " preparation_work=" << plan.WaterPreparationWork()
        << " milliseconds=" << elapsed << '\n';
    // 输出供实际测量，不提高测试时限，也不把耗时当成保存或游戏正确性的证据。
}

void FullSizeStrokeUsesActualPhaseBudget() {
    for (const bool blight : {false, true}) {
        Fixture f(blight, false, 513);
        const auto rules = f.Rules(*f.edits); const auto brush = f.Brush(); const Unchanged before(*f.edits);
        // 直接算法与Session使用相同真实规则和实际缺失DOO；中间owned地形在Session准备前释放。
        const auto expected = [&] {
            const auto pure = Take(PrepareTerrainWaterStroke(*f.edits->Terrain(), *rules.WaterBrushRules(),
                brush, std::span(&f.point, 1), DoodadPlacements{}));
            return std::pair{pure.result.work, Take(SerializeMapTerrain(pure.terrain))};
        }();
        const auto plan = Take(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1)));
        Require(plan.HasChanges() && plan.WaterResult()->work == expected.first
            && plan.WaterPreparationWork() > expected.first
            && plan.WaterPreparationWork() <= TerrainWaterBrushLimits{}.maximumWork,
            "a full-size one-point stroke fits the unchanged hard budget without charging unrelated full-WPM work");
        Require(HasCode(plan.TerrainPathingGate(), "TERRAIN_SAVE_GATE_NOT_INSPECTED"),
            "successful full-size draft does not claim native save qualification");
        TerrainWaterBrushLimits exact; exact.maximumWork = plan.WaterPreparationWork();
        const auto bounded = Take(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1), true, exact));
        Require(bounded.WaterPreparationWork() == exact.maximumWork, "phase accounting is repeatable at the exact work budget");
        --exact.maximumWork;
        Require(HasCode(f.edits->PrepareTerrainWaterEdit(rules, brush, std::span(&f.point, 1), true, exact),
            "TERRAIN_WATER_SESSION_BUDGET"), "one unit below the measured phase budget rejects before transaction publication");
        before.Verify(*f.edits);
        Take(f.edits->ApplyPreparedTerrainEdit(plan));
        Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == expected.second,
            "session budgeting preserves the complete pure water result");
        const auto output = f.edits->Replacements();
        const bool writesWpm = std::any_of(output.begin(), output.end(), [](const auto& member) { return member.name == "war3map.wpm"; });
        Require(writesWpm == blight, "WPM output exists only for actual CleanBlight net changes");
        Require(f.edits->Undo() && !f.edits->IsDirty(), "full-size budgeted transaction remains one complete Undo");
    }
}
} // namespace

int main() {
    try {
        WaterBlightAndBothDooShareOneUndo();
        StaleForeignAndSourceGenerationReject();
        RuleOwnerAndOriginalIdentityAreRequired();
        FailuresAndRejectedPublicationStayAtomic();
        ActualDoodadStateAndFollowOptionAreRespected();
        UserRestrictionsAndSeedRemainIndependent();
        LargerOriginalTerrainRecordsPreparationCost();
        FullSizeStrokeUsesActualPhaseBudget();
        std::cout << "TerrainWaterSessionTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
