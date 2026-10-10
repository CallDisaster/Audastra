#include "FeedbackClient.h"
#include "FeedbackTransport.h"
#include "FeedbackWorker.h"
#include "AudastraVersion.generated.h"
#include "../feedback/Fixtures.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QtTest>

using namespace War3Map;
using namespace War3Map::Feedback;
using namespace War3Map::QtFeedback;
using namespace FeedbackFixtures;

namespace War3Map::QtFeedback::Detail {

class FeedbackWorkerTestAccess {
public:
    static void Enqueue(FeedbackWorker& worker, quint64 request, std::function<void()> task) {
        worker.Enqueue(std::move(task), request);
    }
};

} // namespace War3Map::QtFeedback::Detail

namespace {

class Server final : public QObject {
public:
    struct Request { QByteArray header; QByteArray body; };
    QTcpServer listener;
    QList<Request> requests;
    int responseCode = 200;
    QByteArray responseBody = "{}";
    QByteArray responseHeaders;
    bool stall = false;
    QList<QPointer<QTcpSocket>> sockets;

    Server() {
        connect(&listener, &QTcpServer::newConnection, this, [this] {
            while (listener.hasPendingConnections()) {
                auto* socket = listener.nextPendingConnection();
                sockets.push_back(socket);
                auto buffer = std::make_shared<QByteArray>();
                auto completed = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, completed] {
                    if (*completed) { socket->readAll(); return; }
                    buffer->append(socket->readAll());
                    if (buffer->size() > 6 * 1024 * 1024) { socket->abort(); return; }
                    const auto boundary = buffer->indexOf("\r\n\r\n");
                    if (boundary < 0) return;
                    qint64 bytes = -1;
                    for (const auto& line : buffer->left(boundary).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            bool valid = false;
                            bytes = line.mid(15).trimmed().toLongLong(&valid);
                            if (!valid) bytes = -1;
                        }
                    }
                    if (bytes < 0 || bytes > 6 * 1024 * 1024 || buffer->size() - boundary - 4 < bytes) return;
                    *completed = true;
                    requests.push_back({buffer->left(boundary), buffer->mid(boundary + 4, bytes)});
                    if (stall) return;
                    const auto response = QByteArray("HTTP/1.1 ") + QByteArray::number(responseCode)
                        + " OriginalFixture\r\nContent-Length: " + QByteArray::number(responseBody.size())
                        + "\r\nConnection: close\r\n" + responseHeaders + "\r\n" + responseBody;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    ~Server() override { for (const auto& socket : sockets) if (socket) socket->abort(); }
    bool Start() { return listener.listen(QHostAddress::LocalHost, 0); }
    LoopbackFeedbackEndpoint Endpoint(int timeoutMs = 1000, int maxReply = 65536) const {
        return {QUrl(QStringLiteral("http://127.0.0.1:%1/feedback").arg(listener.serverPort())), timeoutMs, maxReply};
    }
};

DraftResult DraftThroughClient(FeedbackClient& client, DraftInput input) {
    QSignalSpy ready(&client, &FeedbackClient::DraftReady);
    const auto request = client.SubmitDraft(std::move(input));
    if (request == 0 || !ready.wait(3000)) throw std::runtime_error("original client draft did not return");
    for (const auto& result : ready) if (result[0].toULongLong() == request) return qvariant_cast<DraftResult>(result[1]);
    throw std::runtime_error("original client draft correlation mismatch");
}

class WorkerThreadGuard {
public:
    QThread thread;
    ~WorkerThreadGuard() {
        thread.quit();
        if (!thread.wait(3000)) qFatal("original feedback worker fixture did not exit naturally");
    }
};

} // namespace

class FeedbackClientTests final : public QObject {
    Q_OBJECT
private slots:
    void ConsentZeroOneAndCloseClearUseRealLoopback();
    void FullReviewedBodyAndAttachmentsNeverEnterUrlOrCredentials();
    void EndpointValidationAndExplicitSettingsKeepConsentLocal();
    void InFlightCancelRejectsNewRequestsAndDoesNotBlockCoreQueries();
    void ThreeRateLimitAttemptsThenStopAndUnknownTimeoutNeverRetries();
    void RedirectAndResponseBudgetFailClosed();
    void InvalidNetworkBudgetsStayClosedAfterGateReopens();
    void ThrowingWorkerTasksKeepTheirRequestIdsAndReleaseBusy();
};

void FeedbackClientTests::ConsentZeroOneAndCloseClearUseRealLoopback() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "original Qt loopback queue");
    Server server; QVERIFY(server.Start());
    auto client = std::make_unique<FeedbackClient>(queue, server.Endpoint());
    QTRY_VERIFY_WITH_TIMEOUT(!client->IsBusy(), 3000);
    const auto draft = DraftThroughClient(*client, AgentDraft());
    QTRY_COMPARE_WITH_TIMEOUT(client->Snapshot().entries.size(), std::size_t{1}, 3000);
    const auto preview = Take(queue->GetPreview(draft.entry.id), "Qt complete original preview");
    client->ConfirmAndSend(QString::fromStdString(draft.entry.id), QString::fromStdString(preview.delivery.previewSha256), client->EffectiveEndpoint());
    QTRY_VERIFY_WITH_TIMEOUT(!client->IsBusy(), 3000);
    QCOMPARE(server.requests.size(), 0);
    QVERIFY(!queue->Consent() && !client->Preferences().consent);
    auto enabled = client->Preferences(); enabled.consent = true;
    QVERIFY(client->ApplyCommittedPreferences(enabled));
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().consent, 3000);
    QSignalSpy sent(client.get(), &FeedbackClient::LastSentChanged);
    client->ConfirmAndSend(QString::fromStdString(draft.entry.id), QString::fromStdString(preview.delivery.previewSha256), client->EffectiveEndpoint());
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().entries.front().state == QueueState::Sent, 3000);
    QCOMPARE(sent.count(), 1);
    QVERIFY(!client->Snapshot().entries.front().hasPayload);
    const auto pending = DraftThroughClient(*client, UserDraft());
    Q_UNUSED(pending);
    auto disabled = client->Preferences(); disabled.consent = false;
    QVERIFY(client->ApplyCommittedPreferences(disabled));
    QVERIFY(!client->Snapshot().consent);
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().entries.size() == 1 && client->Snapshot().entries.front().state == QueueState::Sent, 3000);
    QCOMPARE(server.requests.size(), 1);
    client.reset();
    QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
}

