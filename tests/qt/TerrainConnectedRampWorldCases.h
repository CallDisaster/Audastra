#pragma once

#include "../TerrainConnectedRampFixtures.h"

void WorldModuleTests::TerrainConnectedRampUsesFrozenPreviewAndUndo() {
    TerrainConnectedRampFixtures::ConnectedFixture fixture;
    QtProject::EditorProject project; InstallStraightWorldProject(project, fixture);
    const auto sourceSha = Sha256File(fixture.source->SourcePath()); QVERIFY(sourceSha.Succeeded());
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(900, 600); page->show(); QTRY_COMPARE(viewport->VertexCount(), std::size_t{1089});
    auto* brush = page->findChild<QToolButton*>("worldTerrainBrushStraightRamp");
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle");
    auto* status = page->findChild<QLabel*>("worldTerrainWaterBrushStatus");
    auto* terrainStatus = page->findChild<QLabel*>("worldTerrainStatus");
    QVERIFY(brush && toggle && status && terrainStatus); brush->click(); QTRY_VERIFY(status->property("ready").toBool());
    QVERIFY(toggle->isEnabled()); toggle->click(); QVERIFY(viewport->BrushMode());
    viewport->TerrainStrokeStarted(fixture.point.x + 7, fixture.point.y + 7);
    QTRY_VERIFY(terrainStatus->text().startsWith(QStringLiteral("笔刷预览中")));
    // 预览保持当前来源与Undo不动；拖动不会把连接变成第二个编辑点。
    fixture.CheckState(*project.mapEdits_, false); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    viewport->TerrainStrokeMoved(-4096, -4096); viewport->TerrainStrokeFinished();
    QTRY_COMPARE(project.mapDataUndoStack_->count(), 1);
    fixture.CheckState(*project.mapEdits_, true);
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked());
    project.mapDataUndoStack_->undo(); fixture.CheckState(*project.mapEdits_, false);
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked());
    project.mapDataUndoStack_->redo(); fixture.CheckState(*project.mapEdits_, true);
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked());
    auto* remove = page->findChild<QPushButton*>("worldTerrainRampMode1"); QVERIFY(remove); remove->click();
    viewport->TerrainStrokeStarted(fixture.point.x, fixture.point.y); viewport->TerrainStrokeFinished();
    QTRY_COMPARE(project.mapDataUndoStack_->count(), 2);
    fixture.CheckState(*project.mapEdits_, false);
    QCOMPARE(Sha256File(fixture.source->SourcePath()).value, sourceSha.value);
}

void WorldModuleTests::TerrainConnectedRampRejectsStaleStroke() {
    TerrainConnectedRampFixtures::ConnectedFixture fixture;
    QtProject::EditorProject project; InstallStraightWorldProject(project, fixture);
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(900, 600); page->show(); QTRY_COMPARE(viewport->VertexCount(), std::size_t{1089});
    auto* brush = page->findChild<QToolButton*>("worldTerrainBrushStraightRamp");
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle");
    auto* status = page->findChild<QLabel*>("worldTerrainWaterBrushStatus");
    QVERIFY(brush && toggle && status); brush->click(); QTRY_VERIFY(status->property("ready").toBool());
    toggle->click(); QVERIFY(viewport->BrushMode());
    viewport->TerrainStrokeStarted(fixture.point.x, fixture.point.y);
    viewport->TerrainStrokeFinished(); QVERIFY(!toggle->isEnabled());
    // 已经进入冻结候选的后台准备，再改变live会话，必须由发布前CAS拒绝。
    MapData::TerrainBrush external; external.kind = MapData::TerrainBrushKind::Raise;
    external.size = 1; external.strength = 1;
    const MapData::TerrainStrokePoint elsewhere{128, 128};
    QVERIFY(project.mapEdits_->EditTerrain(external, std::span(&elsewhere, 1), false).Succeeded());
    const auto changed = Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    const auto revision = project.mapEdits_->Revision();
    QTRY_VERIFY(status->property("verifyMilliseconds").isValid());
    QCOMPARE(project.mapEdits_->Revision(), revision);
    QCOMPARE(Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), changed);
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
}
