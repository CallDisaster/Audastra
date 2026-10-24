#include "TerrainCornerRampServiceTests.h"
#include "../TerrainCornerRampFixtures.h"

namespace War3Map::TerrainSaveTests {
namespace {
using namespace TerrainCornerRampFixtures;
TerrainBrush CornerBrush(bool create = true) { TerrainBrush brush; brush.kind = TerrainBrushKind::CornerRamp; brush.value = create; return brush; }

void FrozenServiceCandidateRestoresOneAtomicCommand() {
    Fixture f; const auto rules = f.Rules(); const auto brush = CornerBrush();
    const auto revision = f.edits->Revision();
    Require(!f.edits->FreezeForSave(1).Succeeded(), "frozen raw source is bounded before copying");
    const auto frozen = Take(f.edits->FreezeForSave(kBudget));
    Require(frozen->Terrain() == f.edits->Terrain() && frozen->SessionIdentity() == f.edits->SessionIdentity(), "frozen worker shares immutable source and actual identity");
    const auto prepared = Take(PrepareTerrainCornerRampEdit(*frozen, f.Document(), f.Binding(), rules, brush, std::span(&f.point, 1), true));
    Require(prepared.Plan().TerrainPreview() && Take(SerializeMapTerrain(*prepared.Plan().TerrainPreview())) == f.TerrainLiteral(true)
        && f.edits->Revision() == revision && !f.edits->IsDirty() && !prepared.Plan().TerrainPathingGate().Succeeded(),
        "worker exposes the same sealed candidate without consuming revision or granting save");
    Take(ApplyTerrainEdit(*f.edits, f.Document(), prepared)); f.CheckState(*f.edits, true);
    Require(!ApplyTerrainEdit(*f.edits, f.Document(), prepared).Succeeded(), "same sealed worker candidate is revision-CAS protected");
    Require(f.edits->Undo() && !f.edits->CanUndo(), "one Undo reverses the whole corner plus both placements"); f.CheckState(*f.edits, false);
    Require(f.edits->Redo() && !f.edits->CanRedo(), "one Redo restores the same complete corner command"); f.CheckState(*f.edits, true);
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits)); const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    Require(wire[3] == std::byte{'4'} && journal.strokes.size() == 1 && journal.strokes[0].intent.brush.kind == TerrainBrushKind::CornerRamp
        && journal.strokes[0].rulesSha256 == Take(rules.WaterBrushDependencyFingerprint()) && !journal.strokes[0].cliffEnforceWaterLimits,
        "one typed FTJ4 command captures actual full rule/DOO identity");
    auto restored = Take(TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, Take(DecodeTerrainStrokeJournal(wire)), *f.edits, f.game));
    Require(restored.receipts.size() == 1 && restored.waterOwners.size() == 1 && restored.session->MatchesTerrainJournalDraft(*f.edits),
        "FTJ4 uses actual owned replay and preserves command lineage"); f.CheckState(*restored.session, true);
    Require(restored.session->Replay(restored.receipts[0], false).Succeeded(), "restored receipt supports normal Qt Undo"); f.CheckState(*restored.session, false);
    Require(restored.session->Replay(restored.receipts[0], true).Succeeded(), "restored receipt supports normal Qt Redo"); f.CheckState(*restored.session, true);
    auto changed = journal; changed.strokes[0].rulesSha256.assign(64, 'f');
    Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, changed, *f.edits, f.game).Succeeded(), "matching output cannot substitute changed dependencies");
    TerrainReplayLimits tight; tight.maximumWork = 1;
    Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, journal, *f.edits, f.game, tight).Succeeded(), "recovery budget failure publishes no partial session");
    f.CheckState(*f.edits, true);
    const auto relaxed = f.Rules(false);
    Require(!PrepareTerrainCornerRampEdit(*f.edits, f.Document(), f.Binding(), relaxed, CornerBrush(false), std::span(&f.point, 1), true).Succeeded(),
        "corner refuses unpersisted alternate water-limit identity");
}

