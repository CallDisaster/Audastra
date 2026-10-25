#pragma once

// 在TerrainWaterEditServiceTests.cpp的匿名命名空间末尾包含，复用其原创地图和实际保存工具。
Bytes TerrainSettingsBytes(const MapDataEditSession& session) {
    const auto document = Take(session.Snapshot(MapDataKind::Settings, kResourceBudget));
    const auto& sources = document.Member(MapDataKind::Settings).sources;
    Require(sources.size() == 1, "settings test reads one complete native W3I");
    return sources.front().data;
}

template<class FixtureType>
std::unique_ptr<MapDataEditSession> TerrainSettingsFreshSession(const FixtureType& fixture) {
    auto session = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(fixture.members));
    Require(session->AttachTerrain(fixture.members[0].data).Succeeded()
        && session->AttachPathing(fixture.members[1].data).Succeeded(), "settings fixture attaches actual W3E/WPM before editing");
    return session;
}

template<class FixtureType>
MapSettings InitializeTerrainSettingsFixture(FixtureType& fixture) {
    auto member = std::find_if(fixture.members.begin(), fixture.members.end(),
        [](const auto& value) { return value.name == "war3map.w3i"; });
    Require(member != fixture.members.end(), "settings fixture has native W3I");
    auto settings = Take(MapSettings::Parse(member->data));
    settings.mapName = "Original terrain"; settings.author = "Original author";
    settings.description = "Original description"; settings.recommendedPlayers = "1";
    MapPlayerSettings player; player.number = 0; player.controller = 1; player.race = 1;
    player.name = "Original player"; player.startX = 768; player.startY = 1024;
    settings.players = {player};
    MapForceSettings force; force.flags = 1; force.playerMask = 1; force.name = "Original team";
    settings.forces = {force}; member->data = Take(settings.Serialize());
    fixture.source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
        Take(WriteVerifiedArchive(fixture.root / "settings-source.w3x", fixture.members)).path)));
    fixture.edits = TerrainSettingsFreshSession(fixture);
    return settings;
}

MapSettings ChangedTerrainSettings(const MapSettings& original) {
    auto settings = original;
    settings.mapName = "Edited terrain"; settings.author = "Edited author";
    settings.description = "Water, ramp and environment"; settings.recommendedPlayers = "2";
    settings.flags |= kMapFlagTerrainFog;
    settings.environment.fogStyle = 1; settings.environment.fogStart = 512;
    settings.environment.fogEnd = 4096; settings.environment.fogDensity = 0.5f;
    settings.environment.fogColor = {10, 50, 90, 255};
    settings.environment.weather = {std::byte{'R'}, std::byte{'A'}, std::byte{'l'}, std::byte{'r'}};
    settings.environment.soundEnvironment = "Forest"; settings.environment.lightEnvironment = 'L';
    settings.environment.waterColor = {40, 80, 120, 200};
    settings.players[0].name = "Edited player"; settings.players[0].controller = 2; settings.players[0].race = 4;
    settings.forces[0].name = "Edited team";
    return settings;
}

template<class FixtureType>
std::unique_ptr<MapDataEditSession> TerrainSettingsRawDraft(const FixtureType& fixture) {
    auto raw = TerrainSettingsFreshSession(fixture);
    auto pathing = fixture.members[1].data;
    for (const auto& member : fixture.edits->Replacements()) {
        if (member.name == "war3map.wpm") pathing = member.data;
        else if (member.name == "war3map.w3i")
            Require(raw->SetSerialized(MapDataKind::Settings, member.data).Succeeded(), "raw settings draft retains complete W3I");
        else if (member.name == "war3map.doo")
            Require(raw->SetSerialized(MapDataKind::Doodads, member.data).Succeeded(), "raw settings draft retains complete DOO");
        else if (member.name == "war3mapUnits.doo")
            Require(raw->SetSerialized(MapDataKind::Units, member.data).Succeeded(), "raw settings draft retains complete Units");
        else Require(member.name == "war3map.w3e", "settings test does not silently omit another raw member");
    }
    Require(fixture.edits->Removals().empty(), "settings fixture does not omit raw removals");
    const auto terrain = Take(SerializeMapTerrain(*fixture.edits->Terrain()));
    const auto layers = Take(fixture.edits->SerializePathingRestrictionDraft());
    // 规范空P/S不含APR载荷；只有实际附加层才走声明式Draft入口。
    const auto restored = layers.empty() ? raw->SetTerrainAndPathing(terrain, pathing)
        : raw->SetTerrainAndPathingDraft(terrain, pathing, layers);
    Require(restored.Succeeded(), "raw terrain draft retains complete W3E/WPM and restriction layers");
    Require(raw->MatchesTerrainJournalDraft(*fixture.edits), "raw draft exactly matches all final members and pathing layers");
    return raw;
}

