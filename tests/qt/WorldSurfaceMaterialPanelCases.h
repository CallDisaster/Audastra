#pragma once

// 世界页材质面板：列表合并“表里的”与“摆放过的”类型；数值、二选一、清除、新类型、删除都只改涉及的行，
// 每次是资源撤销栈上的一步；撤销后面板跟着重读。在 WorldModuleTests.cpp 末尾包含。
#include "ProjectSurfaceMaterials.h"
#include "WorldSurfaceMaterialPanel.h"

#include <QMessageBox>
#include <QTimer>
#include <QTreeWidget>

namespace {
std::vector<std::byte> MaterialPanelTga(int width, int height, std::array<std::uint8_t, 4> rgba) {
    std::vector<std::byte> bytes(18, std::byte{0});
    bytes[2] = std::byte{2}; bytes[16] = std::byte{32}; bytes[17] = std::byte{0x28};
    bytes[12] = static_cast<std::byte>(width & 0xFF); bytes[13] = static_cast<std::byte>(width >> 8);
    bytes[14] = static_cast<std::byte>(height & 0xFF); bytes[15] = static_cast<std::byte>(height >> 8);
    for (int i = 0; i < width * height; ++i)
        for (const auto channel : {rgba[2], rgba[1], rgba[0], rgba[3]}) bytes.push_back(static_cast<std::byte>(channel));
    return bytes;
}
// 下一个弹出的确认框：记下文字并按 answer 作答。
void AnswerNextMaterialQuestion(QMessageBox::StandardButton answer, QString* text) {
    auto* timer = new QTimer;
    timer->setInterval(10);
    QObject::connect(timer, &QTimer::timeout, [timer, answer, text] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible() && box->button(answer)) {
                *text = box->text();
                timer->stop(); timer->deleteLater();
                box->button(answer)->click();
                return;
            }
    });
    timer->start();
}
QTreeWidgetItem* MaterialPanelRow(QTreeWidget* tree, const QString& rawcode) {
    for (int i = 0; tree && i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == rawcode) return tree->topLevelItem(i);
    return nullptr;
}
} // namespace

