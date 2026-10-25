#pragma once

// 在TerrainOrdinaryJournalSaveCases.h后包含。复用原创特殊水刷、普通非对称占地、
// 正式MapSaveService与独立字面WPM，不用raw草稿充当保存站的资格证明。
std::unique_ptr<MapDataEditSession> HistoryRawDraft(Fixture& f) {
    const auto source = MapDocument::FromArchive(*f.archive);
    auto raw = std::make_unique<MapDataEditSession>(source);
    const auto terrainSource = Take(f.archive->ReadMember("war3map.w3e"));
    const auto pathingSource = Take(f.archive->ReadMember("war3map.wpm"));
    Check(raw->AttachTerrain(terrainSource)); Check(raw->AttachPathing(pathingSource));
    const auto currentTerrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto replacements = f.edits->Replacements();
    const auto pathing = std::find_if(replacements.begin(), replacements.end(), [](const auto& member) { return member.name == "war3map.wpm"; });
    const auto layers = Take(f.edits->SerializePathingRestrictionDraft());
    const auto& currentPathing = pathing == replacements.end() ? pathingSource : pathing->data;
    if (layers.empty()) Check(raw->SetTerrainAndPathing(currentTerrain, currentPathing));
    else Check(raw->SetTerrainAndPathingDraft(currentTerrain, currentPathing, layers));
    const auto current = f.edits->Snapshot();
    for (const auto& member : current.Members()) {
        const auto& original = source.Member(member.kind);
        if (member.sources.size() == 1 && (original.sources.empty() || member.sources.front().data != original.sources.front().data))
            Check(raw->SetSerialized(member.kind, member.sources.front().data));
        else if (member.state == MapDataState::Missing && original.state != MapDataState::Missing)
            Check(raw->Remove(member.kind));
    }
    Require(raw->MatchesTerrainJournalDraft(*f.edits), "raw cold draft preserves the physical baseline and all current member/P layers");
    return raw;
}

void HistorySaveStation(Fixture& f, bool moveFirst = true) {
    OrdinarySpecialWaterFixture(f);
    if (moveFirst) MoveOrdinarySpecialInstance(f);
    PaintBridgeSpecialWater(f);
    if (!moveFirst) MoveOrdinarySpecialInstance(f);
    const auto terrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto doodads = Take(OrdinaryCurrent(f).Serialize());
    const auto lease = Take(f.edits->PrepareTerrainHistoryForSave());
    f.SaveExpected("history-physical-station.w3x", terrain, OrdinarySpecialExpectedPathing(f, true, true), &doodads, true);
    Check(f.edits->CommitTerrainHistorySave(lease));
    Require(!f.edits->IsDirty(), "real map-save readback creates the physical clean station");
}

