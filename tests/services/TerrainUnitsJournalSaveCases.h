#pragma once

// 在TerrainOrdinaryJournalSaveCases.h后包含，复用实际special水刷与普通占地。
UnitPlacements UnitsJournalServicePlacements() {
    UnitPlacements units; UnitPlacement unit;
    unit.rawcode = "Hfoo"; unit.position = {512, 384, 37}; unit.scale = {1, 1.25f, 0.75f};
    unit.owner = 2; unit.creationId = 913; unit.hitPoints = 83; unit.mana = 57;
    unit.unknownOwnerBytes = {0x31, 0x7e}; unit.rotation = 0.625f;
    PlacementItemSet drops; drops.items = {{"gold", 70}, {"lmbr", 30}}; unit.itemSets.push_back(drops);
    unit.inventory = {{1, "gold"}}; unit.abilities = {{"AHhb", true, 2}};
    unit.random = RandomCustomTable{{{"hfoo", 60}, {"hrif", 40}}}; units.units.push_back(unit);
    return units;
}

void UnitsJournalFixture(Fixture& f) {
    OrdinarySpecialWaterFixture(f);
    f.Member("war3mapUnits.doo") = Take(UnitsJournalServicePlacements().Serialize()); f.Open();
}

std::unique_ptr<MapDataEditSession> UnitsJournalRawDraft(Fixture& f) {
    auto raw = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(f.members));
    Check(raw->AttachTerrain(f.Member("war3map.w3e"))); Check(raw->AttachPathing(f.Member("war3map.wpm")));
    const auto replacements = f.edits->Replacements();
    const auto wpm = std::find_if(replacements.begin(), replacements.end(), [](const auto& member) { return member.name == "war3map.wpm"; });
    const auto& pathing = wpm == replacements.end() ? f.Member("war3map.wpm") : wpm->data;
    const auto terrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto layers = Take(f.edits->SerializePathingRestrictionDraft());
    if (layers.empty()) Check(raw->SetTerrainAndPathing(terrain, pathing));
    else Check(raw->SetTerrainAndPathingDraft(terrain, pathing, layers));
    Check(raw->SetSerialized(MapDataKind::Doodads, Take(f.edits->Snapshot().Doodads()->Serialize())));
    Check(raw->SetSerialized(MapDataKind::Units, Take(f.edits->Snapshot().Units()->Serialize())));
    Require(raw->MatchesTerrainJournalDraft(*f.edits), "raw cold draft matches all Units/DOO/terrain/APR bytes without private provenance");
    return raw;
}

void UnitsJournalRecoveryRetainsWaterBothOrdersAndCompleteRaw() {
    for (const bool unitsFirst : {false, true}) {
        Fixture f; UnitsJournalFixture(f);
        const auto sourceSha = Take(Sha256File(f.archive->SourcePath()));
        const auto sourceUnits = f.Member("war3mapUnits.doo");
        auto moved = UnitsJournalServicePlacements(); moved.units.front().position[0] += 128;
        moved.units.front().hitPoints = 1; moved.units.front().abilities.push_back({"AHds", false, 3});
        const auto expectedUnits = Take(moved.Serialize());
        if (unitsFirst) Check(f.edits->EditUnits(moved));
        PaintBridgeSpecialWater(f);
        if (!unitsFirst) Check(f.edits->EditUnits(moved));
        const auto expectedDoodads = Take(OrdinaryCurrent(f).Serialize());
        const auto expectedTerrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
        auto raw = UnitsJournalRawDraft(f);
        const auto unsafe = Take(TerrainPathingSave::PrepareForSave(*f.archive, *raw, f.gameRoot));
        Require(!unsafe.SaveGate().Succeeded() && HasCode(unsafe.SaveGate(), "TERRAIN_SPECIAL_PROVENANCE_REQUIRED"),
            "adding Units bytes cannot mint special deletion authority");
        const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits));
        const auto wire = Take(EncodeTerrainStrokeJournal(journal)); const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
        Require(wire[3] == std::byte{'9'} && decoded.strokes.size() == 2
            && decoded.strokes[unitsFirst ? 0 : 1].units && decoded.strokes[unitsFirst ? 1 : 0].intent.waterBrush,
            "FTJ9 retains the actual two operation orders and both full Unit members");
        auto restored = Take(TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
            f.Member("war3map.w3e"), f.Member("war3map.wpm"), decoded, *raw, f.gameRoot));
        Require(restored.receipts.size() == 2 && restored.waterOwners.size() == 1
            && restored.session->MatchesTerrainJournalDraft(*raw), "owned replay restores complete draft and two genuine receipts");
        const auto scope = restored.session->TerrainJournalStateFingerprint();
        for (auto i = restored.receipts.rbegin(); i != restored.receipts.rend(); ++i) Check(restored.session->Replay(*i, false));
        Require(!restored.session->IsDirty() && Take(restored.session->Snapshot().Units()->Serialize()) == sourceUnits,
            "cold receipts undo Units and special water back to untouched source");
        for (const auto& receipt : restored.receipts) Check(restored.session->Replay(receipt, true));
        Require(restored.session->MatchesTerrainJournalDraft(*raw) && restored.session->TerrainJournalStateFingerprint() == scope,
            "cold redo returns complete raw/APR and exact actual command scope");
        const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.archive, *restored.session, f.gameRoot)); Check(plan.SaveGate());
        auto output = restored.session->Replacements(); Check(plan.InstallPathingOutput(*restored.session, output));
        Check(plan.VerifyForSave(*restored.session, f.archive->SourceSha256(), output));
        Services::MapSaveTask task; Check(task.FreezeArchive(*f.archive));
        task.target = f.root / "units-water-restored.w3x"; task.replacements = std::move(output);
        task.beforePublish = [&] { return plan.VerifyForSave(*restored.session, task.sourceSha256, task.replacements, task.removals); };
        Services::MapSaveService::Run(task);
        Require(task.executed && task.succeeded && task.nextSession, "normal formal SaveAs worker saves recovered Units/water state");
        auto saved = Take(MapArchiveSession::Open(task.written)); Check(plan.VerifyReadback(saved));
        Require(Take(saved.ReadMember("war3map.w3e")) == expectedTerrain
            && Take(saved.ReadMember("war3map.wpm")) == OrdinarySpecialExpectedPathing(f, true, false)
            && Take(saved.ReadMember("war3map.doo")) == expectedDoodads
            && Take(saved.ReadMember("war3mapUnits.doo")) == expectedUnits, "independent full expected members match saved archive");
        for (const auto& member : f.members)
            if (member.name != "war3map.w3e" && member.name != "war3map.wpm" && member.name != "war3map.doo" && member.name != "war3mapUnits.doo")
                Require(Take(saved.ReadMember(member.name)) == member.data, "every unrelated original member is preserved");
        Check(plan.CommitSaved(*restored.session, MapDocument::FromArchive(saved)));
        Require(!restored.session->IsDirty() && restored.session->Undo() && restored.session->Redo(),
            "formal rebase keeps restored real history usable");
        Require(Take(Sha256File(f.archive->SourcePath())) == sourceSha, "all recovery and SaveAs work leaves source read-only");
    }
}

