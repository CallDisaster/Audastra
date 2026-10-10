#include "ApplicationSettingsCommit.h"
#include "ApplicationSettingsTransfer.h"
#include "AgentSettingsDialog.h"
#include "AgentSettingsStore.h"

#include <QApplication>
#include <QDir>
#include <QDataStream>
#include <QFile>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

using namespace War3Map::Agent;

namespace {
int rejectWrites = 0;
bool ReadFormat(QIODevice& device, QSettings::SettingsMap& map) {
    if (device.size() == 0) return true;
    QDataStream stream(&device);
    stream >> map;
    return stream.status() == QDataStream::Ok;
}
bool WriteFormat(QIODevice& device, const QSettings::SettingsMap& map) {
    if (rejectWrites != 0) {
        if (rejectWrites > 0) --rejectWrites;
        return false;
    }
    QDataStream stream(&device);
    stream << map;
    return stream.status() == QDataStream::Ok;
}
QSettings::Format TestFormat() {
    static const auto format = QSettings::registerFormat(QStringLiteral("application-commit-test"), ReadFormat, WriteFormat, Qt::CaseSensitive);
    return format;
}
bool Synchronize(QSettings& settings, ApplicationSettingsSyncStage, QString* error) {
    settings.sync();
    if (settings.status() == QSettings::NoError) return true;
    if (error) *error = QStringLiteral("原创测试后端同步失败");
    return false;
}
QByteArray Read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
bool Write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
AgentSettings Candidate() {
    AgentSettings value;
    value.aiEnabled = true;
    // 未改的普通AI/MCP/权限字段也须从本机完整candidate保留。
    value.mcpEnabled = true;
    value.mcpPort = 19876;
    value.allowCommands = true;
    value.disabledTools = {QStringLiteral("fixture-hidden-tool")};
    ProviderSettings provider;
    provider.id = QStringLiteral("fixture-provider");
    provider.name = QStringLiteral("原创提供商");
    provider.baseUrl = QStringLiteral("https://fixture.invalid/v1");
    provider.credentialRef = QStringLiteral("fixture-existing-reference");
    value.providers.push_back(provider);
    return value;
}
ApplicationPreferences Preferences() {
    return {QStringLiteral("D:/fixture/game"), QStringLiteral("D:/fixture/ui"),
        QStringLiteral("原创字体"), QStringLiteral("light")};
}
void Seed(QSettings& preferences) {
    preferences.setFallbacksEnabled(false);
    preferences.setValue(QStringLiteral("war3/gameRoot"), QStringLiteral("old-game"));
    // dataRoot/theme原不存在；font保留非QString位型证明失败时原存在状态和值一起恢复。
    preferences.setValue(QStringLiteral("ui/fontFamily"), 27);
    preferences.setValue(QStringLiteral("mcp/port"), 23456);
    preferences.setValue(QStringLiteral("window/geometry"), QByteArray("fixture-window"));
    preferences.sync();
}
const QByteArray originalJson("{\r\n  \"original-opaque\": [3,2,1],\r\n  \"spacing\":true\r\n}\r\n");
} // namespace

class ApplicationSettingsCommitTests final : public QObject {
    Q_OBJECT
private slots:
    void init() { rejectWrites = 0; }
    void cleanup() { rejectWrites = 0; }
    void temporaryIniSuccessPreservesOtherSettings();
    void invalidAgentAndPreferencesMakeNoChanges();
    void preferencesWriteFailureNeverSavesAi();
    void lastAiSaveFailureRestoresPreferencesAndOriginalBytes();
    void lastAiIoFailureRestoresPreferences();
    void originallyMissingJsonRemainsMissingAfterFailure();
    void persistentRecoveryFailureIsReported();
    void existingStickyErrorStopsBeforeAiSave();
    void concurrentChangesArePreserved();
    void concurrentAiChangeBeforeFinalCommitIsNotOverwritten();
    void concurrentPreferencesAfterSyncDoNotTurnAiSuccessIntoFailure();
    void absentKeyWithConcurrentChildrenIsNotRemoved();
    void unchangedPreferencesAreNotWritten();
    void dialogCancelHasNoSideEffects();
    void localFeedbackPreferencesUseTheExistingCommit();
    void failedFinalSaveRestoresLocalFeedbackPreferences();
    void invalidFeedbackEndpointMakesNoChanges();
    void localUpdatePreferencesCommitAndRollbackTogether();
    void mapTestAndBackupPreferencesCommitOrRollback();
    void recoveryPreferencesCommitOrRollback();
    void invalidRecoveryIntervalMakesNoChanges();
    void failedFinalSaveRestoresRecoveryTypesAndAbsentKeys();
    void recoveryRollbackPreservesConcurrentInterval();
    void worldEffectsCommitAndRollback();
};

