#pragma once

#include "../support/TerrainResizeFixture.h"
#include "War3MapCore/Services/TerrainHistorySave.h"
#include "War3MapCore/Services/TerrainResizeEditService.h"

// 原创归档与最小启动资源；只隔离脚本编译准备，地图保存、回读、工程
// 冷恢复均消费产品代码。这些断言不构成原版编辑器或游戏运行验收。
namespace {
SavedHistoryQtBytes PreResizeHistoryQtState(const EditorProject& project) {
    auto result = SavedHistoryQtState(project);
    const auto shadow = project.mapEdits_->TerrainResizeShadowDraft();
    if (shadow.Succeeded() && shadow.value) {
        result["war3map.shd"] = shadow.value->present
            ? std::optional(shadow.value->bytes) : std::nullopt;
    } else {
        if (project.mapEdits_->HasTerrainTypedResizeState())
            throw std::runtime_error("typed resize history shadow unavailable");
        if (!project.archiveSession_->HasMember("war3map.shd")) result["war3map.shd"] = std::nullopt;
        else {
            const auto read = project.archiveSession_->ReadMember("war3map.shd");
            if (!read.Succeeded() || !read.value) throw std::runtime_error("history source shadow read failed");
            result["war3map.shd"] = *read.value;
        }
    }
    return result;
}
}

void EditorProjectTests::SavedTerrainHistoryBeforeFirstResizeRoundtrip_data() {
    QTest::addColumn<bool>("shadow"); QTest::addColumn<bool>("inPlace"); QTest::addColumn<int>("cursor");
    QTest::newRow("present-shadow-save-as-after-resize") << true << false << 2;
    QTest::newRow("present-shadow-in-place-before-station") << true << true << 0;
    QTest::newRow("absent-shadow-save-as-before-station") << false << false << 0;
    QTest::newRow("absent-shadow-in-place-after-resize") << false << true << 2;
}

