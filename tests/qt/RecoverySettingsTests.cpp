#include "RecoverySettingsPage.h"
#include "RecoveryPreferences.h"

#include <QCheckBox>
#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

using War3Map::QtApp::RecoverySettingsPage;
using War3Map::Agent::LoadRecoveryPreferences;

namespace {
QByteArray ReadRecoveryIni(const QString& path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
}

class RecoverySettingsTests final : public QObject {
    Q_OBJECT
private slots:
    void DefaultsAndDraftDoNotWrite() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath(QStringLiteral("never-written.ini"));
        QSettings stored(path, QSettings::IniFormat); stored.setFallbacksEnabled(false);
        {
            RecoverySettingsPage page(stored);
            auto* enabled = page.findChild<QCheckBox*>(QStringLiteral("recoveryEnabled"));
            auto* interval = page.findChild<QSpinBox*>(QStringLiteral("recoveryIntervalMinutes"));
            QVERIFY(enabled); QVERIFY(interval); QVERIFY(enabled->isChecked()); QVERIFY(interval->isEnabled());
            QCOMPARE(interval->minimum(), 1); QCOMPARE(interval->maximum(), 120); QCOMPARE(interval->value(), 5);
            // 恢复说明写在使用帮助里，设置页不放说明文字。
            QVERIFY(!page.findChild<QLabel*>(QStringLiteral("autoRecoveryNote")));
            auto preferences = page.Preferences(); QVERIFY(preferences.recoveryEnabled.has_value());
            QVERIFY(*preferences.recoveryEnabled); QCOMPARE(*preferences.recoveryIntervalMinutes, 5);
            enabled->setChecked(false); QVERIFY(!interval->isEnabled()); interval->setValue(120);
            preferences = page.Preferences(); QVERIFY(!*preferences.recoveryEnabled); QCOMPARE(*preferences.recoveryIntervalMinutes, 120);
            QString error = QStringLiteral("旧错误"); QVERIFY(page.Validate(&error)); QVERIFY(error.isEmpty());
            QVERIFY(stored.allKeys().isEmpty()); QVERIFY(!QFile::exists(path));
        }
        stored.sync(); QVERIFY(stored.allKeys().isEmpty()); QVERIFY(!QFile::exists(path));
    }
    void StoredValuesAndCancellationPreserveBytes() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QSettings stored(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat); stored.setFallbacksEnabled(false);
        stored.setValue(QStringLiteral("project/autoRecoveryEnabled"), false);
        stored.setValue(QStringLiteral("project/autoRecoveryIntervalMinutes"), 120);
        stored.setValue(QStringLiteral("project/latestRecovery"), QStringLiteral("existing.forge")); stored.sync();
        const auto bytes = ReadRecoveryIni(stored.fileName());
        {
            RecoverySettingsPage page(stored);
            auto* enabled = page.findChild<QCheckBox*>(QStringLiteral("recoveryEnabled"));
            auto* interval = page.findChild<QSpinBox*>(QStringLiteral("recoveryIntervalMinutes"));
            QVERIFY(enabled); QVERIFY(interval); QVERIFY(!enabled->isChecked()); QVERIFY(!interval->isEnabled()); QCOMPARE(interval->value(), 120);
            enabled->setChecked(true); QVERIFY(interval->isEnabled()); interval->setValue(1);
            QVERIFY(*page.Preferences().recoveryEnabled); QCOMPARE(*page.Preferences().recoveryIntervalMinutes, 1);
        }
        stored.sync(); QCOMPARE(ReadRecoveryIni(stored.fileName()), bytes);
        const auto current = LoadRecoveryPreferences(stored); QVERIFY(!current.enabled); QCOMPARE(current.intervalMinutes, 120);
        QCOMPARE(stored.value(QStringLiteral("project/latestRecovery")).toString(), QStringLiteral("existing.forge"));
    }
    void FreshProcessReloadPreservesDisabledPreference() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath(QStringLiteral("restart.ini"));
        {
            QSettings stored(path, QSettings::IniFormat); stored.setFallbacksEnabled(false);
            stored.setValue(QString::fromLatin1(War3Map::Agent::kRecoveryEnabledSettingsKey), false);
            stored.setValue(QString::fromLatin1(War3Map::Agent::kRecoveryIntervalSettingsKey), 120);
            stored.sync(); QCOMPARE(stored.status(), QSettings::NoError);
        }
        const auto bytes = ReadRecoveryIni(path);
        QProcess reader;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("AUD_RECOVERY_TEST_READ_FILE"), path);
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        reader.setProcessEnvironment(environment);
        reader.start(QCoreApplication::applicationFilePath(), QStringList{});
        QVERIFY(reader.waitForStarted(4000)); QVERIFY(reader.waitForFinished(4000));
        QCOMPARE(reader.exitStatus(), QProcess::NormalExit); QCOMPARE(reader.exitCode(), 0);
        const auto result = QJsonDocument::fromJson(reader.readAllStandardOutput()).object();
        QVERIFY(result.contains(QStringLiteral("enabled"))); QVERIFY(!result.value(QStringLiteral("enabled")).toBool());
        QCOMPARE(result.value(QStringLiteral("intervalMinutes")).toInt(), 120);
        QCOMPARE(ReadRecoveryIni(path), bytes);
    }
    void InvalidValuesUseTheSameReadonlyDefaults_data() {
        QTest::addColumn<QVariant>("enabled"); QTest::addColumn<QVariant>("interval");
        QTest::addColumn<bool>("expectedEnabled"); QTest::addColumn<int>("expectedMinutes");
        QTest::newRow("string-one") << QVariant(QStringLiteral("true")) << QVariant(QStringLiteral(" 1 ")) << true << 1;
        QTest::newRow("string-max") << QVariant(QStringLiteral("false")) << QVariant(QStringLiteral("120")) << false << 120;
        QTest::newRow("zero-interval") << QVariant(0) << QVariant(0) << false << 5;
        QTest::newRow("unknown-bool-and-double") << QVariant(2) << QVariant(6.5) << true << 5;
        QTest::newRow("unknown-strings") << QVariant(QStringLiteral("unknown")) << QVariant(QStringLiteral("bad")) << true << 5;
        QTest::newRow("legacy-bytes") << QVariant(QByteArray("false")) << QVariant(QByteArray("60")) << true << 5;
        QTest::newRow("over-budget") << QVariant(false) << QVariant(121) << false << 5;
        QTest::newRow("negative") << QVariant(true) << QVariant(-1) << true << 5;
        QTest::newRow("bool-is-not-minutes") << QVariant(true) << QVariant(true) << true << 5;
    }
    void InvalidValuesUseTheSameReadonlyDefaults() {
        QFETCH(QVariant, enabled); QFETCH(QVariant, interval); QFETCH(bool, expectedEnabled); QFETCH(int, expectedMinutes);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QSettings stored(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat); stored.setFallbacksEnabled(false);
        stored.setValue(QStringLiteral("project/autoRecoveryEnabled"), enabled);
        stored.setValue(QStringLiteral("project/autoRecoveryIntervalMinutes"), interval); stored.sync();
        const auto bytes = ReadRecoveryIni(stored.fileName());
        const auto shared = LoadRecoveryPreferences(stored); QCOMPARE(shared.enabled, expectedEnabled); QCOMPARE(shared.intervalMinutes, expectedMinutes);
        RecoverySettingsPage page(stored);
        const auto* toggle = page.findChild<QCheckBox*>(QStringLiteral("recoveryEnabled"));
        const auto* spin = page.findChild<QSpinBox*>(QStringLiteral("recoveryIntervalMinutes"));
        QVERIFY(toggle); QVERIFY(spin); QCOMPARE(toggle->isChecked(), shared.enabled); QCOMPARE(spin->value(), shared.intervalMinutes);
        QCOMPARE(spin->isEnabled(), shared.enabled); QVERIFY(page.Validate());
        QCOMPARE(ReadRecoveryIni(stored.fileName()), bytes);
    }
};

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    const auto readPath = qEnvironmentVariable("AUD_RECOVERY_TEST_READ_FILE");
    if (!readPath.isEmpty()) {
        QSettings stored(readPath, QSettings::IniFormat); stored.setFallbacksEnabled(false);
        const auto preferences = LoadRecoveryPreferences(stored);
        QTextStream output(stdout);
        output << QJsonDocument(QJsonObject{{QStringLiteral("enabled"), preferences.enabled},
            {QStringLiteral("intervalMinutes"), preferences.intervalMinutes}}).toJson(QJsonDocument::Compact);
        return stored.status() == QSettings::NoError ? 0 : 1;
    }
    RecoverySettingsTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "RecoverySettingsTests.moc"
