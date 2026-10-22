#include "War3MapCore/MapData/MapDataEditSession.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace War3Map;
using namespace War3Map::MapData;

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Fixture {
    TerrainGridSnapshot terrain;
    std::vector<std::byte> w3e, wpm;
    MapDataEditSession edits{MapDocument::FromMembers({})};
    TerrainStrokePoint point{48, 48}; // WPM (1,1)，其来源已经限制行走。
    explicit Fixture(std::uint32_t side = 9) {
        terrain.header.width = terrain.header.height = side; terrain.header.mainTileset = 'L';
        terrain.header.groundTilesets = {"Ldrt"}; terrain.header.cliffTilesets = {"CLdi"};
        terrain.vertices.resize(static_cast<std::size_t>(side) * side);
        for (auto& vertex : terrain.vertices) { vertex.groundHeightRaw = 8192; vertex.cliffAndLayerRaw = 2; }
        RefreshTerrainDerived(terrain, 0, 0, side, side);
        w3e = *SerializeMapTerrain(terrain).value;
        wpm = {std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
        const auto pathingSide = (side - 1) * 4;
        for (auto value : {0u, pathingSide, pathingSide}) for (unsigned shift = 0; shift < 32; shift += 8)
            wpm.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        wpm.resize(16 + static_cast<std::size_t>(pathingSide) * pathingSide, std::byte{0x80});
        wpm[16 + pathingSide + 1] = std::byte{0x82};
        Require(edits.AttachTerrain(w3e).Succeeded() && edits.AttachPathing(wpm).Succeeded(), "fixture attaches");
    }
    TerrainBrush Brush(std::uint8_t flags = 0x04, bool add = true) const {
        TerrainBrush brush; brush.kind = TerrainBrushKind::PathingRestriction;
        brush.size = 1; brush.pathingRestrictions = flags; brush.value = add; return brush;
    }
    Status Attach(MapDataEditSession& target) const {
        auto status = target.AttachTerrain(w3e);
        return status.Succeeded() ? target.AttachPathing(wpm) : status;
    }
    std::vector<std::byte> Current() const {
        for (const auto& member : edits.Replacements()) if (member.name == "war3map.wpm") return member.data;
        return wpm;
    }
};

void PreviewAndSharedUndoKeepEverySourceBit() {
    Fixture f; auto brush = f.Brush();
    const auto prepared = f.edits.PrepareTerrainEdit(brush, std::span(&f.point, 1));
    Require(prepared.Succeeded() && prepared.value->HasChanges() && !prepared.value->TerrainPathingGate().Succeeded(), "draft preview has no native approval");
    Require(!f.edits.CanUndo() && f.Current() == f.wpm, "preview does not mutate");
    Require(f.edits.ApplyPreparedTerrainEdit(*prepared.value).Succeeded(), "sealed command applies");
    const auto query = f.edits.InspectTerrainPoint(f.point);
    Require(query.Succeeded() && query.value->pathingFlags == 0x86 && query.value->originalPathingFlags == 0x82
        && query.value->addedPathingRestrictions == 0x04, "query distinguishes source from added restriction");
    Require(*SerializeMapTerrain(*f.edits.Terrain()).value == f.w3e && f.edits.Replacements().size() == 1,
        "pathing draft never changes W3E");
    const auto after = f.Current();
    Require(f.edits.Undo() && f.Current() == f.wpm && f.edits.Redo() && f.Current() == after, "original undo and redo restore exact bytes");
    brush = f.Brush(0x0e, false);
    Require(f.edits.EditTerrain(brush, std::span(&f.point, 1)).Succeeded() && f.Current() == f.wpm,
        "removal retains original walk and unknown bits");
    const auto revision = f.edits.Revision();
    Require(f.edits.EditTerrain(brush, std::span(&f.point, 1)).Succeeded() && f.edits.Revision() == revision,
        "removing original restrictions is a no-op");
    Require(f.edits.VerifyTerrainPathingForSave().Succeeded() && f.edits.SerializePathingRestrictionDraft().value->empty(),
        "returning to exact source restores original eligibility and removes draft");
}

void OverlappingIntentIsAnIndependentTransaction() {
    Fixture f; const auto brush = f.Brush(0x02);
    const auto plan = f.edits.PrepareTerrainEdit(brush, std::span(&f.point, 1));
    Require(plan.Succeeded() && plan.value->HasChanges() && plan.value->TerrainPathingGate().Succeeded(),
        "source-blocked paint prepares A-only without inventing a native change");
    Require(!f.edits.ApplyPreparedTerrainEdit(*plan.value, [](const auto&) {
        return Status::Failure("Test", "REFUSED", "拒绝"); }).Succeeded()
        && f.edits.Revision() == 0 && !f.edits.IsDirty() && !f.edits.CanUndo(), "A-only host refusal is atomic");
    Require(f.edits.ApplyPreparedTerrainEdit(*plan.value).Succeeded(), "A-only command applies");
    const auto receipt = f.edits.LastEdit();
    const auto revision = f.edits.Revision();
    Require(revision == 1 && f.edits.IsDirty() && f.edits.Replacements().empty() && f.Current() == f.wpm,
        "A-only revision and dirtiness do not create W3E or WPM replacements");
    Require(f.edits.InspectTerrainPoint(f.point).value->addedPathingRestrictions == 0x02
        && f.edits.QueryPathingRestrictions().value->totalCells == 1, "queries expose overlap intent");
    const auto draft = *f.edits.SerializePathingRestrictionDraft().value;
    Require(draft[3] == std::byte{2} && draft.size() == 132 + f.wpm.size() - 16, "APR2 holds the complete A grid");
    Require(f.edits.EditTerrain(brush, std::span(&f.point, 1)).Succeeded() && f.edits.Revision() == revision,
        "repainting the same intent is the actual no-op");
    Require(!f.edits.ApplyPreparedTerrainEdit(*plan.value).Succeeded(), "A-only invalidates older previews");
    Require(f.edits.Undo() && !f.edits.IsDirty() && f.Current() == f.wpm
        && f.edits.SerializePathingRestrictionDraft().value->empty(), "undo removes intent without clearing source");
    Require(f.edits.Redo() && f.edits.SerializePathingRestrictionDraft().value == draft, "redo restores exact intent");
    Require(f.edits.Replay(receipt, false).Succeeded() && f.edits.Replay(receipt, true).Succeeded(),
        "Qt receipt replay shares the A-only transaction");
    f.edits.CommitSaved(MapDocument::FromMembers({}));
    Require(!f.edits.IsDirty() && f.edits.Replacements().empty()
        && f.edits.SerializePathingRestrictionDraft().value == draft, "native save marks A baseline while retaining its declaration");
    Require(f.edits.Undo() && f.edits.IsDirty() && f.Current() == f.wpm
        && f.edits.SerializePathingRestrictionDraft().value->empty(), "undo across A-only save changes A without clearing source");
    Require(f.edits.Redo() && !f.edits.IsDirty() && f.edits.SerializePathingRestrictionDraft().value == draft,
        "redo across A-only save restores its saved baseline");
    Require(f.edits.EditTerrain(f.Brush(0x02, false), std::span(&f.point, 1)).Succeeded()
        && f.Current() == f.wpm && f.edits.InspectTerrainPoint(f.point).value->addedPathingRestrictions == 0,
        "withdrawing overlap never clears the opaque source restriction or unknown bits");

    MapDataEditSession restored(MapDocument::FromMembers({})); Require(f.Attach(restored).Succeeded(), "A-only restore source");
    Require(restored.SetTerrainAndPathingDraft(f.w3e, f.wpm, draft).Succeeded() && restored.IsDirty()
        && restored.Replacements().empty(), "A-only restores in one transaction without a WPM overlay");
    Require(restored.Undo() && restored.SerializePathingRestrictionDraft().value->empty() && restored.Redo(), "restored A-only undo/redo");
}

void SavedSourceRestrictionsSurviveOlderHistory() {
    Fixture f;
    Require(f.edits.EditTerrain(f.Brush(0x04), std::span(&f.point, 1)).Succeeded(), "prepare synthetic rebased source");
    const auto receipt = f.edits.LastEdit();
    Require(!f.edits.VerifyTerrainPathingForSave().Succeeded(), "ordinary native saving still rejects the new restriction");
    // 公共Core重绑定不代表产品保存放行；只验证未来来源重绑定也不能让历史清除底层。
    f.edits.CommitSaved(MapDocument::FromMembers({}));
    Require(!f.edits.IsDirty() && f.edits.InspectTerrainPoint(f.point).value->originalPathingFlags == 0x86,
        "synthetic saved source includes its opaque restriction");
    Require(f.edits.Undo() && f.edits.InspectTerrainPoint(f.point).value->pathingFlags == 0x86
        && f.edits.SerializePathingRestrictionDraft().value->empty(), "old undo preserves new source restriction and clears only A");
    Require(f.edits.Redo() && !f.edits.IsDirty() && f.edits.InspectTerrainPoint(f.point).value->addedPathingRestrictions == 4,
        "old redo recomposes against the current source");
    Require(f.edits.Replay(receipt, false).Succeeded() && f.edits.Replay(receipt, true).Succeeded()
        && f.edits.InspectTerrainPoint(f.point).value->pathingFlags == 0x86
        && f.edits.SerializePathingRestrictionDraft().Succeeded(), "receipt replay uses the same source-preserving composition");
}

void LegacyDeclarationsMigrateWithoutInventingOverlap() {
    Fixture f; const auto brush = f.Brush(0x04);
    Require(f.edits.EditTerrain(brush, std::span(&f.point, 1)).Succeeded(), "legacy current WPM");
    const auto current = f.Current();
    const auto legacy = *EncodePathingRestrictionDraft(f.terrain, f.wpm, current).value;
    Require(legacy[3] == std::byte{1}, "legacy encoder remains APR1");
    MapDataEditSession restored(MapDocument::FromMembers({})); Require(f.Attach(restored).Succeeded(), "legacy source attaches");
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        auto invalid = legacy;
        if (mutation == 0) invalid[4] ^= std::byte{1};
        else if (mutation == 1) invalid.pop_back();
        else invalid[3] = std::byte{3};
        Require(!restored.SetTerrainAndPathingDraft(f.w3e, current, invalid).Succeeded() && !restored.CanUndo(),
            "old wrong identity, truncation and unknown versions remain closed");
    }
    Require(restored.SetTerrainAndPathingDraft(f.w3e, current, legacy).Succeeded(), "valid APR1 restores");
    const auto migrated = *restored.SerializePathingRestrictionDraft().value;
    Require(migrated[3] == std::byte{2} && restored.InspectTerrainPoint(f.point).value->addedPathingRestrictions == 4,
        "migration retains only declared old additions, never source walk overlap");
    auto forged = migrated; forged[132] |= std::byte{2};
    MapDataEditSession bad(MapDocument::FromMembers({})); Require(f.Attach(bad).Succeeded(), "bad source attaches");
    Require(!bad.SetTerrainAndPathingDraft(f.w3e, current, forged).Succeeded() && !bad.CanUndo(),
        "a declaration cannot claim a restriction missing from the composed WPM");
}

