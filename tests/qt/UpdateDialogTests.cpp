#include "UpdateDialog.h"
#include "UpdateFixtures.h"
#include "Theme/ThemeManager.h"
#include "Fonts/FontLoader.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLibraryInfo>
#include <QLocale>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextTable>
#include <QImage>
#include <QTextCursor>
#include <QUrl>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>

using namespace War3Map::QtUpdate;
using namespace UpdateFixtures;

namespace {
void Capture(QWidget& widget, const QString& name) {
    const auto output = qEnvironmentVariable("FORGE_QT_UPDATE_CAPTURE_DIR");
    if (output.isEmpty()) return;
    QVERIFY2(QDir().mkpath(output), "Cannot create the original-fixture capture directory");
    const QDir directory(output);
    const auto path = directory.filePath(name + QStringLiteral(".png"));
    const auto pixmap = widget.grab();
    QVERIFY2(pixmap.save(path), "Cannot save the original-fixture PNG");
    const auto* screen = widget.screen();
    const QJsonObject metadata{
        {QStringLiteral("schema"), QStringLiteral("audastra-qt-widget-capture-v1")},
        {QStringLiteral("captureMethod"), QStringLiteral("QWidget::grab")},
        {QStringLiteral("image"), name + QStringLiteral(".png")},
        {QStringLiteral("qpaPlatform"), QGuiApplication::platformName()},
        {QStringLiteral("widgetLogicalWidth"), widget.width()},
        {QStringLiteral("widgetLogicalHeight"), widget.height()},
        {QStringLiteral("pixmapPixelWidth"), pixmap.width()},
        {QStringLiteral("pixmapPixelHeight"), pixmap.height()},
        {QStringLiteral("devicePixelRatio"), pixmap.devicePixelRatio()},
        {QStringLiteral("widgetDevicePixelRatio"), widget.devicePixelRatioF()},
        {QStringLiteral("screenDevicePixelRatio"), screen ? QJsonValue(screen->devicePixelRatio()) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("fontFamily"), widget.font().family()},
        {QStringLiteral("resolvedFontFamily"), QFontInfo(widget.font()).family()},
        {QStringLiteral("locale"), QLocale::system().name()},
        {QStringLiteral("widgetLocale"), widget.locale().name()},
        {QStringLiteral("qtTranslatorLoaded"), qApp->property("updateCaptureQtTranslatorLoaded").toBool()}
    };
    QFile metadataFile(directory.filePath(name + QStringLiteral(".metadata.json")));
    QVERIFY2(metadataFile.open(QIODevice::WriteOnly), "Cannot open the capture metadata");
    const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
    QCOMPARE(metadataFile.write(bytes), static_cast<qint64>(bytes.size()));
    QVERIFY2(metadataFile.flush(), "Cannot flush the capture metadata");
    qInfo().noquote() << "Update Qt original-fixture capture:" << path << "metadata:" << metadataFile.fileName();
}
bool HasAnchor(const QTextDocument& document) {
    for (auto block = document.begin(); block != document.end(); block = block.next())
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment)
            if (fragment.fragment().charFormat().isAnchor()) return true;
    return false;
}
bool HasImage(const QTextDocument& document) {
    for (auto block = document.begin(); block != document.end(); block = block.next())
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment)
            if (fragment.fragment().charFormat().isImageFormat()) return true;
    return false;
}
bool HasTable(const QTextDocument& document) {
    for (auto block = document.begin(); block != document.end(); block = block.next()) {
        QTextCursor cursor(block);
        if (cursor.currentTable()) return true;
    }
    return false;
}
} // namespace

class UpdateDialogTests final : public QObject {
    Q_OBJECT
private:
    QTranslator qtTranslator_;
private slots:
    void initTestCase();
    void init();
    void ConstructionUsesNativeThemeAndDoesNotStartNetwork();
    void AvailableVersionIsPlainTextAndSkipPersistsWithoutDownloading();
    void ReleaseNotesRenderMarkdownWithoutLoadingResources();
    void FailedCheckStaysAnErrorAtNarrowWidthAndCanRetry();
    void DownloadShowsRealProgressAndTheCancelButtonStopsIt();
    void SettingsPageStagesOptionsAndReusesItsManualDialog();
    void CancellingSettingsDiscardsItsOptionsAndSkippedVersion();
    void SettingsOnlyTakeEffectAfterTheHostsSuccessfulCommit();
    void PreparedPackageEmitsAnInstallationRequestWithoutLaunchingAProcess();
};

