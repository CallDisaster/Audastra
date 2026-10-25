#include "EditorProject.h"
#include "WorldModule.h"
#include "War3MapCore/IO/FileBytes.h"
#include "../TerrainCornerRampFixtures.h"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>

using namespace War3Map;
using namespace War3Map::Modules;
namespace F = War3Map::TerrainCornerRampFixtures;

namespace {
void Install(QtProject::EditorProject& project, const F::Fixture& fixture, bool archive,
    const std::vector<MapArchiveMember>* alternate = nullptr) {
    const auto& members = alternate ? *alternate : fixture.members;
    project.war3GameRoot_ = fixture.game;
    project.projectRoot_ = fixture.root / "map";
    std::filesystem::create_directories(project.projectRoot_);
    project.archiveSession_.reset(); project.directorySnapshot_.reset(); project.sourceArchivePath_.clear();
    if (archive) {
        project.sourceArchivePath_ = alternate ? F::Take(WriteVerifiedArchive(fixture.root / "bounds-source.w3x", members)).path
            : fixture.source->SourcePath();
        project.archiveSession_ = std::make_shared<MapArchiveSession>(F::Take(MapArchiveSession::Open(project.sourceArchivePath_)));
        project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    } else {
        for (const auto& member : members) {
            const auto path = project.projectRoot_ / member.name;
            std::filesystem::create_directories(path.parent_path());
            F::Require(IO::WriteFileBytes(path, member.data).Succeeded(), "write independent bounds source");
        }
        project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(F::Take(DirectoryWorkspace::Open(project.projectRoot_)));
        project.mapDocument_ = std::make_unique<MapDocument>(F::Take(MapDocument::FromDirectory(*project.directorySnapshot_)));
    }
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    F::Require(project.mapEdits_->AttachTerrain(members[0].data).Succeeded(), "attach bounds W3E");
    F::Require(project.mapEdits_->AttachPathing(members[1].data).Succeeded(), "attach bounds WPM");
    project.mapDataUndoStack_->clear(); project.ResetEditTracking();
}

struct Controls {
    QSpinBox *west, *south, *east, *north;
    QPushButton *read, *apply;
    QLabel* status;
    explicit Controls(QWidget* page)
        : west(page->findChild<QSpinBox*>("worldTerrainBoundsWest")),
          south(page->findChild<QSpinBox*>("worldTerrainBoundsSouth")),
          east(page->findChild<QSpinBox*>("worldTerrainBoundsEast")),
          north(page->findChild<QSpinBox*>("worldTerrainBoundsNorth")),
          read(page->findChild<QPushButton*>("worldTerrainBoundsRead")),
          apply(page->findChild<QPushButton*>("worldTerrainBoundsApply")),
          status(page->findChild<QLabel*>("worldTerrainBoundsStatus")) {
        F::Require(west && south && east && north && read && apply && status, "actual product bounds controls");
    }
    bool Busy() const { return status->property("busy").toBool(); }
    bool Ready() const { return status->property("ready").toBool(); }
    QString Outcome() const { return status->property("outcome").toString(); }
    void Set(int w, int s, int e, int n) { west->setValue(w); south->setValue(s); east->setValue(e); north->setValue(n); }
};
F::Bytes Terrain(const QtProject::EditorProject& project) { return F::Take(MapData::SerializeMapTerrain(*project.MapEdits()->Terrain())); }
F::Bytes Settings(const QtProject::EditorProject& project) { return F::Take(project.MapEdits()->Snapshot().Settings()->Serialize()); }
}

class WorldTerrainBoundsTests final : public QObject {
    Q_OBJECT
private slots:
    void ControlsRequireCompleteSession();
    void AppliesOneTransactionAndUndo_data();
    void AppliesOneTransactionAndUndo();
    void UsesActualCameraMargins();
    void RejectsChangedRevision();
    void DropsResultAfterMapSwitch();
    void RejectsActualDirectorySourceDrift();
    void RejectsGameResourceWinnerDrift();
    void RejectsNonIntegerCurrentBounds();
    void ResizeControlsUseActualRulesAndSingleUndo();
    void ResizePreviewRejectsClippedContentAndKeepsDraft();
    void ResizeDropsLateResultOnMapSwitch();
};

