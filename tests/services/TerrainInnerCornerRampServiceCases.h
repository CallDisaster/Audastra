#pragma once

#include "../TerrainInnerCornerRampFixtures.h"

namespace War3Map::TerrainInnerCornerRampServiceCases {
namespace F = TerrainCornerRampFixtures;
namespace C = TerrainInnerCornerRampFixtures;
using namespace MapData;
using namespace Services;
using F::Take;
using F::Require;
using F::Bytes;

using Fixture = C::Fixture;

inline PreparedTerrainEdit Prepare(Fixture& f, bool create = true) {
    return Take(PrepareTerrainCornerRampEdit(*f.edits, f.Document(), f.Binding(), f.Rules(),
        C::Brush(create), std::span(&f.point, 1), false, {}, TerrainCornerRampAlgorithm::NativeCorners));
}

inline void SealedCandidateRevisionAndColdJournal() {
    Fixture f; const auto sourceSha = f.source->SourceSha256();
    TerrainStrokeJournal journal; Bytes wire;
    {
        const auto frozen = Take(f.edits->FreezeForSave(F::kBudget));
        const auto rules = f.Rules(); const auto revision = f.edits->Revision();
        const auto prepared = Take(PrepareTerrainCornerRampEdit(*frozen, f.Document(), f.Binding(), rules,
            C::Brush(), std::span(&f.point, 1), false, {}, TerrainCornerRampAlgorithm::NativeCorners));
        Require(prepared.Plan().TerrainPreview()
            && Take(SerializeMapTerrain(*prepared.Plan().TerrainPreview())) == f.TerrainLiteral(true)
            && f.edits->Revision() == revision && !f.edits->IsDirty(), "sealed worker inner preview preserves source revision and clean baseline");
        Take(ApplyTerrainEdit(*f.edits, f.Document(), prepared)); f.CheckState(*f.edits, true);
        Require(!ApplyTerrainEdit(*f.edits, f.Document(), prepared).Succeeded(), "inner command replay is revision-CAS protected");
        const auto stale = Prepare(f, false);
        Require(f.edits->Undo(), "inner Undo consumes current revision"); f.CheckState(*f.edits, false);
        Require(!ApplyTerrainEdit(*f.edits, f.Document(), stale).Succeeded(), "worker prepared before Undo cannot remove at a stale revision");
        Require(f.edits->Redo(), "inner Redo restores exact command"); f.CheckState(*f.edits, true);
        const auto stableRevision = f.edits->Revision();
        Take(ApplyTerrainEdit(*f.edits, f.Document(), Prepare(f)));
        Require(f.edits->Revision() == stableRevision, "validated complete inner no-op creates no extra history entry");
        journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
        wire = Take(EncodeTerrainStrokeJournal(journal));
        Require(!journal.history && wire[3] == std::byte{'F'} && journal.strokes.size() == 1
            && journal.strokes[0].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeCorners
            && journal.strokes[0].rulesSha256 == Take(rules.WaterBrushDependencyFingerprint()), "inner command requires FTJF typed algorithm3 and actual complete rule identity");
        const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
        Require(Take(EncodeTerrainStrokeJournal(decoded)) == wire, "FTJF complete canonical wire roundtrip retains algorithm3");
        for (const char older : std::string_view("89ABCD")) {
            auto disguised = wire; disguised[3] = std::byte(older);
            Require(!DecodeTerrainStrokeJournal(disguised).Succeeded(), "legacy typed header cannot admit new inner algorithm byte3");
        }
        auto unknown = journal; unknown.strokes[0].intent.cornerRampAlgorithm = static_cast<TerrainCornerRampAlgorithm>(255);
        Require(!EncodeTerrainStrokeJournal(unknown).Succeeded(), "unknown future corner algorithm cannot persist");
        for (std::size_t size = 0; size < wire.size(); ++size)
            Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "every truncated FTJF rejects the entire command");
        }

    // 销毁所有热命令及owner；raw仅是待比对草稿，不携带恢复资格。
    f.edits.reset();
    const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    auto raw = f.FreshSession();
    Require(raw->SetTerrain(f.TerrainLiteral(true)).Succeeded(), "cold raw draft attaches final complete terrain bytes");
    auto restored = Take(TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, decoded, *raw, f.game));
    Require(restored.receipts.size() == 1 && restored.waterOwners.size() == 1
        && restored.session->MatchesTerrainJournalDraft(*raw), "cold typed journal rebuilds actual owner and one atomic command");
    f.CheckState(*restored.session, true);
    Require(restored.session->Replay(restored.receipts[0], false).Succeeded(), "cold inner receipt Undo"); f.CheckState(*restored.session, false);
    Require(restored.session->Replay(restored.receipts[0], true).Succeeded(), "cold inner receipt Redo"); f.CheckState(*restored.session, true);
    auto changedRules = decoded; changedRules.strokes[0].rulesSha256.assign(64, 'f');
    Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, changedRules, *raw, f.game).Succeeded(), "matching final bytes cannot replace changed actual rule identity");
    TerrainReplayLimits tight; tight.maximumWork = 1;
    Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, decoded, *raw, f.game, tight).Succeeded(), "inner cold recovery is work bounded before publishing a session");
    Require(f.source->SourceSha256() == sourceSha && Take(Sha256File(f.source->SourcePath())) == sourceSha,
        "cold restore never writes source archive");
}

