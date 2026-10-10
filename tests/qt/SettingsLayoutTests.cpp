#include "AgentSettingsDialog.h"
#include "MapTestSettingsPage.h"
#include "RecoverySettingsPage.h"
#include "WorldEffectsSettingsPage.h"
#include "UpdateClient.h"
#include "UpdateDialog.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QRect InDialog(const QWidget* widget, const QWidget& dialog) {
    return {widget->mapTo(&dialog, QPoint{}), widget->size()};
}

QJsonObject RectJson(const QRect& rectangle) {
    return {{QStringLiteral("x"), rectangle.x()}, {QStringLiteral("y"), rectangle.y()},
        {QStringLiteral("width"), rectangle.width()}, {QStringLiteral("height"), rectangle.height()}};
}

int PageIndex(const QStackedWidget& pages, const QString& objectName) {
    for (int index = 0; index < pages.count(); ++index) {
        const auto* page = pages.widget(index);
        if (page->objectName() == objectName || page->findChild<QWidget*>(objectName)) return index;
    }
    return -1;
}

QString LongStatusText() {
    return QStringLiteral("更新状态：未找到可用版本信息，请检查当前构建的发布配置。"
        "请求地址 https://example.invalid/releases/check?channel=stable&version=0.1.0。"
        "此文字仅用于设置布局回归，需在窗口变窄时完整换行并保留右侧留白。 ").repeated(5);
}

void Capture(QWidget& dialog, const QString& pageName, const QString& fileName,
    const QWidget& content, const QStackedWidget& pages, const QScrollArea& scroll, const QWidget& page) {
    const auto output = qEnvironmentVariable("AUD_SETTINGS_CAPTURE_DIR");
    if (output.isEmpty()) return;
    QVERIFY2(QDir::isAbsolutePath(output), "AUD_SETTINGS_CAPTURE_DIR must be an absolute directory");
    QVERIFY2(QDir().mkpath(output), "Cannot create the settings capture directory");
    const QDir directory(output);
    const auto pixmap = dialog.grab();
    QVERIFY2(!pixmap.isNull(), "Cannot grab the full settings dialog");
    QVERIFY2(pixmap.save(directory.filePath(fileName + QStringLiteral(".png"))), "Cannot save the settings PNG");
    QJsonObject controls;
    const QStringList names{QStringLiteral("updateChannel"), QStringLiteral("updateSettingsStatus"),
        QStringLiteral("mapTestRuntimeRoot"), QStringLiteral("mapTestSlkEnabled"),
        QStringLiteral("mapBackupDirectory"), QStringLiteral("mapBackupLimit"),
        QStringLiteral("recoveryEnabled"), QStringLiteral("recoveryIntervalMinutes"), QStringLiteral("worldEffectQuality")};
    for (const auto& name : names) {
        if (const auto* control = page.findChild<QWidget*>(name)) controls.insert(name, RectJson(InDialog(control, dialog)));
    }
    const auto contentRect = InDialog(&content, dialog);
    const auto stackRect = InDialog(&pages, dialog);
    const QJsonObject metadata{
        {QStringLiteral("schema"), QStringLiteral("audastra-settings-layout-capture-v1")},
        {QStringLiteral("fixtureOnly"), true},
        {QStringLiteral("captureMethod"), QStringLiteral("QWidget::grab")},
        {QStringLiteral("theme"), QStringLiteral("graphite")},
        {QStringLiteral("page"), pageName},
        {QStringLiteral("image"), fileName + QStringLiteral(".png")},
        {QStringLiteral("qpaPlatform"), QGuiApplication::platformName()},
        {QStringLiteral("dialog"), RectJson(dialog.rect())},
        {QStringLiteral("content"), RectJson(contentRect)},
        {QStringLiteral("pages"), RectJson(stackRect)},
        {QStringLiteral("viewport"), RectJson(InDialog(scroll.viewport(), dialog))},
        {QStringLiteral("pageGeometry"), RectJson(InDialog(&page, dialog))},
        {QStringLiteral("gutters"), QJsonObject{{QStringLiteral("left"), stackRect.left() - contentRect.left()},
            {QStringLiteral("top"), stackRect.top() - contentRect.top()},
            {QStringLiteral("right"), contentRect.right() - stackRect.right()},
            {QStringLiteral("bottom"), contentRect.bottom() - stackRect.bottom()}}},
        {QStringLiteral("controls"), controls},
        {QStringLiteral("devicePixelRatio"), dialog.devicePixelRatioF()},
        {QStringLiteral("pixmapDevicePixelRatio"), pixmap.devicePixelRatio()},
        {QStringLiteral("pixmapPixelWidth"), pixmap.width()},
        {QStringLiteral("pixmapPixelHeight"), pixmap.height()},
        {QStringLiteral("resolvedFontFamily"), QFontInfo(dialog.font()).family()}
    };
    QFile metadataFile(directory.filePath(fileName + QStringLiteral(".json")));
    QVERIFY2(metadataFile.open(QIODevice::WriteOnly), "Cannot open the settings geometry JSON");
    const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
    QCOMPARE(metadataFile.write(bytes), qint64(bytes.size()));
    QVERIFY(metadataFile.flush());
}

} // namespace

class SettingsLayoutTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void SharedGuttersAndWrappedUpdateContent_data();
    void SharedGuttersAndWrappedUpdateContent();

private:
    QTemporaryDir settingsRoot_;
};

void SettingsLayoutTests::initTestCase() {
    QVERIFY(settingsRoot_.isValid());
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
    QCoreApplication::setOrganizationName(QStringLiteral("Audastra-SettingsLayout-Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("SettingsLayout"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot_.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsRoot_.path());
    War3Map::UiKit::ThemeManager theme;
    theme.ApplyTheme(QStringLiteral("graphite"));
    const auto fonts = War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory());
    War3Map::UiKit::ApplyApplicationFont(fonts);
}

void SettingsLayoutTests::init() {
    QSettings stored;
    stored.clear(); stored.sync();
    QCOMPARE(stored.status(), QSettings::NoError);
}

void SettingsLayoutTests::SharedGuttersAndWrappedUpdateContent_data() {
    QTest::addColumn<QSize>("windowSize");
    QTest::newRow("minimum") << QSize(760, 620);
    QTest::newRow("normal") << QSize(980, 760);
    QTest::newRow("enlarged") << QSize(1360, 980);
}

void SettingsLayoutTests::SharedGuttersAndWrappedUpdateContent() {
    QFETCH(QSize, windowSize);
    // 仅构造客户端；不安排自动检查，不点击检查按钮，不建立网络连接。
    War3Map::QtUpdate::UpdateClient client;
    QSignalSpy statusChanges(&client, &War3Map::QtUpdate::UpdateClient::StatusChanged);
    War3Map::Agent::AgentSettingsDialog dialog(War3Map::Agent::AgentSettings{});
    auto* update = new War3Map::QtUpdate::UpdateSettingsWidget(client);
    auto* mapTest = new War3Map::QtApp::MapTestSettingsPage(settingsRoot_.filePath(QStringLiteral("installation")));
    dialog.AddApplicationPage(QStringLiteral("检查更新"), update);
    dialog.AddApplicationPage(QStringLiteral("地图测试与备份"), mapTest);
    dialog.AddApplicationPage(QStringLiteral("编辑恢复"), new War3Map::QtApp::RecoverySettingsPage);
    dialog.AddApplicationPage(QStringLiteral("世界特效"), new War3Map::QtApp::WorldEffectsSettingsPage);
    auto* navigation = dialog.findChild<QListWidget*>(QStringLiteral("agentSettingsNavigation"));
    auto* content = dialog.findChild<QWidget*>(QStringLiteral("applicationSettingsContent"));
    auto* pages = dialog.findChild<QStackedWidget*>(QStringLiteral("applicationSettingsPages"));
    auto* channel = update->findChild<QComboBox*>(QStringLiteral("updateChannel"));
    auto* status = update->findChild<QLabel*>(QStringLiteral("updateSettingsStatus"));
    QVERIFY(navigation); QVERIFY(content); QVERIFY(pages); QVERIFY(channel); QVERIFY(status);
    status->setText(LongStatusText());
    dialog.resize(windowSize);
    dialog.show();
    const QStringList pageNames{QStringLiteral("updateSettingsPage"), QStringLiteral("mapTestSettingsPage"),
        QStringLiteral("recoverySettingsPage"), QStringLiteral("worldEffectsSettingsPage")};
    for (const auto& name : pageNames) {
        const auto index = PageIndex(*pages, name);
        QVERIFY2(index >= 0, qPrintable(name));
        navigation->setCurrentRow(index);
        QTest::qWait(20);
        QCOMPARE(dialog.size(), windowSize);
        auto* scroll = qobject_cast<QScrollArea*>(pages->currentWidget());
        QVERIFY(scroll);
        auto* page = scroll->widget(); QVERIFY(page); QCOMPARE(page->objectName(), name);
        const auto contentRect = InDialog(content, dialog);
        const auto stackRect = InDialog(pages, dialog);
        QCOMPARE(stackRect.left() - contentRect.left(), 24);
        QCOMPARE(stackRect.top() - contentRect.top(), 20);
        QCOMPARE(contentRect.right() - stackRect.right(), 24);
        QCOMPARE(contentRect.bottom() - stackRect.bottom(), 20);
        QCOMPARE(InDialog(page, dialog).left(), stackRect.left());
        QCOMPARE(InDialog(page, dialog).top(), stackRect.top());
        QCOMPARE(page->width(), scroll->viewport()->width());
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        if (name == QStringLiteral("updateSettingsPage")) {
            QVERIFY(channel->width() > 0); QVERIFY(channel->width() <= 280);
            QVERIFY(update->rect().contains(QRect(channel->mapTo(update, QPoint{}), channel->size())));
            QVERIFY(update->rect().contains(QRect(status->mapTo(update, QPoint{}), status->size())));
            QVERIFY(status->wordWrap()); QVERIFY(status->hasHeightForWidth());
            QVERIFY(status->heightForWidth(status->width()) > status->fontMetrics().height());
            QVERIFY(status->height() >= status->heightForWidth(status->width()));
            auto* title = update->findChild<QLabel*>(QStringLiteral("sectionTitle")); QVERIFY(title);
            QCOMPARE(InDialog(title, dialog).left(), stackRect.left());
            QCOMPARE(InDialog(title, dialog).top(), stackRect.top());
        } else {
            const QStringList fields = name == QStringLiteral("mapTestSettingsPage")
                ? QStringList{QStringLiteral("mapTestRuntimeRoot"),
                    QStringLiteral("mapBackupDirectory"), QStringLiteral("mapBackupLimit")}
                : name == QStringLiteral("worldEffectsSettingsPage") ? QStringList{QStringLiteral("worldEffectQuality")}
                : QStringList{QStringLiteral("recoveryEnabled"), QStringLiteral("recoveryIntervalMinutes")};
            for (const auto& field : fields) {
                auto* control = page->findChild<QWidget*>(field); QVERIFY(control);
                const auto rectangle = QRect(control->mapTo(page, QPoint{}), control->size());
                QVERIFY2(page->rect().contains(rectangle), qPrintable(field));
            }
        }
        if (windowSize == QSize(980, 760)) Capture(dialog, name,
            name == QStringLiteral("updateSettingsPage") ? QStringLiteral("settings-update")
                : name == QStringLiteral("mapTestSettingsPage") ? QStringLiteral("settings-map-test")
                : name == QStringLiteral("worldEffectsSettingsPage") ? QStringLiteral("settings-world-effects") : QStringLiteral("settings-recovery"),
            *content, *pages, *scroll, *page);
    }
    QVERIFY(!client.IsBusy());
    QCOMPARE(statusChanges.size(), 0);
    QVERIFY(QSettings().allKeys().isEmpty());
}

int main(int argc, char** argv) {
    // 布局回归只在离屏平台执行，不能打开或接管作者桌面窗口。
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    SettingsLayoutTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "SettingsLayoutTests.moc"