void FeedbackClientTests::FullReviewedBodyAndAttachmentsNeverEnterUrlOrCredentials() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "Qt reviewed attachment queue");
    Server server; QVERIFY(server.Start());
    FeedbackPreferences preferences; preferences.consent = true;
    auto client = std::make_unique<FeedbackClient>(queue, server.Endpoint(), preferences);
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().consent, 3000);
    auto input = UserDraft();
    input.userDescription = R"(original UI description C:\Users\Synthetic User\private.txt apiKey="body-short-private-key")";
    input.includeLogs = true; input.recentLogs = "original selected log C:\\Users\\Synthetic User\\log.txt";
    input.includeConversation = true; input.conversationSnippet = "original explicitly selected conversation";
    const auto png = Take(PrepareScreenshot(Png(true)), "original canonical Qt attachment");
    input.includeScreenshot = true; input.screenshot = {png.png, png.sha256};
    const auto draft = DraftThroughClient(*client, input);
    const auto preview = Take(queue->GetPreview(draft.entry.id), "all Qt payload fields preview");
    client->ConfirmAndSend(QString::fromStdString(draft.entry.id), QString::fromStdString(preview.delivery.previewSha256), client->EffectiveEndpoint());
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!client->IsBusy(), 3000);
    const auto& request = server.requests.front();
    QVERIFY(request.header.startsWith("POST /feedback HTTP/1.1\r\n"));
    QVERIFY(!request.header.toLower().contains("authorization:"));
    QVERIFY(!request.header.toLower().contains("cookie:"));
    QVERIFY(!request.header.contains("body-short-private-key"));
    QVERIFY(!request.header.contains("original UI description"));
    QVERIFY(!request.body.contains("body-short-private-key"));
    QVERIFY(!request.body.contains("Synthetic User"));
    QVERIFY(!request.body.contains("original-secret-value-123"));
    QVERIFY(request.body.contains(QByteArray::fromStdString(preview.delivery.reportJson)));
    QVERIFY(request.body.contains(QByteArray::fromStdString(*preview.delivery.cleanedLogs)));
    QVERIFY(request.body.contains(QByteArray(reinterpret_cast<const char*>(png.png.data()), static_cast<qsizetype>(png.png.size()))));
    client.reset();
    QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
}