inline void FormalSaveAsAndInPlacePreserveUndo() {
    for (const bool inPlace : {false, true}) {
        Fixture f; const auto originalSha = f.source->SourceSha256();
        const auto sessionIdentity = f.edits->SessionIdentity();
        Take(ApplyTerrainEdit(*f.edits, f.Document(), Prepare(f)));
        const auto save = [&](bool created, const char* target) {
            f.CheckState(*f.edits, created);
            const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.source, *f.edits, f.game));
            Require(plan.SaveGate().Succeeded(), "inner formal save requires full actual native G0/G1 comparison");
            MapSaveTask task; Require(task.FreezeArchive(*f.source).Succeeded(), "inner save freezes exact physical source archive");
            task.inPlace = inPlace; task.target = inPlace ? f.source->SourcePath() : f.root / target;
            task.inPlaceOptions.backupDirectory = f.root / "inner-backups"; task.inPlaceOptions.keepBackups = 0;
            task.replacements = f.edits->Replacements(); task.removals = f.edits->Removals();
            Require(plan.InstallPathingOutput(*f.edits, task.replacements).Succeeded(), "inner save installs full literal-checkable WPM output");
            task.beforePublish = [&] { return plan.VerifyForSave(*f.edits, task.sourceSha256, task.replacements, task.removals); };
            const auto priorSha = f.source->SourceSha256();
            MapSaveService::Run(task);
            if (!task.succeeded) {
                std::string error; for (const auto& d : task.diagnostics) error += d.code + ": " + d.message + "\n";
                throw std::runtime_error(error.empty() ? "inner formal save failed" : error);
            }
            Require(task.executed && task.nextSession, "inner formal archive is published and reopened");
            if (inPlace) Require(!task.backup.empty() && Take(Sha256File(task.backup)) == priorSha, "inner in-place backup retains full previous map");
            auto actual = Take(MapArchiveSession::Open(task.written));
            Require(plan.VerifyReadback(actual).Succeeded()
                && Take(actual.ReadMember("war3map.w3e")) == f.TerrainLiteral(created)
                && Take(actual.ReadMember("war3map.wpm")) == Fixture::PathingLiteral(created), "inner reopened full W3E/WPM equal independent complete literals");
            for (const auto& member : f.members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm")
                Require(Take(actual.ReadMember(member.name)) == member.data, "inner save preserves all unrelated authored archive members");
            const auto revision = f.edits->Revision(), sourceRevision = f.edits->TerrainSourceRevision();
            Require(plan.CommitSaved(*f.edits, MapDocument::FromArchive(actual)).Succeeded() && !f.edits->IsDirty()
                && f.edits->SessionIdentity() == sessionIdentity && f.edits->Revision() == revision
                && f.edits->TerrainSourceRevision() == sourceRevision + 1, "inner normal save rebases same history and advances only physical source revision");
            f.source = std::make_unique<MapArchiveSession>(std::move(actual));
            const auto savedJournal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
            const auto savedWire = Take(EncodeTerrainStrokeJournal(savedJournal));
            Require(savedJournal.history && savedWire[3] == std::byte{'E'}
                && savedJournal.strokes.size() == 1
                && savedJournal.strokes[0].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeCorners
                && Take(EncodeTerrainStrokeJournal(Take(DecodeTerrainStrokeJournal(savedWire)))) == savedWire,
                "saved-history FTJE nests new typed F without repurposing existing E envelope");
        };
        save(true, "inner-created.w3x");
        Require(f.edits->Undo() && !f.edits->CanUndo(), "inner single Undo survives real save"); save(false, "inner-undone.w3x");
        Require(f.edits->Redo() && !f.edits->CanRedo(), "inner single Redo survives saved Undo"); save(true, "inner-redone.w3x");
        if (!inPlace) Require(Take(Sha256File(f.root / "inner-source.w3x")) == originalSha, "inner save-as retains full original map bytes");
    }
}
inline void Check() { SealedCandidateRevisionAndColdJournal(); FormalSaveAsAndInPlacePreserveUndo(); }
} // namespace War3Map::TerrainInnerCornerRampServiceCases