void ApplicationSettingsCommitTests::temporaryIniSuccessPreservesOtherSettings() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto ini = directory.filePath(QStringLiteral("application.ini"));
    QSettings preferences(ini, QSettings::IniFormat);
    Seed(preferences);
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    const auto credentialCanary = directory.filePath(QStringLiteral("credentials.fixture"));
    QVERIFY(Write(credentialCanary, QByteArray("unrelated-original-credential-fixture")));
    const auto candidate = Candidate();
    QString error;
    QVERIFY2(ApplicationSettingsCommit::Commit(preferences, store, candidate, Preferences(), &error), qPrintable(error));
    QVERIFY(error.isEmpty());
    QSettings reopened(ini, QSettings::IniFormat);
    reopened.sync();
    QCOMPARE(reopened.status(), QSettings::NoError);
    QCOMPARE(reopened.value(QStringLiteral("war3/gameRoot")).toString(), Preferences().gameRoot);
    QCOMPARE(reopened.value(QStringLiteral("ui/dataRoot")).toString(), Preferences().uiDataRoot);
    QCOMPARE(reopened.value(QStringLiteral("ui/fontFamily")).toString(), Preferences().uiFontFamily);
    QCOMPARE(reopened.value(QStringLiteral("ui/theme")).toString(), Preferences().themeId);
    QCOMPARE(reopened.value(QStringLiteral("mcp/port")).toInt(), 23456);
    QCOMPARE(reopened.value(QStringLiteral("window/geometry")).toByteArray(), QByteArray("fixture-window"));
    const auto loaded = store.Load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(loaded.aiEnabled && loaded.mcpEnabled && loaded.allowCommands);
    QCOMPARE(loaded.mcpPort, candidate.mcpPort);
    QCOMPARE(loaded.disabledTools, candidate.disabledTools);
    QCOMPARE(loaded.providers.front().credentialRef, candidate.providers.front().credentialRef);
    QCOMPARE(Read(credentialCanary), QByteArray("unrelated-original-credential-fixture"));
}

void ApplicationSettingsCommitTests::invalidAgentAndPreferencesMakeNoChanges() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat);
    Seed(preferences);
    const auto oldIni = Read(preferences.fileName());
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    auto candidate = Candidate();
    candidate.mcpPort = 0;
    QString error;
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, candidate, Preferences(), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(Read(store.FilePath()), originalJson);
    QCOMPARE(Read(preferences.fileName()), oldIni);
    auto invalid = Preferences();
    invalid.uiDataRoot += QChar('\n');
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), invalid, &error));
    QCOMPARE(Read(store.FilePath()), originalJson);
    QCOMPARE(Read(preferences.fileName()), oldIni);
}

void ApplicationSettingsCommitTests::preferencesWriteFailureNeverSavesAi() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("preferences.fixture"));
    QSettings preferences(path, TestFormat());
    Seed(preferences);
    QCOMPARE(preferences.status(), QSettings::NoError);
    const auto oldPreferences = Read(path);
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    bool aiWasUntouched = false;
    const ApplicationSettingsSync backend = [&](QSettings& current, ApplicationSettingsSyncStage stage, QString* error) {
        if (stage == ApplicationSettingsSyncStage::Write) {
            aiWasUntouched = Read(store.FilePath()) == originalJson;
            rejectWrites = 1;
        }
        return Synchronize(current, stage, error);
    };
    QString error;
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, backend));
    QVERIFY(aiWasUntouched);
    QVERIFY(error.contains(QStringLiteral("已恢复")));
    QCOMPARE(Read(store.FilePath()), originalJson);
    QCOMPARE(Read(path), oldPreferences);
    // 确认真实custom WriteFunc失败使原对象status sticky，新对象能确认原数据已持久化。
    QCOMPARE(preferences.status(), QSettings::AccessError);
    QSettings reopened(path, TestFormat());
    reopened.sync();
    QCOMPARE(reopened.status(), QSettings::NoError);
    QCOMPARE(reopened.value(QStringLiteral("war3/gameRoot")).toString(), QStringLiteral("old-game"));
    QCOMPARE(reopened.value(QStringLiteral("ui/fontFamily")), QVariant(27));
    QVERIFY(!reopened.contains(QStringLiteral("ui/dataRoot")));
    QVERIFY(!reopened.contains(QStringLiteral("ui/theme")));
    QCOMPARE(reopened.value(QStringLiteral("mcp/port")).toInt(), 23456);
}

void ApplicationSettingsCommitTests::lastAiSaveFailureRestoresPreferencesAndOriginalBytes() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("preferences.fixture"));
    QSettings preferences(path, TestFormat());
    Seed(preferences);
    const auto oldPreferences = Read(path);
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    auto oversized = Candidate();
    oversized.providers.clear();
    // 原Validate逐字段合法，但真实Save的最终JSON超过2MiB；没有用假的AI writer代替Save。
    for (int index = 0; index < 128; ++index) {
        ProviderSettings provider;
        provider.id = QStringLiteral("fixture-%1").arg(index);
        provider.name = QString(16384, QChar('x'));
        oversized.providers.push_back(provider);
    }
    QString error;
    QVERIFY2(AgentSettingsStore::Validate(oversized, &error), qPrintable(error));
    bool preferencesWereWritten = false;
    const ApplicationSettingsSync backend = [&](QSettings& current, ApplicationSettingsSyncStage stage, QString* diagnostic) {
        if (!Synchronize(current, stage, diagnostic)) return false;
        if (stage == ApplicationSettingsSyncStage::Write) {
            preferencesWereWritten = current.value(QStringLiteral("ui/dataRoot")).toString() == Preferences().uiDataRoot;
            if (Read(store.FilePath()) != originalJson) return false;
        }
        return true;
    };
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, oversized, Preferences(), &error, backend));
    QVERIFY(preferencesWereWritten);
    QVERIFY(error.contains(QStringLiteral("2 MiB")));
    QVERIFY(error.contains(QStringLiteral("已恢复")));
    QCOMPARE(Read(store.FilePath()), originalJson);
    QCOMPARE(Read(path), oldPreferences);
    QVERIFY(!preferences.contains(QStringLiteral("ui/dataRoot")));
    QVERIFY(!preferences.contains(QStringLiteral("ui/theme")));
    QCOMPARE(preferences.value(QStringLiteral("ui/fontFamily")), QVariant(27));
}

