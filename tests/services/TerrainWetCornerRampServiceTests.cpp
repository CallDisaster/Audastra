#include "../TerrainWetCornerRampFixtures.h"

#include <iostream>

namespace War3Map::TerrainSaveTests {
namespace {
using namespace TerrainWetCornerRampFixtures;

void WetAlgorithmUsesActualRulesAndCompleteLiteralGrids() {
    for (const auto threshold : {ThresholdCase::BelowSurface, ThresholdCase::SurfaceEquality,
        ThresholdCase::BelowDeep, ThresholdCase::DeepEquality}) for (const bool blightAbove : {false, true}) {
        WetFixture f({threshold, blightAbove}); const auto rules = f.Rules(); const auto brush = CornerBrush();
        Require(rules.WaterBrushRules() && rules.WaterBrushRules()->maximumCliffLevel == 14
            && rules.WaterBrushRules()->waterLayerOffset == 32, "actual map resources provide M and Water.height, no test permission flag");
        const auto original = f.TerrainLiteral(false);
        Require(!PlanTerrainCornerRamp(f.grid, f.point, TerrainCornerRampAlgorithm::DryOnly).Succeeded(),
            "old corner planning retains dry-only semantics on the same true wet vertices");
        Require(!PrepareTerrainCornerRampStroke(f.grid, *rules.WaterBrushRules(), brush, std::span(&f.point, 1),
            f.doodads, rules.WaterSpecialShapes(), {}, TerrainCornerRampAlgorithm::DryOnly).Succeeded(),
            "old owned corner algorithm cannot acquire wet capability from an unchanged digest");
        const auto candidate = Take(PrepareTerrainCornerRampStroke(f.grid, *rules.WaterBrushRules(), brush,
            std::span(&f.point, 1), f.doodads, rules.WaterSpecialShapes()));
        Require(Take(SerializeMapTerrain(candidate.terrain)) == f.TerrainLiteral(true)
            && Take(SerializeMapTerrain(f.grid)) == original && candidate.result.removedSpecialSourceRecords.empty(),
            "wet creation changes only literal R/strict blight fields, preserving source, water, B, L and special records");
        // Core全WPM也独立核对；正式保存的owner/源WPM解释在下一个用例核对。
        const std::array ground{TerrainNativeGroundRule{"Tg01", true, true, true}};
        const auto generated = [&](const TerrainGridSnapshot& terrain) {
            TerrainNativePathingInput input; input.terrain = &terrain; input.playableBounds = {0, 0, 4096, 4096};
            input.groundRules = ground; input.water = TerrainNativeWaterRules{0.25f, 64, false};
            input.maximumCliffLevel = rules.WaterBrushRules()->maximumCliffLevel;
            return Take(GenerateTerrainNativePathing(input));
        };
        for (const bool created : {false, true}) {
            const auto actual = generated(created ? candidate.terrain : f.grid);
            const auto literal = WetPathingLiteral(f.wet, created);
            Require(actual.size() + 16 == literal.size(), "complete literal WPM has exact dimensions");
            for (std::size_t i = 0; i < actual.size(); ++i)
                Require((std::to_integer<unsigned>(literal[i + 16]) & 0xee) == std::to_integer<unsigned>(actual[i]),
                    "every generated WPM pixel matches literal depth/equality and corner masks");
        }
        auto remove = CornerBrush(false);
        const auto removed = Take(PrepareTerrainCornerRampStroke(candidate.terrain, *rules.WaterBrushRules(), remove,
            std::span(&f.point, 1), f.doodads, rules.WaterSpecialShapes()));
        // 移除是新的原版编辑，不能复活前笔已经清掉的Blight；Undo另核对准确原始数据。
        auto expectedRemoved = f.grid;
        if (blightAbove) expectedRemoved.vertices[12 * 33 + 12].textureAndFlagsRaw &= std::uint8_t(~0x20);
        Require(Take(SerializeMapTerrain(removed.terrain)) == Take(SerializeMapTerrain(expectedRemoved)),
            "wet removal preserves water/raw heights and does not invent blight restoration");
    }
}

void WetSecondRingAndRejectedDomainsRemainAtomic() {
    WetFixture f; const auto rules = f.Rules(); const auto brush = CornerBrush();
    const auto reject = [&](const TerrainGridSnapshot& source, const TerrainStrokePoint& point) {
        const auto before = Take(SerializeMapTerrain(source));
        Require(!PrepareTerrainCornerRampStroke(source, *rules.WaterBrushRules(), brush, std::span(&point, 1),
            f.doodads, rules.WaterSpecialShapes()).Succeeded() && Take(SerializeMapTerrain(source)) == before,
            "unsupported wet second-ring/domain input rejects without writing any source prefix");
    };
    auto changed = f.grid; changed.vertices[19 * 33 + 19].textureAndFlagsRaw |= 0x10; reject(changed, f.point);
    changed = f.grid; changed.vertices[17 * 33 + 19].groundHeightRaw += 1; reject(changed, f.point);
    changed = f.grid; changed.vertices[17 * 33 + 19].waterHeightAndFlagsRaw |= 0x8000; reject(changed, f.point);
    // 九点完整但dirty halo越地图边界仍拒绝；水扩展不授予边界补平能力。
    changed = f.grid;
    for (unsigned y = 0; y < 33; ++y) for (unsigned x = 0; x < 33; ++x) {
        auto& v = changed.vertices[y * 33 + x]; v.textureAndFlagsRaw = 0x40;
        v.cliffAndLayerRaw = x <= 2 && y <= 2 ? 3 : 2;
    }
    reject(changed, {288, 288});
    auto special = f.doodads; special.specialDoodads.push_back({"Doo0", 0, {16, 16}});
    const std::array shapes{TerrainWaterSpecialDoodadShape{0, "Doo0", 0, {16, 16}, 16, 16, 17, 17, {0, 0, 0, 0}}};
    Require(!PrepareTerrainCornerRampStroke(f.grid, *rules.WaterBrushRules(), brush, std::span(&f.point, 1), special, shapes).Succeeded(),
        "wet corner never deletes intersecting special models");
    const auto prepared = Take(PrepareTerrainCornerRampEdit(*f.edits, f.Document(), f.Binding(), rules, brush,
        std::span(&f.point, 1), true));
    Take(ApplyTerrainEdit(*f.edits, f.Document(), prepared)); f.CheckState(*f.edits, true);
    const auto digest = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
    auto noOpDomain = *f.edits->Terrain(); noOpDomain.vertices[19 * 33 + 19].textureAndFlagsRaw |= 0x10;
    reject(noOpDomain, f.point);
    Require(digest.strokes.size() == 1, "one owned wet command includes all object following");
}

void WetTypedHistoryPreservesVersionsAndExactColdRecovery() {
    WetFixture f; const auto rules = f.Rules(); const auto brush = CornerBrush();
    const auto frozen = Take(f.edits->FreezeForSave(kBudget));
    const auto prepared = Take(PrepareTerrainCornerRampEdit(*frozen, f.Document(), f.Binding(), rules, brush,
        std::span(&f.point, 1), true));
    f.CheckState(*f.edits, false);
    Take(ApplyTerrainEdit(*f.edits, f.Document(), prepared)); f.CheckState(*f.edits, true);
    Require(!ApplyTerrainEdit(*f.edits, f.Document(), prepared).Succeeded(), "wet worker candidate is revision-CAS protected");
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    Require(wire[3] == std::byte{'8'} && journal.strokes.size() == 1
        && journal.strokes[0].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::WetTerrain
        && journal.strokes[0].rulesSha256 == Take(rules.WaterBrushDependencyFingerprint()),
        "new wet corner records algorithm2/FTJ8 with actual complete dependencies");
    const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(Take(EncodeTerrainStrokeJournal(decoded)) == wire, "FTJ8 complete canonical codec roundtrip");
    auto disguised = wire; disguised[3] = std::byte{'4'};
    Require(!DecodeTerrainStrokeJournal(disguised).Succeeded(), "FTJ8 cannot be interpreted as an old unversioned corner");
    for (std::size_t size = 0; size < wire.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "every FTJ8 truncation fails closed");
    auto restored = Take(TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, decoded, *f.edits, f.game));
    Require(restored.receipts.size() == 1 && restored.waterOwners.size() == 1
        && restored.session->MatchesTerrainJournalDraft(*f.edits), "cold recovery keeps one atomic command and actual resource owner");
    f.CheckState(*restored.session, true);
    Require(restored.session->Replay(restored.receipts[0], false).Succeeded(), "restored wet receipt Undo"); f.CheckState(*restored.session, false);
    Require(restored.session->Replay(restored.receipts[0], true).Succeeded(), "restored wet receipt Redo"); f.CheckState(*restored.session, true);
    auto legacy = journal; legacy.strokes[0].intent.cornerRampAlgorithm = TerrainCornerRampAlgorithm::DryOnly;
    const auto oldWire = Take(EncodeTerrainStrokeJournal(legacy));
    Require(oldWire[3] == std::byte{'4'}
        && Take(DecodeTerrainStrokeJournal(oldWire)).strokes[0].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::DryOnly,
        "legacy algorithm1 retains byte-for-byte FTJ4 layout");
    auto mixed = journal; mixed.strokes.push_back(legacy.strokes[0]);
    const auto mixedWire = Take(EncodeTerrainStrokeJournal(mixed));
    const auto mixedDecoded = Take(DecodeTerrainStrokeJournal(mixedWire));
    Require(mixedWire[3] == std::byte{'8'} && mixedDecoded.strokes.size() == 2
        && mixedDecoded.strokes[0].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::WetTerrain
        && mixedDecoded.strokes[1].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::DryOnly
        && !EqualTerrainJournalIntent(mixedDecoded.strokes[0].intent, mixedDecoded.strokes[1].intent)
        && Take(EncodeTerrainStrokeJournal(mixedDecoded)) == mixedWire,
        "FTJ8 explicitly preserves mixed corner algorithms and intent identity");
    Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, legacy, *f.edits, f.game).Succeeded(), "old wet record cannot gain algorithm2 from matching final bytes");
    legacy.strokes[0].intent.cornerRampAlgorithm = static_cast<TerrainCornerRampAlgorithm>(3);
    Require(!EncodeTerrainStrokeJournal(legacy).Succeeded(), "unknown corner algorithm cannot be persisted");
    auto changedRules = journal; changedRules.strokes[0].rulesSha256.assign(64, 'f');
    Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, changedRules, *f.edits, f.game).Succeeded(), "wet recovery rejects changed actual rule identity");
    TerrainReplayLimits tight; tight.maximumWork = 1;
    Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, journal, *f.edits, f.game, tight).Succeeded(), "wet recovery rejects insufficient cumulative work");
    Require(f.edits->Undo() && !f.edits->CanUndo(), "one original Undo restores W3E plus both complete DOO groups"); f.CheckState(*f.edits, false);
    Require(f.edits->Redo() && !f.edits->CanRedo(), "one original Redo restores literal wet corner and both complete groups"); f.CheckState(*f.edits, true);
}