void TerrainHistorySavedStationRestoresBothSidesAndFormalSaving() {
    for (const bool moveFirst : {false, true}) for (const std::size_t current : {0u, 1u, 2u}) {
        Fixture f; HistorySaveStation(f, moveFirst);
        const auto physicalPath = f.archive->SourcePath();
        const auto physicalSha = Take(Sha256File(physicalPath));
        const auto physical = MapDocument::FromArchive(*f.archive);
        const auto savedTerrain = Take(f.archive->ReadMember("war3map.w3e"));
        const auto savedPathing = Take(f.archive->ReadMember("war3map.wpm"));
        for (std::size_t i = 2; i > current; --i) Require(f.edits->Undo(), "hot Undo crosses the real physical save station");
        const auto expectedTerrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
        const auto expectedDoodads = Take(OrdinaryCurrent(f).Serialize());
        const auto expectedPathing = OrdinarySpecialExpectedPathing(f, current == 2 || (current == 1 && !moveFirst),
            current == 2 || (current == 1 && moveFirst));
        auto raw = HistoryRawDraft(f);
        const auto encoded = Take(EncodeTerrainStrokeJournal(Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits))));
        const auto journal = Take(DecodeTerrainStrokeJournal(encoded));
        Require(encoded[3] == std::byte{'E'} && journal.history && journal.history->savedIndex == 2
            && journal.history->currentIndex == current && journal.strokes.size() == 2,
            "cold wire keeps complete linear typed history and both station indices");
        auto restored = Take(TerrainPathingSave::RestoreJournal(*f.archive, physical, savedTerrain, savedPathing,
            journal, *raw, f.gameRoot));
        Require(restored.currentIndex == current && restored.savedIndex == 2 && restored.receipts.size() == 2
            && restored.session->MatchesTerrainJournalDraft(*raw), "owned cold restore retains full Undo/Redo while matching the entire raw endpoint");
        for (std::size_t i = current; i < 2; ++i) Check(restored.session->Replay(restored.receipts[i], true));
        Require(!restored.session->IsDirty() && Take(SerializeMapTerrain(*restored.session->Terrain())) == savedTerrain,
            "actual receipts redo to the exact physical clean station");
        for (std::size_t i = 2; i > current; --i) Check(restored.session->Replay(restored.receipts[i - 1], false));
        Require(restored.session->MatchesTerrainJournalDraft(*raw), "actual receipts undo back across save while preserving P and special lineage");
        f.edits = std::move(restored.session);
        const auto recaptured = Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits));
        Require(recaptured.history && recaptured.history->currentIndex == current && recaptured.history->savedIndex == 2,
            "recovered real command history can be saved to another project without reusing raw authority");
        f.SaveExpected("history-cold-normal-save.w3x", expectedTerrain, expectedPathing, &expectedDoodads, true, current != 2);
        Require(Take(Sha256File(physicalPath)) == physicalSha, "cold recovery and formal new save keep the physical source archive immutable");
    }
}

void TerrainHistorySavedStationRejectsEpochSourceAndRawTampering() {
    Fixture f; HistorySaveStation(f); Require(f.edits->Undo(), "make a before-station cold draft");
    const auto activeTerrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto activeDoodads = Take(OrdinaryCurrent(f).Serialize());
    const auto activeRevision = f.edits->Revision();
    const auto activeScope = f.edits->TerrainJournalStateFingerprint();
    auto raw = HistoryRawDraft(f);
    const auto source = MapDocument::FromArchive(*f.archive);
    const auto terrain = Take(f.archive->ReadMember("war3map.w3e")), pathing = Take(f.archive->ReadMember("war3map.wpm"));
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits));
    const auto restore = [&](const TerrainStrokeJournal& value, const Services::TerrainReplayLimits& limits = Services::TerrainReplayLimits{}) {
        return TerrainPathingSave::RestoreJournal(*f.archive, source, terrain, pathing, value, *raw, f.gameRoot, limits);
    };
    auto changed = journal; changed.sourceSha256[0] = changed.sourceSha256[0] == '0' ? '1' : '0';
    Require(HasCode(restore(changed), "TERRAIN_HISTORY_SOURCE_MISMATCH"), "history never substitutes a historical structure digest for physical archive identity");
    changed = journal; changed.history->savedIndex = 1;
    const auto wrongStation = restore(changed);
    Require(!wrongStation.Succeeded() && !wrongStation.value && HasCode(wrongStation, "TERRAIN_HISTORY_STATION_MISMATCH"),
        "validly shaped wrong saved cursor cannot bypass complete physical station comparison");
    changed = journal; changed.history->baselineMembers[0].bytes.back() ^= std::byte{1};
    Require(!restore(changed).Succeeded(), "modified historical W3E must re-run typed commands and cannot gain raw save authority");
    changed = journal; changed.history->baselineMembers[6].present = true;
    Require(!restore(changed).Succeeded(), "historical absence is part of the saved station proof, including otherwise unrelated regions");
    changed = journal;
    if (changed.history->baselinePathingLayers.empty()) changed.history->baselinePathingLayers.push_back(std::byte{1});
    else changed.history->baselinePathingLayers.back() ^= std::byte{1};
    Require(!restore(changed).Succeeded(), "historical P/savedS layer substitution fails before installing any candidate");
    Services::TerrainReplayLimits small; small.maximumCandidateBytes = 1;
    const auto bounded = restore(journal, small);
    Require(!bounded.Succeeded() && !bounded.value && HasCode(bounded, "TERRAIN_REPLAY_BUDGET"),
        "history structural validation obeys a tightened allocation budget and never returns a partial prefix");
    auto altered = *raw->Snapshot().Doodads(); altered.doodads.front().position[0] += 128;
    Check(raw->EditDoodads(altered));
    Require(HasCode(restore(journal), "TERRAIN_JOURNAL_OUTPUT_MISMATCH"), "complete raw endpoint remains an independent final comparison");
    raw = HistoryRawDraft(f);
    Require(std::filesystem::remove(f.gameRoot / "war3.mpq"), "replace only the owned synthetic game source");
    const std::array<MapArchiveMember, 1> changedRules{{{"UI/WorldEditStrings.txt", Text("[Fixture]\nSource=changed\n")}}};
    Take(WriteVerifiedArchive(f.gameRoot / "war3.mpq", changedRules));
    const auto resourceDrift = restore(journal);
    Require(!resourceDrift.Succeeded() && !resourceDrift.value && HasCode(resourceDrift, "TERRAIN_HISTORY_REPLAY_FAILED")
        && HasCode(resourceDrift, "TERRAIN_JOURNAL_RESOURCE_DRIFT"),
        "historical structure overlay cannot hide changed real winning game resources");
    Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == activeTerrain
        && Take(OrdinaryCurrent(f).Serialize()) == activeDoodads && f.edits->Revision() == activeRevision
        && f.edits->TerrainJournalStateFingerprint() == activeScope,
        "rejected private factories do not mutate the active author session");
}

