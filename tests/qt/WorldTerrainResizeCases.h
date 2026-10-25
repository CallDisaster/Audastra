#pragma once

#include "../support/TerrainResizeFixture.h"

namespace ResizeWorldCases {
namespace R = AudastraTest::TerrainResize;
inline void Install(War3Map::QtProject::EditorProject& project, const R::Fixture& fixture) {
    project.war3GameRoot_ = fixture.gameRoot;
    project.projectRoot_ = fixture.directory;
    project.directorySnapshot_.reset();
    project.sourceArchivePath_ = fixture.archive->SourcePath();
    project.archiveSession_ = std::make_shared<War3Map::MapArchiveSession>(
        R::Take(War3Map::MapArchiveSession::Open(project.sourceArchivePath_)));
    project.mapDocument_ = std::make_unique<War3Map::MapDocument>(
        War3Map::MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<War3Map::MapData::MapDataEditSession>(*project.mapDocument_);
    R::Check(project.mapEdits_->AttachTerrain(R::Take(fixture.archive->ReadMember("war3map.w3e"))));
    R::Check(project.mapEdits_->AttachPathing(R::PathingLiteral()));
    project.mapDataUndoStack_->clear(); project.ResetEditTracking();
}
struct Controls {
    QSpinBox *west, *south, *east, *north;
    QPushButton *read, *preview, *apply;
    QLabel* status;
    explicit Controls(QWidget* page)
        : west(page->findChild<QSpinBox*>("worldTerrainResizeWest")),
          south(page->findChild<QSpinBox*>("worldTerrainResizeSouth")),
          east(page->findChild<QSpinBox*>("worldTerrainResizeEast")),
          north(page->findChild<QSpinBox*>("worldTerrainResizeNorth")),
          read(page->findChild<QPushButton*>("worldTerrainResizeRead")),
          preview(page->findChild<QPushButton*>("worldTerrainResizePreview")),
          apply(page->findChild<QPushButton*>("worldTerrainResizeApply")),
          status(page->findChild<QLabel*>("worldTerrainResizeStatus")) {
        R::Require(west && south && east && north && read && preview && apply && status, "product resize controls");
    }
    bool Busy() const { return status->property("busy").toBool(); }
    bool Ready() const { return status->property("ready").toBool(); }
    QString Outcome() const { return status->property("outcome").toString(); }
    void Set(int w, int s, int e, int n) { west->setValue(w); south->setValue(s); east->setValue(e); north->setValue(n); }
};
inline R::Bytes Terrain(const War3Map::QtProject::EditorProject& project) {
    return R::Take(War3Map::MapData::SerializeMapTerrain(*project.MapEdits()->Terrain()));
}
} // namespace ResizeWorldCases

void WorldTerrainBoundsTests::ResizeControlsUseActualRulesAndSingleUndo() {
    namespace R = AudastraTest::TerrainResize;
    R::Fixture fixture(true); QtProject::EditorProject project; ResizeWorldCases::Install(project, fixture);
    WorldModule module(project, nullptr); ResizeWorldCases::Controls c(module.Widget());
    const auto beforeTerrain = ResizeWorldCases::Terrain(project);
    const auto beforeSettings = R::Take(project.MapEdits()->Snapshot().Settings()->Serialize());
    const auto beforeUnits = R::Take(project.MapEdits()->Snapshot().Units()->Serialize());
    const auto beforeDoodads = R::Take(project.MapEdits()->Snapshot().Doodads()->Serialize());
    const auto sourceSha = R::Take(Sha256File(fixture.archive->SourcePath()));
    QVERIFY(c.read->isEnabled()); QVERIFY(!c.preview->isEnabled()); QVERIFY(!c.apply->isEnabled());
    c.read->click(); QVERIFY(c.Busy());
    QVERIFY(!project.HasPendingTerrainCommit());
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY2(c.Ready(), qPrintable(c.status->text()));
    QCOMPARE(c.west->value(), 0); QCOMPARE(c.south->value(), 0);
    QCOMPARE(c.east->value(), 32); QCOMPARE(c.north->value(), 32);
    c.Set(-1, 0, 32, 32); QVERIFY(!c.preview->isEnabled());
    c.Set(-32, -32, 32, 32); QVERIFY(c.preview->isEnabled()); QVERIFY(!c.apply->isEnabled());
    c.preview->click(); QVERIFY(!project.HasPendingTerrainCommit()); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QVERIFY2(c.Outcome() == QStringLiteral("previewed"), qPrintable(c.status->text()));
    QCOMPARE(ResizeWorldCases::Terrain(project), beforeTerrain);
    QCOMPARE(project.mapDataUndoStack_->count(), 0); QVERIFY(c.apply->isEnabled());
    // 通过的矩形不能给另一个矩形沿用。
    c.Set(-64, -32, 32, 32); QVERIFY(!c.apply->isEnabled());
    c.Set(-32, -32, 32, 32); QVERIFY(!c.apply->isEnabled());
    c.preview->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY(c.apply->isEnabled());
    c.apply->click(); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QVERIFY(project.HasPendingTerrainCommit());
    bool guardedUndo = false;
    const auto pendingObserver = connect(project.mapDataUndoStack_, &QUndoStack::indexChanged, &module,
        [&] { guardedUndo = project.HasPendingTerrainCommit(); });
    QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QVERIFY2(c.Outcome() == QStringLiteral("applied"), qPrintable(c.status->text()));
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    disconnect(pendingObserver); QVERIFY(guardedUndo); QVERIFY(!project.HasPendingTerrainCommit());
    QCOMPARE(ResizeWorldCases::Terrain(project), R::TerrainLiteral(-32, -32, 32, 32));
    const auto shadow = project.MapEdits()->TerrainResizeShadowDraft();
    QVERIFY(shadow.Succeeded() && shadow.value->present);
    QCOMPARE(shadow.value->bytes, R::ShadowLiteral(-32, -32, 32, 32, true));
    auto expected = fixture.settings; expected.cameraMargins = {33, 2, 33, 2};
    QCOMPARE(R::Take(project.MapEdits()->Snapshot().Settings()->Serialize()), R::Take(expected.Serialize()));
    QCOMPARE(R::Take(project.MapEdits()->Snapshot().Units()->Serialize()), beforeUnits);
    QCOMPARE(R::Take(project.MapEdits()->Snapshot().Doodads()->Serialize()), beforeDoodads);
    project.mapDataUndoStack_->undo(); QCOMPARE(ResizeWorldCases::Terrain(project), beforeTerrain);
    QCOMPARE(R::Take(project.MapEdits()->Snapshot().Settings()->Serialize()), beforeSettings);
    project.mapDataUndoStack_->redo(); QCOMPARE(ResizeWorldCases::Terrain(project), R::TerrainLiteral(-32, -32, 32, 32));
    // 走世界页真实范围按钮；扩边后必须读取当前65×65顶点，不能回用磁盘33×33。
    Controls bounds(module.Widget());
    bounds.read->click(); QTRY_VERIFY_WITH_TIMEOUT(!bounds.Busy(), 15000);
    QVERIFY2(bounds.Ready(), qPrintable(bounds.status->text()));
    QCOMPARE(bounds.west->value(), 33); QCOMPARE(bounds.south->value(), 33);
    QCOMPARE(bounds.east->value(), 62); QCOMPARE(bounds.north->value(), 62);
    bounds.Set(34, 34, 60, 60); QVERIFY(bounds.apply->isEnabled()); bounds.apply->click();
    QTRY_VERIFY_WITH_TIMEOUT(!bounds.Busy(), 15000);
    QVERIFY2(bounds.Outcome() == QStringLiteral("applied"), qPrintable(bounds.status->text()));
    QCOMPARE(project.mapDataUndoStack_->count(), 2);
    QCOMPARE(project.MapEdits()->Terrain()->header.width, std::uint32_t{65});
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->playableWidth, 26);
    project.mapDataUndoStack_->undo();
    QCOMPARE(ResizeWorldCases::Terrain(project), R::TerrainLiteral(-32, -32, 32, 32));
    QCOMPARE(R::Take(project.MapEdits()->Snapshot().Settings()->Serialize()), R::Take(expected.Serialize()));
    QCOMPARE(R::Take(Sha256File(fixture.archive->SourcePath())), sourceSha);
}

