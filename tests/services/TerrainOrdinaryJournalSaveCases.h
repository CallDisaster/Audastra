#pragma once

// 在TerrainOrdinarySpecialSaveCases.h后包含，使用同一原创特殊水刷与非对称普通占地。
std::unique_ptr<MapDataEditSession> OrdinaryJournalRawDraft(Fixture& f) {
    auto raw = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(f.members));
    Check(raw->AttachTerrain(f.Member("war3map.w3e"))); Check(raw->AttachPathing(f.Member("war3map.wpm")));
    const auto terrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto replacements = f.edits->Replacements();
    const auto changedPathing = std::find_if(replacements.begin(), replacements.end(), [](const auto& item) { return item.name == "war3map.wpm"; });
    const auto& pathing = changedPathing == replacements.end() ? f.Member("war3map.wpm") : changedPathing->data;
    const auto layers = Take(f.edits->SerializePathingRestrictionDraft());
    if (layers.empty()) Check(raw->SetTerrainAndPathing(terrain, pathing));
    else Check(raw->SetTerrainAndPathingDraft(terrain, pathing, layers));
    Check(raw->SetSerialized(MapDataKind::Doodads, Take(OrdinaryCurrent(f).Serialize())));
    Require(raw->MatchesTerrainJournalDraft(*f.edits), "independent raw cold draft preserves every byte but not special authority");
    return raw;
}

void OrdinaryJournalRecoveryPreservesBothOperationOrders() {
    for (const bool moveFirst : {false, true}) {
        Fixture f; OrdinarySpecialWaterFixture(f);
        const auto sourceSha = f.archive->SourceSha256(); const auto sourcePath = f.archive->SourcePath();
        if (moveFirst) MoveOrdinarySpecialInstance(f);
        PaintBridgeSpecialWater(f);
        if (!moveFirst) MoveOrdinarySpecialInstance(f);
        const auto finalTerrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
        const auto finalDoodads = Take(OrdinaryCurrent(f).Serialize());
        auto raw = OrdinaryJournalRawDraft(f);
        const auto refused = Take(TerrainPathingSave::PrepareForSave(*f.archive, *raw, f.gameRoot));
        Require(!refused.SaveGate().Succeeded() && HasCode(refused.SaveGate(), "TERRAIN_SPECIAL_PROVENANCE_REQUIRED"),
            "raw deleted special tail cannot borrow the real hot session's water provenance");
        const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits));
        const auto wire = Take(EncodeTerrainStrokeJournal(journal)); const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
        Require(wire[3] == std::byte{'7'} && decoded.strokes.size() == 2
            && decoded.strokes[moveFirst ? 0 : 1].ordinaryDoodads && decoded.strokes[moveFirst ? 1 : 0].intent.waterBrush,
            "FTJ7 retains both real command orders and full water-before DOO fingerprints");
        auto recovered = Take(TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
            f.Member("war3map.w3e"), f.Member("war3map.wpm"), decoded, *raw, f.gameRoot));
        Require(recovered.receipts.size() == 2 && recovered.waterOwners.size() == 1
            && recovered.session->MatchesTerrainJournalDraft(*raw), "owned factory restores the complete raw draft and both genuine receipts");
        const auto scope = recovered.session->TerrainJournalStateFingerprint();
        for (auto i = recovered.receipts.rbegin(); i != recovered.receipts.rend(); ++i) Check(recovered.session->Replay(*i, false));
        Require(!recovered.session->IsDirty(), "cold receipts reverse to the exact untouched source");
        for (const auto& receipt : recovered.receipts) Check(recovered.session->Replay(receipt, true));
        Require(recovered.session->MatchesTerrainJournalDraft(*raw) && recovered.session->TerrainJournalStateFingerprint() == scope,
            "cold Qt-equivalent redo restores all bytes and exact active command scope");
        f.edits = std::move(recovered.session);
        f.SaveExpected("ordinary-journal-final.w3x", finalTerrain, OrdinarySpecialExpectedPathing(f, true, true), &finalDoodads, true);
        Require(Take(f.edits->ExportTerrainStrokeJournal()).strokes.empty(), "verified native save rebases the ordinary journal boundary");
        Require(f.edits->Undo() && !f.edits->ExportTerrainStrokeJournal().Succeeded(),
            "saved receipt remains undoable without inventing a new source-forward prefix across the boundary");
        const auto intermediate = Take(OrdinaryCurrent(f).Serialize());
        f.SaveExpected("ordinary-journal-undone.w3x", moveFirst ? f.Member("war3map.w3e") : finalTerrain,
            OrdinarySpecialExpectedPathing(f, !moveFirst, moveFirst), &intermediate, true);
        Require(f.edits->Redo(), "formal save retains cold-restored redo history");
        f.SaveExpected("ordinary-journal-redone.w3x", finalTerrain, OrdinarySpecialExpectedPathing(f, true, true), &finalDoodads, true);
        Require(Take(Sha256File(sourcePath)) == sourceSha, "mixed cold recovery and formal saves preserve original source bytes");
    }
}