void UnitsJournalInterleavingRejectsSourceOrderBudgetAndFinalDrift() {
    Fixture f; UnitsJournalFixture(f);
    auto units = UnitsJournalServicePlacements(); units.units[0].position[0] += 128; Check(f.edits->EditUnits(units));
    MoveOrdinarySpecialInstance(f); PaintBridgeSpecialWater(f);
    units.units[0].inventory.push_back({4, "lmbr"}); units.units[0].position[1] += 128; Check(f.edits->EditUnits(units));
    auto raw = UnitsJournalRawDraft(f);
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits));
    Require(journal.strokes.size() == 4 && journal.strokes[0].units && journal.strokes[1].ordinaryDoodads
        && journal.strokes[2].intent.waterBrush && journal.strokes[3].units, "Units/DOO/water/Units retain one complete ordered journal");
    const auto restore = [&](const TerrainStrokeJournal& value, const Services::TerrainReplayLimits& limits = Services::TerrainReplayLimits{}) {
        return TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
            f.Member("war3map.w3e"), f.Member("war3map.wpm"), value, *raw, f.gameRoot, limits);
    };
    auto valid = Take(restore(journal)); Require(valid.receipts.size() == 4 && valid.session->MatchesTerrainJournalDraft(*raw),
        "all four interleaved commands restore real receipts and exact final raw state");
    auto changed = journal; std::swap(changed.strokes[0], changed.strokes[3]);
    const auto wrongOrder = restore(changed);
    Require(!wrongOrder.Succeeded() && !wrongOrder.value && HasCode(wrongOrder, "TERRAIN_JOURNAL_UNITS_STALE"),
        "later Units full-before identity cannot be replayed at an earlier state");
    changed = journal; changed.strokes[0].units->beforeSha256[0] = 'z';
    Require(!restore(changed).Succeeded(), "noncanonical Units SHA fails before recovery");
    changed = journal; changed.members[4].sha256[0] = changed.members[4].sha256[0] == 'a' ? 'b' : 'a';
    Require(!restore(changed).Succeeded(), "complete source Units member binding remains mandatory");
    Services::TerrainReplayLimits low; low.maximumCandidateBytes = 1;
    Require(!restore(journal, low).Succeeded(), "tight remaining budget publishes no prefix");
    units.units[0].mana = 9; Check(raw->EditUnits(units));
    const auto outputMismatch = restore(journal);
    Require(!outputMismatch.Succeeded() && !outputMismatch.value && HasCode(outputMismatch, "TERRAIN_JOURNAL_OUTPUT_MISMATCH"),
        "owned output must equal every final raw Units byte, not just terrain and DOO");
    Require(f.edits->Revision() == 4, "all refused private candidates leave real active history untouched");
}
