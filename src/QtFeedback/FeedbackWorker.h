#pragma once

#include "FeedbackTransport.h"

#include <QObject>
#include <QPointer>

#include <deque>
#include <functional>
#include <map>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace War3Map::QtFeedback::Detail {

class FeedbackWorker final : public QObject {
    Q_OBJECT
public:
    FeedbackWorker(std::shared_ptr<Feedback::FeedbackQueue> queue, FeedbackEndpointConfig configuration,
        bool loopback, std::shared_ptr<NetworkGate> gate);
    void Apply(FeedbackPreferences preferences, quint64 epoch, bool clearUnsent, bool initial);
    void Draft(quint64 request, Feedback::DraftInput input);
    void List(quint64 request);
    void Preview(quint64 request, QString id);
    void Send(quint64 request, QString id, QString sha, QString endpoint, quint64 epoch);
    void Delete(quint64 request, QString id);
    void Export(quint64 request, QString id, QString target);
    // 不进入普通操作队列，能在网络事件循环内立即取消，不碰Core锁。
    void CancelActive();

signals:
    void SnapshotReady(const War3Map::QtFeedback::FeedbackSnapshot& snapshot);
    void PreferencesApplied(quint64 epoch, bool success, const QString& message);
    void BusyChanged(bool busy);
    void DraftReady(quint64 request, const War3Map::Feedback::DraftResult& draft);
    void PreviewReady(quint64 request, const War3Map::Feedback::Preview& preview, const QString& endpoint);
    void SendFinished(quint64 request, const War3Map::Feedback::SendOutcome& outcome);
    void Finished(quint64 request, bool success, const QString& message);

private:
    // 原创回归驱动真实工作线程中的异常任务，不增加产品侧配置或发送入口。
    friend class FeedbackWorkerTestAccess;
    struct Review { QString sha; QString endpoint; quint64 epoch = 0; };
    struct Operation { quint64 request = 0; std::function<void()> execute; };
    void Enqueue(std::function<void()> operation, quint64 request = 0);
    void RunNext();
    bool PublishSnapshot();
    void ScheduleRetries();
    void RetryDue();
    void SendNow(quint64 request, const QString& id, const Review& review, bool automatic);
    bool Available(quint64 request);
    QString Endpoint() const;
    QNetworkAccessManager& Network();

    std::shared_ptr<Feedback::FeedbackQueue> queue_;
    FeedbackEndpointConfig configuration_;
    FeedbackPreferences preferences_;
    bool loopback_ = false;
    std::shared_ptr<NetworkGate> gate_;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> activeReply_;
    QTimer* retryTimer_ = nullptr;
    std::deque<Operation> operations_;
    std::map<QString, Review> reviewed_;
    bool running_ = false;
    bool retryQueued_ = false;
};

} // namespace War3Map::QtFeedback::Detail
