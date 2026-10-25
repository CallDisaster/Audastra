#pragma once

// 在TerrainEditInspectionTests.cpp的匿名命名空间末尾包含；全部为原创W3I字节。
std::vector<MapArchiveMember> TerrainSettingsCompatibilityMembers() {
    auto members = Members();
    auto settings = Take(MapSettings::Parse(members[4].data));
    // 此小样例不提供WTS；用真实内联文字，避免把缺失字符串表误作字段兼容失败。
    settings.description = "Original description";
    members[4].data = Take(settings.Serialize());
    return members;
}

MapSettings CompatibleTerrainSettings(const MapSettings& original) {
    auto settings = original;
    settings.mapName = "Edited terrain"; settings.author = "Test author";
    settings.description = "Terrain and environment"; settings.recommendedPlayers = "2";
    settings.flags ^= kMapFlagTerrainFog;
    settings.environment.fogStyle = 1; settings.environment.fogStart = 512;
    settings.environment.fogEnd = 4096; settings.environment.fogDensity = 0.5f;
    settings.environment.fogColor = {10, 50, 90, 255};
    settings.environment.weather = {std::byte{'R'}, std::byte{'A'}, std::byte{'l'}, std::byte{'r'}};
    settings.environment.soundEnvironment = "Forest"; settings.environment.lightEnvironment = 'L';
    settings.environment.waterColor = {40, 80, 120, 200};
    settings.players[0].name = "Player"; settings.players[0].controller = 2; settings.players[0].race = 4;
    settings.forces[0].name = "Team";
    return settings;
}

void TerrainSettingsKnownFieldsComposeAndRestoreWithoutGeometry() {
    const auto members = TerrainSettingsCompatibilityMembers();
    const auto original = Take(MapSettings::Parse(members[4].data));
    const auto settings = CompatibleTerrainSettings(original);
    const auto wire = Take(settings.Serialize());
    Require(ValidateTerrainSettingsPathingChange(members[4].data, wire).Succeeded(),
        "known environment, player identity text and descriptions compose with terrain");
    MapDataEditSession restored(MapDocument::FromMembers(members)); Attach(restored, members);
    MapDataEditSession expected(MapDocument::FromMembers(members)); Attach(expected, members);
    Stroke(restored, TerrainBrushKind::Raise); Stroke(expected, TerrainBrushKind::Raise);
    Require(expected.EditSettings(settings).Succeeded(), "edit known settings through the shared transaction");
    const auto before = restored.Revision();
    Require(restored.RestoreTerrainJournalDescriptions(expected, 64u << 20, 64u << 20).Succeeded()
        && restored.Revision() == before + 1 && restored.MatchesTerrainJournalDraft(expected),
        "the typed terrain replay receives exactly the final known settings bytes");
    Require(restored.Undo(), "undo restored settings command");
    auto snapshot = Take(restored.Snapshot(MapDataKind::Settings, 1u << 20));
    Require(snapshot.Member(MapDataKind::Settings).sources[0].data == members[4].data,
        "settings undo preserves the complete original W3I");
    Require(restored.Redo() && restored.MatchesTerrainJournalDraft(expected),
        "settings redo preserves the exact draft and terrain");
}

void TerrainSettingsRejectStructuralAndUnknownChangesAtomically() {
    const auto members = TerrainSettingsCompatibilityMembers(); const auto original = Take(MapSettings::Parse(members[4].data));
    for (unsigned field = 0; field < 20; ++field) {
        auto settings = CompatibleTerrainSettings(original);
        switch (field) {
        case 0: settings.cameraBounds[0] += 128; break;
        case 1: ++settings.cameraMargins[0]; break;
        case 2: ++settings.playableWidth; break;
        case 3: settings.tileset = 'A'; break;
        case 4: ++settings.gameDataSet; break;
        case 5: ++settings.players[0].number; break;
        case 6: settings.players[0].flags ^= 2; break;
        case 7: settings.players[0].startX += 128; break;
        case 8: settings.players[0].allyLowPriorityMask ^= 1; break;
        case 9: settings.players[0].allyHighPriorityMask ^= 1; break;
        case 10: settings.forces[0].flags ^= 2; break;
        case 11: settings.forces[0].playerMask ^= 1; break;
        case 12: settings.players.pop_back(); break;
        case 13: settings.forces.clear(); break;
        case 14: settings.flags ^= 0x40000000u; break;
        case 15: settings.loadingScreen.path = "different.mdx"; break;
        case 16: settings.players[0].controller = 99; break;
        case 17: settings.players[0].race = 99; break;
        case 18: settings.environment.fogStyle = 99; break;
        case 19: ++settings.saves; break;
        }
        const auto wire = Take(settings.Serialize());
        const auto gate = ValidateTerrainSettingsPathingChange(members[4].data, wire);
        Require(!gate.Succeeded() && gate.diagnostics.front().code == "TERRAIN_SETTINGS_PATHING_CHANGED",
            "unrecorded structural changes never borrow the known settings compatibility bridge");
        MapDataEditSession restored(MapDocument::FromMembers(members)); Attach(restored, members);
        auto changed = members; changed[4].data = wire;
        MapDataEditSession expected(MapDocument::FromMembers(changed)); Attach(expected, changed);
        const auto revision = restored.Revision();
        Require(!restored.RestoreTerrainJournalDescriptions(expected, 64u << 20, 64u << 20).Succeeded()
            && restored.Revision() == revision && !restored.CanUndo(), "rejected restore never partially installs settings");
    }
    auto tailed = members[4].data; tailed.push_back(std::byte{0x7f});
    Require(!ValidateTerrainSettingsPathingChange(members[4].data, tailed).Succeeded(), "unknown tail is not normalized away");
    auto nonfinite = members[4].data;
    const auto originalFixture = MapDataFixtures::FullMapSettingsV25();
    const auto fog = originalFixture.offsets.at("fog")
        + members[4].data.size() - originalFixture.bytes.size();
    const std::uint32_t infinity = 0x7f800000u;
    std::memcpy(nonfinite.data() + fog, &infinity, sizeof(infinity));
    Require(!ValidateTerrainSettingsPathingChange(members[4].data, nonfinite).Succeeded(), "nonfinite fog cannot pass complete parse");
    auto unknown = original; unknown.environment.fogStyle = 99; unknown.flags &= ~kMapFlagTerrainFog;
    const auto oldUnknown = Take(unknown.Serialize()); unknown.mapName = "Text only";
    Require(ValidateTerrainSettingsPathingChange(oldUnknown, Take(unknown.Serialize())).Succeeded(),
        "unchanged unknown source fog may accompany a text edit");
    unknown.flags |= kMapFlagTerrainFog;
    Require(!ValidateTerrainSettingsPathingChange(oldUnknown, Take(unknown.Serialize())).Succeeded(),
        "turning on unknown source fog is a new unsupported behavior");
}
