#pragma once

#include "../TerrainInnerCornerRampFixtures.h"

void EditorProjectTests::InnerCornerRampProjectColdRestoreAndNativeSave_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::newRow("inner-corner-save-as") << false;
    QTest::newRow("inner-corner-save-in-place") << true;
}

void EditorProjectTests::InnerCornerRampProjectColdRestoreAndNativeSave() {
    QFETCH(bool, inPlace);
    namespace S = War3Map::TerrainInnerCornerRampFixtures;
    QTemporaryDir projectFiles; QVERIFY(projectFiles.isValid());
    S::Fixture fixture;
    const auto source = fixture.source->SourcePath(), game = fixture.game;
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto originalScript = fixture.source->ReadMember("war3map.j"); QVERIFY(originalScript.Succeeded());
    EditorProject project; project.war3GameRoot_ = game;
    QVERIFY(BindTerrainSaveProject(project, source));
    QUndoStack originalUndo; project.mapDataUndoStack_ = &originalUndo;
    const auto snapshot = project.mapEdits_->Snapshot(); QVERIFY(snapshot.Doodads());
    const auto rules = Services::TerrainPathingSave::CaptureWaterBrushRules(*project.archiveSession_,
        fixture.grid, *project.mapEdits_->Terrain(), *snapshot.Doodads(), game, true, S::kBudget);
    QVERIFY2(rules.Succeeded(), ReadDiagnosticText(rules).c_str());
    const auto frozen = project.mapEdits_->FreezeForSave(S::kBudget); QVERIFY(frozen.Succeeded());
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::CornerRamp; brush.value = true;
    const auto prepared = (*frozen.value)->PrepareTerrainCornerRampEdit(*rules.value, brush,
        std::span(&fixture.point, 1), false, {}, MapData::TerrainCornerRampAlgorithm::NativeCorners);
    QVERIFY2(prepared.Succeeded(), ReadDiagnosticText(prepared).c_str());
    QVERIFY(project.mapEdits_->ApplyPreparedTerrainEdit(*prepared.value).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("内角坡道工程夹具"));
    PushMapDataEdit(project, "Inner corner ramp", "内角坡道");
    QCOMPARE(originalUndo.count(), 1); fixture.CheckState(*project.mapEdits_, true);
    const auto forge = std::filesystem::path(projectFiles.filePath("inner-corner.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(forge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
    QCOMPARE(Sha256File(source).value, sourceSha.value);

    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{1});
    EditorProject restored; QUndoStack undo; restored.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(restored, **opened.value, game));
    QCOMPARE(undo.count(), 1); QCOMPARE(undo.index(), 1); fixture.CheckState(*restored.mapEdits_, true);
    const auto journal = Services::TerrainPathingSave::CaptureJournal(*restored.archiveSession_, *restored.mapEdits_);
    QVERIFY2(journal.Succeeded(), ReadDiagnosticText(journal).c_str());
    QCOMPARE(journal.value->strokes.size(), std::size_t{1});
    QVERIFY(journal.value->strokes[0].intent.cornerRampAlgorithm == MapData::TerrainCornerRampAlgorithm::NativeCorners);
    const auto wire = MapData::EncodeTerrainStrokeJournal(*journal.value); QVERIFY(wire.Succeeded());
    QCOMPARE(wire.value->at(3), std::byte{'F'});
    undo.undo(); fixture.CheckState(*restored.mapEdits_, false);
    undo.redo(); fixture.CheckState(*restored.mapEdits_, true);
    const auto forgeAs = std::filesystem::path(projectFiles.filePath("inner-corner-copy.forge").toStdWString());
    const auto savedAs = restored.SaveProjectToPath(forgeAs); QVERIFY2(savedAs.Succeeded(), ReadDiagnosticText(savedAs).c_str());
    auto reopened = PrepareProjectOpen(forgeAs, game); QVERIFY2(reopened.Succeeded(), ReadDiagnosticText(reopened).c_str());
    QCOMPARE((*reopened.value)->terrainReceipts.size(), std::size_t{1});
    fixture.CheckState(*(*reopened.value)->mapDataEdits, true);

    // 沿现有Qt脚本夹具，仅替代脚本准备；真实派生、MPQ写出、回读和前台提交均执行。
    const auto identity = restored.mapEdits_->SessionIdentity();
    for (int state = 0; state < 3; ++state) {
        if (state == 1) undo.undo();
        if (state == 2) undo.redo();
        const bool created = state != 1; fixture.CheckState(*restored.mapEdits_, created);
        const auto target = inPlace ? source : fixture.root / ("inner-corner-saved-" + std::to_string(state) + ".w3x");
        const auto beforeSha = restored.archiveSession_->SourceSha256();
        auto job = FreezeNativePathingQtJob(restored, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = restored.mapEdits_->FreezeForSave(); job->minimapGameRoot = game;
        job->inPlace = inPlace; job->inPlaceOptions.backupDirectory = fixture.root / "backups";
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded());
        QCOMPARE(actual.value->ReadMember("war3map.w3e").value, std::optional(fixture.TerrainLiteral(created)));
        QCOMPARE(actual.value->ReadMember("war3map.wpm").value, std::optional(S::Fixture::PathingLiteral(created)));
        QCOMPARE(actual.value->ReadMember("war3mapUnits.doo").value, fixture.units.Serialize().value);
        QCOMPARE(actual.value->ReadMember("war3map.doo").value, fixture.doodads.Serialize().value);
        QCOMPARE(actual.value->ReadMember("war3map.j").value, originalScript.value);
        QVERIFY(restored.CommitMapSave(*job));
        QCOMPARE(restored.mapEdits_->SessionIdentity(), identity);
        QVERIFY(!restored.HasUnsavedMapChanges() && restored.mapEdits_->Replacements().empty());
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, std::optional(beforeSha)); }
        else QCOMPARE(Sha256File(source).value, sourceSha.value);
        QCOMPARE(undo.count(), 1);
    }
}
