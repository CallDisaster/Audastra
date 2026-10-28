#pragma once
#include "ProjectMapCustomSettings.h"
void LiveEditorToolsTests::CustomSettingsShareOriginalUndoAndRejectStaleCas() {
    namespace C = AudastraTest::CustomSettings;
    C::Fixture native; Fixture fixture; QVERIFY2(fixture.Initialize(false, false), qPrintable(fixture.error));
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
    if (Script::RandomMapInitializationAvailable()) {
        edit = fixture.Context(); edit["settings_binding"] = summary.value("settings_binding"); edit["dry_run"] = true;
        edit["operations"] = QJsonDocument::fromJson(QByteArray::fromStdString(C::Fixture::RandomUnitOperationsJson())).array();
        auto randomPreview = fixture.tools->Invoke("map_custom_settings_edit", edit); QVERIFY2(randomPreview.value("ok").toBool(), qPrintable(Explain(randomPreview)));
        QVERIFY(randomPreview.value("can_commit").toBool()); QCOMPARE(fixture.project.mapDataUndoStack_->count(), count);
        edit["dry_run"] = false; auto random = fixture.tools->Invoke("map_custom_settings_edit", edit); QVERIFY2(random.value("ok").toBool(), qPrintable(Explain(random)));
        QCOMPARE(fixture.project.mapDataUndoStack_->count(), count + 1);
        QCOMPARE(fixture.project.MapEdits()->Snapshot().Settings()->custom->randomUnits.front().number, 7);
        QVERIFY(!fixture.tools->Invoke("map_custom_settings_edit", edit).value("ok").toBool());
        fixture.project.mapDataUndoStack_->undo(); QVERIFY(fixture.project.MapEdits()->Snapshot().Settings()->custom->randomUnits.empty());
        fixture.project.mapDataUndoStack_->redo(); QCOMPARE(fixture.project.MapEdits()->Snapshot().Settings()->custom->randomUnits.front().rows.front().chance, 30);
        auto frozen = War3Map::QtProject::CaptureProjectMapCustomSettingsValidation(fixture.project); QVERIFY(frozen.Succeeded());
        ++fixture.project.luaMapTargetGeneration_;
        const auto changedTarget = frozen.value->verify(); QVERIFY(!changedTarget.Succeeded());
        QCOMPARE(QString::fromStdString(changedTarget.diagnostics.front().code), QString("SETTINGS_PROJECT_STALE"));
        fixture.project.luaMapTarget_.emplace(); ++fixture.project.luaMapTargetGeneration_;
        request = fixture.Context(); request["section"] = "summary";
        summary = fixture.tools->Invoke("map_custom_settings_get", request);
        edit = fixture.Context(); edit["settings_binding"] = summary.value("settings_binding"); edit["dry_run"] = true;
        edit["operations"] = QJsonDocument::fromJson(QByteArray::fromStdString(C::Fixture::RandomUnitOperationsJson())).array();
        const auto targetUndo = fixture.project.mapDataUndoStack_->count();
        auto blocked = fixture.tools->Invoke("map_custom_settings_edit", edit); QVERIFY(!blocked.value("ok").toBool());
        QCOMPARE(ErrorCode(blocked), QString("SETTINGS_RANDOM_LUA_UNSUPPORTED")); QCOMPARE(fixture.project.mapDataUndoStack_->count(), targetUndo);
    }
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

// 原创小图加一张随机单位组 Camp #3（单位/建筑两列）与一张随机物品表 Drops #0 后打开。
inline bool OpenRandomTableMap(AudastraTest::CustomSettings::Fixture& native, Fixture& fixture) {
    namespace C = AudastraTest::CustomSettings;
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
            auto bytes = settings.Serialize();
            if (!bytes.Succeeded()) return false;
            member.data = *bytes.value;
        }
        if (member.name == "war3map.w3i" || member.name.ends_with(".doo") || member.name.ends_with(".slk") || member.name.starts_with("UI/"))
            if (!WriteFile(fixture.source.filePath("map/" + QString::fromStdString(member.name)), Bytes(member.data))) return false;
    }
    fixture.project.war3GameRoot_ = native.base.inputs.game;
    auto directory = DirectoryWorkspace::Open(fixture.project.projectRoot_);
    if (!directory.Succeeded()) return false;
    fixture.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    fixture.project.LoadMapDocument(); fixture.project.ResetEditTracking();
    return fixture.project.MapEdits() != nullptr;
}

