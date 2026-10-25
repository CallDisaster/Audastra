#pragma once

#include <cctype>

// 在TerrainStartLocationsJournalCases.h后包含，复用完整起点/单位/W3I夹具。
void StartLocationsCommandsPreservePrivateProofAndUndo() {
    SessionFixture grid;
    const auto beforeUnits = Take(StartJournalUnits().Serialize());
    const auto beforeSettings = Take(StartJournalSettings().Serialize());
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", beforeUnits}, {"war3map.w3i", beforeSettings}};
    MapDataEditSession hot{MapDocument::FromMembers(members)};
    Require(hot.AttachTerrain(grid.terrainBytes).Succeeded() && hot.AttachPathing(grid.pathing).Succeeded(), "attach exact start source");
    auto changed = StartJournalUnits(); changed.units.front().position[0] += 128;
    Require(hot.EditUnits(changed).Succeeded() && hot.Revision() == 1, "one PrepareUnits creates one compound receipt");
    const auto receipt = hot.LastEdit(); const auto snapshot = hot.Snapshot();
    const auto afterUnits = Take(snapshot.Units()->Serialize()), afterSettings = Take(snapshot.Settings()->Serialize());
    Require(snapshot.Settings()->players[0].startX == changed.units[0].position[0]
        && hot.VerifyTerrainSettingsForSave().Succeeded(), "typed sloc XY alone receives normal terrain W3I save proof");
    auto frozen = Take(hot.ExportTerrainStrokeJournal());
    Require(frozen.strokes.size() == 1 && frozen.strokes[0].frozenStartLocations
        && !frozen.strokes[0].units && !EncodeTerrainStrokeJournal(frozen).Succeeded(), "frozen compound cannot be encoded before materialization");
    const auto alias = *frozen.strokes[0].frozenStartLocations;
    frozen.sourceSha256 = Journal().sourceSha256; frozen.members = Journal().members;
    frozen.members[4] = {"war3mapunits.doo", true, Take(Sha256(beforeUnits)), beforeUnits.size()};
    frozen.members[2] = {"war3map.w3i", true, Take(Sha256(beforeSettings)), beforeSettings.size()};
    Require(MaterializeTerrainJournalPlacements(frozen).Succeeded() && frozen.strokes[0].startLocations,
        "background builds complete dual delta from original immutable command");
    Require(Take(EncodeTerrainStrokeJournal(frozen))[3] == std::byte{'A'}, "compound emits FTJA only");
    const auto scope = hot.TerrainJournalStateFingerprint();
    Require(hot.Undo() && Take(hot.Snapshot().Units()->Serialize()) == beforeUnits
        && Take(hot.Snapshot().Settings()->Serialize()) == beforeSettings && hot.VerifyTerrainSettingsForSave().Succeeded(),
        "undo restores Units, settings and private proof atomically");
    Require(hot.Redo() && hot.TerrainJournalStateFingerprint() == scope && hot.VerifyTerrainSettingsForSave().Succeeded(),
        "redo restores same actual compound scope and private proof");
    Require(hot.Replay(receipt, false).Succeeded() && !hot.IsDirty()
        && hot.Replay(receipt, true).Succeeded() && hot.VerifyTerrainSettingsForSave().Succeeded(), "receipt replay shares atomic private state");
    MapDataEditSession raw{MapDocument::FromMembers(members)};
    Require(raw.SetSerialized(MapDataKind::Units, afterUnits).Succeeded()
        && raw.SetSerialized(MapDataKind::Settings, afterSettings).Succeeded(), "raw draft can retain complete bytes");
    Require(!raw.VerifyTerrainSettingsForSave().Succeeded(), "equal raw final bytes alone cannot mint native start linkage proof");
    auto ordinary = changed; ordinary.units[1].hitPoints = 1;
    Require(hot.EditUnits(ordinary).Succeeded() && hot.VerifyTerrainSettingsForSave().Succeeded(),
        "ordinary typed unit fields maintain complete Units identity after a start move");
    Require(std::equal(alias.UnitsAfter().begin(), alias.UnitsAfter().end(), afterUnits.begin(), afterUnits.end()),
        "later commands cannot replace frozen compound payload");
    const auto frozenSave = hot.FreezeForSave();
    Require(frozenSave->VerifyTerrainSettingsForSave().Succeeded(), "save worker snapshot keeps private start ownership");
    auto settings = *hot.Snapshot().Settings(); settings.players[1].race = 4;
    Require(hot.EditSettings(settings).Succeeded() && hot.VerifyTerrainSettingsForSave().Succeeded(),
        "known player race composes with a real linked start without changing the start proof");
    Require(hot.Undo(), "undo known race while retaining the real start command");
    settings = *hot.Snapshot().Settings(); settings.players[1].flags ^= 2;
    Require(hot.EditSettings(settings).Succeeded() && !hot.VerifyTerrainSettingsForSave().Succeeded(),
        "real sloc proof cannot authorize unrecorded player flags");
    Require(hot.Undo() && hot.VerifyTerrainSettingsForSave().Succeeded(), "rejecting player change preserves previous source state");
    auto noBudget = Take(hot.ExportTerrainStrokeJournal());
    Require(noBudget.strokes[0].frozenStartLocations.has_value(), "compound remains first ordered record before ordinary Units");
}