void EditorProjectTests::SavedTerrainHistoryBeforeFirstResizeRoundtrip() {
    namespace R = AudastraTest::TerrainResize;
    QFETCH(bool, shadow); QFETCH(bool, inPlace); QFETCH(int, cursor);
    R::Fixture fixture(false, shadow); const auto source = fixture.archive->SourcePath();
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const std::array<MapArchiveMember, 1> gameMembers{{{"fixture.txt", R::Text("original pre-resize history resources")}}};
    QVERIFY(WriteVerifiedArchive(fixture.gameRoot / "war3.mpq", gameMembers).Succeeded());
    EditorProject hot; QVERIFY(BindTerrainSaveProject(hot, source)); hot.war3GameRoot_ = fixture.gameRoot;
    QUndoStack hotUndo; hot.mapDataUndoStack_ = &hotUndo;
    QVERIFY(RaiseTerrainSaveProject(hot)); PushMapDataEdit(hot, "Raise", "首次保存前升高");
    QVERIFY(!hot.mapEdits_->HasTerrainTypedResizeState());
    const auto target = inPlace ? source : fixture.directory / "ordinary-saved.w3x";
    auto job = FreezeNativePathingQtJob(hot, target); QVERIFY(job);
    const auto lease = hot.mapEdits_->PrepareTerrainHistoryForSave();
    QVERIFY2(lease.Succeeded(), ReadDiagnosticText(lease).c_str());
    QVERIFY(Services::TerrainHistorySave::NeedsSourceShadowBinding(*lease.value));
    job->terrainHistorySaveLease = *lease.value;
    job->terrainSessionSnapshot = hot.mapEdits_->FreezeForSave();
    job->inPlace = inPlace; job->inPlaceOptions.backupDirectory = fixture.directory / "backups";
    bool previousCalled = false;
    job->prepareFinalDerived = [&](Services::MapSaveTask&) {
        previousCalled = true; return Status::Ok();
    };
    InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
    QVERIFY(previousCalled); QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
    QVERIFY2(job->terrainHistoryWarning.empty(), job->terrainHistoryWarning.c_str());
    QVERIFY(job->terrainHistorySaveLease);
    QVERIFY(!Services::TerrainHistorySave::NeedsSourceShadowBinding(*job->terrainHistorySaveLease));
    QVERIFY(hot.CommitMapSave(*job)); QVERIFY2(hot.terrainJournalWarning_.empty(), hot.terrainJournalWarning_.c_str());
    QCOMPARE(hotUndo.count(), 1); QCOMPARE(hotUndo.cleanIndex(), 1); QVERIFY(hotUndo.isClean());
    QVERIFY(!hot.mapEdits_->HasTerrainTypedResizeState());
    const auto station = PreResizeHistoryQtState(hot);
    const std::optional<R::Bytes> expectedShadow = shadow ? std::optional(R::ShadowLiteral()) : std::nullopt;
    QCOMPARE(station.at("war3map.shd"), expectedShadow);
    const auto savedArchive = MapArchiveSession::Open(target); QVERIFY(savedArchive.Succeeded());
    QCOMPARE(savedArchive.value->HasMember("war3map.shd"), shadow);
    if (shadow) QCOMPARE(savedArchive.value->ReadMember("war3map.shd").value, expectedShadow);
    if (inPlace) {
        QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, sourceSha.value);
    } else QCOMPARE(Sha256File(source).value, sourceSha.value);

    // 真正跨过保存站撤销再重做，取得当前P分层；不得拿保存前WPM冒充
    // 保存站后的受保护来源。旧SHD则必须仍为原创磁盘全文或明确缺失。
    hotUndo.undo(); const auto zero = PreResizeHistoryQtState(hot);
    QCOMPARE(zero.at("war3map.w3e"), std::optional(R::TerrainLiteral()));
    QCOMPARE(zero.at("war3map.shd"), expectedShadow);
    hotUndo.redo(); QCOMPARE(PreResizeHistoryQtState(hot), station);
    auto resources = Services::TerrainResizeEditService::Capture(*hot.archiveSession_, *hot.mapEdits_, fixture.gameRoot);
    QVERIFY2(resources.Succeeded(), ReadDiagnosticText(resources).c_str());
    const auto resize = hot.mapEdits_->PrepareTerrainResizeEdit(*resources.value, {1, {-32, -32, 32, 32}});
    QVERIFY2(resize.Succeeded(), ReadDiagnosticText(resize).c_str());
    QVERIFY(hot.mapEdits_->ApplyPreparedTerrainResizeEdit(*resize.value).Succeeded());
    QVERIFY(hot.ObserveMapDataEdit("保存后首次扩图")); PushMapDataEdit(hot, "Resize canvas", "保存后首次扩图");
    QCOMPARE(hotUndo.count(), 2); const auto expanded = PreResizeHistoryQtState(hot);
    QCOMPARE(hot.mapEdits_->Terrain()->header.width, std::uint32_t{65});
    QCOMPARE(expanded.at("war3map.shd"), shadow
        ? std::optional(R::ShadowLiteral(-32, -32, 32, 32, true)) : std::nullopt);
    hotUndo.setIndex(cursor); const auto expected = cursor == 0 ? zero : expanded;
    QCOMPARE(PreResizeHistoryQtState(hot), expected);
    const auto forge = fixture.directory / "before-first-resize.forge";
    const auto written = hot.SaveProjectToPath(forge); QVERIFY2(written.Succeeded(), ReadDiagnosticText(written).c_str());
    QVERIFY(!HasReadDiagnostic(written, "TERRAIN_JOURNAL_DRAFT_ONLY"));
    auto reopened = PrepareProjectOpen(forge, fixture.gameRoot);
    QVERIFY2(reopened.Succeeded(), ReadDiagnosticText(reopened).c_str());
    QVERIFY(HasReadDiagnostic(reopened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*reopened.value)->terrainCurrentIndex, std::size_t(cursor));
    QCOMPARE((*reopened.value)->terrainSavedIndex, std::optional<std::size_t>{1});
    QCOMPARE((*reopened.value)->terrainReceipts.size(), std::size_t{2});
    EditorProject cold; QUndoStack coldUndo; cold.mapDataUndoStack_ = &coldUndo;
    QVERIFY(BindSavedHistoryQtProject(cold, **reopened.value, fixture.gameRoot));
    QCOMPARE(coldUndo.count(), 2); QCOMPARE(coldUndo.index(), cursor); QCOMPARE(coldUndo.cleanIndex(), 1);
    QCOMPARE(PreResizeHistoryQtState(cold), expected);
    coldUndo.setIndex(1); QCOMPARE(PreResizeHistoryQtState(cold), station); QVERIFY(coldUndo.isClean());
    coldUndo.setIndex(0); QCOMPARE(PreResizeHistoryQtState(cold), zero);
    coldUndo.setIndex(2); QCOMPARE(PreResizeHistoryQtState(cold), expanded);
    coldUndo.undo(); QCOMPARE(PreResizeHistoryQtState(cold), station); QVERIFY(coldUndo.isClean());
}