void ApplicationSettingsCommitTests::lastAiIoFailureRestoresPreferences() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat);
    Seed(preferences);
    const auto oldPreferences = Read(preferences.fileName());
    const auto blockingParent = directory.filePath(QStringLiteral("parent-is-a-file"));
    QVERIFY(Write(blockingParent, QByteArray("original-parent-fixture")));
    AgentSettingsStore store(blockingParent + QStringLiteral("/settings.json"));
    QString error;
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error));
    QVERIFY(error.contains(QStringLiteral("无法创建")));
    QVERIFY(error.contains(QStringLiteral("已恢复")));
    QVERIFY(!QFile::exists(store.FilePath()));
    QCOMPARE(Read(blockingParent), QByteArray("original-parent-fixture"));
    QCOMPARE(Read(preferences.fileName()), oldPreferences);
}

void ApplicationSettingsCommitTests::originallyMissingJsonRemainsMissingAfterFailure() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("preferences.fixture")), TestFormat());
    Seed(preferences);
    AgentSettingsStore store(directory.filePath(QStringLiteral("originally-missing.json")));
    const ApplicationSettingsSync backend = [](QSettings& current, ApplicationSettingsSyncStage stage, QString* error) {
        if (stage == ApplicationSettingsSyncStage::Write) rejectWrites = 1;
        return Synchronize(current, stage, error);
    };
    QString error;
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, backend));
    QVERIFY(!QFile::exists(store.FilePath()));
    QVERIFY(error.contains(QStringLiteral("已恢复")));
}

void ApplicationSettingsCommitTests::persistentRecoveryFailureIsReported() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("preferences.fixture")), TestFormat());
    Seed(preferences);
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    const ApplicationSettingsSync backend = [](QSettings& current, ApplicationSettingsSyncStage stage, QString* error) {
        if (stage == ApplicationSettingsSyncStage::Write) rejectWrites = -1;
        return Synchronize(current, stage, error);
    };
    QString error;
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, backend));
    QVERIFY(error.contains(QStringLiteral("恢复未确认持久化")));
    QVERIFY(!error.contains(QStringLiteral("本次应用偏好更改已恢复")));
    QCOMPARE(Read(store.FilePath()), originalJson);
    rejectWrites = 0;
    preferences.sync();
}

void ApplicationSettingsCommitTests::existingStickyErrorStopsBeforeAiSave() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("preferences.fixture")), TestFormat());
    Seed(preferences);
    preferences.setValue(QStringLiteral("fixture/pending"), 1);
    rejectWrites = 1;
    preferences.sync();
    QCOMPARE(preferences.status(), QSettings::AccessError);
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    QString error;
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error));
    QCOMPARE(Read(store.FilePath()), originalJson);
    QVERIFY(!error.isEmpty());
}

void ApplicationSettingsCommitTests::concurrentChangesArePreserved() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("application.ini"));
    QSettings preferences(path, QSettings::IniFormat);
    Seed(preferences);
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    const QByteArray externalJson("{\"external-fixture-writer\":true}\n");
    const ApplicationSettingsSync backend = [&](QSettings& current, ApplicationSettingsSyncStage stage, QString* error) {
        if (!Synchronize(current, stage, error)) return false;
        if (stage != ApplicationSettingsSyncStage::Write) return true;
        QSettings external(path, QSettings::IniFormat);
        external.setValue(QStringLiteral("ui/fontFamily"), QStringLiteral("external-font"));
        external.setValue(QStringLiteral("external/retained"), 89);
        external.sync();
        if (!Write(store.FilePath(), externalJson)) return false;
        if (error) *error = QStringLiteral("模拟偏好写入结果不确定");
        return false;
    };
    QString error;
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, backend));
    QVERIFY(error.contains(QStringLiteral("并发更改")));
    QCOMPARE(Read(store.FilePath()), externalJson);
    QSettings reopened(path, QSettings::IniFormat);
    reopened.sync();
    QCOMPARE(reopened.value(QStringLiteral("ui/fontFamily")).toString(), QStringLiteral("external-font"));
    QCOMPARE(reopened.value(QStringLiteral("war3/gameRoot")).toString(), QStringLiteral("old-game"));
    QVERIFY(!reopened.contains(QStringLiteral("ui/dataRoot")));
    QVERIFY(!reopened.contains(QStringLiteral("ui/theme")));
    QCOMPARE(reopened.value(QStringLiteral("external/retained")).toInt(), 89);
    QCOMPARE(reopened.value(QStringLiteral("mcp/port")).toInt(), 23456);
}

