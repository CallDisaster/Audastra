#include "ProgramIssueApplication.h"
#include "FeedbackClient.h"
#include "../feedback/Fixtures.h"

#include <QCoreApplication>
#include <QEvent>
#include <QHostAddress>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QtTest>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace War3Map;
using namespace War3Map::Feedback;
using namespace War3Map::QtFeedback;
using namespace FeedbackFixtures;

namespace {
class ThrowingReceiver final : public QObject {
public:
    explicit ThrowingReceiver(bool standardException) : standard_(standardException) {}
    bool event(QEvent* event) override {
        if (event->type() == QEvent::User) {
            if (standard_) throw std::runtime_error("original-secret-value-123 map script must never be collected");
            throw 123;
        }
        return QObject::event(event);
    }
private:
    bool standard_;
};

int ExceptionFixture(ProgramIssueApplication& application, const QStringList& arguments) {
    const auto directory = std::filesystem::path(arguments.at(2).toStdWString());
    const auto queue = Take(FeedbackQueue::Open(directory, Context()), "child opens owned application queue");
    application.SetProgramIssueReporter(std::make_shared<ProgramIssueReporter>(queue, AgentDraft().application));
    const bool standardException = arguments.at(4) == QStringLiteral("std");
    if (arguments.at(3) == QStringLiteral("startup")) {
        try {
            if (standardException) throw std::runtime_error("original-secret-value-123 private startup detail");
            throw 123;
        } catch (const std::exception&) { application.ReportStartupException(true); }
        catch (...) { application.ReportStartupException(false); }
        return ProgramIssueApplication::kUnhandledExceptionExitCode;
    }
    ThrowingReceiver receiver(standardException);
    if (arguments.at(3) == QStringLiteral("event-active")) {
        QCoreApplication::postEvent(&receiver, new QEvent(QEvent::User));
    } else {
        // 故障在 exec 之前发生，验证排队退出仍返回非零且同步草稿已落盘。
        QEvent event(QEvent::User);
        if (application.notify(&receiver, &event)) return 91;
    }
    const auto result = application.exec();
    return application.HadUnhandledException() ? ProgramIssueApplication::kUnhandledExceptionExitCode : result;
}
} // namespace

class ProgramIssueReporterTests final : public QObject {
    Q_OBJECT
private slots:
    void ProgramIssuesUseOneQueueWithoutConsentOrNetwork();
    void SaveWhitelistRejectsUserAndExternalFailures();
    void RenderFailureRequiresMissingSpecificDiagnostic();
    void RenderFailuresUseMonotonicOperationCooldown();
    void FailedRenderDraftCanRetryImmediately();
    void ConcurrentRenderFailuresPersistOnlyOnce();
    void ExceptionBoundariesPersistAndExit_data();
    void ExceptionBoundariesPersistAndExit();
    void QueueFailureAndMissingReporterRemainBounded();
};

void ProgramIssueReporterTests::ProgramIssuesUseOneQueueWithoutConsentOrNetwork() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "shared automatic draft fixture");
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QSignalSpy connections(&server, &QTcpServer::newConnection);
    FeedbackClient client(queue, LoopbackFeedbackEndpoint{QUrl(QStringLiteral("http://127.0.0.1:%1/report").arg(server.serverPort()))});
    ProgramIssueReporter reporter(queue, AgentDraft().application);
    const auto first = reporter.SaveDefect("SAVE_TASK_ALREADY_EXECUTED");
    QVERIFY(first.has_value());
    QVERIFY(!first->consent);
    const auto repeat = reporter.SaveDefect("SAVE_TASK_ALREADY_EXECUTED");
    QVERIFY(repeat && repeat->deduplicated);
    QCOMPARE(repeat->entry.id, first->entry.id);
    QCOMPARE(repeat->entry.occurrences, std::uint64_t{2});
    QVERIFY(!repeat->entry.previewConfirmed);
    client.RefreshQueue();
    QTRY_COMPARE_WITH_TIMEOUT(client.Snapshot().entries.size(), std::size_t{1}, 3000);
    const auto preview = Take(queue->GetPreview(first->entry.id), "clean automatic preview");
    QVERIFY(preview.delivery.reportJson.find("application-save-defect") != std::string::npos);
    QVERIFY(preview.delivery.reportJson.find("original-secret-value-123") == std::string::npos);
    QVERIFY(!first->entry.hasLogs && !first->entry.hasConversation && !first->entry.hasScreenshot);
    QVERIFY(!queue->LastSentUnixSeconds());
    QCOMPARE(connections.count(), 0);
    QVERIFY(!server.hasPendingConnections());
}