void FeedbackClientTests::EndpointValidationAndExplicitSettingsKeepConsentLocal() {
    Temp temp;
    const auto path = QString::fromStdWString((temp.Path() / L"preferences.ini").wstring());
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("ui/theme"), QStringLiteral("original-theme"));
    auto preferences = FeedbackClient::ReadPreferences(settings);
    QVERIFY(!preferences.consent && preferences.endpointOverride.isEmpty());
    QCOMPARE(PublishedFeedbackEndpoint(), QString::fromUtf8(BuildInfo::FeedbackEndpoint.data(),
        static_cast<qsizetype>(BuildInfo::FeedbackEndpoint.size())));
    const QStringList invalid{QStringLiteral("http://example.invalid/feedback"), QStringLiteral("https://user:password@example.invalid/feedback"),
        QStringLiteral("https://@example.invalid/feedback"),
        QStringLiteral("https://example.invalid/feedback?token=original-private"), QStringLiteral("https://example.invalid/feedback#original-private"),
        QStringLiteral("https://example.invalid/feedback?"), QStringLiteral("https://example.invalid/feedback#")};
    for (const auto& endpoint : invalid) {
        preferences.endpointOverride = endpoint;
        QString error;
        QVERIFY(!FeedbackClient::ValidatePreferences(preferences, &error));
        QVERIFY(!error.isEmpty() && !error.contains("original-private"));
    }
    preferences = {true, QStringLiteral("https://example.invalid/feedback"), 1000};
    QVERIFY(FeedbackClient::WritePreferences(settings, preferences));
    QSettings fresh(path, QSettings::IniFormat);
    QVERIFY(FeedbackClient::ReadPreferences(fresh) == preferences);
    QCOMPARE(fresh.value(QStringLiteral("ui/theme")).toString(), QStringLiteral("original-theme"));
    fresh.setValue(QStringLiteral("feedback/consent"), QStringLiteral("unknown-consent-value")); fresh.sync();
    QVERIFY(!FeedbackClient::ReadPreferences(fresh).consent);
    const QList<QVariant> implicitConsent{QVariant(QStringLiteral("TRUE")), QVariant(QStringLiteral("1")),
        QVariant(QStringLiteral("yes")), QVariant(1), QVariant(QByteArray("true"))};
    for (const auto& value : implicitConsent) {
        fresh.setValue(QStringLiteral("feedback/consent"), value); fresh.sync();
        QSettings explicitRead(path, QSettings::IniFormat);
        QVERIFY(!FeedbackClient::ReadPreferences(explicitRead).consent);
    }
    fresh.setValue(QStringLiteral("feedback/consent"), QStringLiteral("true")); fresh.sync();
    QSettings explicitTrue(path, QSettings::IniFormat);
    QVERIFY(FeedbackClient::ReadPreferences(explicitTrue).consent);
    QVERIFY(!Detail::ValidateEndpoint(QStringLiteral("http://@127.0.0.1/feedback"), true));

    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "empty production endpoint queue");
    Server server; QVERIFY(server.Start());
    auto client = std::make_unique<FeedbackClient>(queue, FeedbackEndpointConfig{}, FeedbackPreferences{});
    const auto draft = DraftThroughClient(*client, AgentDraft());
    auto enabled = client->Preferences(); enabled.consent = true;
    QVERIFY(client->ApplyCommittedPreferences(enabled));
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().consent, 3000);
    const auto preview = Take(queue->GetPreview(draft.entry.id), "empty channel reviewed report");
    client->ConfirmAndSend(QString::fromStdString(draft.entry.id), QString::fromStdString(preview.delivery.previewSha256), {});
    QTRY_VERIFY_WITH_TIMEOUT(!client->IsBusy(), 3000);
    QCOMPARE(server.requests.size(), 0);
    client.reset(); QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
}

void FeedbackClientTests::InFlightCancelRejectsNewRequestsAndDoesNotBlockCoreQueries() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "Qt paused network queue");
    Server server; QVERIFY(server.Start()); server.stall = true;
    FeedbackPreferences preferences; preferences.consent = true;
    auto client = std::make_unique<FeedbackClient>(queue, server.Endpoint(5000), preferences);
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().consent, 3000);
    const auto draft = DraftThroughClient(*client, AgentDraft());
    const auto preview = Take(queue->GetPreview(draft.entry.id), "Qt paused request preview");
    const auto id = QString::fromStdString(draft.entry.id), sha = QString::fromStdString(preview.delivery.previewSha256);
    client->ConfirmAndSend(id, sha, client->EffectiveEndpoint());
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 3000);
    QVERIFY(client->IsBusy());
    QElapsedTimer timer; timer.start();
    QVERIFY(queue->List().Succeeded());
    QVERIFY(queue->Draft(UserDraft(), QDateTime::currentSecsSinceEpoch()).Succeeded());
    QVERIFY(timer.elapsed() < 1000);
    auto disabled = client->Preferences(); disabled.consent = false;
    QVERIFY(client->ApplyCommittedPreferences(disabled));
    QVERIFY(!client->Snapshot().consent);
    client->ConfirmAndSend(id, sha, client->EffectiveEndpoint());
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().entries.empty() && !client->IsBusy(), 3000);
    QCOMPARE(server.requests.size(), 1);
    client.reset(); QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
}

