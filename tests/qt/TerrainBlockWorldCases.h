#pragma once

#include "../TerrainBlockFixtures.h"
#include "Commands/MapDataCommands.h"

void WorldModuleTests::TerrainBlockSelectionUsesCompletedGroundWithoutModels() {
    BackendState state; activeBackend = &state;
    WorldViewport viewport(nullptr, &backend); state.viewport = &viewport;
    viewport.resize(320, 240); QVERIFY(BindTerrain(viewport, TestTerrain())); viewport.show();
    QTRY_VERIFY(state.frames > 0);
    QSignalSpy blocks(&viewport, &WorldViewport::TerrainBlockSelected);
    QSignalSpy instances(&viewport, &WorldViewport::InstancesBoxSelected);
    QSignalSpy moves(&viewport, &WorldViewport::MoveStarted);
    QSignalSpy cancelled(&viewport, &WorldViewport::TerrainBlockSelectionCancelled);
    QSignalSpy copy(&viewport, &WorldViewport::CopyRequested), paste(&viewport, &WorldViewport::PasteRequested);
    QCOMPARE(viewport.ModelInstanceCount(), std::size_t{0});
    viewport.SetTerrainBlockSelection(true); QVERIFY(viewport.TerrainBlockSelection()); QVERIFY(!viewport.BrushMode());
    const QPointF start(120, 120), finish(240, 80);
    Mouse(viewport, QEvent::MouseMove, start, Qt::NoButton, Qt::NoButton);
    QTRY_VERIFY(viewport.HoverCoordinate().has_value());
    const auto camera = viewport.SaveCamera();
    Mouse(viewport, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseMove, finish, Qt::NoButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, finish, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(blocks.count(), 1); QVERIFY(instances.isEmpty()); QVERIFY(moves.isEmpty());
    QCOMPARE(viewport.SaveCamera(), camera);
    QVERIFY(std::abs(blocks[0][0].toDouble() + 32) < 0.01);
    QVERIFY(std::abs(blocks[0][2].toDouble() - 64) < 0.01);
    QTest::keyClick(&viewport, Qt::Key_C, Qt::ControlModifier);
    QTest::keyClick(&viewport, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(copy.count(), 1); QCOMPARE(paste.count(), 1);

    // 真实完成帧在按下后被替换，本次手势整笔取消，不能在新视图反推旧落点。
    Mouse(viewport, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    ++state.viewEpoch; Frame(&state);
    Mouse(viewport, QEvent::MouseMove, finish, Qt::NoButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, finish, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(blocks.count(), 1);
    Mouse(viewport, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseMove, finish, Qt::NoButton, Qt::LeftButton);
    QTest::keyClick(&viewport, Qt::Key_Escape);
    QVERIFY(!viewport.TerrainBlockSelection()); QCOMPARE(cancelled.count(), 1);
    Mouse(viewport, QEvent::MouseButtonRelease, finish, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(blocks.count(), 1);
}

namespace {
void InstallBlockWorldProject(QtProject::EditorProject& project, const TerrainBlockFixtures::BlockFixture& fixture) {
    project.war3GameRoot_ = fixture.game;
    project.projectRoot_ = fixture.root / "block-world-workspace";
    TerrainCornerRampFixtures::Require(std::filesystem::create_directory(project.projectRoot_), "private block world workspace");
    project.sourceArchivePath_ = fixture.source->SourcePath();
    project.archiveSession_ = std::make_shared<MapArchiveSession>(
        TerrainCornerRampFixtures::Take(MapArchiveSession::Open(project.sourceArchivePath_)));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    TerrainCornerRampFixtures::Require(project.mapEdits_->AttachTerrain(fixture.members[0].data).Succeeded()
        && project.mapEdits_->AttachPathing(fixture.members[1].data).Succeeded(), "complete block world source");
    project.ResetEditTracking();
}
}

void WorldModuleTests::TerrainBlockPasteFreezesSourceAndUsesSharedUndo() {
    TerrainBlockFixtures::BlockFixture fixture;
    QtProject::EditorProject project; InstallBlockWorldProject(project, fixture);
    const auto original = TerrainCornerRampFixtures::Take(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    const auto sourceSha = Sha256File(fixture.source->SourcePath()); QVERIFY(sourceSha.Succeeded());
    BackendState state; activeBackend = &state; WorldModule module(project, &backend);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(900, 600); page->show(); QTRY_COMPARE(viewport->VertexCount(), std::size_t{1089});
    auto* select = page->findChild<QPushButton*>("worldTerrainBlockSelect");
    auto* copy = page->findChild<QPushButton*>("worldTerrainBlockCopy");
    auto* paste = page->findChild<QPushButton*>("worldTerrainBlockPaste");
    auto* status = page->findChild<QLabel*>("worldTerrainBlockStatus");
    auto* resources = page->findChild<QLabel*>("worldTerrainWaterBrushStatus");
    QVERIFY(select && copy && paste && status && resources);
    QTRY_VERIFY(select->isEnabled()); select->click(); QVERIFY(viewport->TerrainBlockSelection());
    viewport->TerrainBlockSelected(513, 513, 1023, 1023); copy->click();
    QVERIFY(status->text().contains(QStringLiteral("已复制")));
    paste->click(); QTRY_VERIFY(resources->property("ready").toBool());
    QVERIFY(viewport->Placing());
    viewport->PlacementHovered(2561, 2561, 0, false);
    QVERIFY(!state.overlayLines.empty());
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QCOMPARE(TerrainCornerRampFixtures::Take(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), original);
    viewport->PlaceRequested(2561, 2561, false);
    QTRY_COMPARE(project.mapDataUndoStack_->count(), 1);
    const auto copied = TerrainCornerRampFixtures::Take(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    QVERIFY(copied != original);
    const auto& before = fixture.grid; const auto& after = *project.mapEdits_->Terrain();
    for (std::uint32_t y = 0; y <= 4; ++y) for (std::uint32_t x = 0; x <= 4; ++x) {
        const auto& source = before.vertices[(4 + y) * 33 + 4 + x];
        const auto& target = after.vertices[(20 + y) * 33 + 20 + x];
        QCOMPARE(target.groundHeightRaw, source.groundHeightRaw);
        QCOMPARE(target.waterHeightAndFlagsRaw, source.waterHeightAndFlagsRaw);
        QCOMPARE(target.textureAndFlagsRaw, source.textureAndFlagsRaw);
        QCOMPARE(target.variationRaw, source.variationRaw);
        QCOMPARE(target.cliffAndLayerRaw, source.cliffAndLayerRaw);
    }
    project.mapDataUndoStack_->undo();
    QCOMPARE(TerrainCornerRampFixtures::Take(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), original);
    project.mapDataUndoStack_->redo();
    QCOMPARE(TerrainCornerRampFixtures::Take(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), copied);
    QTRY_VERIFY(viewport->TerrainBlockSelection());
    paste->click(); QTRY_VERIFY(resources->property("ready").toBool());
    viewport->PlaceRequested(3073, 2561, false);
    QTRY_COMPARE(project.mapDataUndoStack_->count(), 2); // 源块没变，同一剪贴板可以重复粘贴。
    QCOMPARE(Sha256File(fixture.source->SourcePath()).value, sourceSha.value);
}

void WorldModuleTests::TerrainBlockPasteRejectsChangedSourceAndPendingCas() {
    TerrainBlockFixtures::BlockFixture fixture;
    QtProject::EditorProject project; InstallBlockWorldProject(project, fixture);
    BackendState state; activeBackend = &state; WorldModule module(project, &backend);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(900, 600); page->show(); QTRY_COMPARE(viewport->VertexCount(), std::size_t{1089});
    auto* select = page->findChild<QPushButton*>("worldTerrainBlockSelect");
    auto* copy = page->findChild<QPushButton*>("worldTerrainBlockCopy");
    auto* paste = page->findChild<QPushButton*>("worldTerrainBlockPaste");
    auto* resources = page->findChild<QLabel*>("worldTerrainWaterBrushStatus");
    auto* status = page->findChild<QLabel*>("worldTerrainBlockStatus");
    QVERIFY(select && copy && paste && resources && status);
    select->click(); viewport->TerrainBlockSelected(513, 513, 1023, 1023); copy->click();
    paste->click(); QTRY_VERIFY(resources->property("ready").toBool());
    viewport->PlaceRequested(2561, 2561, false);
    // 点击已进入冻结后台Prepare；live session再变化，发布CAS必须整笔拒绝。
    MapData::TerrainBrush external; external.kind = MapData::TerrainBrushKind::Raise; external.size = 1; external.strength = 1;
    const MapData::TerrainStrokePoint elsewhere{128, 128};
    QVERIFY(project.mapEdits_->EditTerrain(external, std::span(&elsewhere, 1), false).Succeeded());
    const auto changed = TerrainCornerRampFixtures::Take(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    const auto revision = project.mapEdits_->Revision();
    QTRY_VERIFY(status->text().startsWith(QStringLiteral("地形块未粘贴")));
    QCOMPARE(project.mapEdits_->Revision(), revision); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QCOMPARE(TerrainCornerRampFixtures::Take(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), changed);
    QTRY_VERIFY(viewport->TerrainBlockSelection());
    // 源区修改后即使仍在同session，也不能把Copy时旧值当作可用payload。
    const MapData::TerrainStrokePoint source{640, 640};
    QVERIFY(project.mapEdits_->EditTerrain(external, std::span(&source, 1), false).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("external source edit"));
    QtProject::PushMapDataEdit(project, "external source edit", "external source edit");
    QTRY_VERIFY(viewport->TerrainBlockSelection());
    paste->click(); QTRY_VERIFY(resources->property("ready").toBool());
    const auto sourceChanged = TerrainCornerRampFixtures::Take(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    const auto beforePaste = project.mapEdits_->Revision();
    viewport->PlaceRequested(2561, 2561, false);
    QTRY_VERIFY(status->text().startsWith(QStringLiteral("地形块未粘贴")));
    QCOMPARE(project.mapEdits_->Revision(), beforePaste);
    QCOMPARE(TerrainCornerRampFixtures::Take(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), sourceChanged);
}

void WorldModuleTests::TerrainBlockCancellationKeepsClipboardAndReloadClearsIt() {
    TerrainBlockFixtures::BlockFixture fixture;
    QtProject::EditorProject project; InstallBlockWorldProject(project, fixture);
    BackendState state; activeBackend = &state; WorldModule module(project, &backend);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(900, 600); page->show(); QTRY_COMPARE(viewport->VertexCount(), std::size_t{1089});
    auto* select = page->findChild<QPushButton*>("worldTerrainBlockSelect");
    auto* copy = page->findChild<QPushButton*>("worldTerrainBlockCopy");
    auto* paste = page->findChild<QPushButton*>("worldTerrainBlockPaste");
    auto* resources = page->findChild<QLabel*>("worldTerrainWaterBrushStatus");
    auto* status = page->findChild<QLabel*>("worldTerrainBlockStatus");
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle");
    QVERIFY(select && copy && paste && resources && status && toggle);
    select->click(); viewport->TerrainBlockSelected(513, 513, 1023, 1023); copy->click();
    paste->click(); QTRY_VERIFY(resources->property("ready").toBool());
    QTest::keyClick(viewport, Qt::Key_Escape); QVERIFY(!viewport->Placing());
    paste->click(); QVERIFY(viewport->Placing()); // 取消落点不应销毁同图来源绑定。
    QTRY_VERIFY(resources->property("ready").toBool());
    viewport->PlaceRequested(2561, 2561, false);
    QTest::keyClick(viewport, Qt::Key_Escape); // 冻结Prepare已排队，发布前取消。
    QTRY_VERIFY(toggle->isEnabled());
    QCOMPARE(project.mapDataUndoStack_->count(), 0); QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{0});
    fixture.CheckState(*project.mapEdits_, false);
    paste->click(); QVERIFY(viewport->Placing());
    project.MapDocumentChanged(); // 重载/切图必须使原剪贴板失效。
    QTRY_COMPARE(viewport->VertexCount(), std::size_t{1089});
    paste->click(); QVERIFY(!viewport->Placing());
    QVERIFY(status->text().contains(QStringLiteral("重新复制")));
    QCOMPARE(project.mapDataUndoStack_->count(), 0); fixture.CheckState(*project.mapEdits_, false);
}
