#include "Widgets/ObjectCatalogWidgets.h"
#include "Widgets/ObjectTreeBrowser.h"
#include "ObjectClassification.h"
#include "Icons/IconProvider.h"

#include <QApplication>
#include <QComboBox>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

using War3Map::QtApp::IconProvider;
using War3Map::QtApp::ObjectCatalogWidget;
using War3Map::QtApp::ObjectPickerPopup;
using War3Map::QtApp::ObjectViewEntry;
using War3Map::QtApp::OrderedObjectListWidget;

namespace {

QVector<ObjectViewEntry> Entries() {
    // 合成目录，不载入或分发 Warcraft 数据。故意不存在的路径用于缺失图标测试。
    return {
        {QStringLiteral("unit"), QStringLiteral("hfoo"), QStringLiteral("步兵"), {},
            QStringLiteral("近战"), QStringLiteral("人族"), {}, {}, true},
        {QStringLiteral("unit"), QStringLiteral("hrif"), QStringLiteral("火枪手"), {},
            QStringLiteral("远程"), QStringLiteral("人族"), {}, {}, true},
        {QStringLiteral("ability"), QStringLiteral("Adef"), QStringLiteral("坚守阵地"),
            QStringLiteral("test-only/missing-icon.blp"), QStringLiteral("单位技能"),
            QStringLiteral("人族"), QStringLiteral("合成描述 <保持文字>"), {QStringLiteral("冷却：8")}, true},
        {QStringLiteral("ability"), QStringLiteral("AHhb"), QStringLiteral("圣光治疗"), {},
            QStringLiteral("英雄技能"), QStringLiteral("人族"), {}, {}, true},
        {QStringLiteral("ability"), QStringLiteral("Asta"), QStringLiteral("攻击强化"), {},
            QStringLiteral("单位技能"), QStringLiteral("兽族"), {}, {}, true},
        {QStringLiteral("ability"), QStringLiteral("Anul"), QStringLiteral("不可用技能"), {},
            QStringLiteral("单位技能"), QStringLiteral("其他"), {}, {}, false},
        {QStringLiteral("item"), QStringLiteral("phea"), QStringLiteral("治疗药水"), {},
            QStringLiteral("消耗品"), QStringLiteral("中立"), {}, {}, true}
    };
}

QPushButton* Button(QWidget* parent, const QString& text) {
    for (auto* button : parent->findChildren<QPushButton*>()) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

QComboBox* Combo(QWidget* parent, const QString& name) {
    for (auto* combo : parent->findChildren<QComboBox*>()) {
        if (combo->accessibleName() == name) {
            return combo;
        }
    }
    return nullptr;
}

QListView* Grid(QWidget* parent) {
    return parent->findChild<QListView*>(QStringLiteral("objectCatalogGrid"));
}

void ShowWidget(QWidget& widget) {
    widget.resize(900, 620);
    widget.show();
    QApplication::processEvents();
}

bool ChooseId(ObjectPickerPopup* picker, const QString& id) {
    picker->Catalog()->SetSearchText(id);
    QApplication::processEvents();
    auto* view = Grid(picker);
    if (!view || view->model()->rowCount() != 1) {
        return false;
    }
    const QModelIndex index = view->model()->index(0, 0);
    if (index.data(Qt::UserRole).toString() != id || !(index.flags() & Qt::ItemIsEnabled)) {
        return false;
    }
    view->scrollTo(index);
    const QRect rect = view->visualRect(index);
    if (rect.isEmpty()) {
        return false;
    }
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
    QApplication::processEvents();
    return true;
}

} // namespace

class ObjectCatalogWidgetTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void TypeSearchAndCategoryFilters();
    void ClickChoosesAndHoverReportsMissingIcon();
    void OrderedIdsPreserveUnknownAndDuplicates();
    void AddReplaceCancelAndExternalChange();
    void NonModalPopupStaysOnScreenAndOutsideClickPassesThrough();
    void ClassificationFollowsWorldEditor();
    void ObjectTreeGroupsCountsAndSelects();
};

void ObjectCatalogWidgetTests::initTestCase() {
    qRegisterMetaType<ObjectViewEntry>();
}

