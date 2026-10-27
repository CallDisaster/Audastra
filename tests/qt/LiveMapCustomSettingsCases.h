#pragma once
void LiveEditorToolsTests::CustomSettingsShareOriginalUndoAndRejectStaleCas() {
    namespace C = AudastraTest::CustomSettings;
    C::Fixture native; Fixture fixture; QVERIFY2(fixture.Initialize(), qPrintable(fixture.error));
    for (const auto& member : native.members) if (member.name == "war3map.w3i" || member.name.ends_with(".doo") || member.name.ends_with(".slk") || member.name.starts_with("UI/"))
        QVERIFY(WriteFile(fixture.source.filePath("map/" + QString::fromStdString(member.name)), Bytes(member.data)));
    fixture.project.war3GameRoot_ = native.base.inputs.game;
    auto directory = DirectoryWorkspace::Open(fixture.project.projectRoot_); QVERIFY(directory.Succeeded());
    fixture.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    fixture.project.LoadMapDocument(); fixture.project.ResetEditTracking();
    auto request = fixture.Context(); request["section"] = "summary";
    auto summary = fixture.tools->Invoke("map_custom_settings_get", request); QVERIFY2(summary.value("ok").toBool(), qPrintable(Explain(summary)));
    auto edit = fixture.Context(); edit["settings_binding"] = summary.value("settings_binding"); edit["dry_run"] = true;
    edit["operations"] = QJsonArray{QJsonObject{{"section", "upgrades"}, {"op", "append"}, {"expected_count", 0},
        {"value", QJsonObject{{"player_mask", 1}, {"id_hex", "52303031"}, {"level", 1}, {"availability", 2}}}}};
    const auto before = fixture.project.editRevision_.Revision(); const auto undo = fixture.project.mapDataUndoStack_->count();
    auto preview = fixture.tools->Invoke("map_custom_settings_edit", edit); QVERIFY2(preview.value("ok").toBool(), qPrintable(Explain(preview)));
    QVERIFY(preview.value("has_changes").toBool()); QCOMPARE(fixture.project.editRevision_.Revision(), before); QCOMPARE(fixture.project.mapDataUndoStack_->count(), undo);
    edit["dry_run"] = false; auto committed = fixture.tools->Invoke("map_custom_settings_edit", edit); QVERIFY2(committed.value("ok").toBool(), qPrintable(Explain(committed)));
    QCOMPARE(fixture.project.mapDataUndoStack_->count(), undo + 1); QCOMPARE(fixture.project.MapEdits()->Snapshot().Settings()->custom->upgrades.size(), std::size_t{1});
    QVERIFY(!fixture.tools->Invoke("map_custom_settings_edit", edit).value("ok").toBool());
    fixture.project.mapDataUndoStack_->undo(); QVERIFY(fixture.project.MapEdits()->Snapshot().Settings()->custom->upgrades.empty());
    fixture.project.mapDataUndoStack_->redo(); QCOMPARE(fixture.project.MapEdits()->Snapshot().Settings()->custom->upgrades.size(), std::size_t{1});
    request = fixture.Context(); request["section"] = "summary"; summary = fixture.tools->Invoke("map_custom_settings_get", request);
    auto same = fixture.Context(); same["settings_binding"] = summary.value("settings_binding"); same["dry_run"] = false;
    same["operations"] = QJsonArray{QJsonObject{{"section", "upgrades"}, {"op", "update"}, {"index", 0}, {"expected_row_key", committed.value("step_keys").toArray().first()},
        {"value", QJsonObject{{"player_mask", 1}, {"id_hex", "52303031"}, {"level", 1}, {"availability", 2}}}}};
    const auto beforeNoOp = fixture.project.mapDataUndoStack_->count();
    const auto revisionNoOp = fixture.project.editRevision_.Revision();
    auto unchanged = fixture.tools->Invoke("map_custom_settings_edit", same); QVERIFY2(unchanged.value("ok").toBool(), qPrintable(Explain(unchanged)));
    QVERIFY(!unchanged.value("has_changes").toBool()); QCOMPARE(fixture.project.mapDataUndoStack_->count(), beforeNoOp); QCOMPARE(fixture.project.editRevision_.Revision(), revisionNoOp);
    edit = fixture.Context(); edit["settings_binding"] = summary.value("settings_binding"); edit["dry_run"] = false;
    edit["operations"] = QJsonArray{QJsonObject{{"section", "tech_restrictions"}, {"op", "append"}, {"expected_count", 0}, {"value", QJsonObject{{"player_mask", 1}, {"id_hex", "75303031"}}}},
        QJsonObject{{"section", "tech_restrictions"}, {"op", "append"}, {"expected_count", 0}, {"value", QJsonObject{{"player_mask", 1}, {"id_hex", "69303031"}}}}};
    const auto count = fixture.project.mapDataUndoStack_->count();
    auto rejected = fixture.tools->Invoke("map_custom_settings_edit", edit); QVERIFY(!rejected.value("ok").toBool()); QCOMPARE(ErrorCode(rejected), QString("SETTINGS_COUNT_STALE"));
    QCOMPARE(fixture.project.mapDataUndoStack_->count(), count); QVERIFY(fixture.project.MapEdits()->Snapshot().Settings()->custom->techRestrictions.empty());
}