void UpdateDialogTests::initTestCase() {
    // 与主程序使用相同系统locale和查找顺序，保有translator至测试结束。
    bool loaded = false;
    for (const QString& directory : {
             QCoreApplication::applicationDirPath() + QStringLiteral("/translations"),
             QLibraryInfo::path(QLibraryInfo::TranslationsPath)}) {
        if (qtTranslator_.load(QLocale::system(), QStringLiteral("qt"), QStringLiteral("_"), directory)) {
            loaded = qApp->installTranslator(&qtTranslator_);
            break;
        }
    }
    qApp->setProperty("updateCaptureQtTranslatorLoaded", loaded);
}

void UpdateDialogTests::init() {
    War3Map::UiKit::ThemeManager theme; theme.ApplyTheme(QStringLiteral("graphite"));
    const auto fonts = War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory());
    War3Map::UiKit::ApplyApplicationFont(fonts);
}
void UpdateDialogTests::ConstructionUsesNativeThemeAndDoesNotStartNetwork() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); UpdateClient client(server.Source(bundle, directory.path()), settings);
    UpdateDialog dialog(client); dialog.show(); QTest::qWait(20);
    QCOMPARE(server.requests.size(), 0); QCOMPARE(dialog.windowTitle(), QStringLiteral("检查更新"));
    auto* current = dialog.findChild<QLabel*>("updateCurrentVersion"); auto* primary = dialog.findChild<QPushButton*>("primaryButton");
    auto* automatic = dialog.findChild<QCheckBox*>("updateAutomaticCheck"); auto* channel = dialog.findChild<QComboBox*>("updateChannel");
    QVERIFY(current && primary && automatic && channel); QCOMPARE(current->text(), QStringLiteral("0.1.0")); QVERIFY(!primary->isVisible());
    QVERIFY(automatic->isChecked()); QCOMPARE(channel->currentText(), QStringLiteral("稳定版"));
    QVERIFY(dialog.styleSheet().isEmpty()); // 继承应用工作台主题，模块没有另设颜色或字体。
    QCOMPARE(dialog.findChild<QTextEdit*>("updateReleaseNotes")->isReadOnly(), true);
    Capture(dialog, QStringLiteral("update-idle-graphite"));
}

void UpdateDialogTests::AvailableVersionIsPlainTextAndSkipPersistsWithoutDownloading() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    const auto notes = QStringLiteral("<a href=\"https://example.invalid\">不会自动打开的说明</a>\n第二行发布内容");
    server.routes["/repos/fixture/editor/releases/latest"] = server.Release(bundle, QStringLiteral("0.2.0"), false, notes);
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); UpdateClient client(server.Source(bundle, directory.path()), settings);
    UpdateDialog dialog(client); dialog.show(); dialog.CheckNow();
    QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    auto* primary = dialog.findChild<QPushButton*>("primaryButton"); auto* skip = dialog.findChild<QPushButton*>("updateSkipButton");
    QVERIFY(primary->isVisible() && primary->isEnabled()); QCOMPARE(primary->text(), QStringLiteral("更新")); QVERIFY(skip->isVisible());
    // 原始 HTML 只按文字显示，不生成可点击的链接。
    const auto* shown = dialog.findChild<QTextEdit*>("updateReleaseNotes");
    QVERIFY(shown->toPlainText().contains(QStringLiteral("<a href=\"https://example.invalid\">")));
    QVERIFY(shown->toPlainText().contains(QStringLiteral("第二行发布内容")));
    QVERIFY(!HasAnchor(*shown->document()));
    QCOMPARE(dialog.findChild<QLabel*>("updateStatus")->textFormat(), Qt::PlainText); QCOMPARE(server.requests.size(), 1);
    Capture(dialog, QStringLiteral("update-available-graphite"));
    QTest::mouseClick(skip, Qt::LeftButton); QCOMPARE(client.Status().state, UpdateState::Skipped);
    QCOMPARE(settings.value("update/skippedVersion").toString(), QStringLiteral("0.2.0")); QCOMPARE(server.requests.size(), 1); QVERIFY(!primary->isVisible());
}

