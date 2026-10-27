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

// 地图设置页的科技表与 Agent 工具走同一服务：表单添加、选中删除各一个撤销记录；未应用草稿时拒绝且不改地图。
void LiveEditorToolsTests::SettingsPageEditsTechnologyThroughSharedService() {
    namespace C = AudastraTest::CustomSettings;
    C::Fixture native; Fixture fixture; QVERIFY2(fixture.Initialize(), qPrintable(fixture.error));
    for (const auto& member : native.members) if (member.name == "war3map.w3i" || member.name.ends_with(".doo") || member.name.ends_with(".slk") || member.name.starts_with("UI/"))
        QVERIFY(WriteFile(fixture.source.filePath("map/" + QString::fromStdString(member.name)), Bytes(member.data)));
    fixture.project.war3GameRoot_ = native.base.inputs.game;
    auto directory = DirectoryWorkspace::Open(fixture.project.projectRoot_); QVERIFY(directory.Succeeded());
    fixture.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    fixture.project.LoadMapDocument(); fixture.project.ResetEditTracking();
    War3Map::Modules::MapSettingsModule module(fixture.project);
    auto* widget = module.Widget();
    widget->resize(1100, 800);
    const auto page = [widget] { return widget->findChild<QWidget*>(QStringLiteral("mapTechPage")); };
    QVERIFY(page());
    auto* add = page()->findChild<QPushButton*>(QStringLiteral("techUpgradesAdd"));
    QVERIFY(add && !add->isHidden());
    const auto undo = fixture.project.mapDataUndoStack_->count();
    QString formProblem;
    bool filled = false;
    QTimer driver;
    driver.setInterval(20);
    QObject::connect(&driver, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != QStringLiteral("techRowDialog")) return;
        driver.stop();
        auto* object = dialog->findChild<QComboBox*>(QStringLiteral("techObject"));
        object->setCurrentIndex(object->findData(QStringLiteral("R001")));
        dialog->findChild<QSpinBox*>(QStringLiteral("techLevel"))->setValue(2);
        auto* state = dialog->findChild<QComboBox*>(QStringLiteral("techAvailability"));
        state->setCurrentIndex(state->findData(2));
        dialog->findChild<QCheckBox*>(QStringLiteral("techPlayer0"))->setChecked(true);
        if (const auto directory = qEnvironmentVariable("AUDASTRA_FORM_CAPTURE_DIR"); !directory.isEmpty()) {
            War3Map::UiKit::ThemeManager theme;
            theme.ApplyTheme(QStringLiteral("graphite"));
            dialog->grab().save(directory + QStringLiteral("/tech-row.png"));
        }
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        filled = true;
        if (dialog->isVisible()) {
            formProblem = dialog->findChild<QLabel*>(QStringLiteral("formProblem"))->text();
            dialog->reject();
        }
    });
    driver.start();
    add->click();
    QVERIFY(filled);
    QVERIFY2(formProblem.isEmpty(), qPrintable(formProblem));
    QTRY_COMPARE(fixture.project.mapDataUndoStack_->count(), undo + 1);
    const auto upgrades = fixture.project.MapEdits()->Snapshot().Settings()->custom->upgrades;
    QCOMPARE(upgrades.size(), std::size_t{1});
    QCOMPARE(upgrades[0].level, 1);
    QCOMPARE(upgrades[0].availability, 2);
    QCOMPARE(upgrades[0].playerMask, 1u);
    auto* table = page()->findChild<QTableWidget*>(QStringLiteral("techUpgrades"));
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 0)->toolTip(), QStringLiteral("R001"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("2"));
    QCOMPARE(table->item(0, 2)->text(), QStringLiteral("已研究"));
    if (const auto directory = qEnvironmentVariable("AUDASTRA_FORM_CAPTURE_DIR"); !directory.isEmpty()) {
        War3Map::UiKit::ThemeManager theme;
        theme.ApplyTheme(QStringLiteral("graphite"));
        widget->resize(1100, 640);
        widget->show();
        auto* sections = widget->findChild<QListWidget*>();
        QVERIFY(sections);
        const auto items = sections->findItems(QStringLiteral("科技与随机表"), Qt::MatchExactly);
        QVERIFY(!items.isEmpty());
        sections->setCurrentItem(items.front());
        QTest::qWait(100);
        widget->grab().save(directory + QStringLiteral("/tech-page.png"));
        widget->hide();
    }
    table->selectRow(0);
    page()->findChild<QPushButton*>(QStringLiteral("techUpgradesDelete"))->click();
    QTRY_COMPARE(fixture.project.mapDataUndoStack_->count(), undo + 2);
    QVERIFY(fixture.project.MapEdits()->Snapshot().Settings()->custom->upgrades.empty());
    fixture.project.mapDataUndoStack_->undo();
    QCOMPARE(fixture.project.MapEdits()->Snapshot().Settings()->custom->upgrades.size(), std::size_t{1});
    QTRY_COMPARE(page()->findChild<QTableWidget*>(QStringLiteral("techUpgrades"))->rowCount(), 1);

    // 上方有未应用草稿时科技提交被拒，地图与撤销栈不变。
    auto* gameData = widget->findChild<QComboBox*>(QStringLiteral("mapSettings_gameDataSet"));
    QVERIFY(gameData);
    gameData->setCurrentIndex(gameData->currentIndex() == 0 ? 1 : 0);
    QVERIFY(module.HasPendingDraft());
    const auto pendingUndo = fixture.project.mapDataUndoStack_->count();
    page()->findChild<QTableWidget*>(QStringLiteral("techUpgrades"))->selectRow(0);
    page()->findChild<QPushButton*>(QStringLiteral("techUpgradesDelete"))->click();
    QCoreApplication::processEvents();
    QCOMPARE(fixture.project.mapDataUndoStack_->count(), pendingUndo);
    QVERIFY(page()->findChild<QLabel*>(QStringLiteral("formProblem"))->text().contains(QStringLiteral("未应用")));
}

