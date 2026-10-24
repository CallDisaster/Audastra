#pragma once

// 在EditorProjectTests类声明之后包含，复用原创NativePathingQtMembers与实际工程/保存入口。
void EditorProjectTests::NativePathingRemoveWaterProjectRoundtripAndSave_data() {
    QTest::addColumn<bool>("deep"); QTest::addColumn<bool>("inPlace");
    QTest::newRow("remove-shallow-save-as") << false << false;
    QTest::newRow("remove-deep-save-as") << true << false;
    QTest::newRow("remove-shallow-in-place") << false << true;
    QTest::newRow("remove-deep-in-place") << true << true;
}

void EditorProjectTests::NativePathingRemoveWaterProjectRoundtripAndSave() {
    QFETCH(bool, deep); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath("map"));
    const auto source = std::filesystem::path(root.filePath("map/source.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", std::vector<MapArchiveMember>{
        {"fixture.txt", Tests::WaterText("owned remove-water game source")}}).Succeeded());
    auto members = NativePathingQtMembers(); QVERIFY(members.size() >= 3);
    const auto wet = MapData::ParseMapTerrain(members[1].data); QVERIFY(wet.Succeeded() && wet.value);
    auto grid = *wet.value->Snapshot();
    for (auto& vertex : grid.vertices) vertex.waterHeightAndFlagsRaw = 8192 + (deep ? 80 : 48) * 4;
    MapData::RefreshTerrainDerived(grid, 0, 0, 9, 9);
    const auto wetWire = MapData::SerializeMapTerrain(grid); QVERIFY(wetWire.Succeeded());
    members[1].data = *wetWire.value;
    std::fill(members[2].data.begin() + 16, members[2].data.end(), deep ? std::byte{0x1b} : std::byte{0x19});
    const auto originalWpm = members[2].data;
    members.push_back({"war3mapMap.tga", Tests::WaterText("authored minimap retained by remove-water")});
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); project.war3GameRoot_ = game;
    QUndoStack initialUndo; project.mapDataUndoStack_ = &initialUndo;
    MapData::TerrainBrush removal; removal.kind = MapData::TerrainBrushKind::RemoveWater;
    removal.shape = MapData::TerrainBrushShape::Square; removal.radius = 8;
    const MapData::TerrainStrokePoint center{512, 512};
    QVERIFY(project.mapEdits_->EditTerrain(removal, std::span(&center, 1), true).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("移除整幅原创水面"));
    PushMapDataEdit(project, "Remove water", "移除水面并保留地高"); QCOMPARE(initialUndo.count(), 1);
    const auto dryWire = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(dryWire.Succeeded());
    QVERIFY(std::none_of(project.mapEdits_->Terrain()->vertices.begin(), project.mapEdits_->Terrain()->vertices.end(),
        [](const auto& vertex) { return vertex.HasWater(); }));
    const auto forge = std::filesystem::path(root.filePath("remove.forge").toStdWString());
    const auto savedProject = project.SaveProjectToPath(forge);
    QVERIFY2(savedProject.Succeeded(), ReadDiagnosticText(savedProject).c_str());
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{1});
    EditorProject restored; QUndoStack undo; restored.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(restored, **opened.value, game)); QCOMPARE(undo.count(), 1);
    QCOMPARE(MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value, dryWire.value);
    undo.undo(); QCOMPARE(MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value, wetWire.value);
    undo.redo(); QCOMPARE(MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value, dryWire.value);
    const auto journal = Services::TerrainPathingSave::CaptureJournal(*restored.archiveSession_, *restored.mapEdits_);
    QVERIFY2(journal.Succeeded(), ReadDiagnosticText(journal).c_str());
    const auto encoded = MapData::EncodeTerrainStrokeJournal(*journal.value); QVERIFY(encoded.Succeeded());
    QCOMPARE(encoded.value->at(3), std::byte{'1'});
    const auto forgeAs = std::filesystem::path(root.filePath("remove-copy.forge").toStdWString());
    const auto savedAgain = restored.SaveProjectToPath(forgeAs);
    QVERIFY2(savedAgain.Succeeded(), ReadDiagnosticText(savedAgain).c_str());
    auto openedAgain = PrepareProjectOpen(forgeAs, game); QVERIFY2(openedAgain.Succeeded(), ReadDiagnosticText(openedAgain).c_str());
    QVERIFY(HasReadDiagnostic(openedAgain, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE(MapData::SerializeMapTerrain(*(*openedAgain.value)->mapDataEdits->Terrain()).value, dryWire.value);
    EditorProject final; QUndoStack finalUndo; final.mapDataUndoStack_ = &finalUndo;
    QVERIFY(BindPreparedTerrainJournal(final, **openedAgain.value, game)); QCOMPARE(finalUndo.count(), 1);
    auto dryWpm = originalWpm; std::fill(dryWpm.begin() + 16, dryWpm.end(), std::byte{0x51});
    const auto session = final.mapEdits_->SessionIdentity();
    for (unsigned phase = 0; phase < 3; ++phase) {
        const bool dry = phase != 1;
        if (phase == 1) finalUndo.undo();
        if (phase == 2) finalUndo.redo();
        auto target = inPlace ? final.archiveSession_->SourcePath()
            : std::filesystem::path(root.filePath(QString("remove-save-%1.w3x").arg(phase)).toStdWString());
        auto job = FreezeNativePathingQtJob(final, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = final.mapEdits_->FreezeForSave();
        if (inPlace) {
            job->inPlace = true;
            job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath(QString("backups-%1").arg(phase)).toStdWString());
            QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
        }
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainSavePlan->RebuiltPathing());
        QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded());
        QCOMPARE(actual.value->ReadMember("war3map.w3e").value, dry ? dryWire.value : wetWire.value);
        QCOMPARE(actual.value->ReadMember("war3map.wpm").value, std::optional(dry ? dryWpm : originalWpm));
        for (const auto& member : members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm"
            && member.name != "war3map.mmp")
            QCOMPARE(actual.value->ReadMember(member.name).value, std::optional(member.data));
        QVERIFY(job->minimapMarkers && job->minimapMarkers->MarkerCount() == 0);
        QCOMPARE(actual.value->ReadMember("war3map.mmp").value,
            std::optional(std::vector<std::byte>(8, std::byte{0})));
        QVERIFY(job->minimapMarkers->VerifyReadback(*actual.value).Succeeded());
        QVERIFY(final.CommitMapSave(*job)); QVERIFY(!final.HasUnsavedMapChanges());
        QCOMPARE(final.mapEdits_->SessionIdentity(), session); QCOMPARE(finalUndo.count(), 1);
        QCOMPARE(finalUndo.index(), dry ? 1 : 0);
        if (inPlace) QVERIFY(!job->backup.empty());
    }
    if (!inPlace) QCOMPARE(Sha256File(source).value, sourceSha.value);
}