void OrdinaryJournalRecoveryRejectsTamperingAndPublicPlaceholders() {
    Fixture f; OrdinarySpecialWaterFixture(f); MoveOrdinarySpecialInstance(f); PaintBridgeSpecialWater(f);
    const auto original = MapDocument::FromMembers(f.members);
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits));
    auto raw = OrdinaryJournalRawDraft(f);
    const auto restore = [&](const TerrainStrokeJournal& value, const Services::TerrainReplayLimits& limits = Services::TerrainReplayLimits{}) {
        return TerrainPathingSave::RestoreJournal(*f.archive, original, f.Member("war3map.w3e"), f.Member("war3map.wpm"),
            value, *raw, f.gameRoot, limits);
    };
    auto changed = journal; auto& sha = changed.strokes.front().ordinaryDoodads->beforeSha256;
    sha[0] = sha[0] == '0' ? '1' : '0';
    const auto stale = restore(changed);
    Require(!stale.Succeeded() && !stale.value && HasCode(stale, "TERRAIN_JOURNAL_DOODADS_STALE"), "correct-length wrong before SHA never installs a prefix");
    changed = journal; changed.strokes.front().ordinaryDoodads->removedBytes = std::numeric_limits<std::uint32_t>::max();
    Require(!restore(changed).Succeeded(), "overflowing delta is rejected before source parsing or allocation");
    changed = journal; changed.strokes.front().intent.points = {{256, 256}};
    Require(!restore(changed).Succeeded(), "ordinary record cannot take a typed sampling payload");
    changed = journal; std::swap(changed.strokes[0], changed.strokes[1]);
    Require(!restore(changed).Succeeded(), "reordering real records cannot reproduce another step's full DOO dependency fingerprint");
    Services::TerrainReplayLimits small; small.maximumWork = 1;
    const auto limited = restore(journal, small);
    Require(!limited.Succeeded() && !limited.value, "small total replay budget returns no partial ordinary or water state");
    const TerrainStrokeIntent placeholder;
    const auto publicReplay = TerrainPathingSave::ReplayStrokes(*f.archive, original, f.Member("war3map.w3e"), f.Member("war3map.wpm"),
        std::span(&placeholder, 1), f.gameRoot);
    Require(!publicReplay.Succeeded() && !publicReplay.value && HasCode(publicReplay, "TERRAIN_PATCH_POINT_BUDGET"),
        "public replay cannot select the journal-only ordinary branch with a neutral empty intent");
    auto changedDoodads = *raw->Snapshot().Doodads(); changedDoodads.doodads.front().position[0] += 128;
    Check(raw->EditDoodads(changedDoodads));
    const auto outputMismatch = restore(journal);
    Require(!outputMismatch.Succeeded() && !outputMismatch.value && HasCode(outputMismatch, "TERRAIN_JOURNAL_OUTPUT_MISMATCH"),
        "replay output is compared against every final raw DOO byte before installing the owned session");
    Require(f.edits->Revision() == 2 && OrdinaryCurrent(f).specialDoodads.size() == 2,
        "all rejected private candidates preserve the real active source session and its trusted lineage");
}

void OrdinaryJournalRecoveryRetainsRepeatedInterleaving() {
    Fixture f; OrdinarySpecialWaterFixture(f);
    MoveOrdinarySpecialInstance(f); PaintBridgeSpecialWater(f);
    auto secondMove = OrdinaryCurrent(f); secondMove.doodads.front().position[0] = 768;
    Check(f.edits->EditDoodads(secondMove));
    const auto rules = BridgeSpecialWaterRules(f);
    const Services::MapDataQueryBinding document{"ordinary-journal-interleave", 1, 0, f.archive->SourceSha256()};
    const Services::TerrainEditBinding binding{document, f.edits->SessionIdentity(), f.edits->Revision(), f.edits->TerrainSourceRevision()};
    TerrainWaterStrokeOptions deep; deep.kind = TerrainWaterBrushKind::Deep;
    deep.shape = TerrainBrushShape::Square; deep.randomSeed = 1;
    const TerrainStrokePoint point{256, 256};
    const auto prepared = Take(Services::PrepareTerrainWaterEdit(*f.edits, document, binding, rules,
        deep, std::span(&point, 1), false));
    Take(Services::ApplyTerrainEdit(*f.edits, document, prepared));
    auto raw = OrdinaryJournalRawDraft(f);
    const auto journal = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(
        Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits))))));
    Require(journal.strokes.size() == 4 && journal.strokes[0].ordinaryDoodads && journal.strokes[1].intent.waterBrush
        && journal.strokes[2].ordinaryDoodads && journal.strokes[3].intent.waterBrush
        && journal.strokes[1].rulesSha256 != journal.strokes[3].rulesSha256,
        "multiple ordinary/water pairs retain their distinct complete current-DOO dependencies");
    auto recovered = Take(TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
        f.Member("war3map.w3e"), f.Member("war3map.wpm"), journal, *raw, f.gameRoot));
    Require(recovered.receipts.size() == 4 && recovered.waterOwners.size() == 2
        && recovered.session->MatchesTerrainJournalDraft(*raw), "four genuine commands rebuild the complete interleaved draft");
    const auto finalScope = recovered.session->TerrainJournalStateFingerprint();
    for (auto i = recovered.receipts.rbegin(); i != recovered.receipts.rend(); ++i) Check(recovered.session->Replay(*i, false));
    Require(!recovered.session->IsDirty(), "all four restored commands reverse to the original source");
    for (const auto& receipt : recovered.receipts) Check(recovered.session->Replay(receipt, true));
    Require(recovered.session->TerrainJournalStateFingerprint() == finalScope
        && recovered.session->MatchesTerrainJournalDraft(*raw), "four-command redo preserves both full state and exact mixed scope");
}