void UpdateDialogTests::ReleaseNotesRenderMarkdownWithoutLoadingResources() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    // 本机图片文件存在也不能被说明里的图片语法读取。
    const auto image = directory.filePath(QStringLiteral("local.png"));
    QImage pixel(4, 4, QImage::Format_ARGB32); pixel.fill(Qt::red); QVERIFY(pixel.save(image));
    const auto notes = QStringLiteral("## 下载\n\n> **请在副本上试用。**\n\n| 文件 | 用途 |\n|---|---|\n| `a.zip` | **下载这个** |\n\n")
        + QStringLiteral("![图](") + QUrl::fromLocalFile(image).toString() + QStringLiteral(")\n\n[项目](https://example.invalid)\n");
    server.routes["/repos/fixture/editor/releases/latest"] = server.Release(bundle, QStringLiteral("0.2.0"), false, notes);
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); UpdateClient client(server.Source(bundle, directory.path()), settings);
    UpdateDialog dialog(client); dialog.resize(760, 640); dialog.show(); dialog.CheckNow();
    QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    auto* shown = dialog.findChild<QTextEdit*>("updateReleaseNotes"); QVERIFY(shown);
    const auto text = shown->toPlainText();
    QVERIFY2(!text.contains(QStringLiteral("##")) && !text.contains(QStringLiteral("**")) && !text.contains(QStringLiteral("|---")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("下载这个")) && text.contains(QStringLiteral("项目")));
    QVERIFY(HasTable(*shown->document()));
    // 表头格只有表头文字一段且加粗，不留导入时的空段落。
    QTextTable* table = nullptr;
    for (auto block = shown->document()->begin(); block.isValid() && !table; block = block.next()) table = QTextCursor(block).currentTable();
    const auto header = shown->document()->findBlock(table->cellAt(0, 0).firstPosition());
    QCOMPARE(header.text(), QStringLiteral("文件"));
    QCOMPARE(QTextCursor(header).charFormat().fontWeight(), int(QFont::Bold));
    QVERIFY(!HasImage(*shown->document()) && text.contains(QStringLiteral("[图片]")));
    QCOMPARE(shown->textInteractionFlags() & Qt::LinksAccessibleByMouse, Qt::TextInteractionFlags{});
    const auto resource = shown->document()->resource(QTextDocument::ImageResource, QUrl::fromLocalFile(image));
    QVERIFY(resource.value<QImage>().isNull());
    QCOMPARE(server.requests.size(), 1);
    Capture(dialog, QStringLiteral("update-markdown-notes-graphite"));
}

void UpdateDialogTests::FailedCheckStaysAnErrorAtNarrowWidthAndCanRetry() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    server.responder = [](QTcpSocket* socket, const QByteArray&) { socket->write(Http("private release", 404)); socket->disconnectFromHost(); };
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); UpdateClient client(server.Source(bundle, directory.path()), settings);
    UpdateDialog dialog(client); dialog.resize(420, 480); dialog.show(); dialog.CheckNow();
    QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Failed, 3000);
    auto* message = dialog.findChild<QLabel*>("updateStatus"); auto* retry = dialog.findChild<QPushButton*>("updateCheckButton");
    QVERIFY(message->text().contains(QStringLiteral("404"))); QVERIFY(!message->text().contains(QStringLiteral("已是"))); QVERIFY(message->wordWrap());
    QCOMPARE(retry->text(), QStringLiteral("重试检查")); QVERIFY(retry->isEnabled()); QVERIFY(!dialog.findChild<QPushButton*>("primaryButton")->isVisible());
    QVERIFY(dialog.rect().contains(message->geometry())); QVERIFY(dialog.rect().contains(retry->geometry()));
    Capture(dialog, QStringLiteral("update-error-narrow-graphite"));
    server.responder = {}; QTest::mouseClick(retry, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000); QCOMPARE(server.requests.size(), 2);
}

