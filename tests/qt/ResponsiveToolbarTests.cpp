#include "Dock/ModuleSwitcher.h"
#include "Shell/HeaderBar.h"
#include "Icons/SvgIcons.h"
#include "Theme/ThemeManager.h"
#include "Widgets/ResponsiveToolBar.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QtTest>

using namespace War3Map;

namespace {

void CheckChildren(const QWidget& parent) {
    QList<QRect> rectangles;
    for (const auto* child : parent.findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)) {
        if (child->isHidden() || child->isWindow()) continue;
        const QRect rectangle(child->mapTo(&parent, QPoint{}), child->size());
        QVERIFY2(parent.rect().contains(rectangle), qPrintable(child->objectName() + QStringLiteral(" outside parent")));
        for (const auto& previous : rectangles) QVERIFY2(!previous.intersects(rectangle), qPrintable(child->objectName() + QStringLiteral(" overlaps")));
        rectangles.push_back(rectangle);
    }
}

void Capture(const QWidget& widget, const QString& name) {
    const auto output = qEnvironmentVariable("AUD_TOOLBAR_CAPTURE_DIR");
    if (output.isEmpty()) return;
    QVERIFY(QDir::isAbsolutePath(output)); QVERIFY(QDir().mkpath(output));
    const auto file = QDir(output).filePath(name + QStringLiteral("-dpr%1.png").arg(widget.devicePixelRatioF()));
    QVERIFY(const_cast<QWidget&>(widget).grab().save(file));
}

} // namespace

class ResponsiveToolbarTests final : public QObject {
    Q_OBJECT
private slots:
    void HeaderWidths_data();
    void HeaderWidths();
    void OverflowRetainsCommandsAndState();
    void SearchExpansionPreservesInput();
    void SplitButtonStates_data();
    void SplitButtonStates();
};

void ResponsiveToolbarTests::HeaderWidths_data() {
    QTest::addColumn<QString>("theme"); QTest::addColumn<int>("width");
    for (const auto& theme : {QStringLiteral("graphite"), QStringLiteral("paper")})
        for (const int width : {1920, 1600, 1366, 1280, 1024})
            QTest::newRow(qPrintable(theme + QString::number(width))) << theme << width;
}

void ResponsiveToolbarTests::HeaderWidths() {
    QFETCH(QString, theme); QFETCH(int, width);
    UiKit::ThemeManager manager; manager.ApplyTheme(theme);
    Shell::HeaderBar header;
    std::vector<Shell::ModuleInfo> modules;
    for (const auto& label : {"触发器", "物体", "变量", "声音", "导入", "地图", "战役", "世界"})
        modules.push_back({label, QString::fromUtf8(label), "lucide/box", {}});
    header.Switcher()->SetModules(modules);
    auto* search = new QLineEdit; search->setObjectName("testSearch");
    search->setPlaceholderText(QStringLiteral("搜索工程…")); search->setMaximumWidth(230);
    header.AddWidget(search); header.AddSeparator();
    QList<QToolButton*> buttons;
    for (const auto& label : {"文件", "保存地图", "测试", "后退", "前进", "撤销", "重做", "地图", "布局", "设置", "帮助", "修改", "助手"}) {
        auto* action = new QAction(UiKit::SvgIcon(QStringLiteral("lucide/file")), QString::fromUtf8(label), &header);
        auto* button = header.AddAction(action); button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        if (action->text() == QStringLiteral("保存地图") || action->text() == QStringLiteral("测试")) button->setProperty("headerKeepIcon", true);
        buttons.push_back(button);
    }
    header.resize(width, header.height()); header.show(); QTest::qWait(20);
    auto* toolbar = header.findChild<UiKit::ResponsiveToolBar*>("headerActions"); QVERIFY(toolbar);
    toolbar->Refresh(); QTest::qWait(10);
    QCOMPARE(header.width(), width); CheckChildren(header); CheckChildren(*toolbar); CheckChildren(*header.Switcher());
    for (auto* button : buttons) {
        if (button->isHidden()) {
            QVERIFY(toolbar->OverflowMenu()->actions().contains(button->defaultAction()));
        } else {
            QVERIFY(button->width() >= button->sizeHint().width());
            if (button->toolButtonStyle() == Qt::ToolButtonIconOnly) QVERIFY(!button->icon().isNull());
            else QVERIFY(button->width() >= button->fontMetrics().horizontalAdvance(button->text()) + 12);
        }
        QVERIFY(!button->toolTip().isEmpty());
    }
    for (const auto& module : modules) {
        auto* chip = header.Switcher()->Chip(module.id); QVERIFY(chip);
        QVERIFY(chip->width() >= (chip->toolButtonStyle() == Qt::ToolButtonIconOnly ? chip->IconModeWidth() : chip->TextModeWidth()));
    }
    Capture(header, theme + QStringLiteral("-header-%1").arg(width));
}