void ApplicationSettingsCommitTests::concurrentPreferencesAfterSyncDoNotTurnAiSuccessIntoFailure() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("application.ini"));
    QSettings preferences(path, QSettings::IniFormat);
    Seed(preferences);
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    auto candidate = Candidate();
    candidate.providers.front().credentialRef = QStringLiteral("fixture-new-reference");
    const ApplicationSettingsSync backend = [&](QSettings& current, ApplicationSettingsSyncStage stage, QString* error) {
        if (!Synchronize(current, stage, error)) return false;
        if (stage == ApplicationSettingsSyncStage::Write) {
            QSettings external(path, QSettings::IniFormat);
            external.setValue(QStringLiteral("ui/fontFamily"), QStringLiteral("external-after-sync"));
            external.sync();
            if (external.status() != QSettings::NoError) return false;
        }
        return true;
    };
    QString error;
    QVERIFY2(ApplicationSettingsCommit::Commit(preferences, store, candidate, Preferences(), &error, backend), qPrintable(error));
    QVERIFY(error.isEmpty());
    const auto loaded = store.Load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.providers.front().credentialRef, QStringLiteral("fixture-new-reference"));
    QSettings reopened(path, QSettings::IniFormat);
    QCOMPARE(reopened.value(QStringLiteral("ui/fontFamily")).toString(), QStringLiteral("external-after-sync"));
}

void ApplicationSettingsCommitTests::concurrentAiChangeBeforeFinalCommitIsNotOverwritten() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat);
    Seed(preferences);
    const auto oldPreferences = Read(preferences.fileName());
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    const QByteArray external("{\"external-ai-change\":true}\n");
    const ApplicationSettingsSync backend = [&](QSettings& current, ApplicationSettingsSyncStage stage, QString* error) {
        if (!Synchronize(current, stage, error)) return false;
        return stage != ApplicationSettingsSyncStage::Write || Write(store.FilePath(), external);
    };
    auto candidate = Candidate();
    candidate.providers.front().credentialRef = QStringLiteral("fixture-new-reference");
    QString error;
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, candidate, Preferences(), &error, backend));
    QVERIFY(error.contains(QStringLiteral("并发更改")));
    QCOMPARE(Read(store.FilePath()), external);
    QVERIFY(!Read(store.FilePath()).contains("fixture-new-reference"));
    QCOMPARE(Read(preferences.fileName()), oldPreferences);
}

void ApplicationSettingsCommitTests::absentKeyWithConcurrentChildrenIsNotRemoved() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("application.ini"));
    QSettings preferences(path, QSettings::IniFormat);
    Seed(preferences);
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    const ApplicationSettingsSync backend = [&](QSettings& current, ApplicationSettingsSyncStage stage, QString* error) {
        if (!Synchronize(current, stage, error)) return false;
        if (stage != ApplicationSettingsSyncStage::Write) return true;
        QSettings external(path, QSettings::IniFormat);
        external.setValue(QStringLiteral("ui/dataRoot/external-child"), 71);
        external.sync();
        if (error) *error = QStringLiteral("模拟已写后的第二步失败");
        return false;
    };
    QString error;
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, backend));
    QVERIFY(error.contains(QStringLiteral("并发子键")));
    QSettings reopened(path, QSettings::IniFormat);
    reopened.sync();
    QCOMPARE(reopened.value(QStringLiteral("ui/dataRoot/external-child")).toInt(), 71);
    QCOMPARE(Read(store.FilePath()), originalJson);
}

void ApplicationSettingsCommitTests::unchangedPreferencesAreNotWritten() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat);
    const ApplicationPreferences application;
    preferences.setValue(QStringLiteral("mcp/port"), 24680);
    preferences.sync();
    const auto oldIni = Read(preferences.fileName());
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    int writes = 0;
    const ApplicationSettingsSync backend = [&](QSettings& current, ApplicationSettingsSyncStage stage, QString* error) {
        if (stage == ApplicationSettingsSyncStage::Write) ++writes;
        return Synchronize(current, stage, error);
    };
    QString error;
    QVERIFY2(ApplicationSettingsCommit::Commit(preferences, store, Candidate(), application, &error, backend), qPrintable(error));
    QCOMPARE(writes, 0);
    QCOMPARE(Read(preferences.fileName()), oldIni);
    QVERIFY(!preferences.contains(QStringLiteral("ui/theme")));
}