void InvalidInputsRejectWholeStroke() {
    Fixture f;
    for (auto flags : {std::uint8_t{0}, std::uint8_t{0x20}, std::uint8_t{0x12}}) {
        const auto brush = f.Brush(flags);
        Require(!f.edits.EditTerrain(brush, std::span(&f.point, 1)).Succeeded(), "invalid flag combinations reject");
    }
    const auto brush = f.Brush();
    for (const auto bad : {TerrainStrokePoint{1024, 48}, TerrainStrokePoint{-1, 48},
            TerrainStrokePoint{std::numeric_limits<double>::quiet_NaN(), 48}}) {
        const std::array points{f.point, bad};
        Require(!f.edits.EditTerrain(brush, points).Succeeded() && !f.edits.CanUndo() && f.Current() == f.wpm,
            "bad later point including WPM half-open edge cannot leave partial state");
    }
    Require(!ApplyTerrainStroke(f.terrain, brush, std::span(&f.point, 1)).Succeeded(), "raw terrain entry cannot bypass WPM context");
}

void ShapeAndFootprintUsePathingCells() {
    Fixture f; auto brush = f.Brush(); brush.size = 2;
    const TerrainStrokePoint center{176, 176};
    const auto footprint = GetPathingRestrictionFootprint(f.terrain.header, brush, center);
    Require(footprint && footprint->centerX == 176 && footprint->radius == 48
        && footprint->x0 == 4 && footprint->x1 == 7, "preview snaps 32-unit cells with half-cell border");
    auto count = [&](TerrainBrushShape shape) {
        brush.shape = shape;
        auto applied = ApplyPathingRestrictionStroke(f.terrain.header, f.wpm, f.wpm, brush, std::span(&center, 1));
        Require(applied.Succeeded(), "shaped stroke prepares");
        std::size_t changed = 0;
        for (std::size_t i = 0; i < f.wpm.size(); ++i) if (f.wpm[i] != applied.value->bytes[i]) ++changed;
        return changed;
    };
    Require(count(TerrainBrushShape::Circle) == 5 && count(TerrainBrushShape::Square) == 9,
        "circle and square share footprints but preserve their distinct coverage");
}