void StartLocationsReplayRequiresRealLinkageAndKeepsBaselineHistory() {
    SessionFixture grid;
    auto units = StartJournalUnits(); auto settings = StartJournalSettings();
    // 旧图W3I与sloc本来不同；实际移到W3I已有坐标时仍建立完整identity-settings事务。
    settings.players[0].startX = 512;
    const auto oldUnits = Take(units.Serialize()), oldSettings = Take(settings.Serialize());
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", oldUnits}, {"war3map.w3i", oldSettings}};
    MapDataEditSession source{MapDocument::FromMembers(members)};
    Require(source.AttachTerrain(grid.terrainBytes).Succeeded() && source.AttachPathing(grid.pathing).Succeeded(), "attach inconsistent original source");
    units.units[0].position[0] = 512;
    Require(source.EditUnits(units).Succeeded(), "normal typed editing tolerates original start inconsistency");
    auto journal = Take(source.ExportTerrainStrokeJournal()); Require(journal.strokes[0].frozenStartLocations.has_value(), "identity settings remains compound");
    Require(MaterializeTerrainJournalPlacements(journal).Succeeded(), "materialize actual identity-settings command");
    const auto& edit = *journal.strokes[0].startLocations;
    MapDataEditSession cold{MapDocument::FromMembers(members)};
    Require(!cold.ReplayTerrainJournalStartLocations(edit, 1, TerrainJournalMaximumBytes).Succeeded() && !cold.IsDirty(), "prepaid dual codec failure commits nothing");
    Require(!cold.ReplayTerrainJournalUnits(edit.units, TerrainJournalMaximumBytes, TerrainJournalMaximumBytes).Succeeded() && !cold.IsDirty(),
        "identity W3I does not let old FTJ9 skip sloc linkage");
    Require(cold.ReplayTerrainJournalStartLocations(edit, TerrainJournalMaximumBytes, TerrainJournalMaximumBytes).Succeeded()
        && cold.Revision() == 1 && Take(cold.Snapshot().Settings()->Serialize()) == oldSettings,
        "actual PrepareUnits re-creates identity-settings compound with genuine receipt");
    cold.CommitSaved(cold.Snapshot());
    Require(!cold.IsDirty() && cold.Undo() && cold.VerifyTerrainSettingsForSave().Succeeded()
        && cold.Redo() && cold.VerifyTerrainSettingsForSave().Succeeded(), "saved baseline keeps compound Undo/Redo and settings proof");
    auto moved = units; moved.units[0].position[0] += 128;
    Require(cold.EditUnits(moved).Succeeded() && cold.VerifyTerrainSettingsForSave().Succeeded(), "later start move uses new saved private baseline");
    auto mismatch = edit; mismatch.settings.beforeSha256[0] ^= 1;
    MapDataEditSession rejected{MapDocument::FromMembers(members)};
    Require(!rejected.ReplayTerrainJournalStartLocations(mismatch, TerrainJournalMaximumBytes, TerrainJournalMaximumBytes).Succeeded()
        && rejected.Revision() == 0 && !rejected.IsDirty(), "bad dual identity cannot install half a transaction");
}

