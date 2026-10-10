#include "MainWindow.h"
#include "ObjectModule.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

const char* kWar3MapBuildStamp = "project-watch-integration-test";

namespace {
std::filesystem::path NativePath(const QString& value) {
    return std::filesystem::path(value.toStdWString());
}

bool Write(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray Read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

constexpr auto kUnit = "[hfoo]\n_parent=\"hfoo\"\nuhpm=420\n";
}

class ProjectWatchTests final : public QObject {
    Q_OBJECT

private slots:
    void DirectoryEventsDoNotDiscardEdits();
    void ProjectReplacementIsCheckedAndWatcherRearmed();
    void RecoveryNoticeRespectsStartupModeAndPreservesSnapshot();
    void InvalidRecoveryPreservesOpenSession();
    void WatchBudgetIsVisibleAndResetClearsObservation();
};

void ProjectWatchTests::DirectoryEventsDoNotDiscardEdits() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto root = fixture.filePath("workspace");
    const auto source = root + "/table/unit.ini";
    QVERIFY(Write(source, kUnit));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(root)));
    auto* document = window.project_->objectDocuments_[0].get();
    QVERIFY(document && document->Objects().size() == 1);
    const auto& object = document->Objects().front();
    QVERIFY(!object.modifications.empty());
    QVERIFY(document->SetModification(object.key, object.modifications.front().key, std::int32_t{888}).Succeeded());
    window.objectsModule_->MarkCurrentObjectDocumentDirty();
    const auto edited = document->DocumentFingerprint();
    QVERIFY(window.CheckProjectSources());
    QVERIFY(!window.project_->sourceChangeSuspected_);
    QVERIFY(Write(source, "[hfoo]\n_parent=\"hfoo\"\nuhpm=999\n"));
    QTRY_VERIFY_WITH_TIMEOUT(window.project_->sourceChangeSuspected_, 4000);
    QVERIFY(!window.project_->sourceChangeConfirmed_);
    QVERIFY(!window.projectNoticeBar_->isHidden());
    QCOMPARE(window.projectNoticeAction_->text(), QString::fromUtf8("检查来源"));
    QCOMPARE(document->DocumentFingerprint(), edited);
    QVERIFY(window.project_->objectDirty_[0]);

    window.projectNoticeAction_->click();
    QVERIFY(window.project_->sourceChangeConfirmed_);
    QCOMPARE(window.project_->objectDocuments_[0].get(), document);
    QCOMPARE(document->DocumentFingerprint(), edited);
    QVERIFY(!window.SaveEditorProjectToPath(NativePath(fixture.filePath("rejected.forge"))));
    QVERIFY(!QFileInfo::exists(fixture.filePath("rejected.forge")));
    window.projectNoticeDismiss_->click();
    QVERIFY(window.projectNoticeBar_->isHidden());
    QVERIFY(window.project_->sourceChangeSuspected_);
    QCOMPARE(document->DocumentFingerprint(), edited);

    // 恢复为相同内容后可消除疑似事件，不更改已编辑的内存文档。
    QVERIFY(Write(source, kUnit));
    QVERIFY(window.CheckProjectSources());
    QVERIFY(!window.project_->sourceChangeSuspected_);
    QVERIFY(!window.project_->sourceChangeConfirmed_);
    QCOMPARE(document->DocumentFingerprint(), edited);
}

