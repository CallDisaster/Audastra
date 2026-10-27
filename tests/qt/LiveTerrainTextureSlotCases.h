#pragma once

namespace TerrainSlotLiveDetail {
inline bool Install(Fixture& fixture, const AudastraTest::TextureSlot::Fixture& native) {
    auto archive = AuxiliarySaveFixtures::Take(MapArchiveSession::Open(native.source.path));
    for (const auto& member : archive.Members()) {
        if (member.name == "war3map.w3e" || member.name == "war3map.wpm" || member.name == "war3map.w3i"
            || member.name.ends_with(".doo") || member.name.ends_with(".slk") || member.name.starts_with("UI/"))
            if (!WriteFile(fixture.source.filePath("map/" + QString::fromStdString(member.name)), Bytes(member.data))) return false;
    }
    fixture.project.war3GameRoot_ = native.inputs.game;
    auto directory = DirectoryWorkspace::Open(fixture.project.projectRoot_);
    if (!directory.Succeeded()) return false;
    fixture.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    fixture.project.LoadMapDocument(); fixture.project.ResetEditTracking();
    return fixture.project.mapEdits_ && fixture.project.mapEdits_->Terrain() && fixture.project.mapDataUndoStack_
        && fixture.project.editTrackingReady_ && !fixture.project.editTrackingFailed_;
}
inline QJsonObject Request(Fixture& fixture, QJsonObject command, bool dryRun) {
    auto request = fixture.Context();
    request["terrain_binding"] = fixture.tools->Invoke("get_terrain_edit_state", {}).value("terrain_binding");
    request["command"] = std::move(command); request["dry_run"] = dryRun; return request;
}
}

void LiveEditorToolsTests::TextureSlotToolsCaptureActualResourcesAndKeepOriginalUndo() {
    namespace P = AudastraTest::TextureSlot;
    for (const bool ground : {true, false}) {
        P::Fixture native; Fixture f;
        QVERIFY2(f.Initialize() && TerrainSlotLiveDetail::Install(f, native), qPrintable(f.error));
        auto* session = f.project.mapEdits_.get(); auto* undo = f.project.mapDataUndoStack_;
        const auto original = P::Take(MapData::SerializeMapTerrain(*session->Terrain()));
        const auto triggers = f.Fingerprint(); const auto initialContext = f.Context();
        const std::string prefix = ground ? "Tg" : "Cl";
        const QJsonObject append{{"kind", "texture_slot_append"}, {"table", ground ? "ground" : "cliff"},
            {"expected_count", 1}, {"rawcode", QString::fromStdString(prefix + "02")}};
        auto request = TerrainSlotLiveDetail::Request(f, append, true);
        int beforeWrites = 0; f.tools->SetBeforeWrite([&] { ++beforeWrites; return false; });
        const auto preview = f.tools->Invoke("edit_terrain_texture_slot", request);
        QVERIFY2(preview.value("ok").toBool(), qPrintable(Explain(preview)));
        QVERIFY(!preview.value("applied").toBool()); QVERIFY(!preview.value("map_saved").toBool());
        QCOMPARE(preview.value("before_table").toArray().size(), 1); QCOMPARE(preview.value("after_table").toArray().size(), 2);
        QCOMPARE(preview.value("slot").toInt(), 1); QCOMPARE(beforeWrites, 0); QCOMPARE(undo->count(), 0);
        QCOMPARE(session->Revision(), std::uint64_t{0}); QCOMPARE(f.Context(), initialContext);
        QCOMPARE(P::Take(MapData::SerializeMapTerrain(*session->Terrain())), original);
        for (const char* field : {"project_identity", "expected_revision", "terrain_binding"}) {
            auto stale = request; stale[field] = "stale";
            QVERIFY(!f.tools->Invoke("edit_terrain_texture_slot", stale).value("ok").toBool());
        }
        auto bad = request; bad.remove("dry_run"); QVERIFY(!f.tools->Invoke("edit_terrain_texture_slot", bad).value("ok").toBool());
        bad = request; bad["dry_run"] = "true"; QVERIFY(!f.tools->Invoke("edit_terrain_texture_slot", bad).value("ok").toBool());
        for (const char* field : {"points", "algorithm", "rules", "follow_doodads", "host_path"}) {
            auto command = append; command[field] = true; bad = request; bad["command"] = command;
            QVERIFY(!f.tools->Invoke("edit_terrain_texture_slot", bad).value("ok").toBool());
        }
        QCOMPARE(beforeWrites, 0); QCOMPARE(undo->count(), 0);
        auto write = request; write["dry_run"] = false;
        QCOMPARE(ErrorCode(f.tools->Invoke("edit_terrain_texture_slot", write)), QStringLiteral("LIVE_EDITOR_DRAFT_REJECTED"));
        QCOMPARE(beforeWrites, 1); QCOMPARE(undo->count(), 0); f.tools->SetBeforeWrite({});
        QSignalSpy reloads(&f.project, &EditorProject::MapDocumentChanged);
        const auto applied = f.tools->Invoke("edit_terrain_texture_slot", write);
        QVERIFY2(applied.value("ok").toBool(), qPrintable(Explain(applied)));
        QVERIFY(applied.value("applied").toBool()); QVERIFY(applied.value("dirty_after").toBool());
        QVERIFY(!applied.value("map_saved").toBool()); QVERIFY(applied.value("terrain_binding_after").isObject());
        QCOMPARE(undo->count(), 1); QCOMPARE(reloads.count(), 0);
        QCOMPARE(f.project.mapEdits_.get(), session); QCOMPARE(f.project.mapDataUndoStack_, undo);
        const auto changed = P::Take(MapData::SerializeMapTerrain(*session->Terrain()));
        const auto journal = P::Take(session->ExportTerrainStrokeJournal());
        QVERIFY(journal.strokes.size() == 1 && journal.strokes[0].intent.palette);
        QCOMPARE(journal.strokes[0].intent.palette->algorithm, std::uint8_t(ground ? 2 : 3));
        QCOMPARE(journal.strokes[0].intent.palette->slot, std::uint32_t{1});
        QCOMPARE(journal.strokes[0].intent.palette->replacementRawcode, prefix + "02");
        auto direct = native.Request(ground ? "active-ground" : "active-cliff");
        const auto table = ground ? MapData::TerrainTextureTable::Ground : MapData::TerrainTextureTable::Cliff;
        direct.terrainStrokes = {P::Command(true, table, 1, "", prefix + "02")};
        (void)P::Take(Services::SaveMapEditCandidate(direct));
        auto candidate = P::Take(MapArchiveSession::Open(direct.targetCandidate));
        QCOMPARE(changed, P::Take(candidate.ReadMember("war3map.w3e")));
        QVERIFY(!f.tools->Invoke("edit_terrain_texture_slot", write).value("ok").toBool()); QCOMPARE(undo->count(), 1);
        undo->undo(); QCOMPARE(P::Take(MapData::SerializeMapTerrain(*session->Terrain())), original);
        undo->redo(); QCOMPARE(P::Take(MapData::SerializeMapTerrain(*session->Terrain())), changed);
        auto second = append; second["expected_count"] = 2; second["rawcode"] = QString::fromStdString(prefix + "03");
        const auto twice = f.tools->Invoke("edit_terrain_texture_slot", TerrainSlotLiveDetail::Request(f, second, false));
        QVERIFY2(twice.value("ok").toBool(), qPrintable(Explain(twice))); QCOMPARE(undo->count(), 2);
        QJsonObject replace{{"kind", "texture_slot_replace"}, {"table", ground ? "ground" : "cliff"}, {"slot", 1},
            {"expected_rawcode", QString::fromStdString(prefix + "02")}, {"rawcode", QString::fromStdString(prefix + "04")}};
        const auto replaced = f.tools->Invoke("edit_terrain_texture_slot", TerrainSlotLiveDetail::Request(f, replace, false));
        QVERIFY2(replaced.value("ok").toBool(), qPrintable(Explain(replaced))); QCOMPARE(undo->count(), 3);
        const auto stale = f.tools->Invoke("edit_terrain_texture_slot", TerrainSlotLiveDetail::Request(f, replace, false));
        QVERIFY(!stale.value("ok").toBool()); QCOMPARE(undo->count(), 3);
        replace["slot"] = 0; replace["expected_rawcode"] = QString::fromStdString(prefix + "01"); replace["rawcode"] = replace["expected_rawcode"];
        const auto noOp = f.tools->Invoke("edit_terrain_texture_slot", TerrainSlotLiveDetail::Request(f, replace, false));
        QVERIFY2(noOp.value("ok").toBool(), qPrintable(Explain(noOp))); QVERIFY(!noOp.value("applied").toBool()); QCOMPARE(undo->count(), 3);
        second["expected_count"] = 3; second["rawcode"] = "Miss";
        QVERIFY(!f.tools->Invoke("edit_terrain_texture_slot", TerrainSlotLiveDetail::Request(f, second, true)).value("ok").toBool());
        QCOMPARE(undo->count(), 3); QCOMPARE(f.Fingerprint(), triggers);
        QCOMPARE(Tests::WaterTake(IO::ReadFileBytes(f.project.projectRoot_ / "map/war3map.w3e")), original);
        auto driftRequest = TerrainSlotLiveDetail::Request(f, replace, true);
        QVERIFY(WriteFile(f.source.filePath("map/source-drift.txt"), "changed after capture"));
        QVERIFY(!f.tools->Invoke("edit_terrain_texture_slot", driftRequest).value("ok").toBool()); QCOMPARE(undo->count(), 3);
    }
}