void TerrainHistoryDirectoryRestoresPostStationSuffixAndRedo() {
    Fixture f; HistorySaveStation(f);
    auto moved = OrdinaryCurrent(f); moved.doodads.front().position[0] = 768; Check(f.edits->EditDoodads(moved));
    const auto third = f.edits->LastEdit(); Check(f.edits->Replay(third, false));
    const auto directoryRoot = f.root / "history-directory";
    Require(std::filesystem::create_directory(directoryRoot), "create owned physical saved-directory source");
    for (const auto& member : f.archive->Members()) {
        const auto path = directoryRoot / member.name; std::filesystem::create_directories(path.parent_path());
        Check(IO::WriteFileBytes(path, member.data));
    }
    const auto workspace = Take(DirectoryWorkspace::Open(directoryRoot));
    const auto original = Take(MapDocument::FromDirectory(workspace));
    const auto terrain = Take(f.archive->ReadMember("war3map.w3e")), pathing = Take(f.archive->ReadMember("war3map.wpm"));
    auto raw = HistoryRawDraft(f);
    const auto journal = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(
        Take(TerrainPathingSave::CaptureJournal(workspace.Snapshot(), *f.edits))))));
    Require(journal.strokes.size() == 3 && journal.history->currentIndex == 2 && journal.history->savedIndex == 2,
        "post-save third command and its undone Redo remain in the complete linear history");
    auto restored = Take(TerrainPathingSave::RestoreJournal(workspace.Snapshot(), original, terrain, pathing,
        journal, *raw, f.gameRoot));
    Require(restored.receipts.size() == 3 && restored.currentIndex == 2 && restored.savedIndex == 2
        && !restored.session->IsDirty(), "directory recovery proves the same real station and installs the clean cursor before Redo");
    Check(restored.session->Replay(restored.receipts[2], true));
    Require(restored.session->Snapshot().Doodads()->doodads.front().position[0] == 768 && restored.session->IsDirty(),
        "post-station Redo uses its actual receipt against the new physical source");
    Check(restored.session->Replay(restored.receipts[2], false));
    Require(restored.session->MatchesTerrainJournalDraft(*raw), "post-station Undo restores full station members and all pathing layers");
    auto changed = Take(IO::ReadFileBytes(directoryRoot / "war3map.w3e")); changed.back() ^= std::byte{1};
    Check(IO::WriteFileBytes(directoryRoot / "war3map.w3e", changed));
    const auto refused = TerrainPathingSave::RestoreJournal(workspace.Snapshot(), original, terrain, pathing,
        journal, *raw, f.gameRoot);
    Require(!refused.Succeeded() && !refused.value && HasCode(refused, "TERRAIN_HISTORY_REPLAY_FAILED"),
        "a stale frozen directory reader cannot restore any prefix after physical source changes");
}