void ProgramIssueReporterTests::SaveWhitelistRejectsUserAndExternalFailures() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "save classification fixture");
    ProgramIssueReporter reporter(queue, AgentDraft().application);
    for (const auto* code : {"SAVE_MAP_FORMAT_UNSUPPORTED", "SAVE_PLACEMENT_LAYOUT_UNSUPPORTED", "SAVE_SOURCE_MISMATCH",
        "SAVE_TARGET_INVALID", "SAVE_REVISION_CHANGED", "SAVE_REVISION_UNAVAILABLE", "REPACK_REOPEN_FAILED",
        "REPACK_NATIVE_BASELINE_MISSING", "SAVE_FAILED", "CANDIDATE_SAVE_FAILED", "FEEDBACK_OPERATION_FAILED"})
        QVERIFY(!reporter.SaveDefect(code));
    QCOMPARE(Take(queue->List(), "negative save issues list").size(), std::size_t{0});
    for (const auto* code : {"SAVE_TASK_ALREADY_EXECUTED", "SAVE_SOURCE_SNAPSHOT_INVALID", "SCRIPT_INPUT_MISSING",
        "SAVE_REVISION_BINDING_MISSING", "SAVE_INTERNAL_CPP_EXCEPTION", "SAVE_INTERNAL_NONSTANDARD_EXCEPTION"})
        QVERIFY(reporter.SaveDefect(code).has_value());
    QCOMPARE(Take(queue->List(), "positive save issues list").size(), std::size_t{6});
}

void ProgramIssueReporterTests::RenderFailureRequiresMissingSpecificDiagnostic() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "render classification fixture");
    ProgramIssueReporter reporter(queue, AgentDraft().application);
    QVERIFY(!reporter.RenderFailure("resize", 0, true, 0, 0, {}));
    QVERIFY(!reporter.RenderFailure("unsupported-feature", 1, false, 0, 0, {}));
    QVERIFY(!reporter.RenderFailure("resize", -5, true, 0, -5, "D3D11 failed: original-secret-value-123"));
    QVERIFY(!reporter.RenderFailure("submitModelScene", -5, true, 0, 0, "Model upload failed: original-secret-value-123"));
    QCOMPARE(Take(queue->List(), "explained render issues list").size(), std::size_t{0});
    const auto created = reporter.RenderFailure("createRenderer", -1, false, 0, 0, {});
    QVERIFY(created);
    const auto resized = reporter.RenderFailure("resize", -5, true, 0, -5, {});
    QVERIFY(resized);
    const auto staleSuccess = reporter.RenderFailure("submitModelScene", -8, true, 0, 0,
        "D3D11 WARP / Feature Level 11.0 / model ready");
    QVERIFY(staleSuccess);
    const auto repeat = reporter.RenderFailure("resize", -5, true, 0, -5, {});
    QVERIFY(!repeat);
    QCOMPARE(Take(queue->GetStatus(resized->entry.id), "suppressed render occurrence").occurrences, std::uint64_t{1});
    const auto preview = Take(queue->GetPreview(resized->entry.id), "unexplained render preview");
    QVERIFY(preview.delivery.reportJson.find("unexplained-render-failure") != std::string::npos);
    QVERIFY(preview.delivery.reportJson.find("original-secret-value-123") == std::string::npos);
    QVERIFY(!queue->Consent() && !queue->LastSentUnixSeconds());
}