void UpdateDialogTests::DownloadShowsRealProgressAndTheCancelButtonStopsIt() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    server.responder = [&server, &bundle](QTcpSocket* socket, const QByteArray& path) {
        if (path == "/assets/fixture.zip") { socket->write(QByteArray("HTTP/1.1 200 OK\r\nContent-Length: ") + QByteArray::number(bundle.package.size()) + "\r\n\r\n" + bundle.package.left(32)); return; }
        socket->write(Http(server.routes.value(path))); socket->disconnectFromHost();
    };
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); UpdateClient client(server.Source(bundle, directory.filePath("downloads")), settings);
    UpdateDialog dialog(client); dialog.show(); dialog.CheckNow(); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QSignalSpy progress(&client, &UpdateClient::DownloadProgress); QSignalSpy prepared(&client, &UpdateClient::PackagePrepared);
    QTest::mouseClick(dialog.findChild<QPushButton*>("primaryButton"), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!progress.empty() && progress.last()[0].toLongLong() == 32, 3000);
    auto* bar = dialog.findChild<QProgressBar*>("updateDownloadProgress"); auto* cancel = dialog.findChild<QPushButton*>("updateCloseButton");
    QVERIFY(bar->isVisible()); QCOMPARE(bar->maximum(), 1000); QVERIFY(bar->value() > 0 && bar->value() < 1000);
    QVERIFY(!dialog.findChild<QComboBox*>("updateChannel")->isEnabled()); QCOMPARE(cancel->text(), QStringLiteral("取消"));
    Capture(dialog, QStringLiteral("update-download-graphite"));
    QTest::mouseClick(cancel, Qt::LeftButton); QCOMPARE(client.Status().state, UpdateState::Cancelled); QCOMPARE(prepared.count(), 0);
    QVERIFY(dialog.isVisible()); QCOMPARE(cancel->text(), QStringLiteral("关闭")); QVERIFY(dialog.findChild<QComboBox*>("updateChannel")->isEnabled());
    QCOMPARE(QDir(directory.filePath("downloads")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
}

void UpdateDialogTests::SettingsPageStagesOptionsAndReusesItsManualDialog() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    server.routes["/repos/fixture/editor/releases?per_page=100"] = QJsonDocument(QJsonArray{
        QJsonDocument::fromJson(server.Release(bundle)).object()}).toJson();
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    settings.setValue("unrelated/value", QStringLiteral("保持不变")); settings.sync();
    const auto before = Read(settings.fileName());
    UpdateClient client(server.Source(bundle, directory.path()), settings);
    UpdateSettingsWidget page(client); page.resize(600, 390); page.show();
    auto* automatic = page.findChild<QCheckBox*>("updateAutomaticCheck"); auto* channel = page.findChild<QComboBox*>("updateChannel");
    QVERIFY(automatic && channel); automatic->setChecked(false); channel->setCurrentIndex(1);
    QVERIFY(!page.Preferences().automaticCheckEnabled); QCOMPARE(page.Preferences().channel, War3Map::Update::UpdateChannel::Beta);
    QVERIFY(client.AutomaticCheckEnabled()); QCOMPARE(client.Channel(), War3Map::Update::UpdateChannel::Stable);
    settings.sync(); QCOMPARE(Read(settings.fileName()), before); QCOMPARE(server.requests.size(), 0);
    auto* check = page.findChild<QPushButton*>("updateSettingsCheck"); QVERIFY(check); QTest::mouseClick(check, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    const auto dialogs = page.findChildren<UpdateDialog*>(); QCOMPARE(dialogs.size(), 1);
    QVERIFY(server.requests.front().startsWith("GET /repos/fixture/editor/releases?per_page=100 "));
    QVERIFY(!dialogs.front()->findChild<QCheckBox*>("updateAutomaticCheck")->isChecked());
    QCOMPARE(dialogs.front()->findChild<QComboBox*>("updateChannel")->currentIndex(), 1);
    QTest::mouseClick(check, Qt::LeftButton); QCOMPARE(page.findChildren<UpdateDialog*>().size(), 1); QCOMPARE(server.requests.size(), 1);
    QTest::mouseClick(dialogs.front()->findChild<QPushButton*>("updateSkipButton"), Qt::LeftButton);
    QCOMPARE(page.Preferences().skippedVersion, QStringLiteral("0.2.0")); QVERIFY(client.SkippedVersion().isEmpty());
    QVERIFY(dialogs.front()->findChild<QLabel*>("updateStatus")->text().contains(QStringLiteral("保存设置后生效")));
    settings.sync(); QCOMPARE(Read(settings.fileName()), before); QCOMPARE(server.requests.size(), 1);
    dialogs.front()->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    Capture(page, QStringLiteral("update-settings-graphite"));
}

void UpdateDialogTests::CancellingSettingsDiscardsItsOptionsAndSkippedVersion() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    server.routes["/repos/fixture/editor/releases?per_page=100"] = QJsonDocument(QJsonArray{
        QJsonDocument::fromJson(server.Release(bundle)).object()}).toJson();
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    settings.setValue("unrelated/value", QStringLiteral("保留原值")); settings.sync(); const auto before = Read(settings.fileName());
    UpdateClient client(server.Source(bundle, directory.path()), settings);
    {
        UpdateSettingsWidget cancelled(client); cancelled.show();
        cancelled.findChild<QCheckBox*>("updateAutomaticCheck")->setChecked(false);
        cancelled.findChild<QComboBox*>("updateChannel")->setCurrentIndex(1);
        QTest::mouseClick(cancelled.findChild<QPushButton*>("updateSettingsCheck"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
        auto* dialog = cancelled.findChild<UpdateDialog*>(); QVERIFY(dialog);
        QTest::mouseClick(dialog->findChild<QPushButton*>("updateSkipButton"), Qt::LeftButton);
        QCOMPARE(cancelled.Preferences().skippedVersion, QStringLiteral("0.2.0"));
        // 模拟主设置页取消并销毁；没有宿主提交，不调用任何持久化入口。
        cancelled.close();
    }
    settings.sync(); QCOMPARE(Read(settings.fileName()), before);
    QVERIFY(client.AutomaticCheckEnabled()); QCOMPARE(client.Channel(), War3Map::Update::UpdateChannel::Stable); QVERIFY(client.SkippedVersion().isEmpty());
    UpdateSettingsWidget reopened(client);
    QVERIFY(reopened.Preferences().automaticCheckEnabled); QCOMPARE(reopened.Preferences().channel, War3Map::Update::UpdateChannel::Stable);
    QVERIFY(reopened.Preferences().skippedVersion.isEmpty());
}

void UpdateDialogTests::SettingsOnlyTakeEffectAfterTheHostsSuccessfulCommit() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start());
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    settings.setValue("unrelated/value", QStringLiteral("原值")); settings.sync();
    UpdateClient client(server.Source(bundle, directory.path()), settings); UpdateSettingsWidget page(client);
    page.findChild<QCheckBox*>("updateAutomaticCheck")->setChecked(false);
    page.findChild<QComboBox*>("updateChannel")->setCurrentIndex(1);
    QString error; QVERIFY(page.Validate(&error)); QVERIFY(error.isEmpty());
    const auto preferences = page.Preferences(); QVERIFY(client.AutomaticCheckEnabled());
    // 这里只模拟已成功的宿主提交；跨服务提交/失败回滚由主设置事务测试负责。
    QVERIFY(UpdateClient::WritePreferences(settings, preferences, &error)); settings.sync(); QCOMPARE(settings.status(), QSettings::NoError);
    QVERIFY(client.ApplyCommittedPreferences(preferences, &error)); page.ApplyCommitted(preferences);
    QVERIFY(client.Preferences() == preferences); QVERIFY(UpdateClient::ReadPreferences(settings) == preferences);
    QCOMPARE(settings.value("unrelated/value").toString(), QStringLiteral("原值")); QCOMPARE(server.requests.size(), 0);
}

