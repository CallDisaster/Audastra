#pragma once

#include "../support/TerrainResizeFixture.h"
#include "War3MapCore/Services/TerrainResizeEditService.h"

void EditorProjectTests::ResizeProjectColdRestoreAndNativeSave_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::newRow("resize-project-save-as") << false;
    QTest::newRow("resize-project-save-in-place") << true;
}

void EditorProjectTests::ResizeProjectColdRestoreAndNativeSave() {
    namespace R = AudastraTest::TerrainResize;
    QFETCH(bool, inPlace);
    QTemporaryDir outputs; QVERIFY(outputs.isValid());
    const auto outputRoot = std::filesystem::path(outputs.path().toStdWString());
    const auto mapRoot = outputRoot / "maps"; QVERIFY(std::filesystem::create_directory(mapRoot));
    R::Fixture fixture(true); const auto source = fixture.archive->SourcePath();
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const std::array<MapArchiveMember, 1> gameMembers{{{"fixture.txt", R::Text("owned resize resources")}}};
    QVERIFY(WriteVerifiedArchive(fixture.gameRoot / "war3.mpq", gameMembers).Succeeded());
    EditorProject project; project.war3GameRoot_ = fixture.gameRoot;
    QVERIFY(BindTerrainSaveProject(project, source)); QUndoStack initialUndo; project.mapDataUndoStack_ = &initialUndo;
    auto resources = Services::TerrainResizeEditService::Capture(*project.archiveSession_, *project.mapEdits_, fixture.gameRoot);
    QVERIFY2(resources.Succeeded(), ReadDiagnosticText(resources).c_str());
    const auto prepared = project.mapEdits_->PrepareTerrainResizeEdit(*resources.value, {1, {-32, -32, 32, 32}});
    QVERIFY2(prepared.Succeeded(), ReadDiagnosticText(prepared).c_str());
    QVERIFY(project.mapEdits_->ApplyPreparedTerrainResizeEdit(*prepared.value).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("扩大地图尺寸")); PushMapDataEdit(project, "Resize canvas", "扩大地图尺寸");
    const auto expectedTerrain = R::TerrainLiteral(-32, -32, 32, 32);
    const auto expectedPathing = R::PathingLiteral(-32, -32, 32, 32);
    const auto expectedShadow = R::ShadowLiteral(-32, -32, 32, 32, true);
    const auto originalShadow = R::ShadowLiteral();
    const auto shadowDifference = [](const R::Bytes& actual, const R::Bytes& expected) {
        const auto common = (std::min)(actual.size(), expected.size());
        std::size_t first = 0;
        while (first < common && actual[first] == expected[first]) ++first;
        auto text = QStringLiteral("SHD 字节不一致：实际长度=%1，预期长度=%2，首差异=%3")
            .arg(static_cast<qulonglong>(actual.size())).arg(static_cast<qulonglong>(expected.size()))
            .arg(static_cast<qulonglong>(first));
        if (first < common) text += QStringLiteral("，实际值=%1，预期值=%2")
            .arg(std::to_integer<unsigned>(actual[first])).arg(std::to_integer<unsigned>(expected[first]));
        return text;
    };
    QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, std::optional(expectedTerrain));
    const auto finalDraft = TerrainJournalDraftBytes(*project.mapEdits_);
    const auto capsule = project.mapEdits_->ExportTerrainResizeDraft();
    QVERIFY(capsule.Succeeded() && !capsule.value->empty());
    const auto forge = outputRoot / "resize.forge";
    const auto saved = project.SaveProjectToPath(forge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
    QCOMPARE(Sha256File(source).value, sourceSha.value);
    auto opened = PrepareProjectOpen(forge, fixture.gameRoot);
    QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{1});
    QCOMPARE(TerrainJournalDraftBytes(*(*opened.value)->mapDataEdits), finalDraft);
    QCOMPARE((*opened.value)->mapDataEdits->ExportTerrainResizeDraft().value, capsule.value);
    EditorProject restored; QUndoStack undo; restored.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(restored, **opened.value, fixture.gameRoot));
    QCOMPARE(undo.count(), 1); QCOMPARE(undo.index(), 1);
    undo.undo();
    QCOMPARE(MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value, std::optional(R::TerrainLiteral()));
    QCOMPARE(restored.mapEdits_->Snapshot().Settings()->Serialize().value, fixture.settings.Serialize().value);
    const auto coldUndoShadow = restored.mapEdits_->TerrainResizeShadowDraft();
    QVERIFY2(coldUndoShadow.Succeeded() && coldUndoShadow.value, ReadDiagnosticText(coldUndoShadow).c_str());
    QVERIFY(coldUndoShadow.value->present);
    QVERIFY2(coldUndoShadow.value->bytes == originalShadow,
        qPrintable(shadowDifference(coldUndoShadow.value->bytes, originalShadow)));
    QVERIFY(!restored.mapEdits_->IsDirty());
    undo.redo(); QCOMPARE(TerrainJournalDraftBytes(*restored.mapEdits_), finalDraft);
    const auto forgeAs = outputRoot / "resize-copy.forge";
    const auto copied = restored.SaveProjectToPath(forgeAs); QVERIFY2(copied.Succeeded(), ReadDiagnosticText(copied).c_str());
    auto reopened = PrepareProjectOpen(forgeAs, fixture.gameRoot);
    QVERIFY2(reopened.Succeeded(), ReadDiagnosticText(reopened).c_str());
    QCOMPARE(TerrainJournalDraftBytes(*(*reopened.value)->mapDataEdits), finalDraft);

    // 仅隔离脚本工具准备；归档派生、真实写盘回读、保存后原Undo仍走产品代码。
    // 此测试不替代root单独执行的真实工具链专项、原版重开或游戏验收。
    const auto session = restored.mapEdits_->SessionIdentity();
    for (int state = 0; state < 3; ++state) {
        if (state == 1) undo.undo();
        if (state == 2) undo.redo();
        const bool expanded = state != 1;
        // 热Undo必须保留原盘的完整强度字节，包括01/02/80；写盘规范化只作用于独立期望副本。
        const auto& expectedHotShadow = expanded ? expectedShadow : originalShadow;
        const auto hotShadow = restored.mapEdits_->TerrainResizeShadowDraft();
        QVERIFY2(hotShadow.Succeeded() && hotShadow.value, ReadDiagnosticText(hotShadow).c_str());
        QVERIFY(hotShadow.value->present);
        QVERIFY2(hotShadow.value->bytes == expectedHotShadow,
            qPrintable(shadowDifference(hotShadow.value->bytes, expectedHotShadow)));
        auto expectedWrittenShadow = expectedHotShadow;
        for (auto& value : expectedWrittenShadow) if (value != std::byte{0}) value = std::byte{0xff};
        if (!expanded) QVERIFY(expectedWrittenShadow != expectedHotShadow);
        const auto target = inPlace ? source : mapRoot / ("resize-saved-" + std::to_string(state) + ".w3x");
        const auto beforeSha = restored.archiveSession_->SourceSha256();
        auto job = FreezeNativePathingQtJob(restored, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = restored.mapEdits_->FreezeForSave(); job->minimapGameRoot = fixture.gameRoot;
        job->inPlace = inPlace; job->inPlaceOptions.backupDirectory = mapRoot / "backups";
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded());
        QCOMPARE(actual.value->ReadMember("war3map.w3e").value,
            std::optional(expanded ? expectedTerrain : R::TerrainLiteral()));
        QCOMPARE(actual.value->ReadMember("war3map.wpm").value,
            std::optional(expanded ? expectedPathing : R::PathingLiteral()));
        const auto writtenShadow = actual.value->ReadMember("war3map.shd");
        QVERIFY2(writtenShadow.Succeeded() && writtenShadow.value, ReadDiagnosticText(writtenShadow).c_str());
        QVERIFY2(*writtenShadow.value == expectedWrittenShadow,
            qPrintable(shadowDifference(*writtenShadow.value, expectedWrittenShadow)));
        auto expectedSettings = fixture.settings;
        if (expanded) expectedSettings.cameraMargins = {33, 2, 33, 2};
        QCOMPARE(actual.value->ReadMember("war3map.w3i").value, expectedSettings.Serialize().value);
        for (const auto* name : {"war3mapUnits.doo", "war3map.doo", "war3map.w3r", "war3map.w3c", "war3mapMap.tga"})
            QCOMPARE(actual.value->ReadMember(name).value, fixture.archive->ReadMember(name).value);
        QVERIFY(restored.CommitMapSave(*job));
        QCOMPARE(restored.mapEdits_->SessionIdentity(), session); QCOMPARE(undo.count(), 1);
        QVERIFY(!restored.HasUnsavedMapChanges() && restored.mapEdits_->Replacements().empty());
        const auto committedShadow = restored.mapEdits_->TerrainResizeShadowDraft();
        QVERIFY2(committedShadow.Succeeded() && committedShadow.value, ReadDiagnosticText(committedShadow).c_str());
        QVERIFY(committedShadow.value->present);
        QVERIFY2(committedShadow.value->bytes == expectedWrittenShadow,
            qPrintable(shadowDifference(committedShadow.value->bytes, expectedWrittenShadow)));
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, std::optional(beforeSha)); }
        else QCOMPARE(Sha256File(source).value, sourceSha.value);
        // 保存后再写工程并冷开，完整raw尺寸、通行及SHD必须保留；旧物理epoch
        // 的历史若不能恢复资格须诚实降级，不能把阴影覆盖静默丢弃。
        const auto epochForge = outputRoot / ("resize-epoch-" + std::to_string(state) + ".forge");
        const auto epochSaved = restored.SaveProjectToPath(epochForge);
        QVERIFY2(epochSaved.Succeeded(), ReadDiagnosticText(epochSaved).c_str());
        auto epoch = PrepareProjectOpen(epochForge, fixture.gameRoot);
        QVERIFY2(epoch.Succeeded(), ReadDiagnosticText(epoch).c_str());
        QCOMPARE(MapData::SerializeMapTerrain(*(*epoch.value)->mapDataEdits->Terrain()).value,
            std::optional(expanded ? expectedTerrain : R::TerrainLiteral()));
        const auto epochArchive = MapArchiveSession::Open((*epoch.value)->BaselinePath());
        QVERIFY2(epochArchive.Succeeded(), ReadDiagnosticText(epochArchive).c_str());
        const auto epochShadow = epochArchive.value->ReadMember("war3map.shd");
        QVERIFY2(epochShadow.Succeeded() && epochShadow.value, ReadDiagnosticText(epochShadow).c_str());
        QVERIFY2(*epochShadow.value == expectedWrittenShadow,
            qPrintable(shadowDifference(*epochShadow.value, expectedWrittenShadow)));
        const auto shadow = (*epoch.value)->mapDataEdits->TerrainResizeShadowDraft();
        if (shadow.Succeeded() && shadow.value) {
            QVERIFY(shadow.value->present);
            QVERIFY2(shadow.value->bytes == expectedWrittenShadow,
                qPrintable(shadowDifference(shadow.value->bytes, expectedWrittenShadow)));
        } else {
            // 无resize覆盖的原形状工程由原始基线提供SHD。
            QVERIFY(!expanded);
            QCOMPARE((*epoch.value)->mapDataEdits->ExportTerrainResizeDraft().value, std::optional(R::Bytes{}));
        }
    }
}
