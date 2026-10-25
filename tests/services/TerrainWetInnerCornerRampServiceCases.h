#pragma once

#include "../TerrainWetInnerCornerRampFixtures.h"

namespace War3Map::TerrainWetInnerCornerRampServiceCases {
namespace F = TerrainCornerRampFixtures;
namespace C = TerrainInnerCornerRampFixtures;
namespace W = TerrainWetInnerCornerRampFixtures;
using namespace MapData;
using namespace Services;
using F::Take;
using F::Require;
using F::Bytes;
using Fixture = W::Fixture;

inline PreparedTerrainEdit Prepare(Fixture& f, bool create = true, bool follow = false) {
    // 默认入口必须选新算法；历史恢复仍显式传各笔原算法。
    return Take(PrepareTerrainCornerRampEdit(*f.edits, f.Document(), f.Binding(), f.Rules(),
        C::Brush(create), std::span(&f.point, 1), follow));
}

inline void SealedCandidateAndTypedColdRestore() {
    for (const bool follow : {false, true}) {
        Fixture f({W::Threshold::SurfaceEquality, true});
        const auto sourceSha = f.source->SourceSha256(); TerrainStrokeJournal journal; Bytes wire;
        {
            const auto rules = f.Rules();
            Require(rules.WaterBrushRules() && rules.WaterBrushRules()->waterLayerOffset == 32
                && rules.WaterBrushRules()->deepWaterThreshold == 64, "wet inner uses captured Water.height and DeepLevelPathing");
            const auto frozen = Take(f.edits->FreezeForSave(F::kBudget));
            const auto revision = f.edits->Revision();
            const auto prepared = Take(PrepareTerrainCornerRampEdit(*frozen, f.Document(), f.Binding(), rules,
                C::Brush(), std::span(&f.point, 1), follow));
            Require(prepared.Plan().TerrainPreview()
                && Take(SerializeMapTerrain(*prepared.Plan().TerrainPreview())) == f.TerrainLiteral(true)
                && f.edits->Revision() == revision && !f.edits->IsDirty(), "default sealed wet inner preview preserves original revision and full source bytes");
            Take(ApplyTerrainEdit(*f.edits, f.Document(), prepared)); f.CheckState(*f.edits, true, follow);
            Require(!ApplyTerrainEdit(*f.edits, f.Document(), prepared).Succeeded(), "wet inner candidate replay is revision-CAS protected");
            const auto stale = Prepare(f, false, follow);
            Require(f.edits->Undo(), "wet inner Undo reverses whole terrain and both placement streams"); f.CheckState(*f.edits, false, follow);
            Require(!ApplyTerrainEdit(*f.edits, f.Document(), stale).Succeeded(), "prepared wet inner removal cannot cross Undo revision");
            Require(f.edits->Redo(), "wet inner Redo restores same complete command"); f.CheckState(*f.edits, true, follow);
            const auto stableRevision = f.edits->Revision();
            Take(ApplyTerrainEdit(*f.edits, f.Document(), Prepare(f, true, follow)));
            Require(f.edits->Revision() == stableRevision, "validated wet inner no-op adds no fake history entry");
            journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
            wire = Take(EncodeTerrainStrokeJournal(journal));
            Require(!journal.history && wire[3] == std::byte{'J'} && journal.strokes.size() == 1
                && journal.strokes[0].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeWetCorners
                && journal.strokes[0].intent.followDoodads == follow
                && journal.strokes[0].rulesSha256 == Take(rules.WaterBrushDependencyFingerprint()),
                "default service records FTJJ algorithm4 with actual rules and explicit placement-follow intent");
            const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
            Require(Take(EncodeTerrainStrokeJournal(decoded)) == wire, "FTJJ canonical whole-wire roundtrip");
            for (const char older : std::string_view("123456789ABCDFG")) {
                auto disguised = wire; disguised[3] = std::byte(older);
                Require(!DecodeTerrainStrokeJournal(disguised).Succeeded(), "old typed versions cannot admit algorithm4");
            }
            for (const char history : std::string_view("EHI")) {
                auto disguised = wire; disguised[3] = std::byte(history);
                Require(!DecodeTerrainStrokeJournal(disguised).Succeeded(), "typed J body cannot masquerade as an existing history envelope");
            }
            for (std::size_t size = 0; size < wire.size(); ++size)
                Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "every truncated FTJJ rejects the complete command");
        }
        // 只携带原始草稿与typed记录，销毁热命令、prepared和规则owner后重新取实际来源。
        f.edits.reset();
        auto raw = f.FreshSession();
        Require(raw->SetTerrain(f.TerrainLiteral(true)).Succeeded(), "cold raw wet inner draft attaches complete final W3E");
        if (follow) {
            Require(raw->SetSerialized(MapDataKind::Units, Take(f.UnitsLiteral(true).Serialize())).Succeeded()
                && raw->SetSerialized(MapDataKind::Doodads, Take(f.DoodadsLiteral(true).Serialize())).Succeeded(),
                "cold raw draft includes complete followed placement payloads");
        }
        const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
        auto restored = Take(TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
            f.members[0].data, f.members[1].data, decoded, *raw, f.game));
        Require(restored.receipts.size() == 1 && restored.waterOwners.size() == 1
            && restored.session->MatchesTerrainJournalDraft(*raw), "cold wet inner replay reconstructs actual owner and one atomic command");
        f.CheckState(*restored.session, true, follow);
        Require(restored.session->Replay(restored.receipts[0], false).Succeeded(), "cold wet inner receipt Undo"); f.CheckState(*restored.session, false, follow);
        Require(restored.session->Replay(restored.receipts[0], true).Succeeded(), "cold wet inner receipt Redo"); f.CheckState(*restored.session, true, follow);
        auto changedRules = decoded; changedRules.strokes[0].rulesSha256.assign(64, 'f');
        Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
            f.members[1].data, changedRules, *raw, f.game).Succeeded(), "matching final bytes cannot substitute changed actual wet inner rule identity");
        TerrainReplayLimits tight; tight.maximumWork = 1;
        Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
            f.members[1].data, decoded, *raw, f.game, tight).Succeeded(), "wet inner cold recovery is cumulatively work bounded");
        Require(Take(Sha256File(f.source->SourcePath())) == sourceSha, "cold wet inner recovery never rewrites the source archive");

        auto mixed = decoded;
        for (const auto algorithm : {TerrainCornerRampAlgorithm::DryOnly, TerrainCornerRampAlgorithm::WetTerrain,
            TerrainCornerRampAlgorithm::NativeCorners}) {
            auto legacy = decoded; legacy.strokes[0].intent.cornerRampAlgorithm = algorithm;
            const auto oldWire = Take(EncodeTerrainStrokeJournal(legacy));
            const char expected = algorithm == TerrainCornerRampAlgorithm::DryOnly ? '4'
                : algorithm == TerrainCornerRampAlgorithm::WetTerrain ? '8' : 'F';
            Require(oldWire[3] == std::byte(expected) && Take(EncodeTerrainStrokeJournal(Take(DecodeTerrainStrokeJournal(oldWire)))) == oldWire,
                "explicit old corner algorithms preserve their original typed bytes");
            Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
                f.members[1].data, legacy, *raw, f.game).Succeeded(), "old algorithm cannot acquire wet inner capability from matching rules or final bytes");
            auto forgedJ = oldWire; forgedJ[3] = std::byte{'J'};
            Require(!DecodeTerrainStrokeJournal(forgedJ).Succeeded(), "FTJJ requires a real algorithm4 stroke");
            mixed.strokes.push_back(legacy.strokes[0]);
        }
        const auto mixedWire = Take(EncodeTerrainStrokeJournal(mixed));
        const auto mixedDecoded = Take(DecodeTerrainStrokeJournal(mixedWire));
        Require(mixedWire[3] == std::byte{'J'} && mixedDecoded.strokes.size() == 4
            && mixedDecoded.strokes[0].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeWetCorners
            && mixedDecoded.strokes[1].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::DryOnly
            && mixedDecoded.strokes[2].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::WetTerrain
            && mixedDecoded.strokes[3].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeCorners
            && !EqualTerrainJournalIntent(mixedDecoded.strokes[0].intent, mixedDecoded.strokes[3].intent)
            && Take(EncodeTerrainStrokeJournal(mixedDecoded)) == mixedWire, "J codec keeps each mixed 4/1/2/3 intent without upgrading the old commands");
        TerrainJournalStroke resize; resize.intent.resize = TerrainResizeIntent{1, {-32, -32, 64, 64}};
        resize.rulesSha256.assign(64, 'e'); mixed.strokes.push_back(std::move(resize));
        const auto resizedWire = Take(EncodeTerrainStrokeJournal(mixed));
        Require(resizedWire[3] == std::byte{'J'} && Take(EncodeTerrainStrokeJournal(Take(DecodeTerrainStrokeJournal(resizedWire)))) == resizedWire,
            "J typed version also retains prior resize variant without colliding with G or saved-history envelopes");
        auto invalid = decoded; invalid.strokes[0].intent.cornerRampAlgorithm = static_cast<TerrainCornerRampAlgorithm>(255);
        Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "unknown future corner algorithm cannot persist");
        invalid = decoded; invalid.strokes[0].intent.brush.kind = TerrainBrushKind::Raise;
        Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "non-corner command cannot smuggle algorithm4");
    }
}