void FeedbackClientTests::ThreeRateLimitAttemptsThenStopAndUnknownTimeoutNeverRetries() {
    Temp temp;
    QueueLimits limits; limits.initialRetrySeconds = 1;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context(), limits), "Qt bounded retry queue");
    Server server; QVERIFY(server.Start()); server.responseCode = 429;
    FeedbackPreferences preferences; preferences.consent = true;
    auto client = std::make_unique<FeedbackClient>(queue, server.Endpoint(), preferences);
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().consent, 3000);
    const auto draft = DraftThroughClient(*client, AgentDraft());
    const auto preview = Take(queue->GetPreview(draft.entry.id), "rate limit preview");
    QTRY_COMPARE_WITH_TIMEOUT(client->Snapshot().entries.size(), std::size_t{1}, 3000);
    client->ConfirmAndSend(QString::fromStdString(draft.entry.id), QString::fromStdString(preview.delivery.previewSha256), client->EffectiveEndpoint());
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().entries.front().state == QueueState::Failed, 8000);
    QCOMPARE(server.requests.size(), 3);
    QCOMPARE(client->Snapshot().entries.front().attempts, std::uint32_t{3});
    client.reset(); QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);

    auto unknownQueue = Take(FeedbackQueue::Open(temp.Path() / L"unknown", Context(), limits), "Qt unknown timeout queue");
    Server stalled; QVERIFY(stalled.Start()); stalled.stall = true;
    client = std::make_unique<FeedbackClient>(unknownQueue, stalled.Endpoint(100), preferences);
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().consent, 3000);
    const auto unknown = DraftThroughClient(*client, AgentDraft());
    const auto unknownPreview = Take(unknownQueue->GetPreview(unknown.entry.id), "unknown timeout complete preview");
    QTRY_COMPARE_WITH_TIMEOUT(client->Snapshot().entries.size(), std::size_t{1}, 3000);
    client->ConfirmAndSend(QString::fromStdString(unknown.entry.id), QString::fromStdString(unknownPreview.delivery.previewSha256), client->EffectiveEndpoint());
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().entries.front().state == QueueState::Failed, 3000);
    QCOMPARE(stalled.requests.size(), 1);
    QVERIFY(!client->Snapshot().entries.front().nextAttemptUnixSeconds);
    QVERIFY(client->Snapshot().entries.front().failureReason.find("结果未知") != std::string::npos);
    client.reset(); QTRY_COMPARE_WITH_TIMEOUT(unknownQueue.use_count(), long{1}, 3000);
}

void FeedbackClientTests::RedirectAndResponseBudgetFailClosed() {
    Temp temp;
    FeedbackPreferences preferences; preferences.consent = true;
    for (const bool oversized : {false, true}) {
        auto queue = Take(FeedbackQueue::Open(temp.Path() / (oversized ? L"budget" : L"redirect"), Context()), "Qt fail closed response queue");
        Server server; QVERIFY(server.Start());
        if (oversized) server.responseBody = QByteArray(4096, 'x');
        else { server.responseCode = 302; server.responseHeaders = "Location: https://example.invalid/other?token=private-fixture\r\n"; }
        auto client = std::make_unique<FeedbackClient>(queue, server.Endpoint(1000, 1024), preferences);
        QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().consent, 3000);
        const auto draft = DraftThroughClient(*client, AgentDraft());
        const auto preview = Take(queue->GetPreview(draft.entry.id), "fail closed response preview");
        QTRY_COMPARE_WITH_TIMEOUT(client->Snapshot().entries.size(), std::size_t{1}, 3000);
        client->ConfirmAndSend(QString::fromStdString(draft.entry.id), QString::fromStdString(preview.delivery.previewSha256), client->EffectiveEndpoint());
        QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().entries.front().state == QueueState::Failed, 3000);
        QCOMPARE(server.requests.size(), 1);
        QVERIFY(client->Snapshot().entries.front().failureReason.find("private-fixture") == std::string::npos);
        client.reset(); QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
    }
}