void FormalSaveAsAndInPlaceRetainCompleteHistory() {
    for (const bool inPlace : {false, true}) {
        Fixture f; const auto originalSha = f.source->SourceSha256(); const auto sessionId = f.edits->SessionIdentity();
        const auto rules = f.Rules(); const auto brush = CornerBrush();
        Take(ApplyTerrainEdit(*f.edits, f.Document(), Take(PrepareTerrainCornerRampEdit(*f.edits,
            f.Document(), f.Binding(), rules, brush, std::span(&f.point, 1), true))));
        const auto save = [&](bool created, const char* target) {
            f.CheckState(*f.edits, created);
            const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.source, *f.edits, f.game));
            Require(plan.SaveGate().Succeeded(), "normal save uses complete native G0/G1 comparison");
            MapSaveTask task; Require(task.FreezeArchive(*f.source).Succeeded(), "normal worker freezes exact actual source");
            task.inPlace = inPlace; task.target = inPlace ? f.source->SourcePath() : f.root / target;
            task.inPlaceOptions.backupDirectory = f.root / "backups"; task.inPlaceOptions.keepBackups = 0;
            task.replacements = f.edits->Replacements(); task.removals = f.edits->Removals();
            Require(plan.InstallPathingOutput(*f.edits, task.replacements).Succeeded(), "normal worker installs final native WPM output");
            task.beforePublish = [&] { return plan.VerifyForSave(*f.edits, task.sourceSha256, task.replacements, task.removals); };
            const auto priorSha = f.source->SourceSha256();
            MapSaveService::Run(task);
            if (!task.succeeded) {
                std::string error; for (const auto& d : task.diagnostics) error += d.code + ": " + d.message + "\n";
                throw std::runtime_error(error.empty() ? "corner formal save failed" : error);
            }
            Require(task.executed && task.nextSession, "formal save publishes and reopens candidate");
            if (inPlace) Require(!task.backup.empty() && Take(Sha256File(task.backup)) == priorSha, "in-place save verifies complete pre-save backup");
            auto actual = Take(MapArchiveSession::Open(task.written));
            Require(plan.VerifyReadback(actual).Succeeded() && Take(actual.ReadMember("war3map.w3e")) == f.TerrainLiteral(created)
                && Take(actual.ReadMember("war3map.wpm")) == PathingLiteral(created)
                && Take(actual.ReadMember("war3mapUnits.doo")) == Take(f.UnitsLiteral(created).Serialize())
                && Take(actual.ReadMember("war3map.doo")) == Take(f.DoodadsLiteral(created).Serialize()),
                "complete reopened archive matches literal W3E/WPM and both followed placement groups");
            for (const auto& member : f.members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm"
                && member.name != "war3mapUnits.doo" && member.name != "war3map.doo")
                Require(Take(actual.ReadMember(member.name)) == member.data, "formal corner save preserves unrelated authored members");
            const auto revision = f.edits->Revision(), sourceRevision = f.edits->TerrainSourceRevision();
            Require(plan.CommitSaved(*f.edits, MapDocument::FromArchive(actual)).Succeeded() && !f.edits->IsDirty()
                && f.edits->SessionIdentity() == sessionId && f.edits->Revision() == revision
                && f.edits->TerrainSourceRevision() == sourceRevision + 1, "normal save rebases existing session while preserving undo identity");
            f.source = std::make_unique<MapArchiveSession>(std::move(actual));
        };
        save(true, "created.w3x");
        Require(f.edits->Undo() && !f.edits->CanUndo(), "one corner Undo remains available after normal save"); save(false, "undone.w3x");
        Require(f.edits->Redo() && !f.edits->CanRedo(), "one corner Redo remains available after saved Undo"); save(true, "redone.w3x");
        if (!inPlace) Require(Take(Sha256File(f.root / "source.w3x")) == originalSha, "save-as preserves original complete map bytes");
    }
}
} // namespace

void CheckTerrainCornerRampContracts() {
    FrozenServiceCandidateRestoresOneAtomicCommand(); FormalSaveAsAndInPlaceRetainCompleteHistory();
}
} // namespace War3Map::TerrainSaveTests