void EditorProjectTests::SavedTerrainHistoryBeforeFirstResizePreviousFailure() {
    namespace R = AudastraTest::TerrainResize;
    R::Fixture fixture; EditorProject project;
    QVERIFY(BindTerrainSaveProject(project, fixture.archive->SourcePath()));
    QVERIFY(RaiseTerrainSaveProject(project));
    const auto lease = project.mapEdits_->PrepareTerrainHistoryForSave(); QVERIFY(lease.Succeeded());
    MapSaveJob job; job.terrainHistorySaveLease = *lease.value;
    // 故意没有archive；若顺序错误，绑定会清掉lease并产生历史降级警告。
    job.prepareFinalDerived = [](Services::MapSaveTask&) {
        return Status::Failure("Fixture", "PRE_RESIZE_PREVIOUS_REJECTED", "原创前置准备拒绝");
    };
    InstallMapSaveDerivedPreparation(job);
    const auto rejected = job.prepareFinalDerived(job);
    QVERIFY(!rejected.Succeeded()); QVERIFY(HasReadDiagnostic(rejected, "PRE_RESIZE_PREVIOUS_REJECTED"));
    QVERIFY(job.terrainHistorySaveLease); QVERIFY(job.terrainHistoryWarning.empty());
    QVERIFY(Services::TerrainHistorySave::NeedsSourceShadowBinding(*job.terrainHistorySaveLease));
    QVERIFY(!job.executed); QVERIFY(!job.succeeded);

    MapSaveJob missingSource; missingSource.terrainHistorySaveLease = *lease.value;
    missingSource.minimapChoice = Services::TerrainMinimapSaveChoice::Preserve;
    InstallMapSaveDerivedPreparation(missingSource);
    const auto rejectedSource = missingSource.prepareFinalDerived(missingSource);
    // 历史绑定可以降级，但同一个缺失归档仍必须被正式保存门拒绝。
    QVERIFY(!rejectedSource.Succeeded());
    QVERIFY(HasReadDiagnostic(rejectedSource, "TERRAIN_SAVE_PLAN_SOURCE_MISSING"));
    QVERIFY(!missingSource.terrainHistorySaveLease); QVERIFY(!missingSource.terrainHistoryWarning.empty());
    QVERIFY(!missingSource.executed); QVERIFY(!missingSource.succeeded);
}

void EditorProjectTests::SavedTerrainHistoryBeforeFirstResizeBindingFailureKeepsMapSave() {
    namespace R = AudastraTest::TerrainResize;
    R::Fixture fixture; const auto source = fixture.archive->SourcePath();
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const std::array<MapArchiveMember, 1> gameMembers{{{"fixture.txt", R::Text("original optional history resources")}}};
    QVERIFY(WriteVerifiedArchive(fixture.gameRoot / "war3.mpq", gameMembers).Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); project.war3GameRoot_ = fixture.gameRoot;
    QVERIFY(RaiseTerrainSaveProject(project)); PushMapDataEdit(project, "Raise", "可选历史失败时保留正式保存");
    const auto expectedTerrain = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain());
    QVERIFY(expectedTerrain.Succeeded());
    const auto target = fixture.directory / "optional-history-saved.w3x";
    auto job = FreezeNativePathingQtJob(project, target); QVERIFY(job);
    job->terrainSessionSnapshot = project.mapEdits_->FreezeForSave();
    // 默认lease没有真实会话身份，helper必须拒绝；这只失去可选历史，
    // 随后的正式来源、通行写出与归档回读仍须全部成功，不能伪造通过。
    job->terrainHistorySaveLease.emplace();
    InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
    QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
    QVERIFY(!job->terrainHistorySaveLease); QVERIFY(!job->terrainHistoryWarning.empty());
    QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
    const auto saved = MapArchiveSession::Open(target); QVERIFY(saved.Succeeded());
    QCOMPARE(saved.value->ReadMember("war3map.w3e").value, expectedTerrain.value);
    QCOMPARE(saved.value->ReadMember("war3map.shd").value, std::optional(R::ShadowLiteral()));
    QVERIFY(project.CommitMapSave(*job)); QVERIFY(!project.HasUnsavedMapChanges());
    QVERIFY(!project.terrainJournalWarning_.empty());
    QCOMPARE(Sha256File(source).value, sourceSha.value);
}
