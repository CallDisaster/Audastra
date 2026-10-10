#pragma once

#include "FeedbackClient.h"

#include <QPointer>

#include <mutex>

class QNetworkAccessManager;
class QNetworkReply;

namespace War3Map::QtFeedback::Detail {

struct NetworkGate {
    std::mutex mutex;
    bool allowed = false;
    bool stopping = false;
    quint64 epoch = 0;
};

[[nodiscard]] bool ValidateEndpoint(const QString& text, bool loopback, QString* error = nullptr);
[[nodiscard]] bool ValidNetworkLimits(int timeoutMs, int maxReplyBytes) noexcept;
[[nodiscard]] bool GateOpen(const std::shared_ptr<NetworkGate>& gate, quint64 epoch);

class FeedbackTransport final : public Feedback::IReportTransport {
public:
    FeedbackTransport(QNetworkAccessManager& network, QUrl endpoint, int timeoutMs, int maxReplyBytes,
        std::shared_ptr<NetworkGate> gate, quint64 epoch, QPointer<QNetworkReply>& activeReply);
    [[nodiscard]] bool Configured() const noexcept override;
    [[nodiscard]] Feedback::TransportResult Send(const Feedback::Delivery& delivery) override;
private:
    QNetworkAccessManager& network_;
    QUrl endpoint_;
    int timeoutMs_;
    int maxReplyBytes_;
    std::shared_ptr<NetworkGate> gate_;
    quint64 epoch_;
    QPointer<QNetworkReply>& activeReply_;
};

} // namespace War3Map::QtFeedback::Detail