void OldPlansCannotOverwriteNewChanges() {
    Fixture f;
    const auto planned = f.edits.PrepareTerrainEdit(f.Brush(), std::span(&f.point, 1));
    Require(planned.Succeeded(), "first plan prepares");
    Require(f.edits.EditTerrain(f.Brush(0x08), std::span(&f.point, 1)).Succeeded(), "concurrent local edit applies");
    const auto kept = f.Current();
    Require(!f.edits.ApplyPreparedTerrainEdit(*planned.value).Succeeded() && f.Current() == kept, "stale plan keeps new edit");
    const auto callbackPlan = f.edits.PrepareTerrainEdit(f.Brush(), std::span(&f.point, 1));
    const auto revision = f.edits.Revision();
    Require(!f.edits.ApplyPreparedTerrainEdit(*callbackPlan.value, [](const auto&) {
        return Status::Failure("Test", "HOST_REFUSED", "拒绝发布"); }).Succeeded()
        && f.edits.Revision() == revision && f.Current() == kept, "host refusal is atomic");
}

void DraftRestoresOnlyWithExactDeclaration() {
    Fixture f;
    Require(f.edits.EditTerrain(f.Brush(), std::span(&f.point, 1)).Succeeded(), "draft edit applies");
    const auto changed = f.Current();
    const auto draft = f.edits.SerializePathingRestrictionDraft();
    Require(draft.Succeeded() && !draft.value->empty(), "declaration serializes");
    MapDataEditSession reopened(MapDocument::FromMembers({})); Require(f.Attach(reopened).Succeeded(), "reopen source attaches");
    Require(!reopened.SetTerrainAndPathing(f.w3e, changed).Succeeded() && !reopened.CanUndo(), "old raw restore remains strict");
    auto badDraft = *draft.value; badDraft[4] ^= std::byte{1};
    Require(!reopened.SetTerrainAndPathingDraft(f.w3e, changed, badDraft).Succeeded(), "source SHA mismatch rejects");
    badDraft = *draft.value; badDraft.back() = std::byte{0x20};
    Require(!reopened.SetTerrainAndPathingDraft(f.w3e, changed, badDraft).Succeeded(), "undeclared or unknown mask rejects");
    badDraft.pop_back();
    Require(!reopened.SetTerrainAndPathingDraft(f.w3e, changed, badDraft).Succeeded(), "truncated declaration rejects");
    auto badWpm = changed; badWpm[16] |= std::byte{0x04};
    Require(!reopened.SetTerrainAndPathingDraft(f.w3e, badWpm, *draft.value).Succeeded() && !reopened.CanUndo(),
        "candidate cannot carry an unmentioned cell");
    Require(reopened.SetTerrainAndPathingDraft(f.w3e, changed, *draft.value).Succeeded(), "typed declaration restores together");
    Require(reopened.InspectTerrainPoint(f.point).value->pathingFlags == 0x86
        && !reopened.VerifyTerrainPathingForSave().Succeeded(), "restoration retains native save refusal");
    Require(reopened.Undo() && reopened.InspectTerrainPoint(f.point).value->pathingFlags == 0x82
        && reopened.Redo(), "restored draft remains one undo transaction");
    Require(reopened.EditTerrain(f.Brush(0x0e, false), std::span(&f.point, 1)).Succeeded()
        && reopened.InspectTerrainPoint(f.point).value->pathingFlags == 0x82, "reopened draft can remove only its own additions");
}