void ProgramIssueReporterTests::RenderFailuresUseMonotonicOperationCooldown() {
    Temp temp;
    const auto directory = temp.Path() / L"queue";
    auto queue = Take(FeedbackQueue::Open(directory, Context()), "render cooldown fixture");
    auto now = std::chrono::steady_clock::time_point{};
    ProgramIssueReporter reporter(queue, AgentDraft().application, [&now] { return now; });
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QSignalSpy connections(&server, &QTcpServer::newConnection);
    FeedbackClient client(queue, LoopbackFeedbackEndpoint{QUrl(QStringLiteral("http://127.0.0.1:%1/report").arg(server.serverPort()))});
    QSignalSpy snapshots(&client, &FeedbackClient::SnapshotChanged);
    QTRY_VERIFY_WITH_TIMEOUT(snapshots.count() >= 1, 3000);
    snapshots.clear();

    const auto first = reporter.RenderFailure("getViewInfo", -5, true, 0, 0, {});
    QVERIFY(first);
    client.RefreshQueue();
    QTRY_COMPARE_WITH_TIMEOUT(snapshots.count(), 1, 3000);
    const auto persisted = Read(directory / L"queue.json");
    // 同一操作的结果变化也不绕过槽；nullopt 保持宿主的刷新/通知分支关闭。
    for (int attempt = 0; attempt < 100; ++attempt)
        QVERIFY(!reporter.RenderFailure("getViewInfo", -5 - attempt, true, 0, -attempt, {}));
    now += std::chrono::seconds(59);
    QVERIFY(!reporter.RenderFailure("getViewInfo", -105, false, -1, 0, {}));
    QCOMPARE(Read(directory / L"queue.json"), persisted);
    QCOMPARE(Take(queue->GetStatus(first->entry.id), "cooldown keeps one persisted occurrence").occurrences, std::uint64_t{1});
    QCoreApplication::processEvents();
    QCOMPARE(snapshots.count(), 1);

    now += std::chrono::seconds(1);
    const auto due = reporter.RenderFailure("getViewInfo", -105, false, -1, 0, {});
    QVERIFY(due && due->deduplicated);
    QCOMPARE(due->entry.id, first->entry.id);
    QCOMPARE(due->entry.occurrences, std::uint64_t{2});
    client.RefreshQueue();
    QTRY_COMPARE_WITH_TIMEOUT(snapshots.count(), 2, 3000);
    QVERIFY(reporter.RenderFailure("setCamera", -5, true, 0, 0, {}));
    QVERIFY(!reporter.RenderFailure("getViewInfo", -106, true, 0, 0, {}));
    const auto saved = reporter.SaveDefect("SAVE_TASK_ALREADY_EXECUTED");
    QVERIFY(saved);
    const auto savedAgain = reporter.SaveDefect("SAVE_TASK_ALREADY_EXECUTED");
    QVERIFY(savedAgain && savedAgain->deduplicated);
    QCOMPARE(savedAgain->entry.occurrences, std::uint64_t{2});
    QVERIFY(!queue->Consent() && !queue->LastSentUnixSeconds());
    QCOMPARE(connections.count(), 0);
    QVERIFY(!server.hasPendingConnections());
}

void ProgramIssueReporterTests::FailedRenderDraftCanRetryImmediately() {
    Temp temp;
    const auto directory = temp.Path() / L"queue";
    auto queue = Take(FeedbackQueue::Open(directory, Context()), "render storage retry fixture");
    ProgramIssueReporter reporter(queue, AgentDraft().application, [] { return std::chrono::steady_clock::time_point{}; });
    const auto target = directory / L"queue.json";
    const auto preserved = directory / L"queue-preserved.json";
    const auto original = Read(target);
    // 仅阻断此测试拥有的临时队列；还原同一字节身份后重试，不改变目录权限。
    std::filesystem::rename(target, preserved);
    QVERIFY(std::filesystem::create_directory(target));
    const auto failed = reporter.RenderFailure("getViewInfo", -5, true, 0, 0, {});
    QVERIFY(std::filesystem::remove(target));
    std::filesystem::rename(preserved, target);
    QVERIFY(!failed);
    QCOMPARE(Read(target), original);
    QCOMPARE(Take(queue->List(), "storage failure has no false persisted draft").size(), std::size_t{0});
    const auto retried = reporter.RenderFailure("getViewInfo", -5, true, 0, 0, {});
    QVERIFY(retried);
    QCOMPARE(retried->entry.occurrences, std::uint64_t{1});
    QVERIFY(!reporter.RenderFailure("getViewInfo", -5, true, 0, 0, {}));
}

