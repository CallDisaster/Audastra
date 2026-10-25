#pragma once

#include "../TerrainBlockFixtures.h"

void LiveEditorToolsTests::TerrainBlockToolsUseCapturedResourcesCasAndOriginalUndo() {
    TerrainBlockFixtures::BlockFixture native; Fixture f;
    QVERIFY2(f.Initialize() && f.InstallBounds(native), qPrintable(f.error));
    auto* session = f.project.mapEdits_.get(); auto* undo = f.project.mapDataUndoStack_;
    const auto original = TerrainCornerRampFixtures::Take(MapData::SerializeMapTerrain(*session->Terrain()));
    const auto triggerFingerprint = f.Fingerprint();
    const auto state = f.tools->Invoke("get_terrain_edit_state", {});
    QVERIFY2(state.value("ok").toBool(), qPrintable(Explain(state)));
    auto request = f.Context(); request["terrain_binding"] = state.value("terrain_binding");
    request["source_x"] = 4; request["source_y"] = 4; request["width"] = 4; request["height"] = 4;
    request["destination_x"] = 20; request["destination_y"] = 20; request["follow_doodads"] = true;
    request["dry_run"] = true;
    int beforeWrites = 0; f.tools->SetBeforeWrite([&] { ++beforeWrites; return false; });
    const auto preview = f.tools->Invoke("edit_terrain_block", request);
    QVERIFY2(preview.value("ok").toBool(), qPrintable(Explain(preview)));
    QVERIFY(!preview.value("applied").toBool()); QVERIFY(!preview.value("map_saved").toBool());
    QCOMPARE(preview.value("source_block_sha256").toString().size(), qsizetype{64});
    QCOMPARE(session->Revision(), std::uint64_t{0}); QCOMPARE(undo->count(), 0); QCOMPARE(beforeWrites, 0);
    QCOMPARE(TerrainCornerRampFixtures::Take(MapData::SerializeMapTerrain(*session->Terrain())), original);
    for (const char* field : {"raw", "algorithm", "source_bytes", "rules_path", "expected_block_sha256", "verified"}) {
        auto invalid = request; invalid[field] = 1;
        QVERIFY(!f.tools->Invoke("edit_terrain_block", invalid).value("ok").toBool());
    }
    QCOMPARE(beforeWrites, 0);
    auto write = request; write["dry_run"] = false;
    QCOMPARE(ErrorCode(f.tools->Invoke("edit_terrain_block", write)), QStringLiteral("LIVE_EDITOR_DRAFT_REJECTED"));
    QCOMPARE(beforeWrites, 1); QCOMPARE(undo->count(), 0);
    f.tools->SetBeforeWrite({});
    QSignalSpy reloads(&f.project, &EditorProject::MapDocumentChanged);
    const auto applied = f.tools->Invoke("edit_terrain_block", write);
    QVERIFY2(applied.value("ok").toBool(), qPrintable(Explain(applied)));
    QVERIFY(applied.value("applied").toBool()); QVERIFY(!applied.value("map_saved").toBool());
    QCOMPARE(f.project.mapEdits_.get(), session); QCOMPARE(f.project.mapDataUndoStack_, undo);
    QCOMPARE(undo->count(), 1); QCOMPARE(reloads.count(), 0);
    native.CheckState(*session, true, true);
    const auto copied = TerrainCornerRampFixtures::Take(MapData::SerializeMapTerrain(*session->Terrain()));
    QVERIFY(copied != original);
    QVERIFY(!f.tools->Invoke("edit_terrain_block", write).value("ok").toBool()); QCOMPARE(undo->count(), 1);
    undo->undo(); native.CheckState(*session, false, true);
    undo->redo(); native.CheckState(*session, true, true);
    QCOMPARE(f.Fingerprint(), triggerFingerprint);
    QCOMPARE(Tests::WaterTake(IO::ReadFileBytes(f.project.projectRoot_ / "map/war3map.w3e")), original);
}
