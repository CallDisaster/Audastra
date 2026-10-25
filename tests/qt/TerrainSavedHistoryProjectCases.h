#pragma once

#include "../TerrainBlockFixtures.h"
#include "../TerrainConnectedRampFixtures.h"

#include <cctype>

// 原创结构样例；真实地图写出/回读与工程冷恢复均走产品入口。
// FreezeNativePathingQtJob仅隔离脚本工具链，不构成原版编辑器或游戏验收。
namespace {
using SavedHistoryQtBytes = std::map<std::string, std::optional<std::vector<std::byte>>>;

SavedHistoryQtBytes SavedHistoryQtState(const EditorProject& project) {
    SavedHistoryQtBytes result;
    for (const auto name : MapData::TerrainJournalMemberNames) {
        if (!project.archiveSession_->HasMember(name)) result[std::string(name)] = std::nullopt;
        else {
            const auto read = project.archiveSession_->ReadMember(name);
            if (!read.Succeeded() || !read.value) throw std::runtime_error("history fixture source read failed");
            result[std::string(name)] = *read.value;
        }
    }
    for (const auto& replacement : project.mapEdits_->Replacements()) {
        auto key = replacement.name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (result.contains(key)) result[key] = replacement.data;
    }
    for (const auto& removed : project.mapEdits_->Removals()) result[removed] = std::nullopt;
    return result;
}

bool BindSavedHistoryQtProject(EditorProject& project, PreparedProjectOpen& candidate,
    const std::filesystem::path& game) {
    if (!candidate.map || !candidate.mapDataEdits) return false;
    const auto source = candidate.sourceMapMatchesBaseline ? candidate.sourceMapPath : candidate.BaselinePath();
    if (!BindTerrainSaveProject(project, source)) return false;
    const auto original = project.editRevision_.Current();
    project.war3GameRoot_ = game; project.recordedSourceMapPath_ = candidate.sourceMapPath;
    project.restoredFiles_ = std::move(candidate.extraction);
    project.mapEdits_ = std::move(candidate.mapDataEdits);
    project.terrainJournalWarning_ = candidate.terrainJournalWarning;
    if (!InstallRestoredMapDataHistory(project, std::move(candidate.terrainReceipts),
        candidate.terrainCurrentIndex, candidate.terrainSavedIndex).Succeeded()) return false;
    project.lastProjectFingerprint_ = project.ProjectFingerprint();
    project.projectFilePath_ = candidate.sourcePath; project.projectFileSha256_ = candidate.sourceSha256;
    const auto scopes = project.CaptureEditScopes(); if (!scopes) return false;
    project.editTrackingReady_ = project.editRevision_.Reset(*scopes, *scopes, original).Succeeded();
    project.editTrackingFailed_ = !project.editTrackingReady_; project.UpdateEditDirtyFlags();
    return project.editTrackingReady_;
}
}

void EditorProjectTests::SavedTerrainHistoryProjectRoundtrip_data() {
    QTest::addColumn<bool>("block"); QTest::addColumn<int>("cursor"); QTest::addColumn<bool>("inPlace");
    QTest::newRow("block-before-save-station") << true << 0 << false;
    QTest::newRow("block-after-linear-tail") << true << 2 << true;
    QTest::newRow("wet-connected-before-save-station") << false << 0 << true;
    QTest::newRow("wet-connected-after-linear-tail") << false << 2 << false;
}