void FeedbackClientTests::InvalidNetworkBudgetsStayClosedAfterGateReopens() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "Qt invalid network budget queue");
    const auto draft = Take(queue->Draft(UserDraft(), 1000), "invalid budget full draft");
    const auto preview = Take(queue->GetPreview(draft.entry.id), "invalid budget full preview");
    QVERIFY(queue->SetConsent(true).Succeeded());
    QVERIFY(queue->ConfirmPreview(draft.entry.id, preview.delivery.previewSha256).Succeeded());
    Server server; QVERIFY(server.Start());
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    QPointer<QNetworkReply> active;
    auto gate = std::make_shared<Detail::NetworkGate>();
    const std::vector<std::pair<int, int>> invalid{{99, 65536}, {60001, 65536}, {1000, 1023}, {1000, 1024 * 1024 + 1}};
    for (const auto& [timeout, budget] : invalid) {
        {
            std::scoped_lock lock(gate->mutex);
            gate->allowed = false;
            ++gate->epoch;
        }
        Detail::FeedbackTransport transport(network, server.Endpoint().endpoint, timeout, budget, gate, gate->epoch, active);
        QVERIFY(!transport.Configured());
        { std::scoped_lock lock(gate->mutex); gate->allowed = true; }
        QVERIFY(!transport.Configured());
        const auto outcome = Take(queue->TrySend(draft.entry.id, preview.delivery.previewSha256, 1001, transport),
            "re-enabled invalid budget Core gate");
        QVERIFY(outcome.state == AttemptState::NoEndpoint && outcome.entry.attempts == 0);
        // 即使直接调用具体transport，也必须在post前拒绝，不能依赖此前的构造/Configured检查。
        const auto directly = transport.Send(preview.delivery);
        QVERIFY(!directly.sent && !directly.retryable && !active);
        QCOMPARE(server.requests.size(), 0);
    }
    QCOMPARE(Take(queue->GetStatus(draft.entry.id), "invalid budget durable readback").attempts, std::uint32_t{0});
}

void FeedbackClientTests::ThrowingWorkerTasksKeepTheirRequestIdsAndReleaseBusy() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "Qt throwing worker original queue");
    qRegisterMetaType<FeedbackSnapshot>();
    {
        WorkerThreadGuard runner;
        auto* worker = new Detail::FeedbackWorker(queue, {}, false, std::make_shared<Detail::NetworkGate>());
        worker->moveToThread(&runner.thread);
        connect(&runner.thread, &QThread::finished, worker, &QObject::deleteLater);
        QObject observer;
        struct Completion { quint64 request; bool success; QString message; };
        QList<Completion> completed;
        QList<bool> busy;
        int snapshots = 0;
        connect(worker, &Detail::FeedbackWorker::Finished, &observer,
            [&](quint64 request, bool success, const QString& message) { completed.push_back({request, success, message}); },
            Qt::QueuedConnection);
        connect(worker, &Detail::FeedbackWorker::BusyChanged, &observer,
            [&](bool value) { busy.push_back(value); }, Qt::QueuedConnection);
        connect(worker, &Detail::FeedbackWorker::SnapshotReady, &observer,
            [&](const FeedbackSnapshot&) { ++snapshots; }, Qt::QueuedConnection);
        runner.thread.start(QThread::LowPriority);
        QVERIFY(QMetaObject::invokeMethod(worker, [worker] {
            Detail::FeedbackWorkerTestAccess::Enqueue(*worker, 401, [] { throw std::runtime_error("original-private-exception-details"); });
            Detail::FeedbackWorkerTestAccess::Enqueue(*worker, 402, [] { throw 17; });
            worker->List(403);
        }, Qt::QueuedConnection));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 3, 3000);
        QCOMPARE(completed[0].request, quint64{401});
        QCOMPARE(completed[1].request, quint64{402});
        QCOMPARE(completed[2].request, quint64{403});
        QVERIFY(!completed[0].success && !completed[1].success && completed[2].success);
        QVERIFY(!completed[0].message.contains(QStringLiteral("original-private-exception-details")));
        QTRY_VERIFY_WITH_TIMEOUT(!busy.empty() && busy.front() && !busy.back(), 3000);
        QCOMPARE(snapshots, 1);
    }
    QCOMPARE(queue.use_count(), long{1});
}

QTEST_MAIN(FeedbackClientTests)
#include "FeedbackClientTests.moc"
