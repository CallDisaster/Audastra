#pragma once

// 在TerrainOrdinaryJournalProjectCases.h后包含；复用原创special与原生保存夹具。
void EditorProjectTests::GroundPaletteAppendWaterProjectColdRestoreAndNativeSave_data() {
    QTest::addColumn<bool>("appendFirst"); QTest::addColumn<bool>("inPlace");
    QTest::newRow("append-water-save-as") << true << false;
    QTest::newRow("water-append-save-as") << false << false;
    QTest::newRow("append-water-in-place") << true << true;
    QTest::newRow("water-append-in-place") << false << true;
}

void EditorProjectTests::GroundPaletteAppendWaterProjectColdRestoreAndNativeSave() {
    QFETCH(bool, appendFirst); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    auto members = OrdinaryJournalQtMembers();
    for (auto& entry : members) if (entry.name == "TerrainArt\\Terrain.slk")
        entry.data = NativeDoodadQtTable({"tileID", "dir", "file", "walkable", "flyable", "buildable"},
            {{"Tg01", "Fixture", "Ground", "1", "1", "1"}, {"Tg02", "Fixture", "Ground", "0", "1", "1"}});
    const auto member = [&](std::string_view name) -> const std::vector<std::byte>& {
        return std::find_if(members.begin(), members.end(), [&](const auto& entry) { return entry.name == name; })->data;
    };
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto parsed = MapData::ParseMapTerrain(member("war3map.w3e")); QVERIFY(parsed.Succeeded());
    const auto originalDoodads = MapData::ParseDoodadPlacements(member("war3map.doo")); QVERIFY(originalDoodads.Succeeded());
    auto deleted = *originalDoodads.value;
    QCOMPARE(deleted.specialDoodads.size(), std::size_t{3});
    deleted.specialDoodads = {deleted.specialDoodads[2], deleted.specialDoodads[1]};
    const auto deletedWire = deleted.Serialize(); QVERIFY(deletedWire.Succeeded());
    const auto forge = std::filesystem::path(root.filePath("ground-append.forge").toStdWString());
    std::map<std::string, std::vector<std::byte>> finalDraft;
    std::vector<std::byte> finalTerrain, intermediateTerrain;
    {
        EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); project.war3GameRoot_ = game;
        QUndoStack undo; project.mapDataUndoStack_ = &undo;
        const auto append = [&]() -> Status {
            const auto result = Services::TerrainPathingSave::EditTextureSlot(*project.archiveSession_, *project.mapEdits_,
                MapData::TerrainTextureTable::Ground, 1, "Tg02", game);
            if (!result.Succeeded()) return {result.diagnostics};
            if (!result.value || !result.value->headerChanged || !project.ObserveMapDataEdit("追加可恢复地面贴图槽"))
                return Status::Failure("Test", "APPEND_REQUIRED", "原创追加必须形成真实已观察命令。");
            PushMapDataEdit(project, "Append ground texture", "追加地面贴图"); return Status::Ok();
        };
        const auto water = [&]() -> Status {
            const auto current = project.mapEdits_->Snapshot();
            if (!current.Doodads()) return Status::Failure("Test", "DOO_REQUIRED", "水刷必须从完整DOO取得来源。");
            const auto rules = Services::TerrainPathingSave::CaptureWaterBrushRules(*project.archiveSession_,
                *parsed.value->Snapshot(), *project.mapEdits_->Terrain(), *current.Doodads(), game, true, 64u * 1024u * 1024u);
            if (!rules.Succeeded() || !rules.value) return {rules.diagnostics};
            const Services::MapDataQueryBinding document{"ground-append-project", 1, 0, project.archiveSession_->SourceSha256()};
            const Services::TerrainEditBinding binding{document, project.mapEdits_->SessionIdentity(), project.mapEdits_->Revision(), project.mapEdits_->TerrainSourceRevision()};
            MapData::TerrainWaterStrokeOptions options; options.shape = MapData::TerrainBrushShape::Square; options.randomSeed = 1;
            const MapData::TerrainStrokePoint point{256, 256};
            const auto prepared = Services::PrepareTerrainWaterEdit(*project.mapEdits_, document, binding, *rules.value,
                options, std::span(&point, 1), false);
            if (!prepared.Succeeded() || !prepared.value) return {prepared.diagnostics};
            const auto applied = Services::ApplyTerrainEdit(*project.mapEdits_, document, *prepared.value);
            if (!applied.Succeeded()) return {applied.diagnostics};
            if (!project.ObserveMapDataEdit("地面追加前后删除特殊装饰物")) return Status::Failure("Test", "OBSERVE_FAILED", "水刷观察失败。");
            PushMapDataEdit(project, "Water with special removal", "水刷删除特殊装饰物"); return Status::Ok();
        };
        const auto first = appendFirst ? append() : water(); QVERIFY2(first.Succeeded(), ReadDiagnosticText(first).c_str());
        const auto intermediate = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(intermediate.Succeeded());
        intermediateTerrain = *intermediate.value;
        const auto second = appendFirst ? water() : append(); QVERIFY2(second.Succeeded(), ReadDiagnosticText(second).c_str());
        QCOMPARE(undo.count(), 2); QCOMPARE(project.mapEdits_->Terrain()->header.groundTilesets,
            (std::vector<std::string>{"Tg01", "Tg02"}));
        QCOMPARE(project.mapEdits_->Snapshot().Doodads()->Serialize().value, deletedWire.value);
        const auto final = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(final.Succeeded()); finalTerrain = *final.value;
        finalDraft = TerrainJournalDraftBytes(*project.mapEdits_);
        const auto saved = project.SaveProjectToPath(forge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
        QVERIFY(!HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY"));
        const auto archive = MapArchiveSession::Open(forge); QVERIFY(archive.Succeeded());
        const auto wire = archive.value->ReadMember("overlay/editor/terrain-strokes.bin"); QVERIFY(wire.Succeeded());
        QCOMPARE(wire.value->at(3), std::byte{'6'});
        const auto decoded = MapData::DecodeTerrainStrokeJournal(*wire.value); QVERIFY(decoded.Succeeded());
        QCOMPARE(decoded.value->strokes.size(), std::size_t{2});
        const auto& palette = decoded.value->strokes[appendFirst ? 0 : 1].intent.palette;
        QVERIFY(palette && palette->algorithm == 2 && palette->slot == 1 && palette->expectedRawcode.empty());
        undo.clear();
    } // 丢弃热会话，不能把原有special来源指针带入磁盘恢复。
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{2});
    QVERIFY((*opened.value)->terrainJournalWarning.empty());
    const auto forgeAgain = std::filesystem::path(root.filePath("ground-append-again.forge").toStdWString());
    {
        EditorProject cold; QUndoStack undo; cold.mapDataUndoStack_ = &undo;
        QVERIFY(BindPreparedTerrainJournal(cold, **opened.value, game)); QCOMPARE(undo.count(), 2);
        const auto scope = cold.mapEdits_->TerrainJournalStateFingerprint();
        QCOMPARE(TerrainJournalDraftBytes(*cold.mapEdits_), finalDraft);
        undo.undo(); QCOMPARE(MapData::SerializeMapTerrain(*cold.mapEdits_->Terrain()).value, std::optional(intermediateTerrain));
        undo.undo(); QVERIFY(!cold.mapEdits_->IsDirty());
        QCOMPARE(MapData::SerializeMapTerrain(*cold.mapEdits_->Terrain()).value, std::optional(member("war3map.w3e")));
        QCOMPARE(cold.mapEdits_->Snapshot().Doodads()->Serialize().value, std::optional(member("war3map.doo")));
        undo.redo(); undo.redo(); QCOMPARE(TerrainJournalDraftBytes(*cold.mapEdits_), finalDraft);
        QCOMPARE(cold.mapEdits_->TerrainJournalStateFingerprint(), scope);
        const auto saved = cold.SaveProjectToPath(forgeAgain); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
        QVERIFY(!HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY")); undo.clear();
    }
    auto reopened = PrepareProjectOpen(forgeAgain, game); QVERIFY2(reopened.Succeeded(), ReadDiagnosticText(reopened).c_str());
    QVERIFY(HasReadDiagnostic(reopened, "TERRAIN_JOURNAL_RESTORED"));
    EditorProject restored; QUndoStack undo; restored.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(restored, **reopened.value, game)); QCOMPARE(undo.count(), 2);
    const auto session = restored.mapEdits_->SessionIdentity();
    for (unsigned phase = 0; phase < 3; ++phase) {
        if (phase == 1) undo.undo();
        if (phase == 2) undo.redo();
        const bool water = phase != 1 || !appendFirst;
        const auto& expectedTerrain = phase == 1 ? intermediateTerrain : finalTerrain;
        const auto& expectedDoodads = water ? *deletedWire.value : member("war3map.doo");
        auto expectedPathing = member("war3map.wpm");
        if (water) {
            for (unsigned y = 4; y < 12; ++y) for (unsigned x = 4; x < 12; ++x) expectedPathing[16 + y * 32 + x] = std::byte{0xdb};
            for (unsigned y = 6; y < 10; ++y) for (unsigned x = 6; x < 10; ++x) expectedPathing[16 + y * 32 + x] = std::byte{0x1b};
        }
        const auto beforeSha = Sha256File(restored.archiveSession_->SourcePath()); QVERIFY(beforeSha.Succeeded());
        const auto target = inPlace ? restored.archiveSession_->SourcePath()
            : std::filesystem::path(root.filePath(QString("ground-append-saved-%1.w3x").arg(phase)).toStdWString());
        auto job = FreezeNativePathingQtJob(restored, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = restored.mapEdits_->FreezeForSave();
        if (inPlace) {
            job->inPlace = true; job->inPlaceOptions.backupDirectory =
                std::filesystem::path(root.filePath(QString("backups-%1").arg(phase)).toStdWString());
            QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
        }
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded());
        QCOMPARE(actual.value->ReadMember("war3map.w3e").value, std::optional(expectedTerrain));
        QCOMPARE(actual.value->ReadMember("war3map.wpm").value, std::optional(expectedPathing));
        QCOMPARE(actual.value->ReadMember("war3map.doo").value, std::optional(expectedDoodads));
        for (const auto& original : members) if (original.name != "war3map.w3e" && original.name != "war3map.wpm" && original.name != "war3map.doo")
            QCOMPARE(actual.value->ReadMember(original.name).value, std::optional(original.data));
        QVERIFY(restored.CommitMapSave(*job)); QVERIFY(!restored.HasUnsavedMapChanges());
        QCOMPARE(restored.mapEdits_->SessionIdentity(), session); QCOMPARE(undo.count(), 2); QCOMPARE(undo.index(), phase == 1 ? 1 : 2);
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, beforeSha.value); }
        else QCOMPARE(Sha256File(source).value, sourceSha.value);
    }
}