void ApplicationSettingsCommitTests::dialogCancelHasNoSideEffects() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat);
    Seed(preferences);
    const auto oldIni = Read(preferences.fileName());
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    int commits = 0;
    ApplicationServicePreferences services;
    services.mapTestRuntimeRoot = directory.filePath(QStringLiteral("cancelled-runtime"));
    AgentSettingsDialog dialog(AgentSettings{});
    dialog.SetCommitHandler([&](const AgentSettings& candidate, QString* error) {
        ++commits;
        return ApplicationSettingsCommit::Commit(preferences, store, candidate, Preferences(), error, {}, services);
    });
    dialog.reject();
    QCOMPARE(commits, 0);
    QCOMPARE(Read(store.FilePath()), originalJson);
    QCOMPARE(Read(preferences.fileName()), oldIni);
    QVERIFY(!preferences.contains(QStringLiteral("mapTest/slkToolRoot")));
    QVERIFY(!preferences.contains(QStringLiteral("mapTest/runtimeRoot")));
}

void ApplicationSettingsCommitTests::localFeedbackPreferencesUseTheExistingCommit() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat);
    Seed(preferences);
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QString error;
    const ApplicationServicePreferences services{true, QStringLiteral("https://feedback.example.test/report")};
    QVERIFY2(ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, services), qPrintable(error));
    QCOMPARE(preferences.value(QStringLiteral("feedback/consent")), QVariant(true));
    QCOMPARE(preferences.value(QStringLiteral("feedback/endpointOverride")).toString(), *services.feedbackEndpoint);
    QCOMPARE(preferences.value(QStringLiteral("mcp/port")).toInt(), 23456);
    const auto exported = ApplicationSettingsTransfer::Encode(Preferences(), Candidate(), &error);
    QVERIFY(!exported.isEmpty());
    QVERIFY(!exported.contains("feedback"));
    QVERIFY(!Read(store.FilePath()).contains("feedback"));
    const ApplicationServicePreferences disabled{false, QString()};
    QVERIFY2(ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, disabled), qPrintable(error));
    QCOMPARE(preferences.value(QStringLiteral("feedback/consent")), QVariant(false));
    QVERIFY(preferences.value(QStringLiteral("feedback/endpointOverride")).toString().isEmpty());
}

void ApplicationSettingsCommitTests::failedFinalSaveRestoresLocalFeedbackPreferences() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat);
    Seed(preferences);
    preferences.setValue(QStringLiteral("feedback/consent"), QByteArray("original-opaque-value"));
    preferences.sync();
    const auto beforeIni = Read(preferences.fileName());
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    auto oversized = Candidate();
    oversized.providers.clear();
    for (int index = 0; index < 128; ++index) {
        ProviderSettings provider;
        provider.id = QStringLiteral("feedback-fixture-%1").arg(index);
        provider.name = QString(16384, QChar('x'));
        oversized.providers.push_back(provider);
    }
    QString error;
    bool consentActuallyWritten = false;
    const ApplicationSettingsSync backend = [&](QSettings& current, ApplicationSettingsSyncStage stage, QString* diagnostic) {
        if (!Synchronize(current, stage, diagnostic)) return false;
        if (stage == ApplicationSettingsSyncStage::Write)
            consentActuallyWritten = current.value(QStringLiteral("feedback/consent")) == QVariant(true);
        return true;
    };
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, oversized, Preferences(), &error, backend,
        ApplicationServicePreferences{true, QStringLiteral("https://feedback.example.test/report")}));
    QVERIFY(consentActuallyWritten);
    QVERIFY(error.contains(QStringLiteral("已恢复")));
    QCOMPARE(Read(preferences.fileName()), beforeIni);
    QCOMPARE(Read(store.FilePath()), originalJson);
    QCOMPARE(preferences.value(QStringLiteral("feedback/consent")), QVariant(QByteArray("original-opaque-value")));
    QVERIFY(!preferences.contains(QStringLiteral("feedback/endpointOverride")));
}

void ApplicationSettingsCommitTests::invalidFeedbackEndpointMakesNoChanges() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat);
    Seed(preferences);
    const auto beforeIni = Read(preferences.fileName());
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    for (const auto& endpoint : {QStringLiteral("http://feedback.example.test/report"),
        QStringLiteral("https://name:password@feedback.example.test/report"),
        QStringLiteral("https://@feedback.example.test/report"),
        QStringLiteral("https://feedback.example.test:0/report"),
        QStringLiteral("https://feedback.example.test/report?token=fixture"),
        QStringLiteral("https://feedback.example.test/report#fixture"), QStringLiteral("https://feedback.example.test/ bad")}) {
        QString error;
        QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {},
            ApplicationServicePreferences{true, endpoint}));
        QCOMPARE(Read(preferences.fileName()), beforeIni);
        QCOMPARE(Read(store.FilePath()), originalJson);
        QVERIFY(!preferences.contains(QStringLiteral("feedback/consent")));
    }
}