inline void FormalSaveAsAndInPlaceRetainHistory() {
    for (const bool inPlace : {false, true}) for (const bool follow : {false, true})
        for (const bool mixed : {false, true}) for (const auto threshold : {W::Threshold::SurfaceEquality, W::Threshold::DeepEquality}) {
        Fixture f({threshold, mixed});
        const auto originalPath = f.source->SourcePath(); const auto originalSha = f.source->SourceSha256();
        const auto identity = f.edits->SessionIdentity();
        Take(ApplyTerrainEdit(*f.edits, f.Document(), Prepare(f, true, follow)));
        std::size_t saveIndex = 0;
        const auto epochLimited = [](const auto& result) {
            return !result.Succeeded() && !result.value && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                [](const auto& diagnostic) { return diagnostic.code == "TERRAIN_HISTORY_EPOCH_LIMIT"; });
        };
        const auto save = [&](bool created, const char* target) {
            f.CheckState(*f.edits, created, follow);
            const auto historyLease = f.edits->PrepareTerrainHistoryForSave();
            Require(saveIndex == 0 ? historyLease.Succeeded() && historyLease.value.has_value() : epochLimited(historyLease),
                "wet inner first save freezes actual history station; further epochs remain explicitly unavailable");
            const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.source, *f.edits, f.game));
            Require(plan.SaveGate().Succeeded(), "wet inner formal save compares actual full native G0/G1");
            MapSaveTask task; Require(task.FreezeArchive(*f.source).Succeeded(), "wet inner formal save freezes exact physical archive");
            task.inPlace = inPlace; task.target = inPlace ? f.source->SourcePath() : f.root / target;
            task.inPlaceOptions.backupDirectory = f.root / "wet-inner-backups"; task.inPlaceOptions.keepBackups = 0;
            task.replacements = f.edits->Replacements(); task.removals = f.edits->Removals();
            Require(plan.InstallPathingOutput(*f.edits, task.replacements).Succeeded(), "wet inner save installs complete native WPM");
            task.beforePublish = [&] { return plan.VerifyForSave(*f.edits, task.sourceSha256, task.replacements, task.removals); };
            const auto priorSha = f.source->SourceSha256(); MapSaveService::Run(task);
            if (!task.succeeded) {
                std::string error; for (const auto& diagnostic : task.diagnostics) error += diagnostic.code + ": " + diagnostic.message + "\n";
                throw std::runtime_error(error.empty() ? "wet inner formal save failed" : error);
            }
            Require(task.executed && task.nextSession, "wet inner save publishes and reopens actual candidate");
            if (inPlace) Require(!task.backup.empty() && Take(Sha256File(task.backup)) == priorSha, "wet inner in-place backup contains exact previous archive");
            auto actual = Take(MapArchiveSession::Open(task.written));
            Require(plan.VerifyReadback(actual).Succeeded()
                && Take(actual.ReadMember("war3map.w3e")) == f.TerrainLiteral(created)
                && Take(actual.ReadMember("war3map.wpm")) == f.PathingLiteral(created)
                && Take(actual.ReadMember("war3mapUnits.doo")) == Take(f.UnitsLiteral(created && follow).Serialize())
                && Take(actual.ReadMember("war3map.doo")) == Take(f.DoodadsLiteral(created && follow).Serialize()),
                "wet inner archive readback equals independent whole W3E/WPM and both S96/R64 placement streams");
            for (const auto& member : f.members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm"
                && member.name != "war3mapUnits.doo" && member.name != "war3map.doo")
                Require(Take(actual.ReadMember(member.name)) == member.data, "wet inner save preserves every unrelated authored member");
            const auto revision = f.edits->Revision(), sourceRevision = f.edits->TerrainSourceRevision();
            Require(plan.CommitSaved(*f.edits, MapDocument::FromArchive(actual)).Succeeded() && !f.edits->IsDirty()
                && f.edits->SessionIdentity() == identity && f.edits->Revision() == revision
                && f.edits->TerrainSourceRevision() == sourceRevision + 1, "wet inner commit preserves hot history identity and only advances physical source revision");
            if (saveIndex == 0) Require(f.edits->CommitTerrainHistorySave(*historyLease.value).Succeeded(), "wet inner first readback commits the real same-revision history station");
            f.source = std::make_unique<MapArchiveSession>(std::move(actual));
            const auto captured = TerrainPathingSave::CaptureJournal(*f.source, *f.edits);
            if (saveIndex == 0) {
                const auto saved = Take(captured); const auto savedWire = Take(EncodeTerrainStrokeJournal(saved));
                const auto decoded = Take(DecodeTerrainStrokeJournal(savedWire));
                const auto typedWire = Take(EncodeTerrainStrokeJournal(Take(TerrainHistoryBaselineJournal(decoded))));
                Require(saved.history && savedWire[3] == std::byte{'E'} && typedWire[3] == std::byte{'J'}
                    && decoded.history && decoded.history->currentIndex == 1 && decoded.history->savedIndex == 1
                    && decoded.strokes[0].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeWetCorners
                    && Take(EncodeTerrainStrokeJournal(decoded)) == savedWire, "saved E envelope retains its canonical J typed body and real clean station");
            } else Require(epochLimited(captured) && epochLimited(f.edits->ExportTerrainHistoryJournal()),
                "later saves retain hot Undo/Redo and cannot forge a second cold history epoch");
            ++saveIndex;
        };
        save(true, "wet-inner-created.w3x");
        Require(f.edits->Undo() && !f.edits->CanUndo(), "wet inner single Undo survives actual map save"); save(false, "wet-inner-undone.w3x");
        Require(f.edits->Redo() && !f.edits->CanRedo(), "wet inner single Redo survives saved Undo"); save(true, "wet-inner-redone.w3x");
        if (!inPlace) Require(Take(Sha256File(originalPath)) == originalSha, "wet inner SaveAs preserves entire original archive");
    }
}

inline void Check() { SealedCandidateAndTypedColdRestore(); FormalSaveAsAndInPlaceRetainHistory(); }
} // namespace War3Map::TerrainWetInnerCornerRampServiceCases