void ResponsiveToolbarTests::OverflowRetainsCommandsAndState() {
    UiKit::ResponsiveToolBar toolbar;
    auto* original = new QAction(UiKit::SvgIcon("lucide/save"), QStringLiteral("保存地图"), &toolbar);
    original->setShortcut(QKeySequence(QStringLiteral("Ctrl+J")));
    QSignalSpy invoked(original, &QAction::triggered);
    auto* button = toolbar.AddAction(original);
    auto* legacy = new QPushButton(QStringLiteral("显示原始字段")); legacy->setCheckable(true);
    QSignalSpy clicked(legacy, &QPushButton::clicked); toolbar.AddWidget(legacy);
    for (int i = 0; i < 6; ++i) toolbar.AddAction(new QAction(UiKit::SvgIcon("lucide/file"), QStringLiteral("工具%1").arg(i), &toolbar), 100);
    toolbar.resize(90, 44); toolbar.show(); QTest::qWait(20); toolbar.Refresh();
    QVERIFY(button->isHidden()); QVERIFY(legacy->isHidden());
    QVERIFY(toolbar.OverflowMenu()->actions().contains(original));
    original->setEnabled(false); toolbar.Refresh(); QVERIFY(!toolbar.OverflowMenu()->actions().at(0)->isEnabled());
    original->setEnabled(true); toolbar.Refresh();
    original->trigger(); QCOMPARE(invoked.count(), 1);
    QAction* legacyAction = nullptr;
    for (auto* action : toolbar.OverflowMenu()->actions()) if (action->text() == QStringLiteral("显示原始字段")) legacyAction = action;
    QVERIFY(legacyAction); legacyAction->trigger(); QCOMPARE(clicked.count(), 1); QVERIFY(legacy->isChecked());
    toolbar.Refresh(); QVERIFY(legacyAction->isChecked());
    QApplication::setActiveWindow(&toolbar); toolbar.setFocus();
    QTest::keyClick(&toolbar, Qt::Key_J, Qt::ControlModifier); QTRY_COMPARE(invoked.count(), 2);
    toolbar.resize(1800, 44); QTest::qWait(20); toolbar.Refresh();
    QVERIFY(!button->isHidden()); QVERIFY(!legacy->isHidden());
    QCOMPARE(legacy->text(), QStringLiteral("显示原始字段"));
    CheckChildren(toolbar);
}

void ResponsiveToolbarTests::SearchExpansionPreservesInput() {
    UiKit::ResponsiveToolBar toolbar;
    auto* search = new QLineEdit; search->setMaximumWidth(230); search->setText(QStringLiteral("已有查询"));
    QSignalSpy changed(search, &QLineEdit::textChanged); toolbar.AddWidget(search);
    toolbar.resize(110, 44); toolbar.show(); QTest::qWait(20); toolbar.Refresh();
    auto* toggle = toolbar.findChild<QToolButton*>("responsiveSearchButton"); QVERIFY(toggle && !toggle->isHidden());
    QCOMPARE(search->text(), QStringLiteral("已有查询")); QCOMPARE(changed.count(), 0);
    QTest::mouseClick(toggle, Qt::LeftButton); toolbar.Refresh(); QApplication::processEvents();
    QVERIFY(!search->isHidden()); QCOMPARE(search->text(), QStringLiteral("已有查询"));
    CheckChildren(toolbar); CheckChildren(*search->parentWidget());
    search->setText(QStringLiteral("新查询")); QCOMPARE(changed.count(), 1);
    QTest::keyClick(search, Qt::Key_Escape); toolbar.resize(110, 44); QTest::qWait(20); toolbar.Refresh();
    QVERIFY(search->isHidden()); QCOMPARE(search->text(), QStringLiteral("新查询"));
    CheckChildren(toolbar);
}

void ResponsiveToolbarTests::SplitButtonStates_data() {
    QTest::addColumn<QString>("theme"); QTest::addColumn<int>("state");
    for (const auto& theme : {QStringLiteral("graphite"), QStringLiteral("paper")})
        for (int state = 0; state < 4; ++state) QTest::newRow(qPrintable(theme + QString::number(state))) << theme << state;
}

void ResponsiveToolbarTests::SplitButtonStates() {
    QFETCH(QString, theme); QFETCH(int, state);
    UiKit::ThemeManager manager; manager.ApplyTheme(theme);
    QToolButton button; button.setObjectName(QStringLiteral("splitFixture"));
    UiKit::SetToolButtonSvgIcon(button, "lucide/plus", QStringLiteral("新建"), Qt::ToolButtonTextBesideIcon);
    QMenu menu; menu.addAction(QStringLiteral("新建触发器"));
    button.setMenu(&menu); button.setPopupMode(QToolButton::MenuButtonPopup);
    button.resize(button.sizeHint()); button.show();
    if (state == 1) button.setAttribute(Qt::WA_UnderMouse);
    if (state == 2) button.setDown(true);
    if (state == 3) button.setEnabled(false);
    QTest::qWait(10);
    QStyleOptionToolButton option; option.initFrom(&button); option.rect = button.rect();
    option.features = QStyleOptionToolButton::MenuButtonPopup | QStyleOptionToolButton::HasMenu;
    const QRect menuRect = button.style()->subControlRect(QStyle::CC_ToolButton, &option, QStyle::SC_ToolButtonMenu, &button);
    QVERIFY(!menuRect.isEmpty());
    const auto image = button.grab().toImage();
    const auto pixel = image.pixelColor(QPoint(menuRect.center().x(), menuRect.top() + 3) * button.devicePixelRatioF());
    QVERIFY2(pixel.red() + pixel.green() + pixel.blue() > 24, "menu half must not fall back to a black native block");
    Capture(button, theme + QStringLiteral("-split-%1").arg(state));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv); application.setQuitOnLastWindowClosed(false);
    ResponsiveToolbarTests tests; return QTest::qExec(&tests, argc, argv);
}

#include "ResponsiveToolbarTests.moc"
