#pragma once

#include "War3MapCore/Feedback/Queue.h"

#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

class QSettings;

namespace War3Map::QtFeedback {

struct FeedbackPreferences {
    bool consent = false;
    QString endpointOverride;
    qint64 lastSentUnixSeconds = 0;
    [[nodiscard]] bool operator==(const FeedbackPreferences&) const = default;
};

struct FeedbackEndpointConfig {
    // 发布构建常量由宿主注入。空地址只允许本地起草/导出。
    QString publishedEndpoint;
    int requestTimeoutMs = 15000;
    int maxReplyBytes = 64 * 1024;
};

struct LoopbackFeedbackEndpoint {
    // 唯一明文 HTTP 入口：测试显式注入数字回环地址；不接受重定向。
    QUrl endpoint;
    int requestTimeoutMs = 1000;
    int maxReplyBytes = 64 * 1024;
};

struct FeedbackSnapshot {
    std::vector<Feedback::EntrySummary> entries;
    bool consent = false;
    std::optional<std::int64_t> lastSentUnixSeconds;
};

// 不读取默认QSettings、不创建第二条队列、不采集作者数据。
// 所有Core/网络操作排入单独工作线程，网络撤销gate不等待Core互斥锁。
class FeedbackClient final : public QObject {
    Q_OBJECT
public:
    FeedbackClient(std::shared_ptr<Feedback::FeedbackQueue> queue,
        FeedbackEndpointConfig endpoint, FeedbackPreferences preferences = {}, QObject* parent = nullptr);
    FeedbackClient(std::shared_ptr<Feedback::FeedbackQueue> queue,
        LoopbackFeedbackEndpoint endpoint, FeedbackPreferences preferences = {}, QObject* parent = nullptr);
    ~FeedbackClient() override;

    [[nodiscard]] static FeedbackPreferences ReadPreferences(QSettings& rootSettings);
    // 三个明确的本机key，不进入P7迁移。宿主统一提交/回滚；首次QSettings错误是sticky。
    [[nodiscard]] static bool WritePreferences(QSettings& rootSettings, const FeedbackPreferences& preferences,
        QString* error = nullptr);
    [[nodiscard]] static bool ValidatePreferences(const FeedbackPreferences& preferences, QString* error = nullptr);
    [[nodiscard]] const FeedbackPreferences& Preferences() const noexcept;
    [[nodiscard]] const FeedbackSnapshot& Snapshot() const noexcept;
    [[nodiscard]] QString EffectiveEndpoint() const;
    [[nodiscard]] QString PublishedEndpoint() const;
    [[nodiscard]] QString ConfigurationError() const;
    [[nodiscard]] bool IsBusy() const noexcept;

    // 只接受宿主已经持久化成功的偏好。启用等待Core确认，关闭立即撤销网络gate。
    // 端点变化撤销正在发送的请求；新目的地必须重新查看完整预览。
    [[nodiscard]] bool ApplyCommittedPreferences(const FeedbackPreferences& preferences, QString* error = nullptr);
    quint64 SubmitDraft(Feedback::DraftInput input);
    quint64 RefreshQueue();
    quint64 RequestPreview(const QString& reportId);
    quint64 ConfirmAndSend(const QString& reportId, const QString& previewSha256,
        const QString& reviewedEndpoint);
    quint64 DeleteReport(const QString& reportId);
    quint64 ExportReport(const QString& reportId, const QString& nonExistingTarget);

signals:
    void SnapshotChanged();
    void PreferencesChanged();
    void BusyChanged(bool busy);
    void DraftReady(quint64 requestId, const War3Map::Feedback::DraftResult& draft);
    void PreviewReady(quint64 requestId, const War3Map::Feedback::Preview& preview, const QString& endpoint);
    void SendFinished(quint64 requestId, const War3Map::Feedback::SendOutcome& outcome);
    void OperationFinished(quint64 requestId, bool success, const QString& message);
    void LastSentChanged(qint64 unixSeconds);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 使用统一BuildInfo发布配置，默认空；不得带query或凭据。
[[nodiscard]] QString PublishedFeedbackEndpoint();

} // namespace War3Map::QtFeedback

Q_DECLARE_METATYPE(War3Map::Feedback::DraftInput)
Q_DECLARE_METATYPE(War3Map::Feedback::DraftResult)
Q_DECLARE_METATYPE(War3Map::Feedback::Preview)
Q_DECLARE_METATYPE(War3Map::Feedback::SendOutcome)
Q_DECLARE_METATYPE(War3Map::QtFeedback::FeedbackSnapshot)