void ObjectCatalogWidgetTests::TypeSearchAndCategoryFilters() {
    ObjectCatalogWidget catalog(nullptr);
    catalog.SetEntries(Entries());
    ShowWidget(catalog);
    auto* view = Grid(&catalog);
    QVERIFY(view);
    QCOMPARE(view->model()->rowCount(), 7);
    catalog.SetKindFilter(QStringLiteral("ability"), true);
    QCOMPARE(view->model()->rowCount(), 4);
    QVERIFY(!Combo(&catalog, QStringLiteral("物体类型"))->isEnabled());
    catalog.SetSearchText(QStringLiteral("hfoo"));
    QCOMPARE(view->model()->rowCount(), 0);
    catalog.SetSearchText(QStringLiteral("adef"));
    QCOMPARE(view->model()->rowCount(), 1);
    QCOMPARE(view->model()->index(0, 0).data(Qt::UserRole).toString(), QStringLiteral("Adef"));
    catalog.SetSearchText(QStringLiteral("人族 技能"));
    QCOMPARE(view->model()->rowCount(), 2);
    catalog.SetSearchText({});
    auto* categories = Combo(&catalog, QStringLiteral("分类筛选"));
    QVERIFY(categories);
    categories->setCurrentIndex(categories->findData(QStringLiteral("英雄技能")));
    QCOMPARE(view->model()->rowCount(), 1);
    QCOMPARE(view->model()->index(0, 0).data(Qt::UserRole).toString(), QStringLiteral("AHhb"));
    categories->setCurrentIndex(0);
    auto* races = Combo(&catalog, QStringLiteral("种族筛选"));
    races->setCurrentIndex(races->findData(QStringLiteral("兽族")));
    QCOMPARE(view->model()->rowCount(), 1);
    QCOMPARE(view->model()->index(0, 0).data(Qt::UserRole).toString(), QStringLiteral("Asta"));
    races->setCurrentIndex(0);
    catalog.SetKindFilter({}, false);
    auto* directory = catalog.findChild<QTreeWidget*>(QStringLiteral("objectCatalogDirectory"));
    QVERIFY(directory);
    // 真实点击目录分类，覆盖重建目录时项目指针的生命周期。
    const auto items = directory->findItems(QStringLiteral("英雄技能"), Qt::MatchExactly | Qt::MatchRecursive);
    QCOMPARE(items.size(), 1);
    directory->expandItem(items.front()->parent());
    directory->scrollToItem(items.front());
    QTest::mouseClick(directory->viewport(), Qt::LeftButton, Qt::NoModifier, directory->visualItemRect(items.front()).center());
    QCOMPARE(view->model()->rowCount(), 1);
    QCOMPARE(view->model()->index(0, 0).data(Qt::UserRole).toString(), QStringLiteral("AHhb"));
}

void ObjectCatalogWidgetTests::ClickChoosesAndHoverReportsMissingIcon() {
    IconProvider provider;
    ObjectCatalogWidget catalog(&provider);
    catalog.SetEntries(Entries());
    catalog.SetDirectoryVisible(false);
    catalog.SetKindFilter(QStringLiteral("ability"), true);
    catalog.SetSearchText(QStringLiteral("Adef"));
    ShowWidget(catalog);
    auto* view = Grid(&catalog);
    const auto index = view->model()->index(0, 0);
    catalog.SetCurrentId({});
    QSignalSpy chosen(&catalog, &ObjectCatalogWidget::EntryChosen);
    QSignalSpy selected(&catalog, &ObjectCatalogWidget::EntrySelected);
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->visualRect(index).center());
    QCOMPARE(chosen.count(), 1);
    QCOMPARE(qvariant_cast<ObjectViewEntry>(chosen.front().front()).id, QStringLiteral("Adef"));
    QCOMPARE(selected.count(), 1);
    QVERIFY(index.data(Qt::DecorationRole).value<QIcon>().isNull());
    const auto tooltip = index.data(Qt::ToolTipRole).toString();
    QVERIFY(tooltip.contains(QStringLiteral("图标未找到或无法解码")));
    QVERIFY(tooltip.contains(QStringLiteral("&lt;保持文字&gt;")));
    QVERIFY(tooltip.contains(QStringLiteral("冷却：8")));
    QVERIFY(index.data(Qt::DisplayRole).toString().contains(QStringLiteral("图标未找到")));
    // 高亮不提交；键盘 Enter 才提交。
    chosen.clear();
    catalog.SetSearchText({});
    catalog.SetCurrentId(QStringLiteral("AHhb"));
    QCOMPARE(chosen.count(), 0);
    view->setFocus();
    QTest::keyClick(view, Qt::Key_Return);
    QCOMPARE(chosen.count(), 1);
    QCOMPARE(qvariant_cast<ObjectViewEntry>(chosen.front().front()).id, QStringLiteral("AHhb"));
    catalog.SetSearchText(QStringLiteral("Anul"));
    const auto unavailable = view->model()->index(0, 0);
    QVERIFY(!(unavailable.flags() & Qt::ItemIsEnabled));
    QVERIFY(unavailable.data(Qt::DisplayRole).toString().contains(QStringLiteral("资源不可用")));
}