void ProjectWatchTests::ProjectReplacementIsCheckedAndWatcherRearmed() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto project = fixture.filePath("opened.forge");
    const auto replacement = fixture.filePath("replacement.forge");
    QVERIFY(Write(project, "first project snapshot"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    window.project_->projectFilePath_ = NativePath(project);
    const auto sha = War3Map::Sha256File(window.project_->projectFilePath_);
    QVERIFY(sha.Succeeded());
    window.project_->projectFileSha256_ = *sha.value;
    window.RefreshProjectWatch();
    QVERIFY(window.CheckProjectSources());
    QVERIFY(Write(replacement, "concurrently replaced snapshot"));
    QVERIFY(QFile::remove(project));
    QVERIFY(QFile::rename(replacement, project));
    QTRY_VERIFY_WITH_TIMEOUT(window.project_->sourceChangeSuspected_, 4000);
    QVERIFY(!window.project_->sourceChangeConfirmed_);
    QVERIFY(!window.CheckProjectSources());
    QCOMPARE(window.project_->projectFileSha256_, *sha.value);
    QVERIFY(window.project_->sourceChangeConfirmed_);
    QVERIFY(window.project_->projectWatch_->files().contains(QDir::cleanPath(project)));
    QVERIFY(Write(project, "first project snapshot"));
    QVERIFY(window.CheckProjectSources());
    QVERIFY(!window.project_->sourceChangeConfirmed_);
}

void ProjectWatchTests::RecoveryNoticeRespectsStartupModeAndPreservesSnapshot() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto root = fixture.filePath("workspace");
    const auto recovery = fixture.filePath("recover.forge");
    QVERIFY(Write(root + "/table/unit.ini", kUnit));
    MainWindow writer(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(writer.OpenWorkspacePath(NativePath(root)));
    QVERIFY(writer.SaveEditorProjectToPath(NativePath(recovery), true));
    const auto bytes = Read(recovery);
    QVERIFY(!bytes.isEmpty());
    QSettings{}.setValue("project/latestRecovery", recovery);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    window.ShowRecoveryNotice();
    QVERIFY(window.project_->recoveryNoticePath_.empty());
    QVERIFY(window.projectNoticeBar_->isHidden());

    // 只切换该通知方法的模式；不载入机器上的游戏、UI 或目录设置。
    window.startupMode_ = MainWindow::StartupMode::Normal;
    window.ShowRecoveryNotice();
    QCOMPARE(window.project_->recoveryNoticePath_, NativePath(recovery));
    QCOMPARE(window.projectNoticeAction_->text(), QString::fromUtf8("恢复编辑"));
    QVERIFY(!window.projectNoticeBar_->isHidden());
    window.projectNoticeDismiss_->click();
    QVERIFY(window.projectNoticeBar_->isHidden());
    QCOMPARE(Read(recovery), bytes);
    QCOMPARE(QSettings{}.value("project/latestRecovery").toString(), recovery);

    window.ShowRecoveryNotice();
    window.projectNoticeAction_->click();
    QVERIFY(!window.project_->projectRoot_.empty());
    QVERIFY(window.project_->objectDocuments_[0]);
    QVERIFY(window.project_->recoveryNoticePath_.empty());
    QCOMPARE(Read(recovery), bytes);
}

void ProjectWatchTests::InvalidRecoveryPreservesOpenSession() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto root = fixture.filePath("workspace");
    const auto invalid = fixture.filePath("invalid.forge");
    QVERIFY(Write(root + "/table/unit.ini", kUnit));
    QVERIFY(Write(invalid, "not a project archive"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(root)));
    const auto* document = window.project_->objectDocuments_[0].get();
    QVERIFY(document);
    const auto fingerprint = document->DocumentFingerprint();
    QSettings{}.setValue("project/latestRecovery", invalid);
    window.startupMode_ = MainWindow::StartupMode::Normal;
    window.ShowRecoveryNotice();
    window.projectNoticeAction_->click();
    QCOMPARE(window.project_->objectDocuments_[0].get(), document);
    QCOMPARE(document->DocumentFingerprint(), fingerprint);
    QCOMPARE(window.project_->projectRoot_, NativePath(root));
    QVERIFY(!window.project_->recoveryNoticePath_.empty());
    QCOMPARE(Read(invalid), QByteArray("not a project archive"));
}

void ProjectWatchTests::WatchBudgetIsVisibleAndResetClearsObservation() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto root = fixture.filePath("workspace");
    QVERIFY(Write(root + "/table/unit.ini", kUnit));
    for (int i = 0; i < 2050; ++i)
        QVERIFY(Write(root + "/assets/" + QString::number(i) + ".bin", "x"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(root)));
    QVERIFY(window.project_->projectWatchLimited_);
    QVERIFY(window.project_->projectWatch_->files().size() + window.project_->projectWatch_->directories().size() <= 2048);
    QVERIFY(window.projectNoticeText_->text().contains(QString::fromUtf8("部分路径未实时监测")));
    QVERIFY(window.CheckProjectSources());
    const auto unobserved = root + "/assets/new-unobserved.bin";
    QVERIFY(Write(unobserved, "new source member"));
    // 无论该路径是否被实时观察，显式检查仍会枚举全部来源。
    QVERIFY(!window.CheckProjectSources());
    window.ResetProjectState();
    QVERIFY(window.project_->projectWatch_->files().isEmpty());
    QVERIFY(window.project_->projectWatch_->directories().isEmpty());
    QVERIFY(!window.project_->sourceChangeSuspected_);
    QVERIFY(!window.project_->projectWatchLimited_);
    QVERIFY(window.projectNoticeBar_->isHidden());
}

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("War3MapEditor64Tests");
    QCoreApplication::setApplicationName("ProjectWatchTests");
    QTemporaryDir fixture;
    if (!fixture.isValid()) return 1;
    // 设置和恢复提取都留在测试临时目录，不能污染作者的启动状态。
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.filePath("settings"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, fixture.filePath("system-settings"));
    const auto previousTmp = qgetenv("TMP");
    const auto previousTemp = qgetenv("TEMP");
    qputenv("TMP", QFile::encodeName(fixture.path()));
    qputenv("TEMP", QFile::encodeName(fixture.path()));
    ProjectWatchTests tests;
    const int result = QTest::qExec(&tests, argc, argv);
    qputenv("TMP", previousTmp);
    qputenv("TEMP", previousTemp);
    return result;
}

#include "ProjectWatchTests.moc"