void ProgramIssueReporterTests::ConcurrentRenderFailuresPersistOnlyOnce() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "concurrent render cooldown fixture");
    ProgramIssueReporter reporter(queue, AgentDraft().application, [] { return std::chrono::steady_clock::time_point{}; });
    std::atomic<unsigned> successful{0};
    std::array<std::thread, 8> threads;
    for (std::size_t index = 0; index < threads.size(); ++index) {
        threads[index] = std::thread([&reporter, &successful, index] {
            for (unsigned attempt = 0; attempt < 32; ++attempt)
                if (reporter.RenderFailure("getViewInfo", -5 - static_cast<std::int32_t>(index), true, 0, 0, {}))
                    successful.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& thread : threads) thread.join();
    QCOMPARE(successful.load(std::memory_order_relaxed), 1U);
    const auto entries = Take(queue->List(), "concurrent render queue");
    QCOMPARE(entries.size(), std::size_t{1});
    QCOMPARE(entries.front().occurrences, std::uint64_t{1});
}

void ProgramIssueReporterTests::ExceptionBoundariesPersistAndExit_data() {
    QTest::addColumn<QString>("boundary");
    QTest::addColumn<QString>("kind");
    QTest::newRow("startup-std") << QStringLiteral("startup") << QStringLiteral("std");
    QTest::newRow("startup-nonstandard") << QStringLiteral("startup") << QStringLiteral("nonstandard");
    QTest::newRow("event-std-before-exec") << QStringLiteral("event") << QStringLiteral("std");
    QTest::newRow("event-nonstandard-before-exec") << QStringLiteral("event") << QStringLiteral("nonstandard");
    QTest::newRow("event-std-in-active-loop") << QStringLiteral("event-active") << QStringLiteral("std");
    QTest::newRow("event-nonstandard-in-active-loop") << QStringLiteral("event-active") << QStringLiteral("nonstandard");
}

void ProgramIssueReporterTests::ExceptionBoundariesPersistAndExit() {
    QFETCH(QString, boundary);
    QFETCH(QString, kind);
    Temp temp;
    const auto directory = temp.Path() / L"child-queue";
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--exception-fixture"),
        QString::fromStdWString(directory.wstring()), boundary, kind});
    QVERIFY(child.waitForStarted(5000));
    QVERIFY(child.waitForFinished(15000));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), ProgramIssueApplication::kUnhandledExceptionExitCode);
    auto queue = Take(FeedbackQueue::Open(directory, Context()), "read child persisted queue after actual exit");
    const auto entries = Take(queue->List(), "child exception entry list");
    QCOMPARE(entries.size(), std::size_t{1});
    QCOMPARE(entries.front().state, QueueState::Pending);
    QVERIFY(!entries.front().previewConfirmed && !queue->Consent() && !queue->LastSentUnixSeconds());
    const auto preview = Take(queue->GetPreview(entries.front().id), "child exception persisted preview");
    QVERIFY(preview.delivery.reportJson.find("unhandled-exception") != std::string::npos);
    QVERIFY(preview.delivery.reportJson.find(boundary == QStringLiteral("startup") ? "application/startup" : "application/notify") != std::string::npos);
    QVERIFY(preview.delivery.reportJson.find("original-secret-value-123") == std::string::npos);
    QVERIFY(preview.delivery.reportJson.find("map script") == std::string::npos);
    QVERIFY(!entries.front().hasLogs && !entries.front().hasConversation && !entries.front().hasScreenshot);
}

void ProgramIssueReporterTests::QueueFailureAndMissingReporterRemainBounded() {
    ProgramIssueReporter unavailable(nullptr, AgentDraft().application);
    QVERIFY(!unavailable.UnhandledException(ExceptionBoundary::Event, true));
    QVERIFY(!unavailable.SaveDefect("SCRIPT_INPUT_MISSING"));
    Temp temp;
    QueueLimits limits; limits.maxEntries = 1;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context(), limits), "bounded automatic queue");
    ProgramIssueReporter reporter(queue, AgentDraft().application);
    QVERIFY(reporter.SaveDefect("SCRIPT_INPUT_MISSING"));
    QVERIFY(!reporter.UnhandledException(ExceptionBoundary::Event, false));
    QCOMPARE(Take(queue->List(), "full queue retained original failure").size(), std::size_t{1});
    QVERIFY(!queue->Consent() && !queue->LastSentUnixSeconds());
}

int main(int argc, char* argv[]) {
    ProgramIssueApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() == 5 && arguments.at(1) == QStringLiteral("--exception-fixture"))
        return ExceptionFixture(application, arguments);
    ProgramIssueReporterTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "ProgramIssueReporterTests.moc"
