#include "Shell/StartupPage.h"
#include "Theme/ThemeManager.h"

#include <QAction>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QToolButton>
#include <QtTest>

using War3Map::Shell::StartupPage;

class StartupPageTests final : public QObject {
    Q_OBJECT
private slots:
    void sharedActionsAndKeyboard();
    void missingOrDestroyedActionIsDisabled();
    void removedActionIsDisabled();
    void compactLayout_data();
    void compactLayout();
};

void StartupPageTests::sharedActionsAndKeyboard() {
    QAction open(QStringLiteral("打开地图"));
    QAction create(QStringLiteral("新建地图"));
    create.setEnabled(false);
    StartupPage page(&open, &create);
    page.resize(640, 400);
    page.show();
    auto* openButton = page.findChild<QToolButton*>(QStringLiteral("startupOpenMap"));
    auto* createButton = page.findChild<QToolButton*>(QStringLiteral("startupCreateMap"));
    QVERIFY(openButton);
    QVERIFY(createButton);
    QCOMPARE(openButton->defaultAction(), &open);
    QCOMPARE(createButton->defaultAction(), &create);
    QVERIFY(!createButton->isEnabled());
    create.setEnabled(true);
    QVERIFY(createButton->isEnabled());
    QSignalSpy opened(&open, &QAction::triggered);
    QSignalSpy created(&create, &QAction::triggered);
    QTest::mouseClick(openButton, Qt::LeftButton);
    QCOMPARE(opened.count(), 1);
    createButton->setFocus();
    QTest::keyClick(createButton, Qt::Key_Space);
    QCOMPARE(created.count(), 1);
    open.setEnabled(false);
    QVERIFY(!openButton->isEnabled());
    QTest::mouseClick(openButton, Qt::LeftButton);
    QCOMPARE(opened.count(), 1);
    open.setText(QStringLiteral("打开其他地图"));
    QCOMPARE(openButton->text(), open.text());
    open.setEnabled(true);
    QVERIFY(openButton->isEnabled());
    openButton->setFocus();
    QTest::keyClick(openButton, Qt::Key_Tab);
    QCOMPARE(page.focusWidget(), createButton);
}

void StartupPageTests::missingOrDestroyedActionIsDisabled() {
    auto* action = new QAction(QStringLiteral("打开地图"));
    StartupPage page(action, nullptr);
    page.resize(640, 400);
    page.show();
    auto* open = page.findChild<QToolButton*>(QStringLiteral("startupOpenMap"));
    auto* create = page.findChild<QToolButton*>(QStringLiteral("startupCreateMap"));
    QVERIFY(open && create);
    QVERIFY(open->isEnabled());
    QVERIFY(!create->isEnabled());
    int triggered = 0;
    connect(action, &QAction::triggered, &page, [&] { ++triggered; });
    QTest::mouseClick(open, Qt::LeftButton);
    QCOMPARE(triggered, 1);
    QSignalSpy clicked(open, &QToolButton::clicked);
    QSignalSpy createClicked(create, &QToolButton::clicked);
    // 动作在按下和释放之间销毁：立即禁用，并取消尚未完成的点击。
    open->setFocus();
    QTest::mousePress(open, Qt::LeftButton);
    QVERIFY(open->isDown());
    delete action;
    QVERIFY(!open->isEnabled());
    QVERIFY(!open->defaultAction());
    QVERIFY(!open->isDown());
    QTest::mouseRelease(open, Qt::LeftButton);
    QTest::mouseClick(open, Qt::LeftButton);
    QTest::keyClick(open, Qt::Key_Space);
    QTest::keyClick(open, Qt::Key_Return);
    open->click();
    QTest::mouseClick(create, Qt::LeftButton);
    QTest::keyClick(create, Qt::Key_Space);
    create->click();
    QCOMPARE(clicked.count(), 0);
    QCOMPARE(createClicked.count(), 0);
    QCOMPARE(triggered, 1);
    page.setEnabled(false); page.setEnabled(true);
    QVERIFY(!open->isEnabled());
    QVERIFY(!create->isEnabled());
}

void StartupPageTests::removedActionIsDisabled() {
    QAction action(QStringLiteral("打开地图"));
    StartupPage page(&action, nullptr);
    auto* open = page.findChild<QToolButton*>(QStringLiteral("startupOpenMap"));
    QVERIFY(open && open->isEnabled());
    QSignalSpy triggered(&action, &QAction::triggered);
    open->removeAction(&action);
    QVERIFY(!open->defaultAction());
    QVERIFY(!open->isEnabled());
    action.setEnabled(false); action.setEnabled(true);
    QVERIFY(!open->isEnabled());
    open->click();
    QCOMPARE(triggered.count(), 0);
}

void StartupPageTests::compactLayout_data() {
    QTest::addColumn<QString>("theme");
    QTest::addColumn<QSize>("viewport");
    for (const auto& theme : War3Map::UiKit::ThemeManager::ThemeIds()) {
        for (const auto size : {QSize(320, 320), QSize(760, 400), QSize(1040, 560)}) {
            const auto name = QStringLiteral("%1-%2x%3").arg(theme).arg(size.width()).arg(size.height()).toUtf8();
            QTest::newRow(name.constData()) << theme << size;
        }
    }
}

void StartupPageTests::compactLayout() {
    QFETCH(QString, theme);
    QFETCH(QSize, viewport);
    War3Map::UiKit::ThemeManager themes;
    themes.ApplyTheme(theme);
    QAction open(QStringLiteral("打开地图"));
    QAction create(QStringLiteral("新建地图"));
    StartupPage page(&open, &create);
    page.resize(viewport);
    page.show();
    QCoreApplication::processEvents();
    QCOMPARE(page.size(), viewport);
    auto* scroll = page.findChild<QScrollArea*>();
    QVERIFY(scroll);
    QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
    QCOMPARE(scroll->verticalScrollBar()->maximum(), 0);
    const auto* logo = page.findChild<QLabel*>(QStringLiteral("startupLogo"));
    QVERIFY(logo);
    QVERIFY(!logo->pixmap().isNull());
    QCOMPARE(logo->size(), QSize(80, 80));
    QVERIFY(logo->hasScaledContents());
    QVERIFY(!logo->accessibleName().isEmpty());
    const QRect logoBounds(logo->mapTo(scroll->viewport(), QPoint()), logo->size());
    QVERIFY(scroll->viewport()->rect().contains(logoBounds));
    const auto* product = page.findChild<QLabel*>(QStringLiteral("startupProductName"));
    const auto* tagline = page.findChild<QLabel*>(QStringLiteral("startupTagline"));
    QCOMPARE(product->text(), QStringLiteral("Audastra"));
    QCOMPARE(tagline->text(), QStringLiteral("Per Audacia Ad Astra"));
    for (const auto* name : {"startupProductName", "startupTagline", "startupOpenMap", "startupCreateMap"}) {
        auto* widget = page.findChild<QWidget*>(QString::fromLatin1(name));
        QVERIFY(widget);
        const QRect bounds(widget->mapTo(scroll->viewport(), QPoint()), widget->size());
        QVERIFY2(scroll->viewport()->rect().contains(bounds), name);
        QVERIFY2(widget->height() >= widget->sizeHint().height(), name);
        QVERIFY2(widget->width() >= widget->minimumSizeHint().width(), name);
    }
}

QTEST_MAIN(StartupPageTests)
#include "StartupPageTests.moc"
