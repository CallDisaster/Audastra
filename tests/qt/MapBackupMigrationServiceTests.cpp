#include "MapBackupMigrationService.h"
#include <QSignalSpy>
#include <QPointer>
#include <QThread>
#include <QtTest>
#include <atomic>
#include <condition_variable>
#include <thread>

using namespace War3Map;
using namespace War3Map::Agent;
class MapBackupMigrationServiceTests final : public QObject {
    Q_OBJECT
private slots:
    void RequestsRunSeriallyOffTheUiThread() {
        std::mutex mutex; QStringList order; std::atomic<int> running{0}, maximum{0}; std::atomic<bool> wrongThread{false};
        const auto gui = std::this_thread::get_id();
        MapBackupMigrationService service(nullptr, [&](const MapBackupMigrationRequest& r, std::stop_token) {
            if (std::this_thread::get_id() == gui) wrongThread = true;
            const auto active = ++running; maximum = std::max(maximum.load(), active);
            { const std::lock_guard lock(mutex); order << r.from + QLatin1Char('>') + r.to; }
            std::this_thread::sleep_for(std::chrono::milliseconds{15}); --running;
            MapBackupMigrationReport result; result.moved = 1; return result;
        });
        QSignalSpy spy(&service, &MapBackupMigrationService::StatusChanged);
        bool added = false;
        connect(&service, &MapBackupMigrationService::StatusChanged, this, [&](const QString& text, bool) {
            if (!added && text.startsWith(QStringLiteral("已把"))) { added = true; service.Submit({"C", "D", 10, {}}); }
        });
        service.Submit({"A", "B", 10, {}}); service.Submit({"B", "C", 10, {}});
        QVERIFY(service.Busy());
        QTRY_VERIFY_WITH_TIMEOUT(!service.Busy(), 3000);
        QCOMPARE(maximum.load(), 1); QVERIFY(!wrongThread.load());
        { const std::lock_guard lock(mutex); QCOMPARE(order, QStringList({"A>B", "B>C", "C>D"})); }
        QVERIFY(spy.size() >= 6); QVERIFY(service.Text().contains(QStringLiteral("1 份")));
    }
    void DestructionCancelsWithoutBlockingOrTouchingThePage() {
        struct Shared { std::mutex mutex; std::condition_variable_any cv; std::atomic<bool> started{false}, finished{false}; };
        auto shared = std::make_shared<Shared>();
        auto* service = new MapBackupMigrationService(nullptr, [shared](const auto&, std::stop_token stop) {
            shared->started = true;
            std::unique_lock lock(shared->mutex);
            shared->cv.wait_for(lock, stop, std::chrono::seconds{5}, [] { return false; });
            shared->finished = stop.stop_requested();
            return MapBackupMigrationReport{};
        });
        service->Submit({"A", "B", 0, {}});
        QTRY_VERIFY_WITH_TIMEOUT(shared->started.load(), 1000);
        delete service;
        QTRY_VERIFY_WITH_TIMEOUT(shared->finished.load(), 1000);
    }
    void CompletionObserverMayDestroyService() {
        QPointer<MapBackupMigrationService> service = new MapBackupMigrationService(nullptr, [](const auto&, auto) {
            return MapBackupMigrationReport{};
        });
        connect(service.data(), &MapBackupMigrationService::StatusChanged, this, [&](const QString&, bool busy) { if (!busy) delete service.data(); });
        service->Submit({"A", "B", 0, {}});
        QTRY_VERIFY_WITH_TIMEOUT(service.isNull(), 1000);
    }
    void ReportUsesOnlyNonzeroCountsAndPreservesFailures_data() {
        QTest::addColumn<int>("kind"); QTest::addColumn<QString>("expected");
        QTest::newRow("moved") << 0 << QStringLiteral("已把 1 份备份移到新目录");
        QTest::newRow("empty") << 1 << QStringLiteral("旧目录里没有编辑器的备份");
        QTest::newRow("skipped") << 2 << QStringLiteral("2 份没有移动：新目录已有同名的不同文件");
        QTest::newRow("pruned") << 3 << QStringLiteral("按保留数量删除了 1 份旧备份");
        QTest::newRow("unknown-empty") << 4 << QStringLiteral("旧备份迁移未完成，原文件保留\n旧备份目录不可确认");
        QTest::newRow("combined") << 5 << QStringLiteral("已把 1 份备份移到新目录，2 份没有移动：新目录已有同名的不同文件，按保留数量删除了 1 份旧备份");
    }
    void ReportUsesOnlyNonzeroCountsAndPreservesFailures() {
        QFETCH(int, kind); QFETCH(QString, expected);
        MapBackupMigrationService service(nullptr, [kind](const auto&, auto) {
            MapBackupMigrationReport report;
            if (kind == 0 || kind == 5) report.moved = 1;
            if (kind == 2 || kind == 5) { report.skipped = 2; report.diagnostics << QStringLiteral("新目录已有同名的不同文件"); }
            if (kind == 3 || kind == 5) report.pruned = 1;
            if (kind == 4) { report.complete = false; report.diagnostics << QStringLiteral("旧备份目录不可确认"); }
            return report;
        });
        service.Submit({"A", "B", 0, {}});
        QTRY_VERIFY_WITH_TIMEOUT(!service.Busy(), 1000);
        QCOMPARE(service.Text(), expected);
        QVERIFY(!service.Text().contains(QStringLiteral("0 份")));
    }
};
QTEST_GUILESS_MAIN(MapBackupMigrationServiceTests)
#include "MapBackupMigrationServiceTests.moc"