void WorldModuleTests::SurfaceMaterialPanelEditsTableInPlaceWithAssetUndo() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    auto doodads = MapData::ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11()); QVERIFY(doodads.Succeeded());
    for (const std::int32_t id : {3, 4}) {
        MapData::DoodadPlacement placed; placed.rawcode = "D002"; placed.scale = {1, 1, 1}; placed.creationId = id;
        doodads.value->doodads.push_back(placed);
    }
    const auto doodadBytes = doodads.value->Serialize(); QVERIFY(doodadBytes.Succeeded());
    const std::string original = "; 原注释\r\n[D001]\r\nnormal = Synthetic\\a.tga ; 法线\r\nfoo = 1\r\n";
    const auto* begin = reinterpret_cast<const std::byte*>(original.data());
    const std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3map.doo", *doodadBytes.value},
        {"WarVK\\materials.ini", std::vector<std::byte>(begin, begin + original.size())},
        {"Synthetic\\a.tga", MaterialPanelTga(4, 4, {128, 128, 255, 255})}};
    const auto mapPath = directory.filePath(QStringLiteral("material-panel.w3x"));
    QVERIFY(WriteVerifiedArchive(mapPath.toStdWString(), members).Succeeded());
    auto archive = MapArchiveSession::Open(mapPath.toStdWString()); QVERIFY(archive.Succeeded());
    QtProject::EditorProject project;
    project.sourceArchivePath_ = mapPath.toStdWString();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.ResetEditTracking();
    auto* undo = project.AssetUndoStack(); QVERIFY(undo);
    const auto tableText = [&] {
        const auto state = QtProject::ReadSurfaceMaterialTable(project);
        return state.Succeeded() && state.value ? QString::fromStdString(state.value->document.Text()) : QStringLiteral("<unreadable>");
    };

    WorldSurfaceMaterialPanel panel(project);
    panel.resize(380, 760); panel.show();
    auto* types = panel.findChild<QTreeWidget*>(QStringLiteral("worldMaterialTypes")); QVERIFY(types);
    // 表里的 D001 与只摆放过的 D002 都列出；校验在后台完成后问题列不再是“校验中”。
    QTRY_VERIFY(MaterialPanelRow(types, "D001") && MaterialPanelRow(types, "D002"));
    QCOMPARE(MaterialPanelRow(types, "D002")->text(1), QStringLiteral("2"));
    QCOMPARE(MaterialPanelRow(types, "D002")->text(2), QString());
    QCOMPARE(MaterialPanelRow(types, "D001")->text(2), QStringLiteral("表面"));
    QTRY_VERIFY(MaterialPanelRow(types, "D001") && MaterialPanelRow(types, "D001")->text(3).contains(QStringLiteral("提醒")));

    types->setCurrentItem(MaterialPanelRow(types, "D001"));
    QCOMPARE(panel.CurrentType(), QStringLiteral("D001"));
    QLineEdit* normal = nullptr;
    for (auto* path : panel.findChildren<QLineEdit*>(QStringLiteral("worldMaterialPath")))
        if (path->accessibleName() == QStringLiteral("法线")) normal = path;
    QVERIFY(normal);
    QCOMPARE(normal->text(), QStringLiteral("Synthetic\\a.tga"));
    auto* roughness = panel.findChild<QDoubleSpinBox*>(QStringLiteral("worldMaterialValue_roughness")); QVERIFY(roughness);
    QCOMPARE(roughness->value(), 1.0);
    QCOMPARE(roughness->property("materialWritten").toBool(), false);
    auto* diagnostics = panel.findChild<QListWidget*>(QStringLiteral("worldMaterialDiagnostics")); QVERIFY(diagnostics);
    QTRY_VERIFY(!diagnostics->findItems(QStringLiteral("没有摆放"), Qt::MatchContains).isEmpty()
        || !diagnostics->findItems(QStringLiteral("摆放"), Qt::MatchContains).isEmpty());

    // 数值：新键接在最后一个已知键之后，沿用“ = ”；一步撤销。
    roughness->setValue(0.5);
    QCOMPARE(undo->count(), 1);
    QCOMPARE(tableText(), QStringLiteral("; 原注释\r\n[D001]\r\nnormal = Synthetic\\a.tga ; 法线\r\nroughness = 0.5\r\nfoo = 1\r\n"));
    QTRY_COMPARE(roughness->property("materialWritten").toBool(), true);
    QCOMPARE(roughness->value(), 0.5);
    // 拖回原值不写。
    roughness->setValue(0.5);
    QCOMPARE(undo->count(), 1);

    // 清除唯一一张图会让表面类型整节失效：先问，答“否”不写；答“是”后带行内注释的行只留注释。
    auto* clearNormal = panel.findChild<QToolButton*>(QStringLiteral("worldMaterialReset_normal")); QVERIFY(clearNormal);
    QVERIFY(clearNormal->isEnabled());
    QString asked;
    AnswerNextMaterialQuestion(QMessageBox::No, &asked);
    clearNormal->click();
    QVERIFY2(asked.contains(QStringLiteral("不再生效")), qPrintable(asked));
    QCOMPARE(undo->count(), 1);
    QCOMPARE(normal->text(), QStringLiteral("Synthetic\\a.tga"));
    asked.clear();
    AnswerNextMaterialQuestion(QMessageBox::Yes, &asked);
    clearNormal->click();
    QVERIFY(!asked.isEmpty());
    QCOMPARE(undo->count(), 2);
    QCOMPARE(tableText(), QStringLiteral("; 原注释\r\n[D001]\r\n; 法线\r\nroughness = 0.5\r\nfoo = 1\r\n"));
    QTRY_COMPARE(normal->text(), QString());

    // 二选一：点“水面”写 shader，列表跟着变。
    auto* water = panel.findChild<QPushButton*>(QStringLiteral("worldMaterialShader1")); QVERIFY(water);
    water->click();
    QCOMPARE(undo->count(), 3);
    QCOMPARE(tableText(), QStringLiteral("; 原注释\r\n[D001]\r\n; 法线\r\nroughness = 0.5\r\nshader = water\r\nfoo = 1\r\n"));
    QTRY_COMPARE(MaterialPanelRow(types, "D001")->text(2), QStringLiteral("水面"));

    // 撤销三步：表恢复原样，面板重读。
    undo->undo(); undo->undo(); undo->undo();
    QCOMPARE(tableText(), QString::fromStdString(original));
    QTRY_COMPARE(normal->text(), QStringLiteral("Synthetic\\a.tga"));
    QTRY_COMPARE(roughness->value(), 1.0);

    // 世界里选中 D002：面板跟过去；它不在表里，改值时在表尾新加一节，删除后只留下分隔空行。
    panel.FollowType(QStringLiteral("D002"));
    QCOMPARE(panel.CurrentType(), QStringLiteral("D002"));
    auto* remove = panel.findChild<QPushButton*>(QStringLiteral("worldMaterialRemove")); QVERIFY(remove);
    QVERIFY(!remove->isEnabled());
    roughness->setValue(0.25);
    QCOMPARE(tableText(), QString::fromStdString(original) + QStringLiteral("\r\n[D002]\r\nroughness=0.25\r\n"));
    QTRY_VERIFY(remove->isEnabled());
    // 没有图片的表面节整节不生效：材质列直说，问题列计入表级的“没有图片”。
    QTRY_COMPARE(MaterialPanelRow(types, "D002")->text(2), QStringLiteral("不生效"));
    QTRY_VERIFY(MaterialPanelRow(types, "D002")->text(3).contains(QStringLiteral("提醒")));
    // 点已是生效值的“表面”不写。
    auto* surface = panel.findChild<QPushButton*>(QStringLiteral("worldMaterialShader0")); QVERIFY(surface);
    const auto before = undo->count();
    surface->click();
    QCOMPARE(undo->count(), before);
    remove->click();
    QCOMPARE(tableText(), QString::fromStdString(original) + QStringLiteral("\r\n"));
    // 撤销三步后的新修改丢掉了可重做的三步：栈里只剩“加 D002”和“删除 D002”。
    QCOMPARE(undo->count(), 2);
    undo->undo();
    QCOMPARE(tableText(), QString::fromStdString(original) + QStringLiteral("\r\n[D002]\r\nroughness=0.25\r\n"));
    undo->redo();
    QCOMPARE(tableText(), QString::fromStdString(original) + QStringLiteral("\r\n"));

    // 筛选“有材质”只留表里的类型；搜索按四字码。
    auto* filter = panel.findChild<QComboBox*>(QStringLiteral("worldMaterialFilter")); QVERIFY(filter);
    filter->setCurrentIndex(1);
    QTRY_VERIFY(MaterialPanelRow(types, "D001") && !MaterialPanelRow(types, "D002"));
    filter->setCurrentIndex(0);
    auto* search = panel.findChild<QLineEdit*>(QStringLiteral("worldMaterialSearch")); QVERIFY(search);
    search->setText(QStringLiteral("d002"));
    QVERIFY(!MaterialPanelRow(types, "D001") && MaterialPanelRow(types, "D002"));

    // 面板还没读过表时跟随世界选择：记下类型，显示后列表选中它。
    WorldSurfaceMaterialPanel hidden(project);
    hidden.FollowType(QStringLiteral("D002"));
    QCOMPARE(hidden.CurrentType(), QStringLiteral("D002"));
    hidden.resize(380, 760); hidden.show();
    auto* hiddenTypes = hidden.findChild<QTreeWidget*>(QStringLiteral("worldMaterialTypes")); QVERIFY(hiddenTypes);
    QTRY_VERIFY(hiddenTypes->currentItem() && hiddenTypes->currentItem()->data(0, Qt::UserRole).toString() == QStringLiteral("D002"));
}