void ApplicationSettingsCommitTests::localUpdatePreferencesCommitAndRollbackTogether() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat);
    Seed(preferences);
    preferences.setValue(QStringLiteral("update/channel"), QByteArray("original-channel-type"));
    preferences.setValue(QStringLiteral("update/lastAutomaticCheckDate"), QStringLiteral("2026-10-04"));
    preferences.sync();
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    const auto originalIni = Read(preferences.fileName());
    ApplicationServicePreferences services;
    services.updateAutomatic = false;
    services.updateChannel = QStringLiteral("beta");
    services.updateSkippedVersion = QStringLiteral("0.2.0-beta.1");
    QString error;
    // 真实最终JSON保存拒绝，恢复本机服务键的原值/位型及不存在状态。
    auto oversized = Candidate();
    oversized.providers.clear();
    for (int index = 0; index < 128; ++index) {
        ProviderSettings provider;
        provider.id = QStringLiteral("update-fixture-%1").arg(index);
        provider.name = QString(16384, QChar('x'));
        oversized.providers.push_back(provider);
    }
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, oversized, Preferences(), &error, {}, services));
    QCOMPARE(Read(preferences.fileName()), originalIni);
    QCOMPARE(Read(store.FilePath()), originalJson);
    QVERIFY(!preferences.contains(QStringLiteral("update/automaticEnabled")));
    QVERIFY(!preferences.contains(QStringLiteral("update/skippedVersion")));
    QCOMPARE(preferences.value(QStringLiteral("update/channel")), QVariant(QByteArray("original-channel-type")));
    services.updateSkippedVersion = QStringLiteral("invalid-version");
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, services));
    QCOMPARE(Read(preferences.fileName()), originalIni);
    services.updateSkippedVersion = QStringLiteral("0.2.0-beta.1");
    QVERIFY2(ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, services), qPrintable(error));
    QCOMPARE(preferences.value(QStringLiteral("update/automaticEnabled")), QVariant(false));
    QCOMPARE(preferences.value(QStringLiteral("update/channel")), QVariant(QStringLiteral("beta")));
    QCOMPARE(preferences.value(QStringLiteral("update/skippedVersion")), QVariant(QStringLiteral("0.2.0-beta.1")));
    QCOMPARE(preferences.value(QStringLiteral("update/lastAutomaticCheckDate")), QVariant(QStringLiteral("2026-10-04")));
    QVERIFY(!Read(store.FilePath()).contains("update/"));
}

void ApplicationSettingsCommitTests::mapTestAndBackupPreferencesCommitOrRollback() {
    QTemporaryDir directory;
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat);
    Seed(preferences);
    preferences.setValue(QStringLiteral("mapSave/backupLimit"), 27);
    const auto originalSlkTool = directory.filePath(QStringLiteral("old-slk-tool"));
    const auto legacyYdwe = directory.filePath(QStringLiteral("legacy-ydwe"));
    preferences.setValue(QStringLiteral("mapTest/slkToolRoot"), originalSlkTool);
    preferences.setValue(QStringLiteral("mapTest/ydweRoot"), legacyYdwe);
    preferences.sync();
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    const auto originalIni = Read(preferences.fileName());
    ApplicationServicePreferences services;
    services.mapBackupDirectory = directory.filePath(QStringLiteral("new-backups"));
    services.mapBackupLimit = 0;
    services.mapTestRuntimeRoot = directory.filePath(QStringLiteral("runtime"));
    services.mapTestSlk = true;
    QString error;
    const auto failFinal = [](QSettings& current, ApplicationSettingsSyncStage stage, QString* diagnostic) {
        if (!Synchronize(current, stage, diagnostic)) return false;
        if (stage != ApplicationSettingsSyncStage::Write) return true;
        if (diagnostic) *diagnostic = QStringLiteral("模拟地图测试设置同步后的失败");
        return false;
    };
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, failFinal, services));
    QCOMPARE(Read(preferences.fileName()), originalIni);
    QCOMPARE(Read(store.FilePath()), originalJson);
    QVERIFY(!preferences.contains(QStringLiteral("mapTest/slkEnabled")));
    QCOMPARE(preferences.value(QStringLiteral("mapTest/slkToolRoot")).toString(), originalSlkTool);
    QVERIFY(!preferences.contains(QStringLiteral("mapTest/runtimeRoot")));
    QCOMPARE(preferences.value(QStringLiteral("mapTest/ydweRoot")).toString(), legacyYdwe);
    QCOMPARE(preferences.value(QStringLiteral("mapSave/backupLimit")).toInt(), 27);
    services.mapBackupLimit = 1001;
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, services));
    QCOMPARE(Read(preferences.fileName()), originalIni);
    services.mapBackupLimit = 0;
    services.mapBackupDirectory = QStringLiteral("relative/backups");
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, services));
    QCOMPARE(Read(preferences.fileName()), originalIni);
    services.mapBackupDirectory = directory.filePath(QStringLiteral("new-backups"));
    for (auto* path : {&services.mapTestRuntimeRoot}) {
        const auto valid = **path;
        const QStringList invalidDirectories{QStringLiteral("relative/tool"), QString(4097, QChar(u'a')),
            directory.filePath(QStringLiteral("control")) + QChar(char16_t(0x1f))};
        for (const auto& invalid : invalidDirectories) {
            *path = invalid;
            QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, services));
            QCOMPARE(Read(preferences.fileName()), originalIni);
            QCOMPARE(Read(store.FilePath()), originalJson);
        }
        *path = valid;
    }
    QVERIFY2(ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, services), qPrintable(error));
    QCOMPARE(preferences.value(QStringLiteral("mapSave/backupDirectory")).toString(), *services.mapBackupDirectory);
    QCOMPARE(preferences.value(QStringLiteral("mapSave/backupLimit")).toInt(), 0);
    QCOMPARE(preferences.value(QStringLiteral("mapTest/slkToolRoot")).toString(), originalSlkTool);
    QCOMPARE(preferences.value(QStringLiteral("mapTest/runtimeRoot")).toString(), *services.mapTestRuntimeRoot);
    QCOMPARE(preferences.value(QStringLiteral("mapTest/ydweRoot")).toString(), legacyYdwe);
    QCOMPARE(preferences.value(QStringLiteral("mapTest/slkEnabled")).toBool(), true);
    QVERIFY(!QDir(*services.mapBackupDirectory).exists());
    QVERIFY(!QDir(*services.mapTestRuntimeRoot).exists());
    QVERIFY(!Read(store.FilePath()).contains("mapSave/"));
    QVERIFY(!Read(store.FilePath()).contains("mapTest/"));
    services.mapTestRuntimeRoot = QString();
    services.mapTestSlk = false;
    QVERIFY2(ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, services), qPrintable(error));
    QCOMPARE(preferences.value(QStringLiteral("mapTest/slkToolRoot")).toString(), originalSlkTool);
    QVERIFY(preferences.value(QStringLiteral("mapTest/runtimeRoot")).toString().isEmpty());
    QVERIFY(!preferences.value(QStringLiteral("mapTest/slkEnabled")).toBool());
    QCOMPARE(preferences.value(QStringLiteral("mapTest/ydweRoot")).toString(), legacyYdwe);
}