void BlightAndRestrictionDraftsRemainIndependent() {
    Fixture f;
    Require(f.edits.EditTerrain(f.Brush(), std::span(&f.point, 1)).Succeeded(), "restriction applies first");
    TerrainBrush blight; blight.kind = TerrainBrushKind::Blight; blight.size = 1;
    const TerrainStrokePoint vertex{0, 0};
    Require(f.edits.EditTerrain(blight, std::span(&vertex, 1)).Succeeded(), "blight uses same WPM transaction");
    const auto changedTerrain = *SerializeMapTerrain(*f.edits.Terrain()).value;
    auto changedPathing = f.Current(); const auto draft = f.edits.SerializePathingRestrictionDraft();
    MapDataEditSession reopened(MapDocument::FromMembers({})); Require(f.Attach(reopened).Succeeded(), "mixed source attaches");
    Require(reopened.SetTerrainAndPathingDraft(changedTerrain, changedPathing, *draft.value).Succeeded(), "mixed draft replays both contributions");
    Require(reopened.EditTerrain(f.Brush(0x0e, false), std::span(&f.point, 1)).Succeeded()
        && reopened.InspectTerrainPoint(f.point).value->pathingFlags == 0xa2, "removing additions retains source plus blight");
    MapDataEditSession bad(MapDocument::FromMembers({})); Require(f.Attach(bad).Succeeded(), "bad mixed source attaches");
    changedPathing.back() ^= std::byte{0x20};
    Require(!bad.SetTerrainAndPathingDraft(changedTerrain, changedPathing, *draft.value).Succeeded()
        && !bad.CanUndo(), "typed draft cannot hide unrelated blight corruption");
}

