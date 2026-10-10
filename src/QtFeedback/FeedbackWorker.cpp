#include "FeedbackWorker.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QTimer>

#include <algorithm>

namespace War3Map::QtFeedback::Detail {
namespace {

QString Message(const std::vector<Diagnostic>& diagnostics) {
    QString message;
    for (const auto& diagnostic : diagnostics) {
        if (!message.isEmpty()) message += QLatin1Char('\n');
        message += QString::fromStdString(diagnostic.code + ": " + diagnostic.message);
        if (message.size() >= 4096) { message.truncate(4096); break; }
    }
    return message.isEmpty() ? QStringLiteral("反馈操作失败，未宣称已保存或上报。") : message;
}

} // namespace

FeedbackWorker::FeedbackWorker(std::shared_ptr<Feedback::FeedbackQueue> queue, FeedbackEndpointConfig configuration,
    bool loopback, std::shared_ptr<NetworkGate> gate)
    : queue_(std::move(queue)), configuration_(std::move(configuration)), loopback_(loopback), gate_(std::move(gate)) {}

bool FeedbackWorker::Available(quint64 request) {
    if (queue_) return true;
    emit Finished(request, false, QStringLiteral("本地反馈队列不可用，未发送任何报告。"));
    return false;
}

QString FeedbackWorker::Endpoint() const {
    return preferences_.endpointOverride.isEmpty() ? configuration_.publishedEndpoint : preferences_.endpointOverride;
}

void FeedbackWorker::Enqueue(std::function<void()> operation, quint64 request) {
    { std::scoped_lock lock(gate_->mutex); if (gate_->stopping) return; }
    operations_.push_back({request, std::move(operation)});
    if (!running_) {
        running_ = true;
        emit BusyChanged(true);
        QTimer::singleShot(0, this, &FeedbackWorker::RunNext);
    }
}

void FeedbackWorker::RunNext() {
    { std::scoped_lock lock(gate_->mutex); if (gate_->stopping) operations_.clear(); }
    if (operations_.empty()) { running_ = false; emit BusyChanged(false); return; }
    auto operation = std::move(operations_.front());
    operations_.pop_front();
    try { operation.execute(); }
    catch (...) {
        CancelActive();
        if (retryTimer_) retryTimer_->stop();
        reviewed_.clear();
        emit Finished(operation.request, false, QStringLiteral("反馈工作线程失败，未自动重复发送。"));
    }
    // 嵌套网络事件循环只收Enqueue；不重入当前Core操作，后续请求仍可继续执行。
    QTimer::singleShot(0, this, &FeedbackWorker::RunNext);
}

void FeedbackWorker::Apply(FeedbackPreferences preferences, quint64 epoch, bool clearUnsent, bool initial) {
    Enqueue([this, preferences = std::move(preferences), epoch, clearUnsent, initial] {
        const auto oldEndpoint = Endpoint();
        preferences_ = preferences;
        if (oldEndpoint != Endpoint() || !preferences.consent) reviewed_.clear();
        if (retryTimer_) retryTimer_->stop();
        if (!queue_) {
            emit PreferencesApplied(epoch, false, QStringLiteral("本地反馈队列不可用，网络上报保持关闭。"));
            return;
        }
        Status status = Status::Ok();
        if (preferences.consent) status = queue_->SetConsent(true);
        else if (clearUnsent || (initial && queue_->Consent())) status = queue_->SetConsent(false);
        {
            std::scoped_lock lock(gate_->mutex);
            if (gate_->epoch == epoch && !gate_->stopping) gate_->allowed = status.Succeeded() && preferences.consent;
        }
        emit PreferencesApplied(epoch, status.Succeeded(), status.Succeeded() ? QString()
            : Message(status.diagnostics));
        PublishSnapshot();
    });
}

void FeedbackWorker::Draft(quint64 request, Feedback::DraftInput input) {
    Enqueue([this, request, input = std::move(input)] {
        if (!Available(request)) return;
        const auto draft = queue_->Draft(input, QDateTime::currentSecsSinceEpoch());
        if (draft.Succeeded()) {
            emit DraftReady(request, *draft.value);
            emit Finished(request, true, QString::fromStdString(draft.value->message));
            PublishSnapshot();
        } else emit Finished(request, false, Message(draft.diagnostics));
    }, request);
}

void FeedbackWorker::List(quint64 request) {
    Enqueue([this, request] {
        if (!Available(request)) return;
        const bool read = PublishSnapshot();
        emit Finished(request, read, read ? QString() : QStringLiteral("本地反馈队列读取失败。"));
    }, request);
}

void FeedbackWorker::Preview(quint64 request, QString id) {
    Enqueue([this, request, id = std::move(id)] {
        if (!Available(request)) return;
        const auto preview = queue_->GetPreview(id.toStdString());
        if (preview.Succeeded()) {
            emit PreviewReady(request, *preview.value, Endpoint());
            emit Finished(request, true, {});
        } else emit Finished(request, false, Message(preview.diagnostics));
    }, request);
}

QNetworkAccessManager& FeedbackWorker::Network() {
    if (!network_) {
        network_ = new QNetworkAccessManager(this);
        if (loopback_) network_->setProxy(QNetworkProxy::NoProxy);
        connect(network_, &QNetworkAccessManager::authenticationRequired, this,
            [](QNetworkReply* reply, QAuthenticator*) { reply->abort(); });
        connect(network_, &QNetworkAccessManager::proxyAuthenticationRequired, this,
            [this](const QNetworkProxy&, QAuthenticator*) { CancelActive(); });
    }
    return *network_;
}

void FeedbackWorker::Send(quint64 request, QString id, QString sha, QString endpoint, quint64 epoch) {
    Enqueue([this, request, id = std::move(id), review = Review{std::move(sha), std::move(endpoint), epoch}] {
        SendNow(request, id, review, false);
    }, request);
}

void FeedbackWorker::SendNow(quint64 request, const QString& id, const Review& review, bool automatic) {
    if (!Available(request)) return;
    if (!GateOpen(gate_, review.epoch)) {
        emit Finished(request, false, QStringLiteral("未同意上报或同意正在更新；报告只保留在本地。"));
        return;
    }
    if (review.endpoint != Endpoint() || review.endpoint.isEmpty()) {
        reviewed_.erase(id);
        emit Finished(request, false, QStringLiteral("上报通道为空或已经变化，请重新查看完整预览。"));
        return;
    }
    if (!automatic) {
        const auto confirmed = queue_->ConfirmPreview(id.toStdString(), review.sha.toStdString());
        if (!confirmed.Succeeded()) {
            emit Finished(request, false, Message(confirmed.diagnostics));
            PublishSnapshot();
            return;
        }
        reviewed_[id] = review;
    }
    FeedbackTransport transport(Network(), QUrl(Endpoint()), configuration_.requestTimeoutMs,
        configuration_.maxReplyBytes, gate_, review.epoch, activeReply_);
    const auto outcome = queue_->TrySend(id.toStdString(), review.sha.toStdString(), QDateTime::currentSecsSinceEpoch(), transport);
    if (outcome.Succeeded()) {
        emit SendFinished(request, *outcome.value);
        const auto state = outcome.value->state;
        if (state != Feedback::AttemptState::RetryScheduled && state != Feedback::AttemptState::NotDue) reviewed_.erase(id);
        emit Finished(request, state == Feedback::AttemptState::Sent,
            QString::fromStdString(outcome.value->message));
    } else {
        reviewed_.erase(id);
        emit Finished(request, false, Message(outcome.diagnostics));
    }
    PublishSnapshot();
}

void FeedbackWorker::Delete(quint64 request, QString id) {
    Enqueue([this, request, id = std::move(id)] {
        if (!Available(request)) return;
        const auto status = queue_->Delete(id.toStdString());
        if (status.Succeeded()) reviewed_.erase(id);
        emit Finished(request, status.Succeeded(), status.Succeeded() ? QStringLiteral("报告内容已删除。") : Message(status.diagnostics));
        PublishSnapshot();
    }, request);
}

void FeedbackWorker::Export(quint64 request, QString id, QString target) {
    Enqueue([this, request, id = std::move(id), target = std::move(target)] {
        if (!Available(request)) return;
        const auto result = queue_->Export(id.toStdString(), std::filesystem::path(target.toStdWString()));
        emit Finished(request, result.Succeeded(), result.Succeeded() ? QStringLiteral("报告包已导出。") : Message(result.diagnostics));
    }, request);
}

void FeedbackWorker::CancelActive() { if (activeReply_) activeReply_->abort(); }

bool FeedbackWorker::PublishSnapshot() {
    if (!queue_) return false;
    const auto list = queue_->List(0, 128);
    if (!list.Succeeded()) { emit Finished(0, false, Message(list.diagnostics)); return false; }
    FeedbackSnapshot snapshot{*list.value, queue_->Consent(), queue_->LastSentUnixSeconds()};
    emit SnapshotReady(snapshot);
    ScheduleRetries();
    return true;
}

void FeedbackWorker::ScheduleRetries() {
    if (!retryTimer_) {
        retryTimer_ = new QTimer(this);
        retryTimer_->setSingleShot(true);
        connect(retryTimer_, &QTimer::timeout, this, [this] {
            if (retryQueued_) return;
            retryQueued_ = true;
            Enqueue([this] { retryQueued_ = false; RetryDue(); });
        });
    }
    retryTimer_->stop();
    if (!queue_ || !queue_->Consent() || reviewed_.empty()) return;
    const auto list = queue_->List(0, 128);
    if (!list.Succeeded()) return;
    std::optional<std::int64_t> earliest;
    for (const auto& entry : *list.value) {
        if (entry.state != Feedback::QueueState::Pending || !entry.nextAttemptUnixSeconds) continue;
        const auto found = reviewed_.find(QString::fromStdString(entry.id));
        if (found == reviewed_.end() || !GateOpen(gate_, found->second.epoch)) continue;
        if (!earliest || *earliest > *entry.nextAttemptUnixSeconds) earliest = entry.nextAttemptUnixSeconds;
    }
    if (earliest) {
        const auto milliseconds = (std::clamp)((*earliest - QDateTime::currentSecsSinceEpoch()) * 1000,
            std::int64_t{100}, std::int64_t{60000});
        retryTimer_->start(static_cast<int>(milliseconds));
    }
}

void FeedbackWorker::RetryDue() {
    if (!queue_) return;
    const auto list = queue_->List(0, 128);
    if (!list.Succeeded()) return;
    for (const auto& entry : *list.value) {
        if (entry.state != Feedback::QueueState::Pending || !entry.nextAttemptUnixSeconds
            || *entry.nextAttemptUnixSeconds > QDateTime::currentSecsSinceEpoch()) continue;
        const auto id = QString::fromStdString(entry.id);
        const auto found = reviewed_.find(id);
        if (found == reviewed_.end()) continue;
        const auto review = found->second;
        SendNow(0, id, review, true);
    }
    ScheduleRetries();
}

} // namespace War3Map::QtFeedback::Detail