void ApplicationSettingsCommitTests::recoveryPreferencesCommitOrRollback() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat);
    Seed(preferences);
    preferences.setValue(QStringLiteral("project/autoRecoveryEnabled"), QByteArray("old-enabled-type"));
    preferences.setValue(QStringLiteral("project/latestRecovery"), QStringLiteral("old-session.forge"));
    preferences.sync();
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    const auto originalIni = Read(preferences.fileName());
    ApplicationServicePreferences services; services.recoveryEnabled = false; services.recoveryIntervalMinutes = 120;
    const ApplicationSettingsSync failAfterWrite = [](QSettings& current, ApplicationSettingsSyncStage stage, QString* error) {
        if (!Synchronize(current, stage, error)) return false;
        if (stage != ApplicationSettingsSyncStage::Write) return true;
        if (error) *error = QStringLiteral("原创恢复设置写入后的失败");
        return false;
    };
    QString error;
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, failAfterWrite, services));
    QCOMPARE(Read(preferences.fileName()), originalIni); QCOMPARE(Read(store.FilePath()), originalJson);
    QSettings restored(preferences.fileName(), QSettings::IniFormat); restored.sync();
    QCOMPARE(restored.value(QStringLiteral("project/autoRecoveryEnabled")), QVariant(QByteArray("old-enabled-type")));
    QVERIFY(!restored.contains(QStringLiteral("project/autoRecoveryIntervalMinutes")));
    QVERIFY2(ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, services), qPrintable(error));
    QSettings committed(preferences.fileName(), QSettings::IniFormat); committed.sync();
    QCOMPARE(committed.value(QStringLiteral("project/autoRecoveryEnabled")), QVariant(false));
    QCOMPARE(committed.value(QStringLiteral("project/autoRecoveryIntervalMinutes")).toInt(), 120);
    QCOMPARE(committed.value(QStringLiteral("project/latestRecovery")).toString(), QStringLiteral("old-session.forge"));
    QCOMPARE(committed.value(QStringLiteral("mcp/port")).toInt(), 23456);
    const auto committedIni = Read(preferences.fileName());
    QVERIFY2(ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error), qPrintable(error));
    QCOMPARE(Read(preferences.fileName()), committedIni); // 未提供 optional 服务字段，不覆盖已有恢复设置。
    QVERIFY(!Read(store.FilePath()).contains("autoRecovery"));
}

void ApplicationSettingsCommitTests::invalidRecoveryIntervalMakesNoChanges() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat); Seed(preferences);
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json"))); QVERIFY(Write(store.FilePath(), originalJson));
    const auto originalIni = Read(preferences.fileName());
    ApplicationServicePreferences services; services.recoveryEnabled = false;
    QString error;
    for (const int invalid : {-1, 0, 121}) {
        services.recoveryIntervalMinutes = invalid;
        QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, services));
        QVERIFY(error.contains(QStringLiteral("1到120")));
        QCOMPARE(Read(preferences.fileName()), originalIni); QCOMPARE(Read(store.FilePath()), originalJson);
        QVERIFY(!preferences.contains(QStringLiteral("project/autoRecoveryEnabled")));
        QVERIFY(!preferences.contains(QStringLiteral("project/autoRecoveryIntervalMinutes")));
    }
}