void EditorProjectTests::SavedTerrainHistoryProjectRoundtrip() {
    QFETCH(bool, block); QFETCH(int, cursor); QFETCH(bool, inPlace);
    namespace B = War3Map::TerrainBlockFixtures;
    namespace C = War3Map::TerrainConnectedRampFixtures;
    auto blocks = block ? std::make_unique<B::BlockFixture>() : nullptr;
    auto ramps = block ? nullptr : std::make_unique<C::ConnectedFixture>(C::SurfaceCase::DeepEquality);
    const auto source = block ? blocks->source->SourcePath() : ramps->source->SourcePath();
    const auto game = block ? blocks->game : ramps->game;
    const auto root = block ? blocks->root : ramps->root;
    EditorProject hot; QVERIFY(BindTerrainSaveProject(hot, source)); hot.war3GameRoot_ = game;
    QUndoStack hotUndo; hot.mapDataUndoStack_ = &hotUndo;
    const auto original = SavedHistoryQtState(hot);
    const auto originalScript = hot.archiveSession_->ReadMember("war3map.j"); QVERIFY(originalScript.Succeeded());
    const auto snapshot = hot.mapEdits_->Snapshot(); const auto doodads = snapshot.Doodads(); QVERIFY(doodads);
    const auto& sourceGrid = block ? blocks->grid : ramps->grid;
    const auto rules = Services::TerrainPathingSave::CaptureWaterBrushRules(*hot.archiveSession_, sourceGrid,
        *hot.mapEdits_->Terrain(), *doodads, game, true, MapData::TerrainJournalMaximumBytes);
    QVERIFY2(rules.Succeeded(), ReadDiagnosticText(rules).c_str());
    Result<MapData::MapDataEditSession::PreparedTerrainEdit> prepared;
    if (block) {
        const auto hash = MapData::CaptureTerrainBlockFingerprint(*hot.mapEdits_->Terrain(), blocks->sourceCells); QVERIFY(hash.Succeeded());
        const MapData::TerrainBlockPasteIntent intent{1, blocks->sourceCells, blocks->destination, *hash.value};
        prepared = hot.mapEdits_->PrepareTerrainBlockPasteEdit(*rules.value, intent, true);
    } else {
        const auto brush = C::StraightBrush(true);
        prepared = hot.mapEdits_->PrepareTerrainStraightRampEdit(*rules.value, brush, std::span(&ramps->point, 1),
            true, {}, MapData::TerrainStraightRampAlgorithm::Connected);
    }
    QVERIFY2(prepared.Succeeded(), ReadDiagnosticText(prepared).c_str());
    QVERIFY(hot.mapEdits_->ApplyPreparedTerrainEdit(*prepared.value).Succeeded());
    QVERIFY(hot.ObserveMapDataEdit("地形历史保存站原始结构编辑"));
    PushMapDataEdit(hot, "Structure", "保存站结构编辑"); QCOMPARE(hotUndo.count(), 1);

    const auto target = inPlace ? source : root / "history-saved.w3x";
    auto job = FreezeNativePathingQtJob(hot, target); QVERIFY(job);
    const auto lease = hot.mapEdits_->PrepareTerrainHistoryForSave(); QVERIFY2(lease.Succeeded(), ReadDiagnosticText(lease).c_str());
    job->terrainHistorySaveLease = *lease.value;
    job->terrainSessionSnapshot = hot.mapEdits_->FreezeForSave(); job->inPlace = inPlace;
    job->inPlaceOptions.backupDirectory = root / "backups";
    InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
    QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str()); QVERIFY(hot.CommitMapSave(*job));
    QVERIFY2(hot.terrainJournalWarning_.empty(), hot.terrainJournalWarning_.c_str());
    QVERIFY(hotUndo.isClean()); QCOMPARE(hotUndo.cleanIndex(), 1);
    QCOMPARE(hot.archiveSession_->ReadMember("war3map.j").value, originalScript.value);
    const auto station = SavedHistoryQtState(hot);
    const auto nativeStationPathing = block ? B::PathingLiteral(true)
        : C::ConnectedPathingLiteral(C::SurfaceCase::DeepEquality, true);
    QVERIFY(station.at("war3map.wpm")); QCOMPARE(*station.at("war3map.wpm"), nativeStationPathing);
    auto zero = original;
    QVERIFY(zero.at("war3map.wpm")); auto& zeroPathing = *zero.at("war3map.wpm");
    QCOMPARE(zeroPathing.size(), nativeStationPathing.size());
    // 两份原创夹具没有通行笔刷，savedS/currentS均为0；保存后的P是station原生低三位。
    // 跨站Undo恢复旧荒芜，自动地形位和不可撤回P继续来自独立字面station，不退回旧G0。
    for (std::size_t index = 16; index < zeroPathing.size(); ++index)
        zeroPathing[index] = (nativeStationPathing[index] & std::byte{0xdf}) | (zeroPathing[index] & std::byte{0x20});
    const auto stationRestrictions = hot.mapEdits_->SerializePathingRestrictionDraft();
    QVERIFY2(stationRestrictions.Succeeded(), ReadDiagnosticText(stationRestrictions).c_str());
    QVERIFY(stationRestrictions.value->empty());
    QVERIFY(RaiseTerrainSaveProject(hot)); PushMapDataEdit(hot, "Raise", "保存站后线性升高");
    const auto tail = SavedHistoryQtState(hot); QCOMPARE(hotUndo.count(), 2);
    hotUndo.setIndex(cursor);
    const auto expected = cursor == 0 ? zero : tail; QCOMPARE(SavedHistoryQtState(hot), expected);
    QCOMPARE(hot.mapEdits_->SerializePathingRestrictionDraft().value, stationRestrictions.value);
    const auto forge = root / "history.forge";
    const auto written = hot.SaveProjectToPath(forge); QVERIFY2(written.Succeeded(), ReadDiagnosticText(written).c_str());
    QVERIFY(!HasReadDiagnostic(written, "TERRAIN_JOURNAL_DRAFT_ONLY"));
    auto reopened = PrepareProjectOpen(forge, game); QVERIFY2(reopened.Succeeded(), ReadDiagnosticText(reopened).c_str());
    QCOMPARE((*reopened.value)->terrainCurrentIndex, std::size_t(cursor));
    QCOMPARE((*reopened.value)->terrainSavedIndex, std::optional<std::size_t>(1));
    QCOMPARE((*reopened.value)->terrainReceipts.size(), std::size_t(2));
    const auto revision = (*reopened.value)->mapDataEdits->Revision();
    EditorProject cold; QUndoStack coldUndo; cold.mapDataUndoStack_ = &coldUndo;
    QVERIFY(BindSavedHistoryQtProject(cold, **reopened.value, game));
    QCOMPARE(cold.mapEdits_->Revision(), revision); QCOMPARE(coldUndo.index(), cursor); QCOMPARE(coldUndo.cleanIndex(), 1);
    QCOMPARE(SavedHistoryQtState(cold), expected);
    coldUndo.setIndex(1); QCOMPARE(SavedHistoryQtState(cold), station); QVERIFY(coldUndo.isClean());
    coldUndo.setIndex(0); QCOMPARE(SavedHistoryQtState(cold), zero);
    QCOMPARE(cold.mapEdits_->SerializePathingRestrictionDraft().value, stationRestrictions.value);
    coldUndo.setIndex(2); QCOMPARE(SavedHistoryQtState(cold), tail);
    auto finalJob = FreezeNativePathingQtJob(cold, root / "history-cold-saved.w3x"); QVERIFY(finalJob);
    finalJob->terrainSessionSnapshot = cold.mapEdits_->FreezeForSave();
    InstallMapSaveDerivedPreparation(*finalJob); EditorProject::RunMapSave(*finalJob);
    QVERIFY2(finalJob->succeeded, ReadDiagnosticText(*finalJob).c_str()); QVERIFY(cold.CommitMapSave(*finalJob));
    QCOMPARE(cold.archiveSession_->ReadMember("war3map.j").value, originalScript.value);
    QVERIFY(!cold.HasUnsavedMapChanges());
}