void BoundedPreviewReportsOriginalCount() {
    Fixture f; auto brush = f.Brush(); brush.size = 2; brush.shape = TerrainBrushShape::Square;
    const TerrainStrokePoint point{176, 176};
    Require(f.edits.EditTerrain(brush, std::span(&point, 1)).Succeeded(), "nine-cell draft applies");
    const auto summary = f.edits.QueryPathingRestrictions(3);
    Require(summary.Succeeded() && summary.value->totalCells == 9 && summary.value->cells.size() == 3,
        "bounded preview reports total without truncating semantic count");
    Require(!f.edits.QueryPathingRestrictions(0).Succeeded() && !f.edits.QueryPathingRestrictions(4097).Succeeded(), "query budget stays hard");
}

// 显式运行时才分配较大原创样例，不将本机计时作为普通 CTest 的通过条件。
void ReportSyntheticCosts() {
    Fixture f(513); // WPM 为 2048×2048，共 4 MiB；不读作者地图。
    const auto timed = [](const char* name, auto&& action) {
        const auto start = std::chrono::steady_clock::now();
        action();
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << name << "_ms=" << elapsed << '\n';
    };
    Result<MapDataEditSession::PreparedTerrainEdit> plan;
    timed("prepare", [&] { plan = f.edits.PrepareTerrainEdit(f.Brush(), std::span(&f.point, 1)); });
    Require(plan.Succeeded() && plan.value, "benchmark plan prepares");
    timed("apply", [&] { Require(f.edits.ApplyPreparedTerrainEdit(*plan.value).Succeeded(), "benchmark applies"); });
    timed("query", [&] { const auto result = f.edits.QueryPathingRestrictions(1024);
        Require(result.Succeeded() && result.value->totalCells == 1, "benchmark query counts exact addition"); });
    timed("undo", [&] { Require(f.edits.Undo(), "benchmark undo"); });
    timed("redo", [&] { Require(f.edits.Redo(), "benchmark redo"); });
    timed("draft", [&] { const auto result = f.edits.SerializePathingRestrictionDraft();
        Require(result.Succeeded() && result.value && !result.value->empty(), "benchmark draft binds source"); });
    std::cout << "wpm_bytes=" << f.wpm.size() << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark") { ReportSyntheticCosts(); return 0; }
        Require(argc == 1, "unsupported test argument");
        PreviewAndSharedUndoKeepEverySourceBit(); InvalidInputsRejectWholeStroke(); ShapeAndFootprintUsePathingCells();
        OverlappingIntentIsAnIndependentTransaction(); LegacyDeclarationsMigrateWithoutInventingOverlap();
        SavedSourceRestrictionsSurviveOlderHistory();
        OldPlansCannotOverwriteNewChanges(); DraftRestoresOnlyWithExactDeclaration();
        BlightAndRestrictionDraftsRemainIndependent(); BoundedPreviewReportsOriginalCount();
        std::cout << "PathingRestrictionEditing: 10 scenario groups passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