void ObjectCatalogWidgetTests::OrderedIdsPreserveUnknownAndDuplicates() {
    OrderedObjectListWidget ordered(nullptr);
    ordered.SetKindFilter(QStringLiteral("ability"));
    ordered.SetEntries(Entries());
    const QStringList original{QStringLiteral("Adef"), QStringLiteral("UNKNOWN"), QStringLiteral("Adef")};
    QSignalSpy changes(&ordered, &OrderedObjectListWidget::IdsChanged);
    ordered.SetIds(original);
    ordered.SetEntries(Entries());
    ordered.RefreshIcons();
    QCOMPARE(ordered.Ids(), original);
    QCOMPARE(changes.count(), 0);
    auto* list = ordered.findChild<QListWidget*>(QStringLiteral("orderedObjectSelection"));
    QVERIFY(list);
    QCOMPARE(list->count(), 3);
    QVERIFY(list->item(1)->text().contains(QStringLiteral("UNKNOWN")));
    QVERIFY(list->item(1)->text().contains(QStringLiteral("未解析引用")));
    ShowWidget(ordered);
    list->setCurrentRow(1);
    QTest::mouseClick(Button(&ordered, QStringLiteral("下移")), Qt::LeftButton);
    QCOMPARE(ordered.Ids(), (QStringList{QStringLiteral("Adef"), QStringLiteral("Adef"), QStringLiteral("UNKNOWN")}));
    QCOMPARE(changes.count(), 1);
    QTest::mouseClick(Button(&ordered, QStringLiteral("上移")), Qt::LeftButton);
    QCOMPARE(ordered.Ids(), original);
    QCOMPARE(changes.count(), 2);
    ordered.SetReadOnly(true);
    for (const auto& text : {QStringLiteral("添加"), QStringLiteral("替换"), QStringLiteral("删除"), QStringLiteral("上移"), QStringLiteral("下移")}) {
        auto* button = Button(&ordered, text);
        QVERIFY(button);
        QVERIFY(!button->isEnabled());
        QTest::mouseClick(button, Qt::LeftButton);
    }
    QCOMPARE(ordered.Ids(), original);
    QCOMPARE(changes.count(), 2);
    ordered.SetReadOnly(false);
    QTest::mouseClick(Button(&ordered, QStringLiteral("删除")), Qt::LeftButton);
    QCOMPARE(ordered.Ids(), (QStringList{QStringLiteral("Adef"), QStringLiteral("Adef")}));
    QCOMPARE(changes.count(), 3);
}

void ObjectCatalogWidgetTests::AddReplaceCancelAndExternalChange() {
    OrderedObjectListWidget ordered(nullptr);
    ordered.SetEntries(Entries());
    ordered.SetKindFilter(QStringLiteral("ability"));
    ordered.SetIds({QStringLiteral("Adef")});
    ShowWidget(ordered);
    QSignalSpy changes(&ordered, &OrderedObjectListWidget::IdsChanged);
    auto* list = ordered.findChild<QListWidget*>(QStringLiteral("orderedObjectSelection"));
    auto* picker = ordered.findChild<ObjectPickerPopup*>();
    QVERIFY(picker);
    QTest::mouseClick(Button(&ordered, QStringLiteral("添加")), Qt::LeftButton);
    QVERIFY(picker->isVisible());
    QVERIFY(ChooseId(picker, QStringLiteral("AHhb")));
    QVERIFY(!picker->isVisible());
    QCOMPARE(ordered.Ids(), (QStringList{QStringLiteral("Adef"), QStringLiteral("AHhb")}));
    QCOMPARE(changes.count(), 1);
    list->setCurrentRow(0);
    QTest::mouseClick(Button(&ordered, QStringLiteral("替换")), Qt::LeftButton);
    QVERIFY(picker->isVisible());
    QTest::keyClick(Grid(picker), Qt::Key_Escape);
    QVERIFY(!picker->isVisible());
    QCOMPARE(changes.count(), 1);
    QCOMPARE(ordered.Ids().front(), QStringLiteral("Adef"));
    QTest::mouseClick(Button(&ordered, QStringLiteral("替换")), Qt::LeftButton);
    QVERIFY(ChooseId(picker, QStringLiteral("AHhb")));
    QCOMPARE(changes.count(), 1); // 新选择重复对象不会偷偷删除旧项或改变顺序。
    list->setCurrentRow(0);
    QTest::mouseClick(Button(&ordered, QStringLiteral("替换")), Qt::LeftButton);
    QVERIFY(ChooseId(picker, QStringLiteral("Asta")));
    QCOMPARE(ordered.Ids(), (QStringList{QStringLiteral("Asta"), QStringLiteral("AHhb")}));
    QCOMPARE(changes.count(), 2);
    list->setCurrentRow(0);
    QTest::mouseClick(Button(&ordered, QStringLiteral("替换")), Qt::LeftButton);
    QVERIFY(picker->isVisible());
    ordered.SetIds({QStringLiteral("UNKNOWN")});
    QVERIFY(!picker->isVisible());
    QCOMPARE(changes.count(), 2);
    // 模拟选择结果排队期间发生外部撤销；旧位置不能覆盖新列表。
    emit picker->EntryChosen(Entries()[2]);
    QCOMPARE(ordered.Ids(), (QStringList{QStringLiteral("UNKNOWN")}));
    QCOMPARE(changes.count(), 2);
    QTest::mouseClick(Button(&ordered, QStringLiteral("添加")), Qt::LeftButton);
    QVERIFY(picker->isVisible());
    ordered.SetIds(ordered.Ids());
    QVERIFY(!picker->isVisible()); // 同值也可能来自另一个编辑对象。
}

