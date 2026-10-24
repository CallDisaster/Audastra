#pragma once

#include "../TerrainStraightRampFixtures.h"

namespace {
void InstallStraightWorldProject(QtProject::EditorProject& project,
    const TerrainStraightRampFixtures::StraightFixture& fixture) {
    project.war3GameRoot_ = fixture.game;
    project.projectRoot_ = fixture.root / "world-workspace";
    TerrainStraightRampFixtures::Require(std::filesystem::create_directory(project.projectRoot_), "private world workspace");
    project.sourceArchivePath_ = fixture.source->SourcePath();
    project.archiveSession_ = std::make_shared<MapArchiveSession>(
        TerrainStraightRampFixtures::Take(MapArchiveSession::Open(project.sourceArchivePath_)));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    TerrainStraightRampFixtures::Require(project.mapEdits_->AttachTerrain(fixture.members[0].data).Succeeded()
        && project.mapEdits_->AttachPathing(fixture.members[1].data).Succeeded(), "complete straight world source");
    project.ResetEditTracking();
}
}

void WorldModuleTests::TerrainStraightRampUsesFrozenSingleClickAndUndo() {
    TerrainStraightRampFixtures::StraightFixture fixture;
    QtProject::EditorProject project; InstallStraightWorldProject(project, fixture);
    const auto sourceSha = Sha256File(fixture.source->SourcePath()); QVERIFY(sourceSha.Succeeded());
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(900, 600); page->show(); QTRY_COMPARE(viewport->VertexCount(), std::size_t{1089});
    auto* brush = page->findChild<QToolButton*>("worldTerrainBrushStraightRamp");
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle");
    auto* status = page->findChild<QLabel*>("worldTerrainWaterBrushStatus");
    auto* radius = page->findChild<QSpinBox*>("worldTerrainRadius");
    QVERIFY(brush && toggle && status && radius); brush->click();
    QVERIFY(!radius->isEnabled()); QTRY_VERIFY(status->property("ready").toBool());
    QVERIFY(toggle->isEnabled()); toggle->click(); QVERIFY(viewport->BrushMode());
    // 鼠标按下会规范吸附，拖动到无效地形也不能变成第二笔或扩大这笔。
    viewport->TerrainStrokeStarted(fixture.point.x + 7, fixture.point.y + 7);
    viewport->TerrainStrokeMoved(-4096, -4096);
    viewport->TerrainStrokeFinished();
    QTRY_COMPARE(project.mapDataUndoStack_->count(), 1);
    fixture.CheckState(*project.mapEdits_, true);
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked());
    project.mapDataUndoStack_->undo(); fixture.CheckState(*project.mapEdits_, false);
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked());
    project.mapDataUndoStack_->redo(); fixture.CheckState(*project.mapEdits_, true);
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked());
    auto* remove = page->findChild<QPushButton*>("worldTerrainRampMode1"); QVERIFY(remove); remove->click();
    viewport->TerrainStrokeStarted(fixture.point.x, fixture.point.y);
    viewport->TerrainStrokeFinished();
    QTRY_COMPARE(project.mapDataUndoStack_->count(), 2);
    fixture.CheckState(*project.mapEdits_, false);
    QCOMPARE(Sha256File(fixture.source->SourcePath()).value, sourceSha.value);
}

void WorldModuleTests::TerrainStraightRampRejectsStaleStroke() {
    TerrainStraightRampFixtures::StraightFixture fixture;
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
    MapData::TerrainBrush external; external.kind = MapData::TerrainBrushKind::Raise;
    external.size = 1; external.strength = 1;
    const MapData::TerrainStrokePoint elsewhere{128, 128};
    QVERIFY(project.mapEdits_->EditTerrain(external, std::span(&elsewhere, 1), false).Succeeded());
    const auto changed = Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    const auto revision = project.mapEdits_->Revision();
    viewport->TerrainStrokeFinished();
    QCOMPARE(project.mapEdits_->Revision(), revision);
    QCOMPARE(Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), changed);
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
}
