#pragma once

// 在TerrainUnitsJournalSaveCases.h后包含，实际来源、规则和special占地均沿用原创夹具。
void StartLocationsJournalRequiresCompletePrivateReplayAndFinalRaw() {
    for (const bool startFirst : {false, true}) {
        Fixture f; UnitsJournalFixture(f);
        auto units = UnitsJournalServicePlacements(); units.units[0].rawcode = "sloc";
        f.Member("war3mapUnits.doo") = Take(units.Serialize());
        auto settings = Take(MapSettings::Parse(f.Member("war3map.w3i")));
        auto player = std::find_if(settings.players.begin(), settings.players.end(), [](const auto& value) { return value.number == 2; });
        if (player == settings.players.end()) {
            MapPlayerSettings added; added.number = 2; added.controller = 1; settings.players.push_back(added); player = settings.players.end() - 1;
        }
        player->startX = units.units[0].position[0]; player->startY = units.units[0].position[1];
        f.Member("war3map.w3i") = Take(settings.Serialize()); f.Open();
        const auto sourceSha = Take(Sha256File(f.archive->SourcePath()));
        units.units[0].position[0] += 128;
        if (startFirst) Check(f.edits->EditUnits(units));
        PaintBridgeSpecialWater(f);
        if (!startFirst) Check(f.edits->EditUnits(units));
        auto raw = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(f.members));
        Check(raw->AttachTerrain(f.Member("war3map.w3e"))); Check(raw->AttachPathing(f.Member("war3map.wpm")));
        const auto replacements = f.edits->Replacements();
        const auto wpm = std::find_if(replacements.begin(), replacements.end(), [](const auto& member) { return member.name == "war3map.wpm"; });
        const auto& pathing = wpm == replacements.end() ? f.Member("war3map.wpm") : wpm->data;
        const auto terrain = Take(SerializeMapTerrain(*f.edits->Terrain())), layers = Take(f.edits->SerializePathingRestrictionDraft());
        if (layers.empty()) Check(raw->SetTerrainAndPathing(terrain, pathing));
        else Check(raw->SetTerrainAndPathingDraft(terrain, pathing, layers));
        const auto final = f.edits->Snapshot();
        Check(raw->SetSerialized(MapDataKind::Doodads, Take(final.Doodads()->Serialize())));
        Check(raw->SetSerialized(MapDataKind::Units, Take(final.Units()->Serialize())));
        Check(raw->SetSerialized(MapDataKind::Settings, Take(final.Settings()->Serialize())));
        Require(raw->MatchesTerrainJournalDraft(*f.edits) && !raw->VerifyTerrainSettingsForSave().Succeeded(),
            "same final raw Units/W3I cannot manufacture actual start linkage authority");
        const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits));
        Require(journal.strokes.size() == 2 && journal.strokes[startFirst ? 0 : 1].startLocations
            && Take(EncodeTerrainStrokeJournal(journal))[3] == std::byte{'A'}, "capture exact water/start order through formal owned journal");
        const auto restore = [&](const TerrainStrokeJournal& input, const Services::TerrainReplayLimits& limits = Services::TerrainReplayLimits{}) {
            return TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
                f.Member("war3map.w3e"), f.Member("war3map.wpm"), input, *raw, f.gameRoot, limits);
        };
        auto recovered = Take(restore(journal));
        Require(recovered.receipts.size() == 2 && recovered.session->MatchesTerrainJournalDraft(*raw)
            && recovered.session->VerifyTerrainSettingsForSave().Succeeded(), "owned replay restores both complete members and genuine receipts");
        const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.archive, *recovered.session, f.gameRoot)); Check(plan.SaveGate());
        auto wrong = journal;
        auto& settingsDelta = wrong.strokes[startFirst ? 0 : 1].startLocations->settings;
        settingsDelta.beforeSha256[0] = settingsDelta.beforeSha256[0] == 'a' ? 'b' : 'a';
        Require(!restore(wrong).Succeeded(), "full W3I canonical source SHA cannot drift");
        wrong = journal; wrong.members[4].sha256[0] = wrong.members[4].sha256[0] == 'a' ? 'b' : 'a';
        Require(!restore(wrong).Succeeded(), "source Units binding cannot be replaced by a valid final output");
        Services::TerrainReplayLimits tiny; tiny.maximumCandidateBytes = 1;
        Require(!restore(journal, tiny).Succeeded(), "aggregate allocation failure installs no prefix");
        auto forged = *raw->Snapshot().Settings(); forged.players[0].controller = forged.players[0].controller == 1 ? 2 : 1;
        Check(raw->SetSerialized(MapDataKind::Settings, Take(forged.Serialize())));
        const auto mismatch = restore(journal);
        Require(!mismatch.Succeeded() && !mismatch.value && HasCode(mismatch, "TERRAIN_JOURNAL_OUTPUT_MISMATCH"),
            "final raw settings outside recorded start and four descriptions remain rejected");
        Require(f.edits->Revision() == 2 && Take(Sha256File(f.archive->SourcePath())) == sourceSha,
            "all private recovery failures preserve active session and original archive");
    }
}