void WorldTerrainBoundsTests::ControlsRequireCompleteSession() {
    QtProject::EditorProject project;
    WorldModule module(project, nullptr);
    Controls c(module.Widget());
    QVERIFY(!c.read->isEnabled()); QVERIFY(!c.apply->isEnabled()); QVERIFY(!c.Ready());
    F::Fixture fixture; Install(project, fixture, true); project.MapDocumentChanged();
    QVERIFY(c.read->isEnabled()); QVERIFY(!c.west->isEnabled()); QVERIFY(!c.apply->isEnabled());
    c.read->click(); QVERIFY(c.Busy()); QVERIFY(!c.apply->isEnabled());
    // 点击只冻结输入；同一GUI事件中，尚未产生编辑或撤销项。
    QCOMPARE(project.MapEdits()->Revision(), std::uint64_t{0}); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    bool eventLoopResponsive = false;
    QTimer::singleShot(0, [&] { eventLoopResponsive = true; });
    QTRY_VERIFY_WITH_TIMEOUT(eventLoopResponsive, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QVERIFY2(c.Ready(), qPrintable(c.status->text()));
    QCOMPARE(c.west->value(), 0); QCOMPARE(c.south->value(), 0); QCOMPARE(c.east->value(), 32); QCOMPARE(c.north->value(), 32);
    QCOMPARE(c.west->maximum(), 32); QCOMPARE(c.north->maximum(), 32);
    c.Set(8, 0, 8, 32); QVERIFY(!c.apply->isEnabled());
    c.Set(0, 32, 32, 0); QVERIFY(!c.apply->isEnabled());
    c.Set(0, 0, 32, 32); QVERIFY(c.apply->isEnabled());
}

void WorldTerrainBoundsTests::AppliesOneTransactionAndUndo_data() {
    QTest::addColumn<bool>("archive");
    QTest::newRow("archive") << true;
    QTest::newRow("directory") << false;
}

void WorldTerrainBoundsTests::AppliesOneTransactionAndUndo() {
    QFETCH(bool, archive);
    F::Fixture fixture; QtProject::EditorProject project; Install(project, fixture, archive);
    WorldModule module(project, nullptr); Controls c(module.Widget());
    const auto beforeTerrain = Terrain(project), beforeSettings = Settings(project);
    const auto beforeUnits = F::Take(project.MapEdits()->Snapshot().Units()->Serialize());
    const auto beforeDoodads = F::Take(project.MapEdits()->Snapshot().Doodads()->Serialize());
    c.read->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY2(c.Ready(), qPrintable(c.status->text()));
    c.Set(2, 3, 30, 29); QVERIFY(c.apply->isEnabled()); c.apply->click();
    QCOMPARE(Terrain(project), beforeTerrain); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QCOMPARE(c.Outcome(), QStringLiteral("applied")); QCOMPARE(project.mapDataUndoStack_->count(), 1);
    const auto settings = *project.MapEdits()->Snapshot().Settings();
    QCOMPARE(settings.playableWidth, 28); QCOMPARE(settings.playableHeight, 26);
    const std::array<std::int32_t, 4> margins{2, 2, 3, 3}; QVERIFY(settings.cameraMargins == margins);
    const std::array<float, 8> camera{256, 384, 3840, 3712, 256, 3712, 3840, 384}; QVERIFY(settings.cameraBounds == camera);
    const auto terrain = project.MapEdits()->Terrain();
    for (unsigned y = 0; y < 33; ++y) for (unsigned x = 0; x < 33; ++x) {
        const bool edge = x < 32 && y < 32 && (x < 2 || x >= 30 || y < 3 || y >= 29);
        QCOMPARE(terrain->vertices[y * 33 + x].IsMapEdge(), edge);
    }
    QCOMPARE(F::Take(project.MapEdits()->Snapshot().Units()->Serialize()), beforeUnits);
    QCOMPARE(F::Take(project.MapEdits()->Snapshot().Doodads()->Serialize()), beforeDoodads);
    const auto afterTerrain = Terrain(project), afterSettings = Settings(project);
    QVERIFY(afterTerrain != beforeTerrain); QVERIFY(afterSettings != beforeSettings);
    project.mapDataUndoStack_->undo(); QCOMPARE(Terrain(project), beforeTerrain); QCOMPARE(Settings(project), beforeSettings);
    project.mapDataUndoStack_->redo(); QCOMPARE(Terrain(project), afterTerrain); QCOMPARE(Settings(project), afterSettings);
    c.read->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY2(c.Ready(), qPrintable(c.status->text()));
    QCOMPARE(c.west->value(), 2); QCOMPARE(c.north->value(), 29);
    c.apply->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QVERIFY2(c.Outcome() == QStringLiteral("unchanged"), qPrintable(c.status->text()));
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    QVERIFY2(c.Ready(), qPrintable(c.status->text()));
    // 当前W3E已与磁盘来源不同；第二个真实命令仍从原source和当前草稿分别捕获，
    // 不能把来源复核误作保存输出回读，也不能跳过来源而只让no-op通过。
    c.Set(3, 4, 29, 28); QVERIFY(c.apply->isEnabled()); c.apply->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QVERIFY2(c.Outcome() == QStringLiteral("applied"), qPrintable(c.status->text()));
    QCOMPARE(project.mapDataUndoStack_->count(), 2);
    const auto secondTerrain = Terrain(project), secondSettings = Settings(project);
    QVERIFY(secondTerrain != afterTerrain); QVERIFY(secondSettings != afterSettings);
    project.mapDataUndoStack_->undo(); QCOMPARE(Terrain(project), afterTerrain); QCOMPARE(Settings(project), afterSettings);
    project.mapDataUndoStack_->redo(); QCOMPARE(Terrain(project), secondTerrain); QCOMPARE(Settings(project), secondSettings);
}

void WorldTerrainBoundsTests::RejectsChangedRevision() {
    F::Fixture fixture; QtProject::EditorProject project; Install(project, fixture, true);
    WorldModule module(project, nullptr); Controls c(module.Widget());
    c.read->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY2(c.Ready(), qPrintable(c.status->text()));
    const auto beforeTerrain = Terrain(project);
    c.Set(2, 3, 30, 29); c.apply->click(); QVERIFY(c.Busy());
    auto settings = *project.MapEdits()->Snapshot().Settings(); settings.mapName = "concurrent-author-edit";
    QVERIFY(project.MapEdits()->EditSettings(settings).Succeeded());
    const auto revision = project.MapEdits()->Revision();
    const auto beforeSettings = Settings(project);
    QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QCOMPARE(c.Outcome(), QStringLiteral("failed")); QVERIFY(!c.Ready());
    QCOMPARE(project.MapEdits()->Revision(), revision); QCOMPARE(Terrain(project), beforeTerrain); QCOMPARE(Settings(project), beforeSettings);
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
}

void WorldTerrainBoundsTests::UsesActualCameraMargins() {
    F::Fixture fixture; auto members = fixture.members;
    for (auto& member : members) if (member.name == "UI/MiscData.txt") member.data = F::Text(
        "[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n[Water]\nDeepLevelPathing=128\n"
        "[CameraMargins]\nLeft=2\nRight=3\nBottom=4\nTop=1\n");
    QtProject::EditorProject project; Install(project, fixture, true, &members);
    WorldModule module(project, nullptr); Controls c(module.Widget());
    c.read->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY2(c.Ready(), qPrintable(c.status->text()));
    QCOMPARE(c.west->value(), 0); QCOMPARE(c.east->value(), 32);
    c.Set(12, 3, 16, 29); QVERIFY(!c.apply->isEnabled());
    c.Set(2, 3, 30, 29); QVERIFY(c.apply->isEnabled()); c.apply->click();
    QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QCOMPARE(c.Outcome(), QStringLiteral("applied"));
    const auto settings = *project.MapEdits()->Snapshot().Settings();
    const std::array<float, 8> camera{512, 896, 3456, 3584, 512, 3584, 3456, 896};
    QVERIFY(settings.cameraBounds == camera);
}

void WorldTerrainBoundsTests::DropsResultAfterMapSwitch() {
    F::Fixture first, second; QtProject::EditorProject project; Install(project, first, true);
    WorldModule module(project, nullptr); Controls c(module.Widget());
    c.read->click(); QVERIFY(c.Busy());
    Install(project, second, true); project.MapDocumentChanged();
    const auto session = project.MapEdits()->SessionIdentity();
    QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QVERIFY(!c.Ready()); QVERIFY(!c.apply->isEnabled());
    QCOMPARE(project.MapEdits()->SessionIdentity(), session); QCOMPARE(project.MapEdits()->Revision(), std::uint64_t{0});
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    c.read->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY2(c.Ready(), qPrintable(c.status->text()));
}

void WorldTerrainBoundsTests::RejectsActualDirectorySourceDrift() {
    F::Fixture fixture; QtProject::EditorProject project; Install(project, fixture, false);
    WorldModule module(project, nullptr); Controls c(module.Widget());
    c.read->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY2(c.Ready(), qPrintable(c.status->text()));
    const auto beforeTerrain = Terrain(project), beforeSettings = Settings(project);
    // 不发送伪来源变化信号，必须由worker实际文件SHA回读拒绝。
    QVERIFY(IO::WriteFileBytes(project.projectRoot_ / "UI/MiscData.txt", F::Text("[CameraMargins]\nLeft=2\nRight=2\nBottom=2\nTop=2\n")).Succeeded());
    c.Set(2, 3, 30, 29); c.apply->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QCOMPARE(c.Outcome(), QStringLiteral("failed")); QVERIFY(!c.Ready());
    QCOMPARE(Terrain(project), beforeTerrain); QCOMPARE(Settings(project), beforeSettings); QCOMPARE(project.mapDataUndoStack_->count(), 0);
}

void WorldTerrainBoundsTests::RejectsNonIntegerCurrentBounds() {
    F::Fixture fixture; auto members = fixture.members;
    auto settings = F::Take(MapData::MapSettings::Parse(members[2].data));
    settings.flags &= ~0x400u; settings.cameraBounds = {0.5f, 0, 4096, 4096, 0.5f, 4096, 4096, 0};
    members[2].data = F::Take(settings.Serialize());
    QtProject::EditorProject project; Install(project, fixture, true, &members);
    WorldModule module(project, nullptr); Controls c(module.Widget());
    const auto revision = project.MapEdits()->Revision();
    c.read->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QCOMPARE(c.Outcome(), QStringLiteral("failed")); QVERIFY(!c.Ready()); QVERIFY(!c.apply->isEnabled());
    QVERIFY(c.status->text().contains(QStringLiteral("整数地格")));
    QCOMPARE(project.MapEdits()->Revision(), revision); QCOMPARE(project.mapDataUndoStack_->count(), 0);
}

void WorldTerrainBoundsTests::RejectsGameResourceWinnerDrift() {
    F::Fixture fixture; auto members = fixture.members;
    std::erase_if(members, [](const auto& member) { return member.name == "UI/WorldEditData.txt"; });
    QtProject::EditorProject project; Install(project, fixture, true, &members);
    QVERIFY(WriteVerifiedArchive(project.war3GameRoot_ / "War3x.mpq",
        std::vector<MapArchiveMember>{{"UI/WorldEditData.txt", F::Text("[Other]\nValue=1\n")}}).Succeeded());
    WorldModule module(project, nullptr); Controls c(module.Widget());
    c.read->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY2(c.Ready(), qPrintable(c.status->text()));
    const auto beforeTerrain = Terrain(project), beforeSettings = Settings(project);
    QVERIFY(WriteVerifiedArchive(project.war3GameRoot_ / "War3Patch.mpq",
        std::vector<MapArchiveMember>{{"UI/WorldEditData.txt", F::Text("[Other]\nValue=2\n")}}).Succeeded());
    c.Set(2, 3, 30, 29); c.apply->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QCOMPARE(c.Outcome(), QStringLiteral("failed")); QVERIFY(!c.Ready());
    QCOMPARE(Terrain(project), beforeTerrain); QCOMPARE(Settings(project), beforeSettings); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    // 用户重新读取实际来源后，新owner和同一typed范围才可继续。
    c.read->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000); QVERIFY2(c.Ready(), qPrintable(c.status->text()));
    c.Set(2, 3, 30, 29); c.apply->click(); QTRY_VERIFY_WITH_TIMEOUT(!c.Busy(), 15000);
    QCOMPARE(c.Outcome(), QStringLiteral("applied")); QCOMPARE(project.mapDataUndoStack_->count(), 1);
}

#include "WorldTerrainResizeCases.h"

QTEST_MAIN(WorldTerrainBoundsTests)
#include "WorldTerrainBoundsTests.moc"