void ObjectCatalogWidgetTests::NonModalPopupStaysOnScreenAndOutsideClickPassesThrough() {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* anchor = new QPushButton(QStringLiteral("参数"), &host);
    auto* outside = new QPushButton(QStringLiteral("底层操作"), &host);
    layout->addWidget(anchor);
    layout->addStretch();
    layout->addWidget(outside);
    const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    host.resize(std::min(360, screen.width() / 2), std::min(320, screen.height() / 2));
    host.move(screen.bottomRight() - QPoint(host.width() + 15, host.height() + 15));
    host.show();
    QApplication::processEvents();
    ObjectPickerPopup picker(nullptr, &host);
    picker.SetEntries(Entries());
    picker.SetKindFilter(QStringLiteral("ability"));
    picker.OpenAt(anchor);
    QApplication::processEvents();
    QVERIFY(picker.isVisible());
    QCOMPARE(picker.windowModality(), Qt::NonModal);
    QVERIFY(QApplication::activeModalWidget() == nullptr);
    QVERIFY(QApplication::activePopupWidget() != &picker);
    QVERIFY(screen.contains(picker.frameGeometry()));
    QCOMPARE(Grid(&picker)->model()->rowCount(), 4);
    QSignalSpy clicks(outside, &QPushButton::clicked);
    QSignalSpy chosen(&picker, &ObjectPickerPopup::EntryChosen);
    const QPoint outsidePoint(outside->width() - 4, outside->height() / 2);
    QVERIFY(!picker.frameGeometry().contains(outside->mapToGlobal(outsidePoint)));
    QTest::mouseClick(outside, Qt::LeftButton, Qt::NoModifier, outsidePoint);
    QVERIFY(!picker.isVisible());
    QCOMPARE(clicks.count(), 1);
    QCOMPARE(chosen.count(), 0);
    // 窗口改变位置后再次锚定，仍使用当前屏幕的可用几何范围。
    host.move(screen.topLeft() + QPoint(12, 12));
    picker.OpenAt(anchor);
    QApplication::processEvents();
    QVERIFY(screen.contains(picker.frameGeometry()));
    QTest::keyClick(Grid(&picker), Qt::Key_Escape);
    QVERIFY(!picker.isVisible());
    QCOMPARE(chosen.count(), 0);
}

void ObjectCatalogWidgetTests::ClassificationFollowsWorldEditor() {
    using War3Map::ObjectData::ObjectType;
    using War3Map::Modules::ObjectDetail::Classify;
    // 标准对象按 SLK 列，自定义对象的原生字段修改优先。
    const auto lookup = [](std::map<std::string, std::string> values) {
        return [values](std::string_view slk, std::string_view native) {
            if (const auto found = values.find(std::string(native)); found != values.end()) return found->second;
            const auto found = values.find(std::string(slk));
            return found == values.end() ? std::string{} : found->second;
        };
    };
    QCOMPARE(Classify(ObjectType::Unit, "hbar", lookup({{"race", "human"}, {"isbldg", "1"}})),
        (QStringList{"人族", "对战", "建筑"}));
    QCOMPARE(Classify(ObjectType::Unit, "Hpal", lookup({{"race", "human"}})), (QStringList{"人族", "对战", "英雄"}));
    QCOMPARE(Classify(ObjectType::Unit, "nfor", lookup({{"race", "creeps"}, {"hostilepal", "1"}, {"campaign", "1"}})),
        (QStringList{"中立敌对", "战役", "单位"}));
    QCOMPARE(Classify(ObjectType::Unit, "h000", lookup({{"race", "human"}, {"urac", "orc"}, {"uspe", "1"}})),
        (QStringList{"兽族", "对战", "特殊"}));
    QCOMPARE(Classify(ObjectType::Item, "I000", lookup({{"icla", "Charged"}})), QStringList{"可充的"});
    QCOMPARE(Classify(ObjectType::Ability, "AIat", lookup({{"item", "1"}})), QStringList{"物品"});
    QCOMPARE(Classify(ObjectType::Doodad, "LTlt", lookup({{"category", "E"}})), QStringList{"环境"});
    QCOMPARE(War3Map::Modules::ObjectDetail::RootLabel(ObjectType::Unit, true), QStringLiteral("自定义单位"));
}