void StartLocationsComposeWithDescriptionsAndGroundFollowing() {
    for (const bool descriptionFirst : {false, true}) {
        SessionFixture grid; const auto originalUnits = StartJournalUnits();
        const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", Take(originalUnits.Serialize())},
            {"war3map.w3i", Take(StartJournalSettings().Serialize())}};
        MapDataEditSession hot{MapDocument::FromMembers(members)};
        Require(hot.AttachTerrain(grid.terrainBytes).Succeeded() && hot.AttachPathing(grid.pathing).Succeeded(), "attach compound follow source");
        const auto descriptions = [&] {
            auto value = *hot.Snapshot().Settings(); value.mapName = "changed"; value.author = "new author";
            value.description = "description with another length"; value.recommendedPlayers = "two";
            Require(hot.EditSettings(value).Succeeded(), "four descriptions remain independent public editing");
        };
        if (descriptionFirst) descriptions();
        auto moved = originalUnits; moved.units[0].position[0] = 384;
        Require(hot.EditUnits(moved).Succeeded(), "start edit accepts description context");
        if (!descriptionFirst) descriptions();
        Require(hot.VerifyTerrainSettingsForSave().Succeeded(), "description order does not weaken or break start save proof");
        const auto before = Take(hot.Snapshot().Units()->Serialize());
        TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 16;
        const TerrainStrokePoint point{384, 384}; Take(hot.EditTerrain(brush, std::span(&point, 1), true));
        Require(Take(hot.Snapshot().Units()->Serialize()) != before && hot.VerifyTerrainSettingsForSave().Succeeded(),
            "ground Z following maintains private full Units identity without changing start XY");
        Require(hot.Undo() && hot.VerifyTerrainSettingsForSave().Succeeded()
            && hot.Redo() && hot.VerifyTerrainSettingsForSave().Succeeded(), "follow command Undo/Redo owns same private start chain");
    }
    for (const bool unrelatedPlayerField : {false, true}) {
        SessionFixture grid;
        const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", Take(StartJournalUnits().Serialize())},
            {"war3map.w3i", Take(StartJournalSettings().Serialize())}};
        MapDataEditSession hot{MapDocument::FromMembers(members)};
        Require(hot.AttachTerrain(grid.terrainBytes).Succeeded() && hot.AttachPathing(grid.pathing).Succeeded(),
            "attach complete baseline before the combined player-page transaction");
        auto value = StartJournalSettings(); value.players[0].startX = 512; value.mapName = "linked name";
        value.author = "linked author"; value.description = "same command"; value.recommendedPlayers = "two players";
        if (unrelatedPlayerField) value.players[0].race = 4;
        Require(hot.EditSettings(value).Succeeded(), "player page can edit start and descriptions in one transaction");
        Require(hot.VerifyTerrainSettingsForSave().Succeeded() == !unrelatedPlayerField,
            "reverse real PrepareUnits accepts only linked start and four descriptions");
        if (!unrelatedPlayerField) {
            auto journal = Take(hot.ExportTerrainStrokeJournal());
            Require(journal.strokes.size() == 1 && journal.strokes[0].frozenStartLocations.has_value(),
                "same player-page transaction freezes both members with genuine private ownership");
            // 没有附加限制时APR可按合同省略；基线资格核实际W3E/WPM及其它源成员原字。
            std::vector<ArchiveExactMember> baseline;
            for (const auto name : TerrainJournalMemberNames) {
                ArchiveExactMember entry{std::string(name)};
                if (name == "war3map.w3e") { entry.present = true; entry.bytes = grid.terrainBytes; }
                else if (name == "war3map.wpm") { entry.present = true; entry.bytes = grid.pathing; }
                else for (const auto& member : members) {
                    auto canonical = member.name;
                    std::transform(canonical.begin(), canonical.end(), canonical.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (canonical == name) { entry.present = true; entry.bytes = member.data; }
                }
                baseline.push_back(std::move(entry));
            }
            Require(hot.VerifyTerrainJournalBaseline(baseline).Succeeded(), "journal baseline matches complete actual source bytes");
            baseline[1].bytes.back() ^= std::byte{1};
            Require(!hot.VerifyTerrainJournalBaseline(baseline).Succeeded(), "changed source pathing cannot satisfy start journal baseline");
            // 同样的起点事务没有真实地形/通行基线时，不能借单位和W3I原字授予恢复资格。
            MapDataEditSession unbound{MapDocument::FromMembers(members)};
            Require(unbound.EditSettings(value).Succeeded() && unbound.VerifyTerrainSettingsForSave().Succeeded(),
                "unattached start transaction remains editable without fabricating a terrain source");
            const auto unboundRevision = unbound.Revision();
            const auto unboundUnits = Take(unbound.Snapshot().Units()->Serialize());
            const auto unboundSettings = Take(unbound.Snapshot().Settings()->Serialize());
            const auto missing = unbound.ExportTerrainStrokeJournal();
            Require(!missing.Succeeded() && !missing.value && std::any_of(missing.diagnostics.begin(), missing.diagnostics.end(),
                [](const auto& diagnostic) { return diagnostic.code == "TERRAIN_JOURNAL_BASELINE_MISSING"; }),
                "missing real terrain and pathing baseline still rejects journal export");
            Require(unbound.Revision() == unboundRevision && Take(unbound.Snapshot().Units()->Serialize()) == unboundUnits
                && Take(unbound.Snapshot().Settings()->Serialize()) == unboundSettings && unbound.VerifyTerrainSettingsForSave().Succeeded(),
                "failed journal export preserves the complete editable start transaction");
            Require(hot.Undo() && hot.VerifyTerrainSettingsForSave().Succeeded()
                && hot.Redo() && hot.VerifyTerrainSettingsForSave().Succeeded(),
                "one Undo restores all four descriptions and actual start linkage together");
        }
    }
}
