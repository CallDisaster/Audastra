#pragma once

// 在TerrainGroundPaletteAppendProjectCases.h后包含；复用原创地图与正式Qt保存任务。
void EditorProjectTests::CliffGroundPaletteProjectColdRestoreAndNativeSave_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::newRow("save-as") << false; QTest::newRow("in-place") << true;
}

void EditorProjectTests::CliffGroundPaletteProjectColdRestoreAndNativeSave() {
    QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    auto members = OrdinaryJournalQtMembers();
    for (auto& entry : members) {
        if (entry.name == "TerrainArt\\Terrain.slk") entry.data = NativeDoodadQtTable(
            {"tileID", "dir", "file", "walkable", "flyable", "buildable"},
            {{"Tg01", "Fixture", "Ground", "1", "1", "1"}, {"Tg02", "Fixture", "Ground2", "1", "1", "1"}});
        if (entry.name == "TerrainArt\\CliffTypes.slk") entry.data = NativeDoodadQtTable(
            {"cliffID"}, {{"Cl01"}, {"Cl02"}});
    }
    const auto member = [&](std::string_view name) -> const std::vector<std::byte>& {
        return std::find_if(members.begin(), members.end(), [&](const auto& entry) { return entry.name == name; })->data;
    };
    const auto parsed = MapData::ParseMapTerrain(member("war3map.w3e")); QVERIFY(parsed.Succeeded());
    auto expectedGround = *parsed.value->Snapshot(); expectedGround.header.usesCustomTileset = true;
    expectedGround.header.groundTilesets.push_back("Tg02");
    auto expectedCliff = expectedGround; expectedCliff.header.cliffTilesets.push_back("Cl02");
    const auto groundWire = MapData::SerializeMapTerrain(expectedGround); QVERIFY(groundWire.Succeeded());
    const auto cliffWire = MapData::SerializeMapTerrain(expectedCliff); QVERIFY(cliffWire.Succeeded());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto forge = std::filesystem::path(root.filePath("cliff-ground-append.forge").toStdWString());
    std::map<std::string, std::vector<std::byte>> finalDraft;
    {
        EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); project.war3GameRoot_ = game;
        QUndoStack undo; project.mapDataUndoStack_ = &undo;
        const auto append = [&](MapData::TerrainTextureTable table, const char* rawcode) {
            const auto result = Services::TerrainPathingSave::EditTextureSlot(*project.archiveSession_, *project.mapEdits_, table, 1, rawcode, game);
            if (!result.Succeeded() || !result.value || !result.value->headerChanged) return false;
            if (!project.ObserveMapDataEdit("追加可恢复地形贴图槽")) return false;
            PushMapDataEdit(project, "Append terrain palette", "追加地形贴图"); return true;
        };
        QVERIFY(append(MapData::TerrainTextureTable::Ground, "Tg02"));
        QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, groundWire.value);
        QVERIFY(append(MapData::TerrainTextureTable::Cliff, "Cl02")); QCOMPARE(undo.count(), 2);
        QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, cliffWire.value);
        finalDraft = TerrainJournalDraftBytes(*project.mapEdits_);
        const auto saved = project.SaveProjectToPath(forge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
        QVERIFY(!HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY"));
        const auto archive = MapArchiveSession::Open(forge); QVERIFY(archive.Succeeded());
        const auto wire = archive.value->ReadMember("overlay/editor/terrain-strokes.bin"); QVERIFY(wire.Succeeded());
        QCOMPARE(wire.value->at(3), std::byte{'6'});
        const auto journal = MapData::DecodeTerrainStrokeJournal(*wire.value); QVERIFY(journal.Succeeded());
        QCOMPARE(journal.value->strokes.size(), std::size_t{2});
        QVERIFY(journal.value->strokes[0].intent.palette->algorithm == 2
            && journal.value->strokes[1].intent.palette->algorithm == 3);
        undo.clear();
    }
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{2}); QVERIFY((*opened.value)->terrainJournalWarning.empty());
    EditorProject cold; QUndoStack undo; cold.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(cold, **opened.value, game)); QCOMPARE(undo.count(), 2);
    const auto scope = cold.mapEdits_->TerrainJournalStateFingerprint();
    QCOMPARE(TerrainJournalDraftBytes(*cold.mapEdits_), finalDraft);
    undo.undo(); QCOMPARE(MapData::SerializeMapTerrain(*cold.mapEdits_->Terrain()).value, groundWire.value);
    undo.undo(); QVERIFY(!cold.mapEdits_->IsDirty());
    QCOMPARE(MapData::SerializeMapTerrain(*cold.mapEdits_->Terrain()).value, std::optional(member("war3map.w3e")));
    undo.redo(); undo.redo(); QCOMPARE(TerrainJournalDraftBytes(*cold.mapEdits_), finalDraft);
    QCOMPARE(cold.mapEdits_->TerrainJournalStateFingerprint(), scope);
    const auto session = cold.mapEdits_->SessionIdentity();
    for (unsigned phase = 0; phase < 3; ++phase) {
        if (phase == 1) undo.undo();
        if (phase == 2) undo.redo();
        const auto& expected = phase == 1 ? *groundWire.value : *cliffWire.value;
        const auto beforeSha = Sha256File(cold.archiveSession_->SourcePath()); QVERIFY(beforeSha.Succeeded());
        const auto target = inPlace ? cold.archiveSession_->SourcePath()
            : std::filesystem::path(root.filePath(QString("cliff-append-saved-%1.w3x").arg(phase)).toStdWString());
        auto job = FreezeNativePathingQtJob(cold, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = cold.mapEdits_->FreezeForSave();
        if (inPlace) {
            job->inPlace = true; job->inPlaceOptions.backupDirectory =
                std::filesystem::path(root.filePath(QString("cliff-backups-%1").arg(phase)).toStdWString());
            QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
        }
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded());
        QCOMPARE(actual.value->ReadMember("war3map.w3e").value, std::optional(expected));
        for (const auto& original : members) if (original.name != "war3map.w3e")
            QCOMPARE(actual.value->ReadMember(original.name).value, std::optional(original.data));
        QVERIFY(cold.CommitMapSave(*job)); QVERIFY(!cold.HasUnsavedMapChanges());
        QCOMPARE(cold.mapEdits_->SessionIdentity(), session); QCOMPARE(undo.count(), 2); QCOMPARE(undo.index(), phase == 1 ? 1 : 2);
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, beforeSha.value); }
        else QCOMPARE(Sha256File(source).value, sourceSha.value);
    }
}