void ObjectCatalogWidgetTests::ObjectTreeGroupsCountsAndSelects() {
    War3Map::QtApp::ObjectTreeBrowser tree(nullptr);
    tree.resize(360, 600);
    tree.show();
    using Object = War3Map::QtApp::ObjectTreeBrowser::Object;
    std::vector<Object> objects{
        {"hfoo", "步兵", {}, {"人族", "对战", "单位"}, false, true, {}},
        {"hbar", "兵营", {}, {"人族", "对战", "建筑"}, false, false, {}},
        {"ospe", "兽族单位", {}, {"兽族", "对战", "单位"}, false, false, {}},
        {"h000", "巨盾步兵", {}, {"人族", "对战", "单位"}, true, false, {}},
    };
    tree.SetObjects("unit", "标准单位", "自定义单位", objects);
    auto* view = tree.findChild<QTreeView*>("objectTree");
    QVERIFY(view != nullptr);
    auto* model = view->model();
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(model->index(0, 0).data().toString(), QStringLiteral("标准单位 (3)"));
    QCOMPARE(model->index(1, 0).data().toString(), QStringLiteral("自定义单位 (1)"));
    // 种族按原版顺序：人族在兽族前；文件夹带数量。
    const QModelIndex human = model->index(0, 0, model->index(0, 0));
    QCOMPARE(human.data().toString(), QStringLiteral("人族 (2)"));
    QCOMPARE(model->index(1, 0, model->index(0, 0)).data().toString(), QStringLiteral("兽族 (1)"));
    // 单位在建筑前。
    const QModelIndex melee = model->index(0, 0, human);
    QCOMPARE(model->index(0, 0, melee).data().toString(), QStringLiteral("单位 (1)"));
    QCOMPARE(model->index(1, 0, melee).data().toString(), QStringLiteral("建筑 (1)"));

    // 程序选中不发信号，并展开所在文件夹；用户选中发信号。
    QSignalSpy selected(&tree, &War3Map::QtApp::ObjectTreeBrowser::ObjectSelected);
    tree.Select("h000", true);
    QCOMPARE(selected.count(), 0);
    QCOMPARE(tree.Current()->first, QStringLiteral("h000"));
    QVERIFY(view->isExpanded(view->currentIndex().parent()));
    const QModelIndex footman = model->index(0, 0, model->index(0, 0, melee));
    view->setCurrentIndex(footman);
    QCOMPARE(selected.count(), 1);
    QCOMPARE(selected.last().at(0).toString(), QStringLiteral("hfoo"));
    // 修改过的标准对象与自定义对象为品红色。
    QCOMPARE(footman.data(Qt::ForegroundRole).value<QBrush>().color(), QColor(0xd8, 0x6c, 0xd8));

    // 名称 / ID 切换保留对象身份和选中项；搜索仍同时匹配两者。
    tree.SetShowIds(true);
    QVERIFY(tree.ShowIds());
    QCOMPARE(tree.Current()->first, QStringLiteral("hfoo"));
    QCOMPARE(view->currentIndex().data().toString(), QStringLiteral("hfoo"));
    tree.SetShowIds(false);
    QCOMPARE(view->currentIndex().data().toString(), QStringLiteral("步兵"));

    // 刷新同一类别：保持选中与展开状态；搜索按名称或 ID 过滤。
    tree.SetObjects("unit", "标准单位", "自定义单位", objects);
    QCOMPARE(tree.Current()->first, QStringLiteral("hfoo"));
    tree.SetSearchText(QStringLiteral("hbar"));
    QCOMPARE(model->rowCount(), 1);
    QCOMPARE(model->index(0, 0).data().toString(), QStringLiteral("标准单位 (3)"));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    ObjectCatalogWidgetTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "ObjectCatalogWidgetTests.moc"
