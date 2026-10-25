#include "TerrainBlockFixtures.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"

#include <iostream>
#include <limits>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using namespace War3Map::TerrainBlockFixtures;

template<class T> bool HasCode(const T& value, std::string_view code) {
    return std::any_of(value.diagnostics.begin(), value.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}

Bytes LiteralFingerprintWire() {
    // 与产品stream writer独立的测试wire；位置、尺寸、完整header和175字节原格载荷。
    Bytes wire;
    const auto text = [&](std::string_view value) { for (const auto c : value) wire.push_back(std::byte(c)); };
    const auto put = [&](std::uint32_t value, unsigned count = 4) {
        for (unsigned byte = 0; byte < count; ++byte) wire.push_back(std::byte((value >> (8 * byte)) & 255));
    };
    text("Audastra.TerrainBlock.v1"); put(1, 1); put(4); put(4); put(4); put(4);
    put(11); put('L', 1); put(0, 1); put(33); put(33); put(0); put(0);
    put(2); text("Tg01Tg02"); put(1); text("Cl01");
    const auto terrain = TerrainBlockFixtures::TerrainLiteral(false);
    for (unsigned y = 4; y <= 8; ++y) for (unsigned x = 4; x <= 8; ++x) {
        const auto offset = 49 + 7 * (y * 33 + x);
        wire.insert(wire.end(), terrain.begin() + offset, terrain.begin() + offset + 7);
    }
    return wire;
}

void IndependentCompleteGridsAndFrozenOverlap() {
    BlockFixture f; const auto resources = f.Rules(); const auto intent = f.Intent();
    Require(intent.expectedBlockSha256 == Take(Sha256(LiteralFingerprintWire())),
        "source position and complete raw block hash match independent literal domain bytes");
    const auto candidate = Take(PrepareTerrainBlockPasteStroke(f.grid, *resources.WaterBrushRules(), intent));
    Require(Take(SerializeMapTerrain(candidate.terrain)) == TerrainBlockFixtures::TerrainLiteral(true)
        && Take(SerializeMapTerrain(f.grid)) == TerrainBlockFixtures::TerrainLiteral(false)
        && candidate.result.changedVertices == 4 && candidate.result.changed.x0 == 21 && candidate.result.changed.y0 == 21
        && candidate.result.changed.x1 == 23 && candidate.result.changed.y1 == 23
        && candidate.result.requiresPathingRebuild && candidate.result.removedSpecialSourceRecords.empty(),
        "exact seven raw fields copy only four literal changed vertices; source, special and all outside raw remain intact");
    const std::array ground{TerrainNativeGroundRule{"Tg01", true, true, true}, TerrainNativeGroundRule{"Tg02", false, true, false}};
    for (const bool pasted : {false, true}) {
        TerrainNativePathingInput input; input.terrain = pasted ? &candidate.terrain : &f.grid;
        input.playableBounds = {0, 0, 4096, 4096}; input.groundRules = ground;
        input.water = TerrainNativeWaterRules{0, 64, false}; input.maximumCliffLevel = 14;
        const auto actual = Take(GenerateTerrainNativePathing(input));
        const auto expected = TerrainBlockFixtures::PathingLiteral(pasted);
        Require(actual.size() + 16 == expected.size(), "independent block WPM covers the full grid");
        for (std::size_t i = 0; i < actual.size(); ++i)
            Require(actual[i] == (expected[16 + i] & std::byte{0xee}),
                "every native pathing pixel matches independent cliff, water equality, blight and nonwalkable ground literals");
    }
    // 源目标重叠两列：expected只读取冻结before，不采用已粘贴的前一列。
    auto flatSource = f.grid;
    for (auto& vertex : flatSource.vertices) vertex.cliffAndLayerRaw = 0xf2;
    auto overlap = intent; overlap.destinationVertex = {5, 4};
    overlap.expectedBlockSha256 = Take(CaptureTerrainBlockFingerprint(flatSource, f.sourceCells));
    auto expected = flatSource;
    for (unsigned y = 0; y <= 4; ++y) for (unsigned x = 0; x <= 4; ++x)
        expected.vertices[(y + 4) * 33 + x + 5] = flatSource.vertices[(y + 4) * 33 + x + 4];
    const auto moved = Take(PrepareTerrainBlockPasteStroke(flatSource, *resources.WaterBrushRules(), overlap));
    Require(Take(SerializeMapTerrain(moved.terrain)) == Take(SerializeMapTerrain(expected)),
        "overlap reads all five source columns from frozen before and never smears newly written target columns");
    auto noOp = intent; noOp.destinationVertex = {4, 4};
    const auto same = Take(PrepareTerrainBlockPasteStroke(f.grid, *resources.WaterBrushRules(), noOp));
    Require(same.result.changed.Empty() && !same.result.changedVertices && !same.result.requiresPathingRebuild
        && Take(SerializeMapTerrain(same.terrain)) == TerrainBlockFixtures::TerrainLiteral(false),
        "same-position block is a true no-op after full domain and digest verification");
}

void HashIdentityAndAllRejectedDomainsStayAtomic() {
    BlockFixture f; const auto resources = f.Rules(); const auto rules = *resources.WaterBrushRules(); const auto intent = f.Intent();
    auto derived = f.grid;
    derived.vertices[5 * 33 + 5].position = {123, 456, 789}; derived.minimum = {-1, -2, -3};
    Require(Take(CaptureTerrainBlockFingerprint(derived, f.sourceCells)) == intent.expectedBlockSha256,
        "derived floats are excluded from source raw identity");
    auto outside = f.grid; outside.vertices[30 * 33 + 30].variationRaw ^= 1;
    Require(Take(CaptureTerrainBlockFingerprint(outside, f.sourceCells)) == intent.expectedBlockSha256,
        "unrelated raw variation outside source is not inside the copied block digest");
    Require(Take(CaptureTerrainBlockFingerprint(f.grid, {20, 20, 24, 24}))
        != Take(CaptureTerrainBlockFingerprint(f.grid, {26, 20, 30, 24})),
        "same-content blocks at different source positions keep distinct copy tokens");
    for (unsigned field = 0; field < 8; ++field) {
        auto source = f.grid;
        if (field == 0) ++source.vertices[4 * 33 + 4].groundHeightRaw;
        if (field == 1) ++source.vertices[4 * 33 + 4].waterHeightAndFlagsRaw;
        if (field == 2) source.vertices[4 * 33 + 4].textureAndFlagsRaw = 1;
        if (field == 3) ++source.vertices[4 * 33 + 4].variationRaw;
        if (field == 4) source.vertices[4 * 33 + 4].cliffAndLayerRaw = 2;
        if (field == 5) std::swap(source.header.groundTilesets[0], source.header.groundTilesets[1]);
        if (field == 6) source.header.mainTileset = 'A';
        if (field == 7) source.header.usesCustomTileset = true;
        const auto before = Take(SerializeMapTerrain(source));
        Require(HasCode(PrepareTerrainBlockPasteStroke(source, rules, intent), "TERRAIN_BLOCK_SOURCE_STALE")
            && Take(SerializeMapTerrain(source)) == before, "changed raw source/header/ordered palette refuses the frozen token atomically");
    }
    for (const auto vertex : {2u * 33 + 2, 26u * 33 + 26}) for (unsigned field = 0; field < 5; ++field) {
        auto source = f.grid; auto& changed = source.vertices[vertex];
        if (field == 0) changed.textureAndFlagsRaw |= 0x10;
        if (field == 1) changed.textureAndFlagsRaw |= 0x80;
        if (field == 2) changed.waterHeightAndFlagsRaw |= 0x4000;
        if (field == 3) changed.waterHeightAndFlagsRaw |= 0x8000;
        if (field == 4) changed.cliffAndLayerRaw = 0xfe;
        const auto before = Take(SerializeMapTerrain(source));
        auto limited = rules; limited.maximumCliffLevel = 13;
        Require(HasCode(PrepareTerrainBlockPasteStroke(source, limited, intent), "TERRAIN_BLOCK_DOMAIN")
            && Take(SerializeMapTerrain(source)) == before, "complete source and target second halo reject ramp/boundary/edge/unknown/actual-M violations");
    }
    auto seam = f.grid; seam.vertices[21 * 33 + 21].cliffAndLayerRaw = 0;
    Require(HasCode(PrepareTerrainBlockPasteStroke(seam, rules, intent), "TERRAIN_BLOCK_CLIFF_SEAM"),
        "an invalid target before seam cannot be repaired silently by the paste");
    seam = f.grid;
    seam.vertices[5 * 33 + 4].cliffAndLayerRaw = 3;
    seam.vertices[21 * 33 + 19].cliffAndLayerRaw = 1;
    auto seamIntent = intent;
    seamIntent.expectedBlockSha256 = Take(CaptureTerrainBlockFingerprint(seam, f.sourceCells));
    const auto seamBefore = Take(SerializeMapTerrain(seam));
    Require(HasCode(PrepareTerrainBlockPasteStroke(seam, rules, seamIntent), "TERRAIN_BLOCK_CLIFF_SEAM")
        && Take(SerializeMapTerrain(seam)) == seamBefore, "new multi-level seam refuses without clamping or propagating target outside raw");
    auto flat = f.grid;
    for (auto& vertex : flat.vertices) vertex.cliffAndLayerRaw = 0xf2;
    flat.header.cliffTilesets.push_back("Cl02"); flat.vertices[5 * 33 + 5].cliffAndLayerRaw = 2;
    flat.vertices[5 * 33 + 6].cliffAndLayerRaw = 0x12;
    auto flatRules = rules; flatRules.availableCliffTilesets.push_back("Cl02");
    auto flatIntent = intent; flatIntent.expectedBlockSha256 = Take(CaptureTerrainBlockFingerprint(flat, f.sourceCells));
    Require(PrepareTerrainBlockPasteStroke(flat, flatRules, flatIntent).Succeeded(),
        "flat cliff15 and neighboring unused cliff palette bytes remain legal and exact");
    for (unsigned mode = 0; mode < 7; ++mode) {
        auto malformed = intent;
        if (mode == 0) malformed.algorithm = 2;
        if (mode == 1) malformed.sourceCells.x1 = malformed.sourceCells.x0;
        if (mode == 2) malformed.sourceCells.x1 = 2049;
        if (mode == 3) malformed.sourceCells.x1 = 261;
        if (mode == 4) malformed.destinationVertex[0] = std::numeric_limits<std::uint32_t>::max();
        if (mode == 5) malformed.expectedBlockSha256[0] = 'A';
        if (mode == 6) malformed.expectedBlockSha256.pop_back();
        Require(!ValidateTerrainBlockPasteIntent(malformed).Succeeded()
            && !PrepareTerrainBlockPasteStroke(f.grid, rules, malformed).Succeeded(), "malformed block wire cannot acquire capability");
    }
    auto boundary = intent; boundary.destinationVertex = {1, 1};
    Require(HasCode(PrepareTerrainBlockPasteStroke(f.grid, rules, boundary), "TERRAIN_BLOCK_NEIGHBORHOOD"),
        "missing two-ring halo is never clipped to the map border");
    auto unsupportedSentinel = intent; unsupportedSentinel.destinationVertex = {5, 4};
    Require(HasCode(PrepareTerrainBlockPasteStroke(f.grid, rules, unsupportedSentinel), "TERRAIN_BLOCK_CLIFF_SENTINEL"),
        "overlap with a nonflat sentinel side refuses during prepare before the unchanged formal native save gate");
}

void ActualSpecialShapeAndBudgets() {
    BlockFixture f; const auto resources = f.Rules(); const auto rules = *resources.WaterBrushRules(); const auto intent = f.Intent();
    TerrainWaterSpecialDoodadShape shape{0, "Doo0", 0, {10, 10}, 12, 12, 13, 13, {0, 0, 0, 0}};
    auto prepare = [&](const auto& special) { return PrepareTerrainBlockPasteStroke(f.grid, rules, intent, std::span(&special, 1)); };
    Require(prepare(shape).Succeeded(), "a special strictly between source and target halos is preserved, not rejected by their bounding union");
    shape.x0 = shape.y0 = 2; shape.x1 = shape.y1 = 3;
    Require(HasCode(prepare(shape), "TERRAIN_BLOCK_SPECIAL"), "actual special intersecting source halo refuses without removal");
    shape.x0 = shape.y0 = 26; shape.x1 = shape.y1 = 27;
    Require(HasCode(prepare(shape), "TERRAIN_BLOCK_SPECIAL"), "shared last target halo vertex also intersects actual special occupancy");
    shape.x0 = shape.y0 = 12; shape.x1 = shape.y1 = 13; shape.relativeCliffLevels[3] = 1;
    Require(HasCode(prepare(shape), "TERRAIN_BLOCK_SPECIAL_SHAPE"), "unrelated but stale actual shape cannot masquerade as a complete loaded special set");
    shape.hasFootprint = false; shape.relativeCliffLevels.clear();
    Require(prepare(shape).Succeeded(), "actual no-footprint special remains unchanged");
    const auto good = Take(PrepareTerrainBlockPasteStroke(f.grid, rules, intent));
    TerrainWaterBrushLimits limits; limits.maximumWork = good.result.work;
    Require(PrepareTerrainBlockPasteStroke(f.grid, rules, intent, {}, limits).Succeeded(), "reported full pure work budget is sufficient");
    --limits.maximumWork;
    Require(!PrepareTerrainBlockPasteStroke(f.grid, rules, intent, {}, limits).Succeeded(), "one less pure work unit rejects atomically");
    limits = {}; limits.maximumCandidateBytes = good.peakBytes;
    Require(PrepareTerrainBlockPasteStroke(f.grid, rules, intent, {}, limits).Succeeded(), "reported pure owned peak budget is sufficient");
    --limits.maximumCandidateBytes;
    Require(!PrepareTerrainBlockPasteStroke(f.grid, rules, intent, {}, limits).Succeeded(), "one less actual peak byte rejects atomically");
    limits = {}; limits.maximumPoints = 1;
    Require(PrepareTerrainBlockPasteStroke(f.grid, rules, intent, {}, limits).Succeeded(), "block vertices are not miscounted as mouse points");
    limits = {}; ++limits.maximumCandidateBytes;
    Require(!PrepareTerrainBlockPasteStroke(f.grid, rules, intent, {}, limits).Succeeded(), "callers cannot relax hard limits");
}

void SessionSourceCasBudgetAndSingleHistory() {
    BlockFixture f; const auto resources = f.Rules(); const auto intent = f.Intent();
    const auto initialRevision = f.edits->Revision(), initialSource = f.edits->TerrainSourceRevision();
    const auto plan = Take(f.edits->PrepareTerrainBlockPasteEdit(resources, intent, true));
    f.CheckState(*f.edits, false);
    Require(f.edits->Revision() == initialRevision && !f.edits->CanUndo() && plan.HasChanges()
        && plan.BlockPasteIntent() && *plan.BlockPasteIntent() == intent && plan.Points().empty()
        && plan.WaterResult() && !plan.WaterBrush() && plan.WaterPreparationBytes() && plan.RetainedCommandBytes(),
        "prepared block freezes owned input and exact intent without mutating session/history");
    const auto refused = f.edits->ApplyPreparedTerrainEdit(plan, [](const auto&) {
        return Status::Failure("Fixture", "FIXTURE_BLOCK_REFUSED", "synthetic prepublish refusal");
    });
    Require(HasCode(refused, "FIXTURE_BLOCK_REFUSED") && f.edits->Revision() == initialRevision && !f.edits->CanUndo(),
        "rejected publication leaves all correlated members and history unchanged");
    f.CheckState(*f.edits, false);
    auto other = f.FreshSession();
    Require(HasCode(other->ApplyPreparedTerrainEdit(plan), "TERRAIN_EDIT_STALE"), "foreign session cannot consume a block candidate");
    const auto ownerOff = f.Rules(false);
    Require(HasCode(f.edits->PrepareTerrainBlockPasteEdit(ownerOff, intent), "TERRAIN_BLOCK_RULE_OWNER"),
        "block typed replay cannot borrow an enforce=false owner");
    const auto saveOwner = Take(TerrainPathingResources::Capture(*f.source, f.grid, f.grid, f.game, kBudget));
    Require(HasCode(f.edits->PrepareTerrainBlockPasteEdit(saveOwner, intent), "TERRAIN_BLOCK_RULE_OWNER"),
        "save-equivalence resource owner cannot masquerade as a current editing owner");
    auto otherGrid = f.grid; ++otherGrid.vertices[30 * 33 + 30].variationRaw;
    auto otherMembers = f.members; otherMembers[0].data = Take(SerializeMapTerrain(otherGrid));
    auto otherSource = Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(f.root / "other-source.w3x", otherMembers)).path));
    const auto foreignOwner = Take(TerrainPathingSave::CaptureWaterBrushRules(otherSource,
        otherGrid, *f.edits->Terrain(), f.doodads, f.game, true, kBudget));
    Require(!f.edits->PrepareTerrainBlockPasteEdit(foreignOwner, intent).Succeeded(),
        "same current input cannot substitute for the actual original source identity");
    for (const auto kind : {MapDataKind::Doodads, MapDataKind::Units}) {
        auto brokenMembers = f.members;
        for (auto& member : brokenMembers)
            if (member.name == (kind == MapDataKind::Doodads ? "war3map.doo" : "war3mapUnits.doo"))
                member.data = TerrainCornerRampFixtures::Text("broken placement payload");
        MapDataEditSession broken(MapDocument::FromMembers(brokenMembers));
        Require(broken.AttachTerrain(f.members[0].data).Succeeded() && broken.AttachPathing(f.members[1].data).Succeeded(),
            "synthetic unreadable placement session retains known terrain/pathing");
        Require(!broken.PrepareTerrainBlockPasteEdit(resources, intent, true).Succeeded() && !broken.CanUndo(),
            "unknown actual placement payload cannot be replaced with an empty source set");
        if (kind == MapDataKind::Units) Require(broken.PrepareTerrainBlockPasteEdit(resources, intent, false).Succeeded(),
            "explicit no-follow preserves unreadable unit payload without reading or mutating it");
    }
    for (const bool memory : {false, true}) {
        TerrainWaterBrushLimits limits;
        if (memory) limits.maximumCandidateBytes = plan.WaterPreparationBytes(); else limits.maximumWork = plan.WaterPreparationWork();
        Require(f.edits->PrepareTerrainBlockPasteEdit(resources, intent, true, limits).Succeeded(), "reported session phase/work bound is sufficient");
        if (memory) --limits.maximumCandidateBytes; else --limits.maximumWork;
        Require(!f.edits->PrepareTerrainBlockPasteEdit(resources, intent, true, limits).Succeeded()
            && f.edits->Revision() == initialRevision && f.edits->TerrainSourceRevision() == initialSource && !f.edits->CanUndo(),
            "one below session bound refuses without any history/source/linked-member prefix");
        f.CheckState(*f.edits, false);
    }
    auto noOp = intent; noOp.destinationVertex = {4, 4};
    const auto same = Take(f.edits->PrepareTerrainBlockPasteEdit(resources, noOp));
    Require(!same.HasChanges(), "same-position session candidate has no command changes");
    Take(f.edits->ApplyPreparedTerrainEdit(same));
    Require(f.edits->Revision() == initialRevision && !f.edits->CanUndo(), "no-op does not consume a revision or history item");
    Take(f.edits->ApplyPreparedTerrainEdit(plan)); f.CheckState(*f.edits, true);
    Require(f.edits->Revision() == initialRevision + 1 && f.edits->LastEdit().PlacementLineage().size() == 2,
        "W3E, blight WPM and both Z-only placement changes share one receipt");
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
    Require(journal.strokes.size() == 1 && journal.strokes[0].intent.blockPaste == intent
        && journal.strokes[0].intent.points.empty()
        && journal.strokes[0].rulesSha256 == Take(resources.WaterBrushDependencyFingerprint()),
        "actual Apply persists one exact source block token and owner digest");
    Require(f.edits->Undo(), "block Undo"); f.CheckState(*f.edits, false);
    Require(HasCode(f.edits->ApplyPreparedTerrainEdit(plan), "TERRAIN_EDIT_STALE"), "ABA Undo cannot revive prepared revision");
    Require(f.edits->Redo(), "block Redo"); f.CheckState(*f.edits, true);
    const auto fresh = Take(f.edits->PrepareTerrainBlockPasteEdit(f.Rules(), f.Intent(), false));
    f.edits->CommitSaved(f.edits->Snapshot());
    Require(HasCode(f.edits->ApplyPreparedTerrainEdit(fresh), "TERRAIN_EDIT_STALE"), "same-data source rebasing invalidates an old block plan");
}