// 含随机表的地图：随机表按组只读列出（名称、列类型、概率、对象），初始化接通前科技表也只读。
void LiveEditorToolsTests::SettingsPageListsRandomTablesReadOnly() {
    namespace C = AudastraTest::CustomSettings;
    C::Fixture native; Fixture fixture; QVERIFY2(fixture.Initialize(), qPrintable(fixture.error));
    for (auto member : native.members) {
        if (member.name == "war3map.w3i") {
            auto settings = *MapDocument::FromMembers(native.members).Settings();
            MapData::MapRandomUnitTable units;
            units.number = 3; units.name = "Camp"; units.columnTypes = {0, 1};
            units.rows.push_back({60, {C::Raw("u001"), C::Raw("b001")}});
            units.rows.push_back({40, {C::Raw("u001"), MapData::SettingsRawcode{}}});
            settings.custom->randomUnits.push_back(units);
            MapData::MapRandomItemTable items;
            items.number = 0; items.name = "Drops"; items.sets = {{{30, C::Raw("i001")}, {70, MapData::SettingsRawcode{}}}};
            settings.custom->randomItems.push_back(items);
            auto bytes = settings.Serialize(); QVERIFY(bytes.Succeeded());
            member.data = *bytes.value;
        }
        if (member.name == "war3map.w3i" || member.name.ends_with(".doo") || member.name.ends_with(".slk") || member.name.starts_with("UI/"))
            QVERIFY(WriteFile(fixture.source.filePath("map/" + QString::fromStdString(member.name)), Bytes(member.data)));
    }
    fixture.project.war3GameRoot_ = native.base.inputs.game;
    auto directory = DirectoryWorkspace::Open(fixture.project.projectRoot_); QVERIFY(directory.Succeeded());
    fixture.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    fixture.project.LoadMapDocument(); fixture.project.ResetEditTracking();
    War3Map::Modules::MapSettingsModule module(fixture.project);
    auto* page = module.Widget()->findChild<QWidget*>(QStringLiteral("mapTechPage"));
    QVERIFY(page);
    QVERIFY(page->findChild<QPushButton*>(QStringLiteral("techUpgradesAdd"))->isHidden());
    auto* tree = page->findChild<QTreeWidget*>(QStringLiteral("techRandomTables"));
    QVERIFY(tree && tree->topLevelItemCount() == 2);
    auto* units = tree->topLevelItem(0);
    QCOMPARE(units->childCount(), 1);
    auto* camp = units->child(0);
    QVERIFY(camp->text(0).contains(QStringLiteral("Camp")) && camp->text(0).contains(QStringLiteral("3")));
    QCOMPARE(camp->text(1), QStringLiteral("单位"));
    QCOMPARE(camp->text(2), QStringLiteral("建筑"));
    QCOMPARE(camp->childCount(), 2);
    QCOMPARE(camp->child(0)->text(0), QStringLiteral("60%"));
    QCOMPARE(camp->child(0)->toolTip(1), QStringLiteral("u001"));
    QCOMPARE(camp->child(1)->text(2), QStringLiteral("无"));
    auto* drops = tree->topLevelItem(1)->child(0);
    QVERIFY(drops->text(0).contains(QStringLiteral("Drops")));
    QCOMPARE(drops->child(0)->childCount(), 2);
    QCOMPARE(drops->child(0)->child(0)->toolTip(1), QStringLiteral("i001"));
    QCOMPARE(drops->child(0)->child(1)->text(0), QStringLiteral("70%"));
    if (const auto directory = qEnvironmentVariable("AUDASTRA_FORM_CAPTURE_DIR"); !directory.isEmpty()) {
        War3Map::UiKit::ThemeManager theme;
        theme.ApplyTheme(QStringLiteral("graphite"));
        auto* widget = module.Widget();
        widget->resize(1100, 760);
        widget->show();
        auto* sections = widget->findChild<QListWidget*>();
        sections->setCurrentItem(sections->findItems(QStringLiteral("科技与随机表"), Qt::MatchExactly).front());
        tree->expandAll();
        QTest::qWait(100);
        widget->grab().save(directory + QStringLiteral("/tech-random.png"));
        widget->hide();
    }
}
