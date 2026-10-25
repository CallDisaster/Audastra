#pragma once

#include "../TerrainConnectedRampFixtures.h"

void EditorProjectTests::ConnectedRampProjectColdRestoreAndNativeSave_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::addColumn<bool>("deep");
    QTest::addColumn<bool>("remove");
    QTest::newRow("save-as-connect-surface") << false << false << false;
    QTest::newRow("in-place-connect-surface") << true << false << false;
    QTest::newRow("save-as-remove-deep") << false << true << true;
    QTest::newRow("in-place-remove-deep") << true << true << true;
}

void EditorProjectTests::ConnectedRampProjectColdRestoreAndNativeSave() {
    QFETCH(bool, inPlace); QFETCH(bool, deep); QFETCH(bool, remove);
    namespace S = War3Map::TerrainConnectedRampFixtures;
    QTemporaryDir projectFiles; QVERIFY(projectFiles.isValid());
    S::ConnectedFixture fixture(deep ? S::SurfaceCase::DeepEquality : S::SurfaceCase::SurfaceEquality, remove);
    const auto source = fixture.source->SourcePath(), game = fixture.game;
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto originalScript = fixture.source->ReadMember("war3map.j"); QVERIFY(originalScript.Succeeded());
    EditorProject project; project.war3GameRoot_ = game;
    QVERIFY(BindTerrainSaveProject(project, source));
    QUndoStack originalUndo; project.mapDataUndoStack_ = &originalUndo;
    const auto apply = [&](bool create) {
        const auto snapshot = project.mapEdits_->Snapshot(); QVERIFY(snapshot.Doodads());
        const auto rules = Services::TerrainPathingSave::CaptureWaterBrushRules(*project.archiveSession_,
            fixture.grid, *project.mapEdits_->Terrain(), *snapshot.Doodads(), game, true, S::kBudget);
        QVERIFY2(rules.Succeeded(), ReadDiagnosticText(rules).c_str());
        const auto frozen = project.mapEdits_->FreezeForSave(S::kBudget); QVERIFY(frozen.Succeeded());
        const auto prepared = (*frozen.value)->PrepareTerrainStraightRampEdit(*rules.value, S::StraightBrush(create),
            std::span(&fixture.point, 1), true, {}, MapData::TerrainStraightRampAlgorithm::Connected);
        QVERIFY2(prepared.Succeeded(), ReadDiagnosticText(prepared).c_str());
        QVERIFY(project.mapEdits_->ApplyPreparedTerrainEdit(*prepared.value).Succeeded());
        QVERIFY(project.ObserveMapDataEdit("连接直坡工程夹具"));
        PushMapDataEdit(project, "Connected ramp", "连接直坡及摆放联动");
        fixture.CheckState(*project.mapEdits_, create);
    };
    apply(!remove);
    constexpr int count = 1; QCOMPARE(originalUndo.count(), count);
    const auto forge = std::filesystem::path(projectFiles.filePath("connected.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(forge);
    QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
    QCOMPARE(Sha256File(source).value, sourceSha.value);

    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t(count));
    EditorProject restored; QUndoStack undo; restored.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(restored, **opened.value, game));
    QCOMPARE(undo.count(), count); QCOMPARE(undo.index(), count); fixture.CheckState(*restored.mapEdits_, !remove);
    const auto journal = Services::TerrainPathingSave::CaptureJournal(*restored.archiveSession_, *restored.mapEdits_);
    QVERIFY2(journal.Succeeded(), ReadDiagnosticText(journal).c_str());
    QCOMPARE(journal.value->strokes.size(), std::size_t(count));
    for (const auto& stroke : journal.value->strokes)
        QVERIFY(stroke.intent.straightRampAlgorithm == MapData::TerrainStraightRampAlgorithm::Connected);
    const auto wire = MapData::EncodeTerrainStrokeJournal(*journal.value); QVERIFY(wire.Succeeded());
    QCOMPARE(wire.value->at(3), std::byte{'C'});
    undo.undo(); fixture.CheckState(*restored.mapEdits_, remove);
    undo.redo(); fixture.CheckState(*restored.mapEdits_, !remove);
    const auto forgeAs = std::filesystem::path(projectFiles.filePath("connected-copy.forge").toStdWString());
    const auto savedAs = restored.SaveProjectToPath(forgeAs); QVERIFY2(savedAs.Succeeded(), ReadDiagnosticText(savedAs).c_str());
    auto reopened = PrepareProjectOpen(forgeAs, game); QVERIFY(reopened.Succeeded());
    QCOMPARE((*reopened.value)->terrainReceipts.size(), std::size_t(count));
    fixture.CheckState(*(*reopened.value)->mapDataEdits, !remove);

    const auto session = restored.mapEdits_->SessionIdentity();
    for (int state = 0; state < 3; ++state) {
        if (state == 1) undo.undo();
        if (state == 2) undo.redo();
        const bool created = state == 1 ? remove : !remove; fixture.CheckState(*restored.mapEdits_, created);
        const auto target = inPlace ? source : fixture.root / ("connected-saved-" + std::to_string(state) + ".w3x");
        const auto beforeSha = restored.archiveSession_->SourceSha256();
        auto job = FreezeNativePathingQtJob(restored, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = restored.mapEdits_->FreezeForSave(); job->minimapGameRoot = game;
        job->inPlace = inPlace; job->inPlaceOptions.backupDirectory = fixture.root / "backups";
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded());
        QCOMPARE(actual.value->ReadMember("war3map.w3e").value, std::optional(fixture.TerrainLiteral(created)));
        QCOMPARE(actual.value->ReadMember("war3map.wpm").value, std::optional(S::ConnectedPathingLiteral(fixture.surface, created)));
        QCOMPARE(actual.value->ReadMember("war3mapUnits.doo").value, fixture.UnitsLiteral(created).Serialize().value);
        QCOMPARE(actual.value->ReadMember("war3map.doo").value, fixture.DoodadsLiteral(created).Serialize().value);
        QCOMPARE(actual.value->ReadMember("war3map.j").value, originalScript.value);
        QVERIFY(restored.CommitMapSave(*job));
        QCOMPARE(restored.mapEdits_->SessionIdentity(), session);
        QVERIFY(!restored.HasUnsavedMapChanges() && restored.mapEdits_->Replacements().empty());
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, std::optional(beforeSha)); }
        else QCOMPARE(Sha256File(source).value, sourceSha.value);
        QCOMPARE(undo.count(), count);
    }
}