void UpdateDialogTests::PreparedPackageEmitsAnInstallationRequestWithoutLaunchingAProcess() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); UpdateClient client(server.Source(bundle, directory.filePath("downloads")), settings);
    UpdateDialog dialog(client); dialog.show(); dialog.CheckNow(); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QSignalSpy install(&dialog, &UpdateDialog::InstallationRequested);
    QTest::mouseClick(dialog.findChild<QPushButton*>("primaryButton"), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Ready, 5000);
    QCOMPARE(install.count(), 0); auto* primary = dialog.findChild<QPushButton*>("primaryButton");
    QCOMPARE(primary->text(), QStringLiteral("安装并重启")); QVERIFY(primary->isEnabled());
    QTest::mouseClick(primary, Qt::LeftButton); QCOMPARE(install.count(), 1);
    const auto prepared = qvariant_cast<PreparedUpdate>(install.front().front());
    QCOMPARE(Read(prepared.packagePath), bundle.package); QCOMPARE(Read(prepared.manifestPath), bundle.manifest);
    QVERIFY(dialog.isVisible()); Capture(dialog, QStringLiteral("update-ready-graphite"));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen"); QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("AudastraUpdateQtTests")); application.setApplicationName(QStringLiteral("OriginalLoopbackFixtures"));
    UpdateDialogTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "UpdateDialogTests.moc"
