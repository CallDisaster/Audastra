#include "MapTestSettingsPage.h"
#include "../../src/QtCommon/MapBackupPaths.h"
#include "../../src/QtProject/MapSavePreferences.h"
#include "War3MapCore/Hash.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <utility>

using War3Map::QtApp::MapTestSettingsPage;

namespace {

QVariantMap StoredValues() {
    const QSettings stored;
    QVariantMap values;
    for (const auto& key : stored.allKeys()) values.insert(key, stored.value(key));
    return values;
}

QByteArray StoredBytes() {
    const QSettings stored;
    QFile file(stored.fileName());
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QStringList TemporaryEntries(const QString& root) {
    QStringList paths;
    QDirIterator entries(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (entries.hasNext()) paths.push_back(QDir(root).relativeFilePath(entries.next()));
    paths.sort();
    return paths;
}

// 所有计时器跟随局部 scope；选择器异常时最多两秒后退出本对话框，避免留下嵌套循环。
bool RunLegacyChooser(QPushButton* button, const QString& selected, bool accept) {
    QObject timers;
    bool observed = false;
    QTimer::singleShot(0, &timers, [&] {
        auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != QStringLiteral("mapLegacyBackupDirectoryDialog")) return;
        observed = dialog->testOption(QFileDialog::ReadOnly);
        if (accept) {
            dialog->setDirectory(selected);
            dialog->selectFile(selected);
            // accept 是 QFileDialog 的 protected override；metaobject 调用实际目录存在门。
            QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        } else {
            dialog->reject();
        }
    });
    QTimer::singleShot(2000, &timers, [] {
        auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (dialog && dialog->objectName() == QStringLiteral("mapLegacyBackupDirectoryDialog")) dialog->reject();
    });
    button->click();
    return observed;
}

} // namespace

class MapTestSettingsTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void DefaultsAndPlaceholders();
    void StoredValuesAndUnlimitedRetention();
    void LocalYdweFallbackStaysImplicitAndReadonly();
    void DraftDoesNotWriteSettingsOrCreateDirectories();
    void DirectoryValidation();
    void RetentionControlBounds();
    void UnknownStoredValuesKeepConservativeDefaults();
    void DefaultBackupPathAndPreferenceResolution();
    void InvalidDefaultFailsClosedAndExplicitSettingWins();
    void UnreadablePreferenceFormatFailsClosed();
    void RetentionPreferencesKeepBounds();
    void ResolvedDefaultDrivesRealInPlaceBackup();
    void LegacyInspectionDisplayAndCancelAreReadonly();
    void LegacyInspectionSelectionDoesNotRewritePreferences();
    void LegacyInspectionCallbackCanDestroyPage();
    void LegacyChooserOwnerCanDisappear();

private:
    QTemporaryDir settingsRoot_;
    QString previousOrganization_;
    QString previousApplication_;
    QSettings::Format previousFormat_ = QSettings::NativeFormat;
};