template<class FixtureType, class Stroke>
void CheckTerrainSettingsComposedSaveAndRestore(FixtureType& fixture, bool settingsFirst, const Stroke& stroke) {
    const auto original = InitializeTerrainSettingsFixture(fixture);
    const auto beforeSettings = TerrainSettingsBytes(*fixture.edits);
    const auto beforeTerrain = fixture.members[0].data;
    const auto changed = ChangedTerrainSettings(original);
    const auto afterSettings = Take(changed.Serialize());
    const auto sourceSha = fixture.source->SourceSha256();
    if (settingsFirst) Require(fixture.edits->EditSettings(changed).Succeeded(), "known settings may precede the terrain command");
    stroke();
    if (!settingsFirst) Require(fixture.edits->EditSettings(changed).Succeeded(), "known settings may follow the terrain command");
    const auto afterTerrain = Take(SerializeMapTerrain(*fixture.edits->Terrain()));
    Require(afterTerrain != beforeTerrain && TerrainSettingsBytes(*fixture.edits) == afterSettings,
        "the complete environment/player edit and real terrain command coexist");

    // 旧前向工程路径：真实typed前缀经过wire，再与独立恢复的完整raw草稿共同核对。
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*fixture.source, *fixture.edits));
    Require(!journal.history && journal.strokes.size() == 1, "a fresh forward journal does not pretend to contain a saved history station");
    const auto decoded = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(journal))));
    auto raw = TerrainSettingsRawDraft(fixture);
    auto restored = Take(TerrainPathingSave::RestoreJournal(*fixture.source, MapDocument::FromMembers(fixture.members),
        beforeTerrain, fixture.members[1].data, decoded, *raw, fixture.game));
    Require(restored.receipts.size() == 2 && restored.session->MatchesTerrainJournalDraft(*raw)
        && TerrainSettingsBytes(*restored.session) == afterSettings,
        "owned forward replay restores the exact full W3I with a real settings undo command");
    Require(restored.session->Undo() && TerrainSettingsBytes(*restored.session) == beforeSettings
        && Take(SerializeMapTerrain(*restored.session->Terrain())) == afterTerrain,
        "restored settings Undo restores complete original W3I without undoing water or ramp");
    auto baseline = TerrainSettingsFreshSession(fixture);
    Require(restored.session->Undo() && !restored.session->CanUndo()
        && restored.session->MatchesTerrainJournalDraft(*baseline), "the second real Undo restores the entire source domain");
    Require(restored.session->Redo() && TerrainSettingsBytes(*restored.session) == beforeSettings
        && Take(SerializeMapTerrain(*restored.session->Terrain())) == afterTerrain,
        "the first real Redo restores terrain and followed placements without applying settings early");
    Require(restored.session->Redo() && !restored.session->CanRedo()
        && restored.session->MatchesTerrainJournalDraft(*raw), "the second real Redo restores all final fields and pathing layers");

    auto& session = *restored.session;
    const auto plan = Take(TerrainPathingSave::PrepareForSave(*fixture.source, session, fixture.game));
    Require(plan.SaveGate().Succeeded() && plan.RebuiltPathing(), "known settings still pass actual native G0/G1 terrain save qualification");
    auto replacements = session.Replacements();
    Require(plan.InstallPathingOutput(session, replacements).Succeeded()
        && plan.VerifyForSave(session, sourceSha, replacements).Succeeded(), "the exact settings/terrain candidate passes final save CAS");
    const auto written = Take(fixture.source->SaveCandidate(fixture.root / "settings-native.w3x", replacements));
    const auto saved = Take(MapArchiveSession::Open(written.path));
    Require(plan.VerifyReadback(saved).Succeeded() && Take(saved.ReadMember("war3map.w3i")) == afterSettings
        && Take(saved.ReadMember("war3map.w3e")) == afterTerrain,
        "independently reopened archive retains full edited W3I and terrain");
    for (const auto& member : replacements)
        Require(Take(saved.ReadMember(member.name)) == member.data, "native readback matches every complete frozen replacement");
    for (const auto& member : fixture.members) {
        if (std::none_of(replacements.begin(), replacements.end(), [&](const auto& value) { return value.name == member.name; }))
            Require(Take(saved.ReadMember(member.name)) == member.data, "native settings save preserves every unrelated authored member");
    }
    Require(Take(Sha256File(fixture.source->SourcePath())) == sourceSha, "native settings save never writes the source map");
    const auto revision = session.Revision(), sourceRevision = session.TerrainSourceRevision();
    Require(plan.CommitSaved(session, MapDocument::FromArchive(saved)).Succeeded() && !session.IsDirty()
        && session.Revision() == revision && session.TerrainSourceRevision() == sourceRevision + 1,
        "verified native save commits the whole settings/terrain baseline without dropping history");
    Require(session.Undo() && TerrainSettingsBytes(session) == beforeSettings
        && Take(SerializeMapTerrain(*session.Terrain())) == afterTerrain, "settings Undo remains exact after committing the real map save");
    Require(session.Undo() && TerrainSettingsBytes(session) == beforeSettings
        && Take(SerializeMapTerrain(*session.Terrain())) == beforeTerrain, "terrain Undo remains available across the real save boundary");
    Require(session.Redo() && session.Redo() && !session.IsDirty()
        && TerrainSettingsBytes(session) == afterSettings && Take(SerializeMapTerrain(*session.Terrain())) == afterTerrain,
        "real save retains both commands and their exact final settings on Redo");
}