void WetFormalSaveAsAndInPlaceRetainUndoRedo() {
    for (const bool inPlace : {false, true}) for (const bool blightAbove : {false, true}) {
        WetFixture f({ThresholdCase::SurfaceEquality, blightAbove}); const auto rules = f.Rules(); const auto brush = CornerBrush();
        const auto originalPath = f.source->SourcePath(); const auto originalSha = f.source->SourceSha256();
        const auto sessionIdentity = f.edits->SessionIdentity();
        Take(ApplyTerrainEdit(*f.edits, f.Document(), Take(PrepareTerrainCornerRampEdit(*f.edits,
            f.Document(), f.Binding(), rules, brush, std::span(&f.point, 1), true))));
        const auto save = [&](bool created, const char* target) {
            f.CheckState(*f.edits, created);
            const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.source, *f.edits, f.game));
            Require(plan.SaveGate().Succeeded(), "wet formal save requires complete actual native G0/G1 comparison");
            MapSaveTask task; Require(task.FreezeArchive(*f.source).Succeeded(), "wet save freezes exact source archive");
            task.inPlace = inPlace; task.target = inPlace ? f.source->SourcePath() : f.root / target;
            task.inPlaceOptions.backupDirectory = f.root / "wet-backups"; task.inPlaceOptions.keepBackups = 0;
            task.replacements = f.edits->Replacements(); task.removals = f.edits->Removals();
            Require(plan.InstallPathingOutput(*f.edits, task.replacements).Succeeded(), "formal worker installs full derived wet WPM");
            task.beforePublish = [&] { return plan.VerifyForSave(*f.edits, task.sourceSha256, task.replacements, task.removals); };
            const auto priorSha = f.source->SourceSha256(); MapSaveService::Run(task);
            if (!task.succeeded) {
                std::string error; for (const auto& d : task.diagnostics) error += d.code + ": " + d.message + "\n";
                throw std::runtime_error(error.empty() ? "wet formal save failed" : error);
            }
            Require(task.executed && task.nextSession, "wet formal save publishes and reopens actual candidate");
            if (inPlace) Require(!task.backup.empty() && Take(Sha256File(task.backup)) == priorSha, "wet in-place save verifies complete prior archive backup");
            auto actual = Take(MapArchiveSession::Open(task.written));
            Require(plan.VerifyReadback(actual).Succeeded()
                && Take(actual.ReadMember("war3map.w3e")) == f.TerrainLiteral(created)
                && Take(actual.ReadMember("war3map.wpm")) == WetPathingLiteral(f.wet, created)
                && Take(actual.ReadMember("war3mapUnits.doo")) == Take(f.UnitsLiteral(created).Serialize())
                && Take(actual.ReadMember("war3map.doo")) == Take(f.DoodadsLiteral(created).Serialize()),
                "reopened formal archive matches full independent wet W3E/WPM and both DOO streams");
            for (const auto& member : f.members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm"
                && member.name != "war3mapUnits.doo" && member.name != "war3map.doo")
                Require(Take(actual.ReadMember(member.name)) == member.data, "wet save preserves every unrelated authored member");
            const auto revision = f.edits->Revision(), sourceRevision = f.edits->TerrainSourceRevision();
            Require(plan.CommitSaved(*f.edits, MapDocument::FromArchive(actual)).Succeeded() && !f.edits->IsDirty()
                && f.edits->SessionIdentity() == sessionIdentity && f.edits->Revision() == revision
                && f.edits->TerrainSourceRevision() == sourceRevision + 1, "wet formal save rebases source while retaining history identity");
            f.source = std::make_unique<MapArchiveSession>(std::move(actual));
        };
        save(true, "wet-created.w3x");
        Require(f.edits->Undo() && !f.edits->CanUndo(), "wet Undo after formal save remains one command"); save(false, "wet-undone.w3x");
        Require(f.edits->Redo() && !f.edits->CanRedo(), "wet Redo after saved Undo remains one command"); save(true, "wet-redone.w3x");
        if (!inPlace) Require(Take(Sha256File(originalPath)) == originalSha, "wet SaveAs leaves original source bytes unchanged");
    }
}
} // namespace

void CheckTerrainWetCornerRampContracts() {
    WetAlgorithmUsesActualRulesAndCompleteLiteralGrids(); WetSecondRingAndRejectedDomainsRemainAtomic();
    WetTypedHistoryPreservesVersionsAndExactColdRecovery(); WetFormalSaveAsAndInPlaceRetainUndoRedo();
}
} // namespace War3Map::TerrainSaveTests

int main() {
    try { War3Map::TerrainSaveTests::CheckTerrainWetCornerRampContracts(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
