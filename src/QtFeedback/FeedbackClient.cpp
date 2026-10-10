#include "FeedbackClient.h"
#include "FeedbackWorker.h"

#include <QThread>
#include <QTimer>

#include <limits>
#include <set>

namespace War3Map::QtFeedback {

class FeedbackClient::Impl {
public:
    FeedbackClient* owner = nullptr;
    FeedbackPreferences preferences;
    FeedbackEndpointConfig configuration;
    FeedbackSnapshot snapshot;
    QString configurationError;
    bool loopback = false;
    bool busy = false;
    bool workerBusy = false;
    std::shared_ptr<Detail::NetworkGate> gate = std::make_shared<Detail::NetworkGate>();
    QThread* workerThread = nullptr;
    Detail::FeedbackWorker* worker = nullptr;
    std::set<quint64> requests;
    quint64 nextRequest = 1;

    quint64 Begin() {
        if (requests.size() >= 128 || nextRequest == (std::numeric_limits<quint64>::max)()) {
            QTimer::singleShot(0, owner, [q = owner] {
                emit q->OperationFinished(0, false, QStringLiteral("反馈操作队列已满，请等待当前操作完成。"));
            });
            return 0;
        }
        const auto request = nextRequest++;
        requests.insert(request);
        UpdateBusy();
        return request;
    }

    void UpdateBusy() {
        const bool next = workerBusy || !requests.empty();
        if (busy == next) return;
        busy = next;
        emit owner->BusyChanged(next);
    }

    quint64 Epoch() const { std::scoped_lock lock(gate->mutex); return gate->epoch; }