void ApplicationSettingsCommitTests::failedFinalSaveRestoresRecoveryTypesAndAbsentKeys() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings preferences(directory.filePath(QStringLiteral("application.ini")), QSettings::IniFormat); Seed(preferences);
    preferences.setValue(QStringLiteral("project/autoRecoveryIntervalMinutes"), QByteArray("17")); preferences.sync();
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json"))); QVERIFY(Write(store.FilePath(), originalJson));
    const auto originalIni = Read(preferences.fileName());
    auto oversized = Candidate(); oversized.providers.clear();
    for (int index = 0; index < 128; ++index) {
        ProviderSettings provider; provider.id = QStringLiteral("recovery-fixture-%1").arg(index);
        provider.name = QString(16384, QChar('x')); oversized.providers.push_back(provider);
    }
    ApplicationServicePreferences services; services.recoveryEnabled = true; services.recoveryIntervalMinutes = 1;
    QString error;
    bool written = false, rolledBack = false;
    const ApplicationSettingsSync observeStages = [&](QSettings& current, ApplicationSettingsSyncStage stage, QString* diagnostic) {
        written |= stage == ApplicationSettingsSyncStage::Write;
        rolledBack |= stage == ApplicationSettingsSyncStage::Rollback;
        return Synchronize(current, stage, diagnostic);
    };
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, oversized, Preferences(), &error, observeStages, services));
    QVERIFY(written); QVERIFY(rolledBack);
    QCOMPARE(Read(preferences.fileName()), originalIni); QCOMPARE(Read(store.FilePath()), originalJson);
    QSettings restored(preferences.fileName(), QSettings::IniFormat); restored.sync();
    QVERIFY(!restored.contains(QStringLiteral("project/autoRecoveryEnabled")));
    QCOMPARE(restored.value(QStringLiteral("project/autoRecoveryIntervalMinutes")), QVariant(QByteArray("17")));
}

void ApplicationSettingsCommitTests::recoveryRollbackPreservesConcurrentInterval() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("application.ini"));
    QSettings preferences(path, QSettings::IniFormat); Seed(preferences);
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json"))); QVERIFY(Write(store.FilePath(), originalJson));
    ApplicationServicePreferences services; services.recoveryEnabled = false; services.recoveryIntervalMinutes = 1;
    const ApplicationSettingsSync backend = [&](QSettings& current, ApplicationSettingsSyncStage stage, QString* error) {
        if (!Synchronize(current, stage, error)) return false;
        if (stage != ApplicationSettingsSyncStage::Write) return true;
        QSettings external(path, QSettings::IniFormat);
        external.setValue(QStringLiteral("project/autoRecoveryIntervalMinutes"), 119); external.sync();
        if (error) *error = QStringLiteral("原创恢复设置并发修改");
        return false;
    };
    QString error;
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, backend, services));
    QVERIFY(error.contains(QStringLiteral("并发更改")));
    QSettings reopened(path, QSettings::IniFormat); reopened.sync();
    QCOMPARE(reopened.value(QStringLiteral("project/autoRecoveryIntervalMinutes")).toInt(), 119);
    QVERIFY(!reopened.contains(QStringLiteral("project/autoRecoveryEnabled")));
    QCOMPARE(reopened.value(QStringLiteral("mcp/port")).toInt(), 23456);
    QCOMPARE(Read(store.FilePath()), originalJson);
}

void ApplicationSettingsCommitTests::worldEffectsCommitAndRollback() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto ini = directory.filePath(QStringLiteral("effects.ini"));
    QSettings preferences(ini, QSettings::IniFormat);
    preferences.setFallbacksEnabled(false);
    preferences.setValue(QStringLiteral("world/effectQuality"), QStringLiteral("power-saving"));
    preferences.setValue(QStringLiteral("world/unrelated"), 17);
    preferences.sync();
    AgentSettingsStore store(directory.filePath(QStringLiteral("ai.json")));
    QVERIFY(Write(store.FilePath(), originalJson));
    ApplicationServicePreferences services;
    services.worldEffectQuality = QStringLiteral("complete");
    QString error;
    const auto failWrite = [](QSettings& settings, ApplicationSettingsSyncStage stage, QString* message) {
        settings.sync();
        if (stage == ApplicationSettingsSyncStage::Write) {
            if (message) *message = QStringLiteral("原创写入失败");
            return false;
        }
        return settings.status() == QSettings::NoError;
    };
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, failWrite, services));
    QCOMPARE(preferences.value(QStringLiteral("world/effectQuality")).toString(), QStringLiteral("power-saving"));
    QCOMPARE(Read(store.FilePath()), originalJson);
    services.worldEffectQuality = QStringLiteral("unknown");
    QVERIFY(!ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, services));
    QCOMPARE(preferences.value(QStringLiteral("world/effectQuality")).toString(), QStringLiteral("power-saving"));
    for (const auto& name : {QStringLiteral("complete"), QStringLiteral("balanced"), QStringLiteral("power-saving")}) {
        services.worldEffectQuality = name;
        QVERIFY2(ApplicationSettingsCommit::Commit(preferences, store, Candidate(), Preferences(), &error, {}, services), qPrintable(error));
        QSettings actual(ini, QSettings::IniFormat);
        actual.setFallbacksEnabled(false);
        QCOMPARE(actual.value(QStringLiteral("world/effectQuality")).toString(), name);
        QCOMPARE(actual.value(QStringLiteral("world/unrelated")).toInt(), 17);
    }
}

QTEST_MAIN(ApplicationSettingsCommitTests)
#include "ApplicationSettingsCommitTests.moc"