void EditorProjectTests::SavedTerrainHistoryStackInstallationNeverReplays() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("stack.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, TerrainSaveMembers()).Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    QVERIFY(RaiseTerrainSaveProject(project)); const auto first = project.mapEdits_->LastEdit();
    QVERIFY(RaiseTerrainSaveProject(project)); const auto second = project.mapEdits_->LastEdit();
    QVERIFY(project.mapEdits_->Replay(second, false).Succeeded());
    const auto before = SavedHistoryQtState(project); const auto revision = project.mapEdits_->Revision();
    QSignalSpy changed(project.mapDataUndoStack_, &QUndoStack::indexChanged);
    QVERIFY(InstallRestoredMapDataHistory(project, {first, second}, 1, 2).Succeeded());
    QCOMPARE(changed.size(), 0); QCOMPARE(project.mapEdits_->Revision(), revision);
    QCOMPARE(SavedHistoryQtState(project), before); QCOMPARE(project.mapDataUndoStack_->index(), 1);
    QCOMPARE(project.mapDataUndoStack_->cleanIndex(), 2);
    project.mapDataUndoStack_->redo(); QCOMPARE(project.mapEdits_->Revision(), revision + 1);
    project.mapDataUndoStack_->undo(); QCOMPARE(SavedHistoryQtState(project), before);
    const auto unchanged = project.mapEdits_->Revision();
    QVERIFY(!InstallRestoredMapDataHistory(project, {first, second}, 3, 2).Succeeded());
    QCOMPARE(project.mapEdits_->Revision(), unchanged); QCOMPARE(project.mapDataUndoStack_->count(), 2);
}

void EditorProjectTests::SavedTerrainHistoryUnsupportedBranchKeepsRaw() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("branch.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, TerrainSaveMembers()).Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    QVERIFY(RaiseTerrainSaveProject(project)); PushMapDataEdit(project, "Raise", "保存前升高");
    const auto lease = project.mapEdits_->PrepareTerrainHistoryForSave(); QVERIFY(lease.Succeeded());
    auto plan = MapData::TerrainSavePlan::Prepare(*project.archiveSession_, *project.mapEdits_); QVERIFY(plan.Succeeded());
    auto job = std::make_unique<MapSaveJob>(); QVERIFY(job->FreezeArchive(*project.archiveSession_).Succeeded());
    job->target = std::filesystem::path(root.filePath("saved.w3x").toStdWString());
    job->replacements = project.mapEdits_->Replacements(); job->removals = project.mapEdits_->Removals();
    job->preparedRevision = MapSaveRevisionBinding{project.editRevision_.Generation(), project.editRevision_.Revision(),
        project.editRevision_.Current(), project.luaMapTargetGeneration_};
    job->terrainSavePlan = std::make_shared<const MapData::TerrainSavePlan>(std::move(*plan.value));
    job->derivedPreparationInstalled = true; job->terrainHistorySaveLease = *lease.value;
    EditorProject::RunMapSave(*job); QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str()); QVERIFY(project.CommitMapSave(*job));
    project.mapDataUndoStack_->undo();
    QVERIFY(RaiseTerrainSaveProject(project, 30)); PushMapDataEdit(project, "Branch", "保存点前分支");
    const auto draft = SavedHistoryQtState(project);
    const auto saved = project.SaveProjectToPath(std::filesystem::path(root.filePath("raw-branch.forge").toStdWString()));
    QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str()); QVERIFY(HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY"));
    auto opened = PrepareProjectOpen(std::filesystem::path(root.filePath("raw-branch.forge").toStdWString()));
    QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(!(*opened.value)->terrainJournalWarning.empty()); QVERIFY((*opened.value)->terrainReceipts.empty());
    EditorProject raw; QVERIFY(BindSavedHistoryQtProject(raw, **opened.value, {})); QCOMPARE(SavedHistoryQtState(raw), draft);
}
