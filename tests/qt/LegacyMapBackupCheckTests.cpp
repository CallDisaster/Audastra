#include "LegacyMapBackupCheck.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QScopeGuard>
#include <QPointer>
#include <QtTest>

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <mutex>

using namespace War3Map::QtProject;
namespace {
void Write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}
struct Barrier {
    std::mutex mutex;
    std::condition_variable condition;
    bool release = false;
    std::atomic<bool> cancelled = false;
    std::atomic<int> started = 0, finished = 0, active = 0, maximum = 0;
};
} // namespace
class LegacyMapBackupCheckTests : public QObject {
    Q_OBJECT
private slots:
    void NoBackupDirectoryIsCreated();
    void ForeignReceiptIsUnknownAndUntouched();
    void PortableFamilyDiscoveryIsBoundedAndManifestScoped();
    void BackgroundWorkKeepsEventsResponsiveAndNewestRequestWins();
    void DestroyDoesNotJoinBlockedIoOrDeliverToDestroyedObserver();
    void ObserverCanReplaceItselfAndDestroyService();
};

void LegacyMapBackupCheckTests::NoBackupDirectoryIsCreated() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto report = CheckLegacyMapBackups(root.path(), {});
    QCOMPARE(report.checkedDirectories.size(), 1);
    QVERIFY(report.confirmedDirectories.isEmpty());
    QVERIFY(report.unknownDirectories.isEmpty());
    QVERIFY(!QFileInfo::exists(root.filePath("backups")));
    QVERIFY(report.Text().contains(QStringLiteral("其他安装目录")));
}
void LegacyMapBackupCheckTests::ForeignReceiptIsUnknownAndUntouched() {
    QTemporaryDir root;
    QVERIFY(QDir().mkpath(root.filePath("chosen/.forge-backup-owners")));
    const auto foreign = root.filePath("chosen/.forge-backup-owners/forge-map-invalid.w3x.json");
    Write(foreign, "{broken receipt");
    const auto report = CheckLegacyMapBackups(root.path(), root.filePath("chosen"));
    QCOMPARE(report.checkedDirectories, QStringList{root.filePath("chosen")});
    QCOMPARE(report.unknownDirectories.size(), 1);
    QVERIFY(report.confirmedDirectories.isEmpty());
    QFile file(foreign); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("{broken receipt"));
    QVERIFY(!QFileInfo::exists(root.filePath("backups")));
}
void LegacyMapBackupCheckTests::PortableFamilyDiscoveryIsBoundedAndManifestScoped() {
    QTemporaryDir root;
    const QByteArray commit(40, 'a');
    auto portable = [&](QString name, QByteArray product) {
        const auto install = root.filePath(name + "/Audastra");
        QDir().mkpath(install + "/bin");
        Write(install + "/bin/war3-map-editor.exe", "test");
        Write(install + "/manifest.json", "{\"format\":1,\"product\":\"" + product
            + "\",\"approved_main_commit\":\"" + commit
            + "\",\"products\":[{\"path\":\"bin/war3-map-editor.exe\",\"bytes\":4,\"sha256\":\""
            + QByteArray(64, '1') + "\"}]}");
        return install;
    };
    const auto current = portable("20261007-aaaaaaa", "Audastra unsigned author test portable");
    const auto old = portable("20261006-aaaaaaa", "Audastra unsigned author test portable");
    const auto foreign = portable("20261005-aaaaaaa", "other");
    const auto report = CheckLegacyMapBackups(current, {});
    QCOMPARE(report.checkedDirectories.size(), 2);
    QVERIFY(report.checkedDirectories.contains(old + "/backups"));
    QVERIFY(!report.checkedDirectories.contains(foreign + "/backups"));
    QVERIFY(!QFileInfo::exists(old + "/backups"));
    for (int i = 0; i < 75; ++i) QVERIFY(QDir().mkpath(root.filePath(QString("unrelated-%1").arg(i))));
    const auto bounded = CheckLegacyMapBackups(current, {});
    QVERIFY(bounded.discoveryIncomplete);
    QVERIFY(bounded.checkedDirectories.size() <= 8);
}
void LegacyMapBackupCheckTests::BackgroundWorkKeepsEventsResponsiveAndNewestRequestWins() {
    auto barrier = std::make_shared<Barrier>();
    const auto release = qScopeGuard([barrier]() {
        { const std::lock_guard lock(barrier->mutex); barrier->release = true; }
        barrier->condition.notify_all();
    });
    LegacyMapBackupCheck check(QStringLiteral("unused"), nullptr,
        [barrier](const QString&, const QString& directory, std::stop_token) {
            ++barrier->started;
            const auto active = ++barrier->active;
            barrier->maximum.store(std::max(barrier->maximum.load(), active));
            { std::unique_lock lock(barrier->mutex); barrier->condition.wait(lock, [&]() { return barrier->release; }); }
            --barrier->active; ++barrier->finished;
            LegacyMapBackupReport result; result.confirmedDirectories << directory; return result;
        });
    QString delivered;
    check.SetObserver([&](QString text, bool busy) { if (!busy) delivered = text; });
    check.Start(QStringLiteral("first"));
    QTRY_COMPARE(barrier->started.load(), 1);
    check.Start(QStringLiteral("superseded")); check.Start(QStringLiteral("newest"));
    int heartbeat = 0;
    QTimer timer; connect(&timer, &QTimer::timeout, this, [&]() { ++heartbeat; }); timer.start(5);
    QTRY_VERIFY(heartbeat >= 3);
    QCOMPARE(barrier->started.load(), 1);
    { const std::lock_guard lock(barrier->mutex); barrier->release = true; } barrier->condition.notify_all();
    QTRY_VERIFY(!check.Busy());
    QCOMPARE(barrier->started.load(), 2); QCOMPARE(barrier->maximum.load(), 1);
    QVERIFY(delivered.contains(QStringLiteral("newest")));
    QVERIFY(!delivered.contains(QStringLiteral("superseded")));
    QVERIFY(!delivered.contains(QStringLiteral("first")));
}
void LegacyMapBackupCheckTests::DestroyDoesNotJoinBlockedIoOrDeliverToDestroyedObserver() {
    auto barrier = std::make_shared<Barrier>();
    const auto release = qScopeGuard([barrier]() {
        { const std::lock_guard lock(barrier->mutex); barrier->release = true; }
        barrier->condition.notify_all();
    });
    int callbacks = 0;
    auto check = std::make_unique<LegacyMapBackupCheck>(QStringLiteral("unused"), nullptr,
        [barrier](const QString&, const QString&, std::stop_token stop) {
            ++barrier->started;
            { std::unique_lock lock(barrier->mutex); barrier->condition.wait(lock, [&]() { return barrier->release; }); }
            barrier->cancelled = stop.stop_requested(); ++barrier->finished;
            return LegacyMapBackupReport{};
        });
    check->SetObserver([&](QString, bool) { ++callbacks; }); check->Start();
    QTRY_COMPARE(barrier->started.load(), 1);
    check.reset(); // worker 被屏障阻塞；若析构 join，此步骤不能返回。
    const auto atDestruction = callbacks;
    { const std::lock_guard lock(barrier->mutex); barrier->release = true; } barrier->condition.notify_all();
    QTRY_COMPARE(barrier->finished.load(), 1);
    QVERIFY(barrier->cancelled.load()); QCOMPARE(callbacks, atDestruction);
}
void LegacyMapBackupCheckTests::ObserverCanReplaceItselfAndDestroyService() {
    QPointer<LegacyMapBackupCheck> check(new LegacyMapBackupCheck(QStringLiteral("unused")));
    int replacements = 0;
    check->SetObserver([&](QString, bool) {
        check->SetObserver([&](QString, bool) { ++replacements; });
    });
    QCOMPARE(replacements, 1);
    check->SetObserver([&](QString, bool) { delete check.data(); });
    QVERIFY(!check);
}
QTEST_GUILESS_MAIN(LegacyMapBackupCheckTests)
#include "LegacyMapBackupCheckTests.moc"