void FormalSaveReadbackAndSavedUndoRedo() {
    for (const bool inPlace : {false, true}) for (const bool follow : {false, true}) {
        BlockFixture f; const auto originalPath = f.source->SourcePath(); const auto originalSha = f.source->SourceSha256();
        Take(f.edits->ApplyPreparedTerrainEdit(Take(f.edits->PrepareTerrainBlockPasteEdit(f.Rules(), f.Intent(), follow))));
        const auto save = [&](bool pasted, const char* name) {
            f.CheckState(*f.edits, pasted, follow);
            const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.source, *f.edits, f.game));
            Require(plan.SaveGate().Succeeded(), "block formal save uses the complete actual G0/G1 gate");
            MapSaveTask task; Require(task.FreezeArchive(*f.source).Succeeded(), "block formal save freezes actual source identity");
            task.inPlace = inPlace; task.target = inPlace ? f.source->SourcePath() : f.root / name;
            task.inPlaceOptions.backupDirectory = f.root / "block-backups"; task.inPlaceOptions.keepBackups = 0;
            task.replacements = f.edits->Replacements(); task.removals = f.edits->Removals();
            Require(plan.InstallPathingOutput(*f.edits, task.replacements).Succeeded(), "block installs common complete pathing output");
            task.beforePublish = [&] { return plan.VerifyForSave(*f.edits, task.sourceSha256, task.replacements, task.removals); };
            const auto previousSha = f.source->SourceSha256(); MapSaveService::Run(task);
            Require(task.executed && task.succeeded && task.nextSession, "actual block archive was written and reopened");
            auto actual = Take(MapArchiveSession::Open(task.written));
            Require(plan.VerifyReadback(actual).Succeeded()
                && Take(actual.ReadMember("war3map.w3e")) == TerrainBlockFixtures::TerrainLiteral(pasted)
                && Take(actual.ReadMember("war3map.wpm")) == TerrainBlockFixtures::PathingLiteral(pasted)
                && Take(actual.ReadMember("war3mapUnits.doo")) == Take(f.UnitsLiteral(pasted, follow).Serialize())
                && Take(actual.ReadMember("war3map.doo")) == Take(f.DoodadsLiteral(pasted, follow).Serialize()),
                "every actual W3E/WPM/DOO byte matches independent block/follow literals");
            for (const auto& member : f.members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm"
                && member.name != "war3mapUnits.doo" && member.name != "war3map.doo")
                Require(Take(actual.ReadMember(member.name)) == member.data, "all unrelated authored members remain exact");
            if (inPlace) Require(!task.backup.empty() && Take(Sha256File(task.backup)) == previousSha,
                "in-place block backup equals the complete prior archive");
            Require(plan.CommitSaved(*f.edits, MapDocument::FromArchive(actual)).Succeeded() && !f.edits->IsDirty(),
                "block saving rebases actual source without discarding history");
            f.source = std::make_unique<MapArchiveSession>(std::move(actual));
        };
        save(true, "block-created.w3x");
        Require(f.edits->Undo() && !f.edits->CanUndo(), "single block Undo survives formal save"); save(false, "block-undone.w3x");
        Require(f.edits->Redo() && !f.edits->CanRedo(), "single block Redo survives saved Undo"); save(true, "block-redone.w3x");
        if (!inPlace) Require(Take(Sha256File(originalPath)) == originalSha, "all SaveAs operations leave original archive read-only");
    }
}
}

int main() {
    try {
        IndependentCompleteGridsAndFrozenOverlap(); HashIdentityAndAllRejectedDomainsStayAtomic();
        ActualSpecialShapeAndBudgets(); SessionSourceCasBudgetAndSingleHistory(); FormalSaveReadbackAndSavedUndoRedo();
        std::cout << "TerrainBlockEditingTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
