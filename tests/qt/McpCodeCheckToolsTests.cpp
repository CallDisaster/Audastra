#include "McpCodeCheckTools.h"

#include <QJsonDocument>
#include <QTest>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace War3Map;
namespace {
struct Gate {
    std::atomic<bool> entered = false, exited = false;
    std::mutex mutex;
    std::condition_variable condition;
    bool released = false;
    void Release() { { std::lock_guard lock(mutex); released = true; } condition.notify_all(); }
    Services::CodeCheckResult Run(std::stop_token cancel) {
        entered = true;
        std::unique_lock lock(mutex);
        while (!released && !cancel.stop_requested()) condition.wait_for(lock, std::chrono::milliseconds(5));
        Services::CodeCheckResult result;
        result.cancelled = cancel.stop_requested(); result.completed = !result.cancelled; result.syntaxChecked = result.completed;
        exited = true; return result;
    }
};
struct ReleaseGate { std::shared_ptr<Gate> gate; ~ReleaseGate() { gate->Release(); } };
Mcp::CodeCheckToolHost Host(std::shared_ptr<Gate> gate, std::string* binding) {
    return {[gate, binding](Mcp::CodeCheckToolInput input) {
        Mcp::CodeCheckToolWork work; work.binding = *binding; work.targetKey = input.sourceName;
        work.run = [gate](std::stop_token cancel) { return gate->Run(cancel); };
        return Result<Mcp::CodeCheckToolWork>::Success(std::move(work));
    }, [binding] { return *binding; }};
}
QJsonObject Submit(QString name = "one.j") { return {{"source_name", name}, {"text", "function Test takes nothing returns nothing\nendfunction\n"}}; }
QJsonObject StatusArguments(const QString& id) { return {{"action", "status"}, {"request_id", id}}; }
QString Error(const QJsonObject& result) { return result.value("diagnostics").toArray().first().toObject().value("code").toString(); }
}
class McpCodeCheckToolsTests final : public QObject {
    Q_OBJECT
private slots:
    void SubmitReturnsBeforeWorkerAndQueueIsBounded();
    void LatestRequestCancelsAndBindingChangeSuppressesResults();
    void DiagnosticsArePagedAndFieldsAreStrict();
    void ClosingSessionNeverWaitsForWorker();
    void CancellationOverridesLateSuccessfulPublication();
};
void McpCodeCheckToolsTests::SubmitReturnsBeforeWorkerAndQueueIsBounded() {
    Mcp::CodeCheckToolSession session;
    std::string binding = "rev-1"; auto gate = std::make_shared<Gate>(); const auto host = Host(gate, &binding);
    const auto start = std::chrono::steady_clock::now();
    auto first = session.Invoke(Submit(), host);
    QVERIFY(first.value("ok").toBool()); QCOMPARE(first.value("state").toString(), QString("queued"));
    QVERIFY(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
    QVERIFY(!first.value("executes_code").toBool() && !first.value("writes_map").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(gate->entered.load(), 2000);
    for (int i = 0; i < 4; ++i) QVERIFY(session.Invoke(Submit(QString("queued-%1.j").arg(i)), host).value("ok").toBool());
    auto full = session.Invoke(Submit("overflow.j"), host);
    QVERIFY(!full.value("ok").toBool()); QCOMPARE(Error(full), QString("CODE_CHECK_QUEUE_FULL"));
    gate->Release();
    QTRY_COMPARE_WITH_TIMEOUT(session.Invoke(StatusArguments(first.value("request_id").toString()), host).value("state").toString(), QString("complete"), 2000);
    auto cancel = StatusArguments(first.value("request_id").toString()); cancel.insert("action", "cancel");
    auto completed = session.Invoke(cancel, host);
    QCOMPARE(completed.value("state").toString(), QString("complete")); QVERIFY(completed.value("passed").toBool());
}
void McpCodeCheckToolsTests::LatestRequestCancelsAndBindingChangeSuppressesResults() {
    Mcp::CodeCheckToolSession session;
    std::string binding = "rev-1"; auto gate = std::make_shared<Gate>(); const auto host = Host(gate, &binding);
    auto first = session.Invoke(Submit(), host); QTRY_VERIFY_WITH_TIMEOUT(gate->entered.load(), 2000);
    auto second = session.Invoke(Submit(), host); QVERIFY(second.value("ok").toBool());
    QTRY_COMPARE_WITH_TIMEOUT(session.Invoke(StatusArguments(first.value("request_id").toString()), host).value("state").toString(), QString("cancelled"), 2000);
    binding = "rev-2";
    auto stale = session.Invoke(StatusArguments(second.value("request_id").toString()), host);
    QCOMPARE(stale.value("state").toString(), QString("stale")); QVERIFY(stale.value("stale").toBool());
    QVERIFY(!stale.contains("diagnostics") && !stale.value("passed").toBool());
    gate->Release();
}
void McpCodeCheckToolsTests::DiagnosticsArePagedAndFieldsAreStrict() {
    Mcp::CodeCheckToolSession session;
    Mcp::CodeCheckToolHost host;
    host.currentBinding = [] { return std::string("revision"); };
    host.capture = [](Mcp::CodeCheckToolInput) {
        Mcp::CodeCheckToolWork work; work.binding = "revision"; work.targetKey = "page";
        work.run = [](std::stop_token) {
            Services::CodeCheckResult result; result.completed = true; result.syntaxChecked = true;
            for (int i = 0; i < 130; ++i) result.diagnostics.push_back({DiagnosticSeverity::Error,
                "author.j", "EXACT", "owned failure " + std::to_string(i), static_cast<std::uint32_t>(i + 1), 3, 2, true});
            return result;
        };
        return Result<Mcp::CodeCheckToolWork>::Success(std::move(work));
    };
    auto queued = session.Invoke(Submit(), host); const auto id = queued.value("request_id").toString();
    QTRY_COMPARE_WITH_TIMEOUT(session.Invoke(StatusArguments(id), host).value("state").toString(), QString("complete"), 2000);
    auto args = StatusArguments(id); args.insert("offset", "0"); args.insert("limit", "128");
    auto page = session.Invoke(args, host);
    QCOMPARE(page.value("diagnostics").toArray().size(), 128); QCOMPARE(page.value("next_offset").toString(), QString("128"));
    QVERIFY(!page.value("diagnostics_complete").toBool() && !page.value("passed").toBool());
    QCOMPARE(page.value("diagnostics").toArray()[0].toObject().value("byte_column").toInt(), 3);
    QVERIFY(QJsonDocument(page).toJson(QJsonDocument::Compact).size() <= 256 * 1024 + 8192);
    args.insert("offset", "128"); page = session.Invoke(args, host);
    QCOMPARE(page.value("diagnostics").toArray().size(), 2); QVERIFY(page.value("diagnostics_complete").toBool());
    args.insert("offset", 0); QVERIFY(!session.Invoke(args, host).value("ok").toBool());
    args = Submit(); args.insert("trigger_id", "01"); QVERIFY(!session.Invoke(args, host).value("ok").toBool());
    args = Submit(); args.insert("unknown", true); QCOMPARE(Error(session.Invoke(args, host)), QString("CODE_CHECK_ARGUMENT_UNKNOWN"));
    args = Submit(); args.insert("text", QString(512 * 1024 + 1, 'x')); QVERIFY(!session.Invoke(args, host).value("ok").toBool());
}
void McpCodeCheckToolsTests::ClosingSessionNeverWaitsForWorker() {
    std::string binding = "rev"; auto gate = std::make_shared<Gate>(); const auto host = Host(gate, &binding);
    auto session = std::make_unique<Mcp::CodeCheckToolSession>();
    QVERIFY(session->Invoke(Submit(), host).value("ok").toBool()); QTRY_VERIFY_WITH_TIMEOUT(gate->entered.load(), 2000);
    const auto start = std::chrono::steady_clock::now(); session.reset();
    QVERIFY(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
    QTRY_VERIFY_WITH_TIMEOUT(gate->exited.load(), 2000);
}
void McpCodeCheckToolsTests::CancellationOverridesLateSuccessfulPublication() {
    Mcp::CodeCheckToolSession session;
    auto gate = std::make_shared<Gate>(); ReleaseGate release{gate};
    Mcp::CodeCheckToolHost host;
    host.currentBinding = [] { return std::string("revision"); };
    host.capture = [gate](Mcp::CodeCheckToolInput) {
        Mcp::CodeCheckToolWork work; work.binding = "revision"; work.targetKey = "late";
        work.run = [gate](std::stop_token) {
            // 模拟backend最后一次poll之后，结果发布之前发生cancel；不执行任何作者代码。
            gate->entered = true; std::unique_lock lock(gate->mutex);
            while (!gate->released) gate->condition.wait_for(lock, std::chrono::milliseconds(5));
            Services::CodeCheckResult result; result.completed = true; result.syntaxChecked = true;
            gate->exited = true; return result;
        };
        return Result<Mcp::CodeCheckToolWork>::Success(std::move(work));
    };
    const auto first = session.Invoke(Submit(), host); const auto id = first.value("request_id").toString();
    QTRY_VERIFY_WITH_TIMEOUT(gate->entered.load(), 2000);
    auto cancel = StatusArguments(id); cancel.insert("action", "cancel"); static_cast<void>(session.Invoke(cancel, host));
    gate->Release(); QTRY_VERIFY_WITH_TIMEOUT(gate->exited.load(), 2000);
    auto result = session.Invoke(StatusArguments(id), host);
    QCOMPARE(result.value("state").toString(), QString("cancelled"));
    QVERIFY(!result.value("passed").toBool() && !result.value("completed").toBool() && result.value("cancelled").toBool());
    QVERIFY(!result.contains("diagnostics"));
}
QTEST_GUILESS_MAIN(McpCodeCheckToolsTests)
#include "McpCodeCheckToolsTests.moc"