void LiveEditorToolsTests::TextureSlotPreparationRejectsConcurrentRevision() {
    namespace P = AudastraTest::TextureSlot;
    P::Fixture native; Fixture f;
    QVERIFY2(f.Initialize() && TerrainSlotLiveDetail::Install(f, native), qPrintable(f.error));
    auto* session = f.project.mapEdits_.get(); const auto original = P::Take(MapData::SerializeMapTerrain(*session->Terrain()));
    const QJsonObject append{{"kind", "texture_slot_append"}, {"table", "ground"}, {"expected_count", 1}, {"rawcode", "Tg02"}};
    const auto request = TerrainSlotLiveDetail::Request(f, append, false);
    bool timerRan = false, authorChanged = false; QJsonObject reentered;
    QTimer::singleShot(0, f.tools.get(), [&] {
        timerRan = true; auto settings = *session->Snapshot().Settings(); settings.mapName = "concurrent-author-palette-edit";
        authorChanged = session->EditSettings(settings).Succeeded();
        reentered = f.tools->Invoke("edit_terrain_texture_slot", request);
    });
    const auto rejected = f.tools->Invoke("edit_terrain_texture_slot", request);
    QVERIFY(timerRan); QVERIFY(authorChanged); QCOMPARE(ErrorCode(reentered), QStringLiteral("LIVE_EDITOR_BUSY"));
    QVERIFY(!rejected.value("ok").toBool()); QCOMPARE(ErrorCode(rejected), QStringLiteral("TERRAIN_EDIT_STALE"));
    QCOMPARE(session->Revision(), std::uint64_t{1}); QCOMPARE(f.project.mapDataUndoStack_->count(), 0);
    QCOMPARE(session->Snapshot().Settings()->mapName, std::string("concurrent-author-palette-edit"));
    QCOMPARE(P::Take(MapData::SerializeMapTerrain(*session->Terrain())), original);
}