void MapTestSettingsTests::initTestCase() {
    QVERIFY(settingsRoot_.isValid());
    previousOrganization_ = QCoreApplication::organizationName();
    previousApplication_ = QCoreApplication::applicationName();
    previousFormat_ = QSettings::defaultFormat();
    QCoreApplication::setOrganizationName(QStringLiteral("Audastra-MapTestSettings-Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("MapTestSettings"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot_.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsRoot_.path());
}

void MapTestSettingsTests::cleanupTestCase() {
    QCoreApplication::setOrganizationName(previousOrganization_);
    QCoreApplication::setApplicationName(previousApplication_);
    QSettings::setDefaultFormat(previousFormat_);
    // setPath 仅改变本测试子进程的 INI 后端；未设置/访问 NativeFormat 的作者配置。
}

void MapTestSettingsTests::init() {
    QSettings stored;
    stored.clear();
    stored.sync();
    QCOMPARE(stored.status(), QSettings::NoError);
}

void MapTestSettingsTests::DefaultsAndPlaceholders() {
    const auto installation = settingsRoot_.filePath(QStringLiteral("portable-installation"));
    const auto localData = settingsRoot_.filePath(QStringLiteral("local-data"));
    MapTestSettingsPage page(installation, localData);
    QCOMPARE(page.objectName(), QStringLiteral("mapTestSettingsPage"));
    auto* backup = page.findChild<QLineEdit*>(QStringLiteral("mapBackupDirectory"));
    auto* runtime = page.findChild<QLineEdit*>(QStringLiteral("mapTestRuntimeRoot"));
    auto* limit = page.findChild<QSpinBox*>(QStringLiteral("mapBackupLimit"));
    auto* slk = page.findChild<QCheckBox*>(QStringLiteral("mapTestSlkEnabled"));
    QVERIFY(backup); QVERIFY(runtime); QVERIFY(limit); QVERIFY(slk);
    QCOMPARE(backup->placeholderText(), QDir::toNativeSeparators(QDir(localData).filePath(QStringLiteral("Audastra/backups"))));
    QVERIFY(!page.findChild<QLineEdit*>(QStringLiteral("mapTestSlkToolRoot")));
    QCOMPARE(runtime->placeholderText(), QStringLiteral("留空使用已配置的本机 YDWE 目录"));
    QVERIFY(backup->text().isEmpty()); QVERIFY(runtime->text().isEmpty());
    QCOMPARE(limit->value(), 10); QVERIFY(!slk->isChecked());
    const auto preferences = page.Preferences();
    QVERIFY(preferences.mapBackupDirectory.has_value()); QVERIFY(preferences.mapBackupDirectory->isEmpty());
    QVERIFY(preferences.mapBackupLimit.has_value()); QCOMPARE(*preferences.mapBackupLimit, 10);
    QVERIFY(preferences.mapTestRuntimeRoot.has_value()); QVERIFY(preferences.mapTestRuntimeRoot->isEmpty());
    QVERIFY(preferences.mapTestSlk.has_value()); QVERIFY(!*preferences.mapTestSlk);
    QString error = QStringLiteral("旧错误");
    QVERIFY(page.Validate(&error)); QVERIFY(error.isEmpty());
    QVERIFY(StoredValues().isEmpty());
    QVERIFY(!QDir(installation).exists());
    QVERIFY(!QDir(localData).exists());
}

void MapTestSettingsTests::LocalYdweFallbackStaysImplicitAndReadonly() {
    QSettings stored;
    const auto ydwe = settingsRoot_.filePath(QStringLiteral("existing-ydwe"));
    stored.setValue(QStringLiteral("ui/dataRoot"), ydwe);
    stored.sync();
    const auto before = StoredValues();
    MapTestSettingsPage page(settingsRoot_.path(), settingsRoot_.filePath(QStringLiteral("local-data")));
    const auto* runtime = page.findChild<QLineEdit*>(QStringLiteral("mapTestRuntimeRoot"));
    QVERIFY(runtime); QVERIFY(runtime->text().isEmpty());
    QVERIFY(page.Preferences().mapTestRuntimeRoot->isEmpty());
    QVERIFY(page.Validate());
    QCOMPARE(StoredValues(), before);
    QVERIFY(!stored.contains(QStringLiteral("mapTest/runtimeRoot")));
    QVERIFY(!QDir(ydwe).exists());
}

void MapTestSettingsTests::StoredValuesAndUnlimitedRetention() {
    const auto backup = settingsRoot_.filePath(QStringLiteral("custom-backups"));
    const auto slkTool = settingsRoot_.filePath(QStringLiteral("custom-slk-tool"));
    const auto runtime = settingsRoot_.filePath(QStringLiteral("custom-runtime"));
    QSettings stored;
    stored.setValue(QStringLiteral("mapSave/backupDirectory"), backup);
    stored.setValue(QStringLiteral("mapSave/backupLimit"), 0);
    stored.setValue(QStringLiteral("mapTest/slkToolRoot"), slkTool);
    stored.setValue(QStringLiteral("mapTest/runtimeRoot"), runtime);
    stored.setValue(QStringLiteral("mapTest/slkEnabled"), true);
    stored.sync(); QCOMPARE(stored.status(), QSettings::NoError);
    const auto before = StoredValues();
    MapTestSettingsPage page(settingsRoot_.path(), settingsRoot_.filePath(QStringLiteral("local-data")));
    const auto preferences = page.Preferences();
    QVERIFY(preferences.mapBackupDirectory.has_value()); QCOMPARE(*preferences.mapBackupDirectory, backup);
    QVERIFY(preferences.mapBackupLimit.has_value()); QCOMPARE(*preferences.mapBackupLimit, 0);
    QVERIFY(preferences.mapTestRuntimeRoot.has_value()); QCOMPARE(*preferences.mapTestRuntimeRoot, runtime);
    QVERIFY(preferences.mapTestSlk.has_value()); QVERIFY(*preferences.mapTestSlk);
    QVERIFY(page.Validate()); QCOMPARE(StoredValues(), before);
}

void MapTestSettingsTests::DraftDoesNotWriteSettingsOrCreateDirectories() {
    QSettings stored;
    stored.setValue(QStringLiteral("unrelated/keep"), QStringLiteral("原值"));
    stored.setValue(QStringLiteral("mapSave/backupLimit"), 10);
    stored.sync(); QCOMPARE(stored.status(), QSettings::NoError);
    const auto before = StoredValues();
    const auto beforeBytes = StoredBytes();
    const auto beforeEntries = TemporaryEntries(settingsRoot_.path());
    const auto backup = settingsRoot_.filePath(QStringLiteral("uncreated-backups"));
    const auto runtime = settingsRoot_.filePath(QStringLiteral("uncreated-runtime"));
    {
        MapTestSettingsPage page(settingsRoot_.path(), settingsRoot_.filePath(QStringLiteral("local-data")));
        page.findChild<QLineEdit*>(QStringLiteral("mapBackupDirectory"))->setText(QStringLiteral("  ") + backup + QStringLiteral("  "));
        page.findChild<QLineEdit*>(QStringLiteral("mapTestRuntimeRoot"))->setText(QStringLiteral("  ") + runtime + QStringLiteral("  "));
        page.findChild<QSpinBox*>(QStringLiteral("mapBackupLimit"))->setValue(23);
        page.findChild<QCheckBox*>(QStringLiteral("mapTestSlkEnabled"))->setChecked(true);
        QVERIFY(page.Validate());
        const auto preferences = page.Preferences();
        QCOMPARE(*preferences.mapBackupDirectory, backup); QCOMPARE(*preferences.mapBackupLimit, 23);
        QCOMPARE(*preferences.mapTestRuntimeRoot, runtime); QVERIFY(*preferences.mapTestSlk);
        QCOMPARE(StoredValues(), before);
    }
    QCOMPARE(StoredValues(), before);
    QCOMPARE(StoredBytes(), beforeBytes);
    QCOMPARE(TemporaryEntries(settingsRoot_.path()), beforeEntries);
    QVERIFY(!QDir(backup).exists()); QVERIFY(!QDir(runtime).exists());
}

void MapTestSettingsTests::DirectoryValidation() {
    MapTestSettingsPage page(settingsRoot_.path(), settingsRoot_.filePath(QStringLiteral("local-data")));
    const QStringList fields{QStringLiteral("mapBackupDirectory"), QStringLiteral("mapTestRuntimeRoot")};
    for (const auto& field : fields) {
        auto* edit = page.findChild<QLineEdit*>(field); QVERIFY(edit);
        QString error;
        edit->setText(QStringLiteral("relative/directory"));
        QVERIFY(!page.Validate(&error)); QVERIFY(error.contains(QStringLiteral("绝对目录")));
        edit->setText(QString(4097, QChar(u'a')));
        QVERIFY(!page.Validate(&error)); QVERIFY(error.contains(QStringLiteral("4096")));
        for (const auto control : {char16_t(0), char16_t(1), char16_t(0x1f)}) {
            auto invalid = settingsRoot_.filePath(QStringLiteral("directory")); invalid.append(QChar(control));
            edit->setText(invalid);
            QVERIFY(!page.Validate(&error)); QVERIFY(error.contains(QStringLiteral("控制字符")));
        }
        edit->setText(settingsRoot_.filePath(QStringLiteral("not-created")));
        QVERIFY(page.Validate(&error)); QVERIFY(error.isEmpty());
        edit->clear(); QVERIFY(page.Validate());
    }
    QVERIFY(!QDir(settingsRoot_.filePath(QStringLiteral("not-created"))).exists());
}

void MapTestSettingsTests::RetentionControlBounds() {
    MapTestSettingsPage page(settingsRoot_.path(), settingsRoot_.filePath(QStringLiteral("local-data")));
    auto* limit = page.findChild<QSpinBox*>(QStringLiteral("mapBackupLimit")); QVERIFY(limit);
    QCOMPARE(limit->minimum(), 0); QCOMPARE(limit->maximum(), 1000);
    limit->setValue(0); QCOMPARE(*page.Preferences().mapBackupLimit, 0);
    limit->setValue(1000); QCOMPARE(*page.Preferences().mapBackupLimit, 1000);
}

void MapTestSettingsTests::UnknownStoredValuesKeepConservativeDefaults() {
    QSettings stored;
    stored.setValue(QStringLiteral("mapTest/slkEnabled"), QStringLiteral("yes"));
    stored.setValue(QStringLiteral("mapTest/ydweRoot"), settingsRoot_.filePath(QStringLiteral("legacy-ydwe")));
    stored.setValue(QStringLiteral("mapSave/backupLimit"), -1);
    stored.sync();
    const auto before = StoredValues();
    MapTestSettingsPage page(settingsRoot_.filePath(QStringLiteral("installation")),
        settingsRoot_.filePath(QStringLiteral("local-data")));
    auto* slk = page.findChild<QCheckBox*>(QStringLiteral("mapTestSlkEnabled")); QVERIFY(slk);
    auto* limit = page.findChild<QSpinBox*>(QStringLiteral("mapBackupLimit")); QVERIFY(limit);
    QVERIFY(!slk->isChecked());
    QCOMPARE(limit->value(), 10);
    QVERIFY(page.Preferences().mapTestRuntimeRoot->isEmpty());
    QVERIFY(page.Validate());
    QCOMPARE(StoredValues(), before);
}

void MapTestSettingsTests::DefaultBackupPathAndPreferenceResolution() {
    const auto localData = settingsRoot_.filePath(QStringLiteral("独立用户数据"));
    const auto expected = QDir(localData).filePath(QStringLiteral("Audastra/backups"));
    const auto beforeBytes = StoredBytes();
    const auto beforeEntries = TemporaryEntries(settingsRoot_.path());
    QSettings stored;
    const auto options = War3Map::QtProject::ReadMapInPlaceSaveOptions(stored, localData);
    QVERIFY(options.Succeeded());
    QCOMPARE(QString::fromStdWString(options.value->backupDirectory.wstring()), expected);
    QCOMPARE(options.value->keepBackups, std::size_t{10});
    for (const auto installation : {QStringLiteral("version-a"), QStringLiteral("version-b")}) {
        MapTestSettingsPage page(settingsRoot_.filePath(installation), localData);
        const auto* edit = page.findChild<QLineEdit*>(QStringLiteral("mapBackupDirectory"));
        QVERIFY(edit);
        QCOMPARE(edit->placeholderText(), QDir::toNativeSeparators(expected));
        QVERIFY(page.Validate());
        QVERIFY(page.Preferences().mapBackupDirectory->isEmpty());
    }
    // 生产无参路径同样不受当前测试进程的组织名影响；这里只查询 OS 路径，不访问该目录。
    const auto actual = War3Map::QtCommon::DefaultMapBackupDirectory();
    QVERIFY(actual.Succeeded());
    QCOMPARE(*actual.value, QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("Audastra/backups")));
    QCOMPARE(StoredBytes(), beforeBytes);
    QCOMPARE(TemporaryEntries(settingsRoot_.path()), beforeEntries);
    QVERIFY(!QDir(localData).exists());
}

void MapTestSettingsTests::InvalidDefaultFailsClosedAndExplicitSettingWins() {
    QSettings stored;
    const auto expandedTooLong = settingsRoot_.path() + QChar(u'/')
        + QString(4096 - settingsRoot_.path().size() - 1, QChar(u'a'));
    const QStringList invalidRoots{QString{}, QStringLiteral("relative-data"),
        settingsRoot_.filePath(QStringLiteral("data")) + QChar(u'\n'), QString(4097, QChar(u'a')), expandedTooLong};
    for (const auto& root : invalidRoots) {
        const auto options = War3Map::QtProject::ReadMapInPlaceSaveOptions(stored, root);
        QVERIFY(!options.Succeeded()); QVERIFY(!options.value);
        QCOMPARE(options.diagnostics.size(), std::size_t{1});
        QCOMPARE(options.diagnostics.front().code, std::string("MAP_BACKUP_DEFAULT_UNAVAILABLE"));
        MapTestSettingsPage page(settingsRoot_.path(), root);
        QString error;
        QVERIFY(!page.Validate(&error)); QVERIFY(!error.isEmpty());
        QVERIFY(page.findChild<QLineEdit*>(QStringLiteral("mapBackupDirectory"))->placeholderText()
            .contains(QStringLiteral("不可用")));
    }
    const auto explicitDirectory = settingsRoot_.filePath(QStringLiteral("explicit-backups"));
    stored.setValue(QStringLiteral("mapSave/backupDirectory"), QStringLiteral("  ") + explicitDirectory + QStringLiteral("  "));
    stored.sync(); QCOMPARE(stored.status(), QSettings::NoError);
    const auto beforeBytes = StoredBytes();
    const auto explicitOptions = War3Map::QtProject::ReadMapInPlaceSaveOptions(stored, {});
    QVERIFY(explicitOptions.Succeeded());
    QCOMPARE(QString::fromStdWString(explicitOptions.value->backupDirectory.wstring()), explicitDirectory);
    MapTestSettingsPage page(settingsRoot_.path(), QString{});
    QVERIFY(page.Validate());
    QCOMPARE(*page.Preferences().mapBackupDirectory, explicitDirectory);
    QCOMPARE(StoredBytes(), beforeBytes); QVERIFY(!QDir(explicitDirectory).exists());

    const QStringList invalidConfigured{QStringLiteral("relative-backups"),
        explicitDirectory + QChar(u'\n'), QString(4097, QChar(u'a')), QString(4097, QChar(u' ')), QString(QChar(u'\t'))};
    for (const auto& configured : invalidConfigured) {
        stored.setValue(QStringLiteral("mapSave/backupDirectory"), configured);
        const auto rejected = War3Map::QtProject::ReadMapInPlaceSaveOptions(stored, settingsRoot_.path());
        QVERIFY(!rejected.Succeeded()); QVERIFY(!rejected.value);
        QCOMPARE(rejected.diagnostics.front().code, std::string("MAP_BACKUP_DIRECTORY_INVALID"));
    }
    stored.beginGroup(QStringLiteral("unexpected-group"));
    const auto grouped = War3Map::QtProject::ReadMapInPlaceSaveOptions(stored, settingsRoot_.path());
    QVERIFY(!grouped.Succeeded()); QVERIFY(!grouped.value);
    QCOMPARE(grouped.diagnostics.front().code, std::string("MAP_SAVE_PREFERENCES_UNAVAILABLE"));
    stored.endGroup();
}

void MapTestSettingsTests::RetentionPreferencesKeepBounds() {
    QSettings stored;
    const auto localData = settingsRoot_.filePath(QStringLiteral("local-data"));
    for (const auto value : {0, 10, 1000, -1, 1001}) {
        stored.setValue(QStringLiteral("mapSave/backupLimit"), value);
        const auto options = War3Map::QtProject::ReadMapInPlaceSaveOptions(stored, localData);
        QVERIFY(options.Succeeded());
        const auto expected = value >= 0 && value <= 1000 ? static_cast<std::size_t>(value) : std::size_t{10};
        QCOMPARE(options.value->keepBackups, expected);
    }
    stored.setValue(QStringLiteral("mapSave/backupLimit"), QStringLiteral("unknown"));
    const auto options = War3Map::QtProject::ReadMapInPlaceSaveOptions(stored, localData);
    QVERIFY(options.Succeeded()); QCOMPARE(options.value->keepBackups, std::size_t{10});
    QVERIFY(!QDir(localData).exists());
}

void MapTestSettingsTests::UnreadablePreferenceFormatFailsClosed() {
    const auto fileName = settingsRoot_.filePath(QStringLiteral("broken.ini"));
    {
        QFile file(fileName); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray broken("[unterminated\nmapSave/backupDirectory=relative\n");
        QCOMPARE(file.write(broken), qint64(broken.size()));
    }
    const QSettings malformed(fileName, QSettings::IniFormat);
    const auto options = War3Map::QtProject::ReadMapInPlaceSaveOptions(malformed, settingsRoot_.path());
    QCOMPARE(malformed.status(), QSettings::FormatError);
    QVERIFY(!options.Succeeded()); QVERIFY(!options.value);
    QCOMPARE(options.diagnostics.front().code, std::string("MAP_SAVE_PREFERENCES_UNAVAILABLE"));
    QFile after(fileName); QVERIFY(after.open(QIODevice::ReadOnly));
    QCOMPARE(after.readAll(), QByteArray("[unterminated\nmapSave/backupDirectory=relative\n"));
}

void MapTestSettingsTests::ResolvedDefaultDrivesRealInPlaceBackup() {
    // 原创极小归档，只验证偏好到 Core 备份路径；不是原版打开/游戏运行验收。
    const auto localData = settingsRoot_.filePath(QStringLiteral("save-data"));
    const auto source = std::filesystem::path(settingsRoot_.filePath(QStringLiteral("original.w3x")).toStdWString());
    const std::vector<War3Map::MapArchiveMember> members{
        {"war3map.j", {std::byte{'o'}, std::byte{'l'}, std::byte{'d'}}}};
    const auto created = War3Map::WriteVerifiedArchive(source, members);
    QVERIFY(created.Succeeded());
    const auto sourceSha = War3Map::Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto archive = War3Map::MapArchiveSession::Open(source); QVERIFY(archive.Succeeded());
    const QSettings stored;
    const auto options = War3Map::QtProject::ReadMapInPlaceSaveOptions(stored, localData);
    QVERIFY(options.Succeeded()); QVERIFY(!QDir(localData).exists());
    const std::vector<War3Map::MemberReplacement> edits{
        {"war3map.j", {std::byte{'n'}, std::byte{'e'}, std::byte{'w'}}}};
    const auto saved = archive.value->SaveInPlace(edits, {}, *options.value);
    QVERIFY(saved.Succeeded());
    QCOMPARE(saved.value->backup.parent_path(), options.value->backupDirectory);
    const auto backupSha = War3Map::Sha256File(saved.value->backup); QVERIFY(backupSha.Succeeded());
    QCOMPARE(*backupSha.value, *sourceSha.value);
    QVERIFY(QDir(QDir(localData).filePath(QStringLiteral("Audastra/backups/.forge-backup-owners"))).exists());
    QVERIFY(!QDir(settingsRoot_.filePath(QStringLiteral("War3MapEditor Backups"))).exists());
    const auto reopened = War3Map::MapArchiveSession::Open(source); QVERIFY(reopened.Succeeded());
    const auto script = reopened.value->ReadMember("war3map.j"); QVERIFY(script.Succeeded());
    QVERIFY(*script.value == edits.front().data);
}

void MapTestSettingsTests::LegacyInspectionDisplayAndCancelAreReadonly() {
    QSettings stored;
    stored.setValue(QStringLiteral("unrelated/keep"), QStringLiteral("original"));
    stored.sync(); QCOMPARE(stored.status(), QSettings::NoError);
    const auto beforeValues = StoredValues();
    const auto beforeBytes = StoredBytes();
    const auto localData = settingsRoot_.filePath(QStringLiteral("local-data"));
    MapTestSettingsPage page(settingsRoot_.path(), localData);
    auto* note = page.findChild<QLabel*>(QStringLiteral("mapLegacyBackupInspection"));
    auto* browse = page.findChild<QPushButton*>(QStringLiteral("mapLegacyBackupDirectoryBrowse"));
    QVERIFY(note); QVERIFY(browse); QVERIFY(!browse->isEnabled());
    QVERIFY(note->text().isEmpty());
    QCOMPARE(note->textFormat(), Qt::PlainText);
    page.SetLegacyBackupInspectionText(QStringLiteral("<b>已确认旧位置</b>"));
    QCOMPARE(note->text(), QStringLiteral("<b>已确认旧位置</b>"));
    int requested = 0;
    page.SetLegacyBackupInspectionBusy(true);
    page.SetLegacyBackupDirectoryRequestedHandler([&requested](QString) { ++requested; });
    QVERIFY(!browse->isEnabled()); browse->click(); QCOMPARE(requested, 0);
    page.SetLegacyBackupInspectionBusy(false); QVERIFY(browse->isEnabled());
    QVERIFY(RunLegacyChooser(browse, {}, false));
    QCOMPARE(requested, 0); QVERIFY(page.Preferences().mapBackupDirectory->isEmpty());
    QCOMPARE(StoredValues(), beforeValues); QCOMPARE(StoredBytes(), beforeBytes);
    QVERIFY(!QDir(localData).exists());
    page.SetLegacyBackupDirectoryRequestedHandler({}); QVERIFY(!browse->isEnabled());
}

void MapTestSettingsTests::LegacyInspectionSelectionDoesNotRewritePreferences() {
    const auto selected = settingsRoot_.filePath(QStringLiteral("legacy-backups"));
    QVERIFY(QDir().mkpath(selected));
    QSettings stored;
    stored.setValue(QStringLiteral("mapSave/backupLimit"), 7);
    stored.sync(); QCOMPARE(stored.status(), QSettings::NoError);
    const auto beforeValues = StoredValues(); const auto beforeBytes = StoredBytes();
    const auto localData = settingsRoot_.filePath(QStringLiteral("local-data"));
    MapTestSettingsPage page(settingsRoot_.path(), localData);
    QString observed;
    int requested = 0;
    page.SetLegacyBackupDirectoryRequestedHandler([&](QString directory) {
        ++requested; observed = std::move(directory);
        page.SetLegacyBackupInspectionText(QStringLiteral("正在只读核对"));
        page.SetLegacyBackupInspectionBusy(true);
    });
    auto* browse = page.findChild<QPushButton*>(QStringLiteral("mapLegacyBackupDirectoryBrowse")); QVERIFY(browse);
    QVERIFY(RunLegacyChooser(browse, selected, true));
    QCOMPARE(requested, 1); QCOMPARE(observed, selected); QVERIFY(!browse->isEnabled());
    QVERIFY(page.Preferences().mapBackupDirectory->isEmpty());
    QCOMPARE(*page.Preferences().mapBackupLimit, 7);
    QCOMPARE(StoredValues(), beforeValues); QCOMPARE(StoredBytes(), beforeBytes);
    QVERIFY(QDir(selected).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    QVERIFY(!QDir(localData).exists());
}

void MapTestSettingsTests::LegacyInspectionCallbackCanDestroyPage() {
    const auto selected = settingsRoot_.filePath(QStringLiteral("legacy-destroy"));
    QVERIFY(QDir().mkpath(selected));
    const auto beforeBytes = StoredBytes();
    QPointer<MapTestSettingsPage> page(new MapTestSettingsPage(settingsRoot_.path(),
        settingsRoot_.filePath(QStringLiteral("local-data"))));
    const auto cleanup = qScopeGuard([&page] { if (page) delete page.data(); });
    int requested = 0;
    page->SetLegacyBackupDirectoryRequestedHandler([&](QString directory) {
        if (directory == selected) ++requested;
        delete page.data();
    });
    auto* browse = page->findChild<QPushButton*>(QStringLiteral("mapLegacyBackupDirectoryBrowse")); QVERIFY(browse);
    QVERIFY(RunLegacyChooser(browse, selected, true));
    QCOMPARE(requested, 1); QVERIFY(page.isNull());
    QCOMPARE(StoredBytes(), beforeBytes);
}

void MapTestSettingsTests::LegacyChooserOwnerCanDisappear() {
    const auto beforeBytes = StoredBytes();
    QPointer<MapTestSettingsPage> page(new MapTestSettingsPage(settingsRoot_.path(),
        settingsRoot_.filePath(QStringLiteral("local-data"))));
    const auto cleanup = qScopeGuard([&page] { if (page) delete page.data(); });
    int requested = 0;
    page->SetLegacyBackupDirectoryRequestedHandler([&requested](QString) { ++requested; });
    QObject timers;
    bool dialogObserved = false;
    QTimer::singleShot(0, &timers, [&] {
        auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        dialogObserved = dialog && dialog->objectName() == QStringLiteral("mapLegacyBackupDirectoryDialog");
        delete page.data();
    });
    QTimer::singleShot(2000, &timers, [&] { if (page) delete page.data(); });
    auto* browse = page->findChild<QPushButton*>(QStringLiteral("mapLegacyBackupDirectoryBrowse")); QVERIFY(browse);
    browse->click();
    QVERIFY(dialogObserved); QVERIFY(page.isNull()); QCOMPARE(requested, 0);
    QCOMPARE(StoredBytes(), beforeBytes);
}

QTEST_MAIN(MapTestSettingsTests)
#include "MapTestSettingsTests.moc"