// 含随机表的地图：随机表按组只读列出（名称、列类型、概率、对象），初始化接通前科技表也只读。
void LiveEditorToolsTests::SettingsPageListsRandomTablesReadOnly() {
    AudastraTest::CustomSettings::Fixture native; Fixture fixture; QVERIFY2(fixture.Initialize(), qPrintable(fixture.error));
    QVERIFY(OpenRandomTableMap(native, fixture));
    War3Map::Modules::MapSettingsModule module(fixture.project);
    auto* page = module.Widget()->findChild<QWidget*>(QStringLiteral("mapTechPage"));
    QVERIFY(page);
    QVERIFY(page->findChild<QPushButton*>(QStringLiteral("techUpgradesAdd"))->isHidden());
    QVERIFY(!page->findChild<QPushButton*>(QStringLiteral("techRandomAddUnits")));
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

// 随机表表单：修改已有单位组（编号与列不可改，名称不变沿用原编码）、新建物品表、删除，各生成一条带 CAS 的服务命令。
void LiveEditorToolsTests::TechPageBuildsRandomTableCommands() {
    namespace C = AudastraTest::CustomSettings;
    using War3Map::Services::MapCustomSettingsAction;
    using War3Map::Services::MapCustomSettingsSection;
    C::Fixture native; Fixture fixture; QVERIFY2(fixture.Initialize(), qPrintable(fixture.error));
    QVERIFY(OpenRandomTableMap(native, fixture));
    const auto settings = *fixture.project.MapEdits()->Snapshot().Settings();
    std::vector<War3Map::Services::MapCustomSettingsCommand> commands;
    War3Map::Modules::MapTechPage page(fixture.project, settings, false, {}, {},
        [&](War3Map::Services::MapCustomSettingsCommand command, const QString&) {
            commands.push_back(std::move(command));
            return QString{};
        });
    page.resize(1000, 760);
    auto* tree = page.findChild<QTreeWidget*>(QStringLiteral("techRandomTables"));
    auto* edit = page.findChild<QPushButton*>(QStringLiteral("techRandomEdit"));
    QVERIFY(tree && edit && !edit->isEnabled());
    const QString capture = qEnvironmentVariable("AUDASTRA_FORM_CAPTURE_DIR");
    War3Map::UiKit::ThemeManager theme;
    if (!capture.isEmpty()) theme.ApplyTheme(QStringLiteral("graphite"));
    QString problem;
    bool driven = false;
    const auto drive = [&](const QString& name, std::function<void(QDialog*)> act) {
        driven = false;
        auto* timer = new QTimer(&page);
        timer->setInterval(20);
        QObject::connect(timer, &QTimer::timeout, [&, timer, name, act] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != name) return;
            timer->stop();
            timer->deleteLater();
            act(dialog);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            driven = true;
            if (dialog->isVisible()) {
                problem = dialog->findChild<QLabel*>(QStringLiteral("formProblem"))->text();
                dialog->reject();
            }
        });
        timer->start();
    };

    tree->setCurrentItem(tree->topLevelItem(0)->child(0)->child(1));
    QVERIFY(edit->isEnabled());
    bool locked = false;
    drive(QStringLiteral("randomUnitTableDialog"), [&](QDialog* dialog) {
        auto* number = dialog->findChild<QSpinBox*>(QStringLiteral("randomTableNumber"));
        auto* rows = dialog->findChild<QTableWidget*>(QStringLiteral("randomUnitRows"));
        locked = number->value() == 3 && !number->isEnabled()
            && !dialog->findChild<QComboBox*>(QStringLiteral("randomColumnType0"))->isEnabled()
            && dialog->findChild<QPushButton*>(QStringLiteral("randomAddColumn"))->isHidden()
            && dialog->findChild<QLineEdit*>(QStringLiteral("randomTableName"))->text() == QStringLiteral("Camp")
            && rows->rowCount() == 2 && rows->columnCount() == 3;
        rows->item(1, 0)->setText(QStringLiteral("35"));
        const auto index = rows->model()->index(1, 2);
        rows->setCurrentIndex(index);
        rows->edit(index);
        auto* object = rows->findChild<QComboBox*>(QStringLiteral("randomObjectEditor"));
        if (!object) return;
        object->setCurrentIndex(object->findData(QStringLiteral("b001")));
        rows->itemDelegateForIndex(index)->setModelData(object, rows->model(), index);
        if (!capture.isEmpty()) dialog->grab().save(capture + QStringLiteral("/random-units.png"));
    });
    edit->click();
    QVERIFY(driven);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QVERIFY(locked);
    QCOMPARE(commands.size(), std::size_t{1});
    QCOMPARE(commands[0].section, MapCustomSettingsSection::RandomUnits);
    QCOMPARE(commands[0].action, MapCustomSettingsAction::Update);
    QCOMPARE(commands[0].number, std::optional<std::int32_t>{3});
    QVERIFY(commands[0].keepName);
    QCOMPARE(commands[0].expectedKey.size(), std::size_t{64});
    const auto& units = std::get<MapData::MapRandomUnitTable>(commands[0].value);
    QCOMPARE(units.number, 3);
    QCOMPARE(units.columnTypes, (std::vector<std::int32_t>{0, 1}));
    QCOMPARE(units.rows.size(), std::size_t{2});
    QCOMPARE(units.rows[0].chance, 60);
    QCOMPARE(units.rows[1].chance, 35);
    QVERIFY(units.rows[1].ids[0] == C::Raw("u001"));
    QVERIFY(units.rows[1].ids[1] == C::Raw("b001"));

    bool fresh = false;
    drive(QStringLiteral("randomItemTableDialog"), [&](QDialog* dialog) {
        auto* number = dialog->findChild<QSpinBox*>(QStringLiteral("randomTableNumber"));
        auto* sets = dialog->findChild<QTreeWidget*>(QStringLiteral("randomItemSets"));
        fresh = number->isEnabled() && number->value() == 1 && sets->topLevelItemCount() == 1;
        auto* entry = sets->topLevelItem(0)->child(0);
        sets->setCurrentItem(entry, 1);
        const auto index = sets->currentIndex();
        sets->edit(index);
        if (auto* object = sets->findChild<QComboBox*>(QStringLiteral("randomObjectEditor"))) {
            object->setCurrentIndex(object->findData(QStringLiteral("i001")));
            sets->itemDelegateForIndex(index)->setModelData(object, sets->model(), index);
        }
        dialog->findChild<QPushButton*>(QStringLiteral("randomAddSet"))->click();
        dialog->findChild<QPushButton*>(QStringLiteral("randomAddItem"))->click();
        if (!capture.isEmpty()) dialog->grab().save(capture + QStringLiteral("/random-items.png"));
    });
    page.findChild<QPushButton*>(QStringLiteral("techRandomAddItems"))->click();
    QVERIFY(driven);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QVERIFY(fresh);
    QCOMPARE(commands.size(), std::size_t{2});
    QCOMPARE(commands[1].section, MapCustomSettingsSection::RandomItems);
    QCOMPARE(commands[1].action, MapCustomSettingsAction::Append);
    QCOMPARE(commands[1].expectedCount, std::optional<std::uint32_t>{1});
    QVERIFY(!commands[1].keepName);
    const auto& items = std::get<MapData::MapRandomItemTable>(commands[1].value);
    QCOMPARE(items.number, 1);
    QCOMPARE(items.sets.size(), std::size_t{2});
    QCOMPARE(items.sets[0].size(), std::size_t{1});
    QCOMPARE(items.sets[0][0].chance, 100);
    QVERIFY(items.sets[0][0].id == C::Raw("i001"));
    QCOMPARE(items.sets[1].size(), std::size_t{1});
    QVERIFY(items.sets[1][0].id == MapData::SettingsRawcode{});

    tree->setCurrentItem(tree->topLevelItem(1)->child(0));
    page.findChild<QPushButton*>(QStringLiteral("techRandomDelete"))->click();
    QCOMPARE(commands.size(), std::size_t{3});
    QCOMPARE(commands[2].section, MapCustomSettingsSection::RandomItems);
    QCOMPARE(commands[2].action, MapCustomSettingsAction::Delete);
    QCOMPARE(commands[2].number, std::optional<std::int32_t>{0});
    QCOMPARE(commands[2].expectedKey.size(), std::size_t{64});
    if (!capture.isEmpty()) {
        page.show();
        tree->expandAll();
        QTest::qWait(100);
        page.grab().save(capture + QStringLiteral("/random-page.png"));
        page.hide();
    }
}