void WorldTerrainBoundsTests::ResizePreviewRejectsClippedContentAndKeepsDraft() {
    namespace R = AudastraTest::TerrainResize;
    R::Fixture fixture; QtProject::EditorProject project; ResizeWorldCases::Install(project, fixture);
    WorldModule module(project, nullptr); ResizeWorldCases::Controls c(module.Widget());
    c.read->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY2(c.Ready(), qPrintable(c.status->text()));
    const auto before = ResizeWorldCases::Terrain(project);
    // 宽度仍合法，但西界已进入原可玩区；不允许靠移动内容或削小可玩区来通过。
    c.Set(32, 0, 64, 32); QVERIFY(c.preview->isEnabled()); c.preview->click();
    QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QVERIFY2(c.Outcome() == QStringLiteral("failed"), qPrintable(c.status->text()));
    QVERIFY(!c.apply->isEnabled()); QCOMPARE(ResizeWorldCases::Terrain(project), before);
    QCOMPARE(project.mapDataUndoStack_->count(), 0); QCOMPARE(project.MapEdits()->Revision(), std::uint64_t{0});
    // 真startup winner在读取后改变，不发模拟信号；原候选必须拒绝。
    c.Set(-32, 0, 32, 32);
    QVERIFY(IO::WriteFileBytes(fixture.gameRoot / "WorldEditData.txt", R::Text(
        "[WorldEditMisc]\nMinimumMapSize=32\nMaximumMapSize=256\n[CameraMargins]\nLeft=1\nRight=2\nBottom=1\nTop=2\n")).Succeeded());
    c.preview->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QCOMPARE(c.Outcome(), QStringLiteral("failed")); QVERIFY(!c.apply->isEnabled());
    QCOMPARE(ResizeWorldCases::Terrain(project), before); QCOMPARE(project.mapDataUndoStack_->count(), 0);
}

void WorldTerrainBoundsTests::ResizeDropsLateResultOnMapSwitch() {
    namespace R = AudastraTest::TerrainResize;
    R::Fixture first, second; QtProject::EditorProject project; ResizeWorldCases::Install(project, first);
    WorldModule module(project, nullptr); ResizeWorldCases::Controls c(module.Widget());
    c.read->click(); QVERIFY(c.Busy());
    ResizeWorldCases::Install(project, second); project.MapDocumentChanged();
    const auto identity = project.MapEdits()->SessionIdentity();
    QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY(!c.Ready()); QVERIFY(!c.apply->isEnabled());
    QCOMPARE(project.MapEdits()->SessionIdentity(), identity);
    QCOMPARE(project.MapEdits()->Revision(), std::uint64_t{0}); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    c.read->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY2(c.Ready(), qPrintable(c.status->text()));
}
