#pragma once

#include "../TerrainWetInnerCornerRampFixtures.h"
#include "War3MapCore/MapData/TerrainEditInspection.h"

void EditorProjectTests::WetInnerCornerRampProjectColdRestoreAndNativeSave_data() {
    QTest::addColumn<bool>("inPlace"); QTest::addColumn<bool>("follow"); QTest::addColumn<bool>("mixed");
    for (const bool inPlace : {false, true}) for (const bool follow : {false, true}) for (const bool mixed : {false, true}) {
        const auto name = QByteArray(inPlace ? "in-place" : "save-as") + (follow ? "-follow" : "-fixed") + (mixed ? "-mixed-deep" : "-all-equality");
        QTest::newRow(name.constData()) << inPlace << follow << mixed;
    }
}

void EditorProjectTests::WetInnerCornerRampProjectColdRestoreAndNativeSave() {
    QFETCH(bool, inPlace); QFETCH(bool, follow); QFETCH(bool, mixed);
    namespace S = War3Map::TerrainWetInnerCornerRampFixtures;
    QTemporaryDir projectFiles; QVERIFY(projectFiles.isValid());
    S::Fixture fixture({mixed ? S::Threshold::DeepEquality : S::Threshold::SurfaceEquality, mixed});
    const auto source = fixture.source->SourcePath(), game = fixture.game;
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto originalScript = fixture.source->ReadMember("war3map.j"); QVERIFY(originalScript.Succeeded());
    const auto forge = std::filesystem::path(projectFiles.filePath("wet-inner-corner.forge").toStdWString());
    {
        EditorProject project; project.war3GameRoot_ = game;
        QVERIFY(BindTerrainSaveProject(project, source));
        QUndoStack originalUndo; project.mapDataUndoStack_ = &originalUndo;
        const auto snapshot = project.mapEdits_->Snapshot(); QVERIFY(snapshot.Doodads());
        const auto rules = Services::TerrainPathingSave::CaptureWaterBrushRules(*project.archiveSession_, fixture.grid,
            *project.mapEdits_->Terrain(), *snapshot.Doodads(), game, true, S::kBudget);
        QVERIFY2(rules.Succeeded(), ReadDiagnosticText(rules).c_str());
        const auto frozen = project.mapEdits_->FreezeForSave(S::kBudget); QVERIFY(frozen.Succeeded());
        MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::CornerRamp; brush.value = true;
        const auto prepared = (*frozen.value)->PrepareTerrainCornerRampEdit(*rules.value, brush,
            std::span(&fixture.point, 1), follow);
        QVERIFY2(prepared.Succeeded(), ReadDiagnosticText(prepared).c_str());
        QVERIFY(project.mapEdits_->ApplyPreparedTerrainEdit(*prepared.value).Succeeded());
        QVERIFY(project.ObserveMapDataEdit("含水内角坡道工程夹具"));
        PushMapDataEdit(project, "Wet inner corner ramp", "含水内角坡道");
        QCOMPARE(originalUndo.count(), 1); fixture.CheckState(*project.mapEdits_, true, follow);
        const auto saved = project.SaveProjectToPath(forge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
        QCOMPARE(Sha256File(source).value, sourceSha.value);
    }
    // 原项目、热命令及原规则owner已销毁；从实际.forge重新读取全部材料。
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{1});
    EditorProject restored; QUndoStack undo; restored.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(restored, **opened.value, game));
    QCOMPARE(undo.count(), 1); QCOMPARE(undo.index(), 1); fixture.CheckState(*restored.mapEdits_, true, follow);
    const auto journal = Services::TerrainPathingSave::CaptureJournal(*restored.archiveSession_, *restored.mapEdits_);
    QVERIFY2(journal.Succeeded(), ReadDiagnosticText(journal).c_str());
    QCOMPARE(journal.value->strokes.size(), std::size_t{1});
    QVERIFY(journal.value->strokes[0].intent.cornerRampAlgorithm == MapData::TerrainCornerRampAlgorithm::NativeWetCorners);
    QCOMPARE(journal.value->strokes[0].intent.followDoodads, follow);
    const auto wire = MapData::EncodeTerrainStrokeJournal(*journal.value); QVERIFY(wire.Succeeded());
    QCOMPARE(wire.value->at(3), std::byte{'J'});
    undo.undo(); fixture.CheckState(*restored.mapEdits_, false, follow);
    undo.redo(); fixture.CheckState(*restored.mapEdits_, true, follow);
    const auto forgeAs = std::filesystem::path(projectFiles.filePath("wet-inner-corner-copy.forge").toStdWString());
    const auto savedAs = restored.SaveProjectToPath(forgeAs); QVERIFY2(savedAs.Succeeded(), ReadDiagnosticText(savedAs).c_str());
    auto reopened = PrepareProjectOpen(forgeAs, game); QVERIFY2(reopened.Succeeded(), ReadDiagnosticText(reopened).c_str());
    QCOMPARE((*reopened.value)->terrainReceipts.size(), std::size_t{1});
    fixture.CheckState(*(*reopened.value)->mapDataEdits, true, follow);

    // 只使用现有脚本准备夹具；真实派生计划、MPQ保存、完整回读和前台提交仍执行。
    const auto identity = restored.mapEdits_->SessionIdentity();
    for (int state = 0; state < 3; ++state) {
        if (state == 1) undo.undo();
        if (state == 2) undo.redo();
        const bool created = state != 1; fixture.CheckState(*restored.mapEdits_, created, follow);
        const auto target = inPlace ? source : fixture.root / ("wet-inner-corner-saved-" + std::to_string(state) + ".w3x");
        const auto beforeSha = restored.archiveSession_->SourceSha256();
        auto job = FreezeNativePathingQtJob(restored, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = restored.mapEdits_->FreezeForSave(); job->minimapGameRoot = game;
        job->inPlace = inPlace; job->inPlaceOptions.backupDirectory = fixture.root / "backups";
        auto history = restored.mapEdits_->PrepareTerrainHistoryForSave();
        if (state == 0) {
            QVERIFY2(history.Succeeded() && history.value, ReadDiagnosticText(history).c_str());
            job->terrainHistorySaveLease = std::move(*history.value);
        } else {
            QVERIFY(!history.Succeeded() && HasReadDiagnostic(history, "TERRAIN_HISTORY_EPOCH_LIMIT"));
            job->terrainHistoryWarning = "草稿已完整保留；当前地形历史不能绑定到一座保存站，跨保存点撤销的冷恢复不可用。"
                "（TERRAIN_HISTORY_EPOCH_LIMIT）";
        }
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded());
        QCOMPARE(actual.value->ReadMember("war3map.w3e").value, std::optional(fixture.TerrainLiteral(created)));
        QCOMPARE(actual.value->ReadMember("war3map.wpm").value, std::optional(fixture.PathingLiteral(created)));
        QCOMPARE(actual.value->ReadMember("war3mapUnits.doo").value, fixture.UnitsLiteral(created && follow).Serialize().value);
        QCOMPARE(actual.value->ReadMember("war3map.doo").value, fixture.DoodadsLiteral(created && follow).Serialize().value);
        QCOMPARE(actual.value->ReadMember("war3map.j").value, originalScript.value);
        QVERIFY(restored.CommitMapSave(*job));
        QCOMPARE(restored.mapEdits_->SessionIdentity(), identity);
        QVERIFY(!restored.HasUnsavedMapChanges() && restored.mapEdits_->Replacements().empty());
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, std::optional(beforeSha)); }
        else QCOMPARE(Sha256File(source).value, sourceSha.value);
        QCOMPARE(undo.count(), 1);
        if (state == 0) {
            const auto savedHistory = Services::TerrainPathingSave::CaptureJournal(*restored.archiveSession_, *restored.mapEdits_);
            QVERIFY2(savedHistory.Succeeded(), ReadDiagnosticText(savedHistory).c_str());
            const auto savedWire = MapData::EncodeTerrainStrokeJournal(*savedHistory.value); QVERIFY(savedWire.Succeeded());
            // 正式Qt保存绑定旧SHD的明确absence，外层仍使用现有I，不占用J。
            QCOMPARE(savedWire.value->at(3), std::byte{'I'});
            const auto baseline = MapData::TerrainHistoryBaselineJournal(*savedHistory.value); QVERIFY(baseline.Succeeded());
            const auto typedWire = MapData::EncodeTerrainStrokeJournal(*baseline.value); QVERIFY(typedWire.Succeeded());
            QCOMPARE(typedWire.value->at(3), std::byte{'J'});
        }
        if (state == 1) {
            QCOMPARE(undo.index(), 0); QVERIFY(!undo.canUndo() && undo.canRedo());
            const auto exported = restored.mapEdits_->ExportTerrainHistoryJournal();
            QVERIFY(!exported.Succeeded() && HasReadDiagnostic(exported, "TERRAIN_HISTORY_EPOCH_LIMIT"));
            const auto draft = TerrainJournalDraftBytes(*restored.mapEdits_);
            const auto restrictions = restored.mapEdits_->SerializePathingRestrictionDraft(); QVERIFY(restrictions.Succeeded());
            const auto fallbackForge = std::filesystem::path(projectFiles.filePath("wet-inner-second-save-draft.forge").toStdWString());
            const auto draftSaved = restored.SaveProjectToPath(fallbackForge);
            QVERIFY2(draftSaved.Succeeded(), ReadDiagnosticText(draftSaved).c_str());
            QVERIFY(HasReadDiagnostic(draftSaved, "TERRAIN_JOURNAL_DRAFT_ONLY"));
            const auto container = MapArchiveSession::Open(fallbackForge); QVERIFY(container.Succeeded());
            QVERIFY(!container.value->HasMember("overlay/editor/terrain-strokes.bin"));
            auto cold = PrepareProjectOpen(fallbackForge, game); QVERIFY2(cold.Succeeded(), ReadDiagnosticText(cold).c_str());
            QVERIFY(HasReadDiagnostic(cold, "TERRAIN_JOURNAL_DRAFT_ONLY"));
            QVERIFY((*cold.value)->terrainReceipts.empty() && !(*cold.value)->terrainSavedIndex);
            QCOMPARE(TerrainJournalDraftBytes(*(*cold.value)->mapDataEdits), draft);
            QCOMPARE((*cold.value)->mapDataEdits->SerializePathingRestrictionDraft().value, restrictions.value);
            EditorProject coldProject; QUndoStack coldUndo; coldProject.mapDataUndoStack_ = &coldUndo;
            QVERIFY(BindPreparedTerrainJournal(coldProject, **cold.value, game));
            QCOMPARE(coldUndo.count(), 0); QVERIFY(!coldUndo.canUndo() && !coldUndo.canRedo());
            fixture.CheckState(*coldProject.mapEdits_, false, follow);
            const auto inspection = MapData::CaptureTerrainEditInspection(*coldProject.archiveSession_, *coldProject.mapEdits_);
            QVERIFY2(inspection.Succeeded(), ReadDiagnosticText(inspection).c_str());
            QVERIFY(inspection.value->Members()[0].current && inspection.value->Members()[1].current);
            QCOMPARE(*inspection.value->Members()[0].current, fixture.TerrainLiteral(false));
            QCOMPARE(*inspection.value->Members()[1].current, fixture.PathingLiteral(false));
            QCOMPARE(restored.mapEdits_->SessionIdentity(), identity); QCOMPARE(undo.count(), 1); QCOMPARE(undo.index(), 0);
            undo.redo(); fixture.CheckState(*restored.mapEdits_, true, follow);
            undo.undo(); fixture.CheckState(*restored.mapEdits_, false, follow);
            QVERIFY(undo.canRedo()); QCOMPARE(Sha256File(target).value, std::optional(actual.value->SourceSha256()));
        }
    }
}
