#pragma once

// 在TerrainPaletteWorldMembers及WorldGroundPaletteAppendCases.h后包含。
namespace {
std::vector<MapArchiveMember> CliffAppendWorldMembers() {
    auto members = TerrainPaletteWorldMembers();
    for (auto& member : members) if (member.name == "TerrainArt/CliffTypes.slk")
        member.data = Tests::WaterText("ID;PWXL;N;E\nC;X1;Y1;K\"cliffID\"\n"
            "C;X1;Y2;K\"Cl01\"\nC;X1;Y3;K\"Cl02\"\nC;X1;Y4;K\"Cl03\"\nE\n");
    return members;
}
void InstallCliffAppendWorldProject(QtProject::EditorProject& project, const std::filesystem::path& root, bool archive) {
    const auto members = CliffAppendWorldMembers();
    Tests::InstallTerrainWaterProject(project, root, archive, members);
    if (!project.mapEdits_->AttachPathing(members.back().data).Succeeded()) throw std::runtime_error("attach owned cliff append pathing");
}
} // namespace

void WorldModuleTests::TerrainTextureSlotCliffAppendUsesFrozenSourceAndSharedUndo_data() {
    QTest::addColumn<bool>("archive");
    QTest::newRow("directory") << false; QTest::newRow("archive") << true;
}

void WorldModuleTests::TerrainTextureSlotCliffAppendUsesFrozenSourceAndSharedUndo() {
    QFETCH(bool, archive);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QtProject::EditorProject project;
    InstallCliffAppendWorldProject(project, std::filesystem::path(directory.path().toStdWString()), archive);
    const auto sourcePath = archive ? project.sourceArchivePath_ : project.projectRoot_ / "war3map.w3e";
    const auto sourceSha = Sha256File(sourcePath); QVERIFY(sourceSha.Succeeded());
    const auto original = Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend); auto* page = module.Widget(); state.viewport = module.Viewport();
    page->resize(900, 600); page->show(); QTRY_COMPARE(module.Viewport()->VertexCount(), std::size_t{81});
    auto* append = page->findChild<QPushButton*>("worldTerrainAddCliffTexture"); QVERIFY(append);
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle"); QVERIFY(toggle);
    for (unsigned step = 0; step < 2; ++step) {
        QTRY_VERIFY(toggle->isEnabled());
        AcceptTerrainPaletteDialog(page, step ? QStringLiteral("Cl03") : QStringLiteral("Cl02")); append->click();
        QVERIFY(page->property("terrainPalettePending").toBool());
        QTRY_VERIFY(!page->property("terrainPalettePending").toBool());
        QCOMPARE(page->property("terrainPaletteOutcome").toString(), QStringLiteral("applied"));
        QCOMPARE(project.mapDataUndoStack_->count(), static_cast<int>(step + 1));
        const auto exported = project.mapEdits_->ExportTerrainStrokeJournal(); QVERIFY(exported.Succeeded());
        QCOMPARE(exported.value->strokes.size(), std::size_t{step + 1});
        const auto& palette = exported.value->strokes.back().intent.palette;
        QVERIFY(palette && palette->algorithm == 3 && palette->table == MapData::TerrainTextureTable::Cliff
            && palette->slot == step + 1 && palette->expectedRawcode.empty());
    }
    QCOMPARE(project.mapEdits_->Terrain()->header.cliffTilesets, (std::vector<std::string>{"Cl01", "Cl02", "Cl03"}));
    const auto final = Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    const auto captured = archive ? Services::TerrainPathingSave::CaptureJournal(*project.archiveSession_, *project.mapEdits_)
        : Services::TerrainPathingSave::CaptureJournal(project.directorySnapshot_->Snapshot(), *project.mapEdits_);
    QVERIFY(captured.Succeeded()); const auto wire = MapData::EncodeTerrainStrokeJournal(*captured.value); QVERIFY(wire.Succeeded());
    QCOMPARE(wire.value->at(3), std::byte{'6'});
    project.mapDataUndoStack_->undo(); project.mapDataUndoStack_->undo();
    QCOMPARE(Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), original);
    project.mapDataUndoStack_->redo(); project.mapDataUndoStack_->redo();
    QCOMPARE(Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), final);
    QCOMPARE(Sha256File(sourcePath).value, sourceSha.value);
}

void WorldModuleTests::TerrainTextureSlotCliffAppendRejectsStale_data() {
    QTest::addColumn<int>("change");
    QTest::newRow("edit-revision") << 0; QTest::newRow("saving") << 1;
    QTest::newRow("session-replaced") << 2; QTest::newRow("source-suspected") << 3;
    QTest::newRow("game-root-changed") << 4;
}

void WorldModuleTests::TerrainTextureSlotCliffAppendRejectsStale() {
    QFETCH(int, change);
    QTemporaryDir directory; QVERIFY(directory.isValid()); QtProject::EditorProject project;
    InstallCliffAppendWorldProject(project, std::filesystem::path(directory.path().toStdWString()), true);
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend); auto* page = module.Widget(); state.viewport = module.Viewport();
    page->resize(900, 600); page->show(); QTRY_COMPARE(module.Viewport()->VertexCount(), std::size_t{81});
    auto* append = page->findChild<QPushButton*>("worldTerrainAddCliffTexture"); QVERIFY(append);
    AcceptTerrainPaletteDialog(page, QStringLiteral("Cl02")); append->click();
    QVERIFY(page->property("terrainPalettePending").toBool());
    if (change == 0) QVERIFY(project.mapEdits_->EditTerrainTextureSlot(MapData::TerrainTextureTable::Ground, 0, "Tg03").Succeeded());
    if (change == 1) project.savingMap_ = true;
    if (change == 2) {
        auto fresh = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
        QVERIFY(fresh->AttachTerrain(CliffAppendWorldMembers().front().data).Succeeded()); project.mapEdits_ = std::move(fresh);
    }
    if (change == 3) project.sourceChangeSuspected_ = true;
    if (change == 4) project.war3GameRoot_ /= "changed-owned-root";
    const auto expected = Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    QTRY_VERIFY(!page->property("terrainPalettePending").toBool());
    QCOMPARE(page->property("terrainPaletteOutcome").toString(), QStringLiteral("stale"));
    QCOMPARE(Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), expected);
    QCOMPARE(project.mapEdits_->Terrain()->header.cliffTilesets.size(), std::size_t{1});
    QCOMPARE(project.mapDataUndoStack_->count(), 0); project.savingMap_ = false;
}
