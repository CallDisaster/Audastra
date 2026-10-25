#pragma once

#include "../support/TerrainResizeFixture.h"

void LiveEditorToolsTests::ResizeToolsCaptureActualResourcesAndKeepOriginalUndo() {
    namespace R = AudastraTest::TerrainResize;
    R::Fixture native(true); Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    for (const auto& member : native.members)
        QVERIFY(WriteFile(f.source.filePath("map/" + QString::fromStdString(member.name)), Bytes(member.data)));
    f.project.war3GameRoot_ = native.gameRoot;
    auto workspace = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(workspace.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*workspace.value));
    f.project.LoadMapDocument(); f.project.ResetEditTracking();
    QVERIFY(f.project.mapEdits_ && f.project.mapEdits_->Terrain() && f.project.mapDataUndoStack_);
    auto* session = f.project.mapEdits_.get(); auto* undo = f.project.mapDataUndoStack_;
    const auto terrainBytes = [&] { return R::Take(MapData::SerializeMapTerrain(*session->Terrain())); };
    const auto originalTerrain = terrainBytes(), originalSettings = R::Take(session->Snapshot().Settings()->Serialize());
    const auto originalUnits = R::Take(session->Snapshot().Units()->Serialize());
    const auto originalDoodads = R::Take(session->Snapshot().Doodads()->Serialize());
    const auto originalTriggers = f.Fingerprint(); const auto originalContext = f.Context();
    const auto state = f.tools->Invoke("get_terrain_edit_state", {});
    QVERIFY2(state.value("ok").toBool(), qPrintable(Explain(state)));
    auto request = f.Context(); request["terrain_binding"] = state.value("terrain_binding");
    request["west"] = -32; request["south"] = -32; request["east"] = 32; request["north"] = 32; request["dry_run"] = true;
    int beforeWrites = 0; f.tools->SetBeforeWrite([&] { ++beforeWrites; return false; });
    const auto preview = f.tools->Invoke("edit_terrain_resize", request);
    QVERIFY2(preview.value("ok").toBool(), qPrintable(Explain(preview)));
    QVERIFY(!preview.value("applied").toBool() && !preview.value("map_saved").toBool());
    QVERIFY(preview.value("native_save_required").toBool());
    QCOMPARE(preview.value("changes").toObject().value("after_cells"), QJsonValue(QJsonArray{64, 64}));
    QCOMPARE(beforeWrites, 0); QCOMPARE(undo->count(), 0); QCOMPARE(terrainBytes(), originalTerrain);
    QCOMPARE(f.Context(), originalContext);
    auto write = request; write["dry_run"] = false;
    QCOMPARE(ErrorCode(f.tools->Invoke("edit_terrain_resize", write)), QStringLiteral("LIVE_EDITOR_DRAFT_REJECTED"));
    QCOMPARE(beforeWrites, 1); QCOMPARE(undo->count(), 0);
    f.tools->SetBeforeWrite({}); QSignalSpy reloads(&f.project, &EditorProject::MapDocumentChanged);
    QSignalSpy undoEvents(undo, &QUndoStack::indexChanged);
    const auto applied = f.tools->Invoke("edit_terrain_resize", write);
    QVERIFY2(applied.value("ok").toBool(), qPrintable(Explain(applied)));
    QVERIFY(applied.value("applied").toBool() && applied.value("edit_receipt").isObject());
    QVERIFY(!applied.value("map_saved").toBool());
    QCOMPARE(undo->count(), 1); QCOMPARE(undoEvents.count(), 1); QCOMPARE(reloads.count(), 0);
    QCOMPARE(f.project.mapEdits_.get(), session); QCOMPARE(f.project.mapDataUndoStack_, undo);
    QCOMPARE(terrainBytes(), R::TerrainLiteral(-32, -32, 32, 32));
    QCOMPARE(R::Take(session->Snapshot().Units()->Serialize()), originalUnits);
    QCOMPARE(R::Take(session->Snapshot().Doodads()->Serialize()), originalDoodads);
    QCOMPARE(f.Fingerprint(), originalTriggers); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    QVERIFY(!f.tools->Invoke("edit_terrain_resize", write).value("ok").toBool()); QCOMPARE(undo->count(), 1);
    undo->undo(); QCOMPARE(terrainBytes(), originalTerrain);
    QCOMPARE(R::Take(session->Snapshot().Settings()->Serialize()), originalSettings);
    undo->redo(); QCOMPARE(terrainBytes(), R::TerrainLiteral(-32, -32, 32, 32));
    auto again = f.Context(); again["terrain_binding"] = f.tools->Invoke("get_terrain_edit_state", {}).value("terrain_binding");
    again["west"] = 0; again["south"] = 0; again["east"] = 64; again["north"] = 64; again["dry_run"] = false;
    const auto unchanged = f.tools->Invoke("edit_terrain_resize", again);
    QVERIFY2(unchanged.value("ok").toBool(), qPrintable(Explain(unchanged)));
    QVERIFY(!unchanged.value("applied").toBool()); QCOMPARE(undo->count(), 1);
    // 没有伪造来源信号；实际startup winner变化须由下一次捕获/复核发现。
    QVERIFY(IO::WriteFileBytes(native.gameRoot / "WorldEditData.txt", R::Text(
        "[WorldEditMisc]\nMinimumMapSize=32\nMaximumMapSize=256\n[CameraMargins]\nLeft=1\nRight=2\nBottom=1\nTop=2\n")).Succeeded());
    again["west"] = -32; again["dry_run"] = true;
    // 新请求允许采用新规则；旧物理W3E若变化则必须拒绝而不清原Undo。
    QVERIFY(WriteFile(f.source.filePath("map/war3map.w3e"), Bytes(R::TerrainLiteral(-32, 0, 32, 32))));
    QVERIFY(!f.tools->Invoke("edit_terrain_resize", again).value("ok").toBool());
    QCOMPARE(terrainBytes(), R::TerrainLiteral(-32, -32, 32, 32)); QCOMPARE(undo->count(), 1);
}
