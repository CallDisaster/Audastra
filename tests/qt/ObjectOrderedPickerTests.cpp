#include "Widgets/ObjectCatalogWidgets.h"

#include <QApplication>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

using War3Map::QtApp::ObjectCatalogWidget;
using War3Map::QtApp::ObjectPickerPopup;
using War3Map::QtApp::ObjectViewEntry;
using War3Map::QtApp::OrderedObjectListWidget;

namespace {

QVector<ObjectViewEntry> Entries() {
    return {
        {QStringLiteral("ability"), QStringLiteral("A001"), QStringLiteral("技能甲"), {},
            QStringLiteral("单位技能"), QStringLiteral("人族"), QStringLiteral("合成说明甲"), {}, true},
        {QStringLiteral("ability"), QStringLiteral("A002"), QStringLiteral("技能乙"), {},
            QStringLiteral("单位技能"), QStringLiteral("人族"), QStringLiteral("合成说明乙"), {}, true},
        {QStringLiteral("ability"), QStringLiteral("A003"), QStringLiteral("技能丙"), {},
            QStringLiteral("英雄技能"), QStringLiteral("人族"), QStringLiteral("合成说明丙"), {}, true},
        {QStringLiteral("ability"), QStringLiteral("A004"), QStringLiteral("禁用技能"), {},
            QStringLiteral("单位技能"), QStringLiteral("人族"), {}, {}, false},
        {QStringLiteral("unit"), QStringLiteral("hfoo"), QStringLiteral("步兵"), {},
            QStringLiteral("单位"), QStringLiteral("人族"), {}, {}, true}
    };
}

QPushButton* Button(QWidget& parent, const QString& label) {
    for (auto* button : parent.findChildren<QPushButton*>()) {
        if (button->text() == label) return button;
    }
    return nullptr;
}

bool Choose(ObjectCatalogWidget* catalog, const QString& id) {
    catalog->SetSearchText(id);
    QApplication::processEvents();
    auto* view = catalog->findChild<QListView*>(QStringLiteral("objectCatalogGrid"));
    if (!view || view->model()->rowCount() != 1) return false;
    const auto index = view->model()->index(0, 0);
    if (index.data(Qt::UserRole).toString() != id || !(index.flags() & Qt::ItemIsEnabled)) return false;
    view->scrollTo(index);
    const QRect rect = view->visualRect(index);
    if (rect.isEmpty()) return false;
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
    QApplication::processEvents();
    return true;
}

} // namespace

class ObjectOrderedPickerTests final : public QObject {
    Q_OBJECT

private slots:
    void InlineCatalogEditsOneOrderedValueAtATime();
    void UnknownAndWrongTypeStayVisibleAndReadOnlyStopsEdits();
};

void ObjectOrderedPickerTests::InlineCatalogEditsOneOrderedValueAtATime() {
    OrderedObjectListWidget widget(nullptr);
    const auto all = Entries();
    widget.SetEntries(all.mid(0, 4));
    widget.SetKnownEntries(all);
    widget.SetKindFilter(QStringLiteral("ability"));
    widget.SetIds({QStringLiteral("A001"), QStringLiteral("LOST"), QStringLiteral("hfoo")});
    widget.SetInlineCatalogEnabled(true);
    widget.resize(860, 500);
    widget.show();
    QApplication::processEvents();

    auto* catalog = widget.findChild<ObjectCatalogWidget*>(QStringLiteral("orderedObjectCatalog"));
    auto* popup = widget.findChild<ObjectPickerPopup*>();
    auto* selected = widget.findChild<QListWidget*>(QStringLiteral("orderedObjectSelection"));
    QVERIFY(catalog);
    QVERIFY(popup);
    QVERIFY(selected);
    QVERIFY(catalog->isVisible());
    QVERIFY(!popup->isVisible());
    QSignalSpy changes(&widget, &OrderedObjectListWidget::IdsChanged);

    QVERIFY(Choose(catalog, QStringLiteral("A002")));
    QCOMPARE(widget.Ids(), (QStringList{QStringLiteral("A001"), QStringLiteral("LOST"),
        QStringLiteral("hfoo"), QStringLiteral("A002")}));
    QCOMPARE(changes.count(), 1);
    QVERIFY(!popup->isVisible());

    selected->setCurrentRow(2);
    QTest::mouseClick(Button(widget, QStringLiteral("替换")), Qt::LeftButton);
    QVERIFY(Choose(catalog, QStringLiteral("A003")));
    QCOMPARE(widget.Ids(), (QStringList{QStringLiteral("A001"), QStringLiteral("LOST"),
        QStringLiteral("A003"), QStringLiteral("A002")}));
    QCOMPARE(changes.count(), 2);

    QTest::mouseClick(Button(widget, QStringLiteral("上移")), Qt::LeftButton);
    QCOMPARE(widget.Ids(), (QStringList{QStringLiteral("A001"), QStringLiteral("A003"),
        QStringLiteral("LOST"), QStringLiteral("A002")}));
    QCOMPARE(changes.count(), 3);
    selected->setCurrentRow(2);
    QTest::mouseClick(Button(widget, QStringLiteral("删除")), Qt::LeftButton);
    QCOMPARE(widget.Ids(), (QStringList{QStringLiteral("A001"), QStringLiteral("A003"),
        QStringLiteral("A002")}));
    QCOMPARE(changes.count(), 4);
}

void ObjectOrderedPickerTests::UnknownAndWrongTypeStayVisibleAndReadOnlyStopsEdits() {
    OrderedObjectListWidget widget(nullptr);
    const auto all = Entries();
    widget.SetEntries(all.mid(0, 4));
    widget.SetKnownEntries(all);
    widget.SetKindFilter(QStringLiteral("ability"));
    const QStringList original{QStringLiteral("LOST"), QStringLiteral("hfoo"), QStringLiteral("A001")};
    widget.SetIds(original);
    widget.SetInlineCatalogEnabled(true);
    widget.resize(860, 500);
    widget.show();
    QApplication::processEvents();
    auto* selected = widget.findChild<QListWidget*>(QStringLiteral("orderedObjectSelection"));
    auto* catalog = widget.findChild<ObjectCatalogWidget*>(QStringLiteral("orderedObjectCatalog"));
    QVERIFY(selected);
    QVERIFY(catalog);
    QVERIFY(selected->item(0)->text().contains(QStringLiteral("未解析引用 · 已保留")));
    QVERIFY(selected->item(1)->text().contains(QStringLiteral("类型不匹配 · 已保留")));
    QVERIFY(selected->item(1)->toolTip().contains(QStringLiteral("单位")));
    QSignalSpy changes(&widget, &OrderedObjectListWidget::IdsChanged);

    widget.SetReadOnly(true);
    QVERIFY(!catalog->isEnabled());
    QVERIFY(!Button(widget, QStringLiteral("添加"))->isEnabled());
    QVERIFY(!Button(widget, QStringLiteral("删除"))->isEnabled());
    QCOMPARE(widget.Ids(), original);
    QCOMPARE(changes.count(), 0);

    widget.SetReadOnly(false);
    QVERIFY(catalog->isEnabled());
    catalog->SetSearchText(QStringLiteral("hfoo"));
    auto* grid = catalog->findChild<QListView*>(QStringLiteral("objectCatalogGrid"));
    QVERIFY(grid);
    QCOMPARE(grid->model()->rowCount(), 0);
    widget.SetIds(original);
    QCOMPARE(widget.Ids(), original);
    QCOMPARE(changes.count(), 0);
}

QTEST_MAIN(ObjectOrderedPickerTests)
#include "ObjectOrderedPickerTests.moc"