    void Initialize(std::shared_ptr<Feedback::FeedbackQueue> queue) {
        qRegisterMetaType<Feedback::DraftInput>();
        qRegisterMetaType<Feedback::DraftResult>();
        qRegisterMetaType<Feedback::Preview>();
        qRegisterMetaType<Feedback::SendOutcome>();
        qRegisterMetaType<FeedbackSnapshot>();
        if (!Detail::ValidNetworkLimits(configuration.requestTimeoutMs, configuration.maxReplyBytes)) {
            configurationError = QStringLiteral("反馈网络容量或等待时间配置无效，通道保持关闭。");
            configuration.publishedEndpoint.clear();
            preferences.endpointOverride.clear();
            preferences.consent = false;
        }
        QString error;
        if (!Detail::ValidateEndpoint(configuration.publishedEndpoint, loopback, &error)) {
            configurationError = error;
            configuration.publishedEndpoint.clear();
        }
        if (!FeedbackClient::ValidatePreferences(preferences, &error) || (loopback && !preferences.endpointOverride.isEmpty())) {
            configurationError = error.isEmpty() ? QStringLiteral("回环测试不能覆盖到其他端点。") : error;
            preferences.endpointOverride.clear();
            preferences.consent = false;
        }
        workerThread = new QThread;
        workerThread->setObjectName(QStringLiteral("AudastraFeedbackWorker"));
        worker = new Detail::FeedbackWorker(std::move(queue), configuration, loopback, gate);
        worker->moveToThread(workerThread);
        QObject::connect(workerThread, &QThread::finished, worker, &QObject::deleteLater);
        QObject::connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        QObject::connect(worker, &Detail::FeedbackWorker::SnapshotReady, owner, [this](const FeedbackSnapshot& next) {
            snapshot = next;
            snapshot.consent = preferences.consent && Detail::GateOpen(gate, Epoch()) && next.consent;
            if (next.lastSentUnixSeconds && *next.lastSentUnixSeconds > preferences.lastSentUnixSeconds) {
                preferences.lastSentUnixSeconds = *next.lastSentUnixSeconds;
                emit owner->LastSentChanged(preferences.lastSentUnixSeconds);
            }
            emit owner->SnapshotChanged();
        });
        QObject::connect(worker, &Detail::FeedbackWorker::PreferencesApplied, owner,
            [this](quint64 epoch, bool success, const QString& message) {
                if (epoch != Epoch()) return;
                emit owner->PreferencesChanged();
                if (!success) emit owner->OperationFinished(0, false, message);
            });
        QObject::connect(worker, &Detail::FeedbackWorker::BusyChanged, owner, [this](bool next) {
            workerBusy = next;
            UpdateBusy();
        });
        QObject::connect(worker, &Detail::FeedbackWorker::DraftReady, owner, &FeedbackClient::DraftReady);
        QObject::connect(worker, &Detail::FeedbackWorker::PreviewReady, owner, &FeedbackClient::PreviewReady);
        QObject::connect(worker, &Detail::FeedbackWorker::SendFinished, owner, &FeedbackClient::SendFinished);
        QObject::connect(worker, &Detail::FeedbackWorker::Finished, owner,
            [this](quint64 request, bool success, const QString& message) {
                if (request != 0) requests.erase(request);
                UpdateBusy();
                emit owner->OperationFinished(request, success, message);
            });
        workerThread->start(QThread::LowPriority);
        const auto initialPreferences = preferences;
        QMetaObject::invokeMethod(worker, [target = worker, initialPreferences] {
            target->Apply(initialPreferences, 0, false, true);
        }, Qt::QueuedConnection);
    }
};

FeedbackClient::FeedbackClient(std::shared_ptr<Feedback::FeedbackQueue> queue, FeedbackEndpointConfig endpoint,
    FeedbackPreferences preferences, QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    impl_->configuration = std::move(endpoint);
    impl_->preferences = std::move(preferences);
    impl_->Initialize(std::move(queue));
}

FeedbackClient::FeedbackClient(std::shared_ptr<Feedback::FeedbackQueue> queue, LoopbackFeedbackEndpoint endpoint,
    FeedbackPreferences preferences, QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    impl_->loopback = true;
    impl_->configuration = {endpoint.endpoint.toString(QUrl::FullyEncoded), endpoint.requestTimeoutMs, endpoint.maxReplyBytes};
    impl_->preferences = std::move(preferences);
    impl_->Initialize(std::move(queue));
}

FeedbackClient::~FeedbackClient() {
    {
        std::scoped_lock lock(impl_->gate->mutex);
        impl_->gate->allowed = false;
        impl_->gate->stopping = true;
        ++impl_->gate->epoch;
    }
    // 撤销/退出异步执行，应用关闭不等待离线请求；不terminate或销毁仍运行的QThread。
    QMetaObject::invokeMethod(impl_->worker, [worker = impl_->worker] {
        worker->CancelActive();
        QThread::currentThread()->quit();
    }, Qt::QueuedConnection);
}

const FeedbackPreferences& FeedbackClient::Preferences() const noexcept { return impl_->preferences; }
const FeedbackSnapshot& FeedbackClient::Snapshot() const noexcept { return impl_->snapshot; }
QString FeedbackClient::ConfigurationError() const { return impl_->configurationError; }
QString FeedbackClient::PublishedEndpoint() const { return impl_->configuration.publishedEndpoint; }
bool FeedbackClient::IsBusy() const noexcept { return impl_->busy; }

QString FeedbackClient::EffectiveEndpoint() const {
    return impl_->preferences.endpointOverride.isEmpty() ? impl_->configuration.publishedEndpoint : impl_->preferences.endpointOverride;
}

bool FeedbackClient::ApplyCommittedPreferences(const FeedbackPreferences& preferences, QString* error) {
    if (!ValidatePreferences(preferences, error)) return false;
    if (impl_->loopback && !preferences.endpointOverride.isEmpty()) {
        if (error) *error = QStringLiteral("回环测试不能覆盖到其他端点。");
        return false;
    }
    const auto previous = impl_->preferences;
    const bool permissionChanged = previous.consent != preferences.consent || previous.endpointOverride != preferences.endpointOverride;
    impl_->preferences = preferences;
    if (!permissionChanged) { emit PreferencesChanged(); return true; }
    quint64 epoch;
    {
        std::scoped_lock lock(impl_->gate->mutex);
        impl_->gate->allowed = false;
        epoch = ++impl_->gate->epoch;
    }
    impl_->snapshot.consent = false;
    emit PreferencesChanged();
    emit SnapshotChanged();
    const bool clearUnsent = previous.consent && !preferences.consent;
    QMetaObject::invokeMethod(impl_->worker, [worker = impl_->worker, preferences, epoch, clearUnsent] {
        worker->CancelActive();
        worker->Apply(preferences, epoch, clearUnsent, false);
    }, Qt::QueuedConnection);
    return true;
}

quint64 FeedbackClient::SubmitDraft(Feedback::DraftInput input) {
    const auto request = impl_->Begin();
    if (request) QMetaObject::invokeMethod(impl_->worker, [worker = impl_->worker, request, input = std::move(input)]() mutable {
        worker->Draft(request, std::move(input));
    }, Qt::QueuedConnection);
    return request;
}

quint64 FeedbackClient::RefreshQueue() {
    const auto request = impl_->Begin();
    if (request) QMetaObject::invokeMethod(impl_->worker, [worker = impl_->worker, request] { worker->List(request); }, Qt::QueuedConnection);
    return request;
}

quint64 FeedbackClient::RequestPreview(const QString& reportId) {
    const auto request = impl_->Begin();
    if (request) QMetaObject::invokeMethod(impl_->worker, [worker = impl_->worker, request, reportId] { worker->Preview(request, reportId); }, Qt::QueuedConnection);
    return request;
}

quint64 FeedbackClient::ConfirmAndSend(const QString& reportId, const QString& sha, const QString& endpoint) {
    const auto request = impl_->Begin();
    const auto epoch = impl_->Epoch();
    if (request) QMetaObject::invokeMethod(impl_->worker, [worker = impl_->worker, request, reportId, sha, endpoint, epoch] {
        worker->Send(request, reportId, sha, endpoint, epoch);
    }, Qt::QueuedConnection);
    return request;
}

quint64 FeedbackClient::DeleteReport(const QString& reportId) {
    const auto request = impl_->Begin();
    if (request) QMetaObject::invokeMethod(impl_->worker, [worker = impl_->worker, request, reportId] { worker->Delete(request, reportId); }, Qt::QueuedConnection);
    return request;
}

quint64 FeedbackClient::ExportReport(const QString& reportId, const QString& target) {
    const auto request = impl_->Begin();
    if (request) QMetaObject::invokeMethod(impl_->worker, [worker = impl_->worker, request, reportId, target] { worker->Export(request, reportId, target); }, Qt::QueuedConnection);
    return request;
}

} // namespace War3Map::QtFeedback