void KnownSettingsComposeWithNativeWaterAndRampSaveAndForwardRestore() {
    for (const bool settingsFirst : {false, true}) {
        for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
            Fixture fixture;
            CheckTerrainSettingsComposedSaveAndRestore(fixture, settingsFirst, [&] {
                const auto rules = fixture.Rules(); TerrainWaterStrokeOptions options;
                options.kind = kind; options.shape = TerrainBrushShape::Square; options.cliffTexture = 1; options.randomSeed = 107;
                Take(ApplyTerrainEdit(*fixture.edits, fixture.Document(), Take(PrepareTerrainWaterEdit(*fixture.edits,
                    fixture.Document(), fixture.Binding(), rules, options, std::span(&fixture.point, 1), false))));
            });
        }
        War3Map::TerrainCornerRampFixtures::Fixture fixture;
        CheckTerrainSettingsComposedSaveAndRestore(fixture, settingsFirst, [&] {
            const auto rules = fixture.Rules(); TerrainBrush brush; brush.kind = TerrainBrushKind::CornerRamp; brush.value = true;
            Take(ApplyTerrainEdit(*fixture.edits, fixture.Document(), Take(PrepareTerrainCornerRampEdit(*fixture.edits,
                fixture.Document(), fixture.Binding(), rules, brush, std::span(&fixture.point, 1), true, {}, TerrainCornerRampAlgorithm::DryOnly))));
            fixture.CheckState(*fixture.edits, true);
        });
    }
}

void StructuralSettingsStillRejectNativeTerrainSaveAndForwardRestore() {
    for (unsigned field = 0; field < 3; ++field) {
        Fixture fixture; const auto original = InitializeTerrainSettingsFixture(fixture);
        const auto rules = fixture.Rules(); TerrainWaterStrokeOptions options;
        options.cliffTexture = 1; options.randomSeed = 73;
        Take(ApplyTerrainEdit(*fixture.edits, fixture.Document(), Take(PrepareTerrainWaterEdit(*fixture.edits,
            fixture.Document(), fixture.Binding(), rules, options, std::span(&fixture.point, 1), false))));
        const auto journal = Take(TerrainPathingSave::CaptureJournal(*fixture.source, *fixture.edits));
        auto settings = ChangedTerrainSettings(original);
        if (field == 0) ++settings.playableWidth;
        else if (field == 1) ++settings.players[0].number;
        else settings.forces[0].playerMask ^= 2;
        Require(fixture.edits->EditSettings(settings).Succeeded(), "structural raw edit remains a draft before native-save qualification");
        const auto before = TerrainSettingsBytes(*fixture.edits);
        const auto terrain = Take(SerializeMapTerrain(*fixture.edits->Terrain()));
        const auto revision = fixture.edits->Revision();
        const auto sourceSha = fixture.source->SourceSha256();
        const auto prepared = TerrainPathingSave::PrepareForSave(*fixture.source, *fixture.edits, fixture.game);
        Require(!prepared.Succeeded() || (prepared.value && !prepared.value->SaveGate().Succeeded()),
            "playable dimensions, player numbers and force masks cannot borrow known settings save compatibility");
        auto raw = TerrainSettingsRawDraft(fixture);
        const auto restored = TerrainPathingSave::RestoreJournal(*fixture.source, MapDocument::FromMembers(fixture.members),
            fixture.members[0].data, fixture.members[1].data, journal, *raw, fixture.game);
        Require(!restored.Succeeded() && !restored.value && HasCode(restored, "TERRAIN_JOURNAL_OUTPUT_MISMATCH"),
            "forward restoration refuses the entire structural settings candidate");
        Require(fixture.edits->Revision() == revision && TerrainSettingsBytes(*fixture.edits) == before
            && Take(SerializeMapTerrain(*fixture.edits->Terrain())) == terrain
            && Take(Sha256File(fixture.source->SourcePath())) == sourceSha
            && !std::filesystem::exists(fixture.root / "settings-native.w3x"), "rejected gates preserve source, complete draft and output absence");
    }
}
