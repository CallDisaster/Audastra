#include "FeedbackTransport.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QHttpMultiPart>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>

namespace War3Map::QtFeedback::Detail {

bool ValidateEndpoint(const QString& text, bool loopback, QString* error) {
    auto fail = [&] {
        if (error) *error = loopback ? QStringLiteral("测试通道只接受不含凭据、查询或片段的数字回环 HTTP 地址。")
            : QStringLiteral("反馈端点须为 HTTPS 地址，且不能含凭据、查询参数或片段。");
        return false;
    };
    if (text.isEmpty()) return true;
    if (text.size() > 2048) return fail();
    for (const auto character : text) if (character.isSpace() || character.unicode() < 0x20) return fail();
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.isRelative() || url.host().isEmpty() || !url.userInfo().isEmpty()
        || url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@'))
        || url.hasQuery() || url.hasFragment() || url.port() == 0) return fail();
    if (loopback) {
        QHostAddress address;
        if (url.scheme() != QStringLiteral("http") || !address.setAddress(url.host()) || !address.isLoopback()) return fail();
    } else if (url.scheme() != QStringLiteral("https")) return fail();
    return true;
}

bool ValidNetworkLimits(int timeoutMs, int maxReplyBytes) noexcept {
    return timeoutMs >= 100 && timeoutMs <= 60000 && maxReplyBytes >= 1024 && maxReplyBytes <= 1024 * 1024;
}

bool GateOpen(const std::shared_ptr<NetworkGate>& gate, quint64 epoch) {
    std::scoped_lock lock(gate->mutex);
    return gate->allowed && !gate->stopping && gate->epoch == epoch;
}

FeedbackTransport::FeedbackTransport(QNetworkAccessManager& network, QUrl endpoint, int timeoutMs,
    int maxReplyBytes, std::shared_ptr<NetworkGate> gate, quint64 epoch, QPointer<QNetworkReply>& activeReply)
    : network_(network), endpoint_(std::move(endpoint)), timeoutMs_(timeoutMs), maxReplyBytes_(maxReplyBytes),
      gate_(std::move(gate)), epoch_(epoch), activeReply_(activeReply) {}

bool FeedbackTransport::Configured() const noexcept {
    return ValidNetworkLimits(timeoutMs_, maxReplyBytes_) && !endpoint_.isEmpty()
        && endpoint_.isValid() && GateOpen(gate_, epoch_);
}

Feedback::TransportResult FeedbackTransport::Send(const Feedback::Delivery& delivery) {
    if (!ValidNetworkLimits(timeoutMs_, maxReplyBytes_))
        return {false, false, "反馈网络容量或等待时间配置无效，没有开始网络请求。"};
    // 载荷全部来自Core完整预览。只有固定成员名，报告不进入URL或认证字段。
    auto multipart = std::make_unique<QHttpMultiPart>(QHttpMultiPart::FormDataType);
    auto append = [&](const QString& name, const QString& type, const QByteArray& bytes) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
            QStringLiteral("form-data; name=\"%1\"; filename=\"%1\"").arg(name));
        part.setHeader(QNetworkRequest::ContentTypeHeader, type);
        part.setBody(bytes);
        multipart->append(part);
    };
    append(QStringLiteral("report.json"), QStringLiteral("application/json; charset=utf-8"),
        QByteArray::fromStdString(delivery.reportJson));
    if (delivery.cleanedLogs) append(QStringLiteral("logs.txt"), QStringLiteral("text/plain; charset=utf-8"),
        QByteArray::fromStdString(*delivery.cleanedLogs));
    if (!delivery.userReviewedScreenshotPng.empty()) append(QStringLiteral("screenshot.png"), QStringLiteral("image/png"),
        QByteArray(reinterpret_cast<const char*>(delivery.userReviewedScreenshotPng.data()),
            static_cast<qsizetype>(delivery.userReviewedScreenshotPng.size())));

    QNetworkRequest request(endpoint_);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "Audastra-Feedback/1");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);

    QNetworkReply* reply = nullptr;
    {
        // 与UI撤销同意共用短锁。关闭返回后任何旧epoch都不能开始新post。
        std::scoped_lock lock(gate_->mutex);
        if (!gate_->allowed || gate_->stopping || gate_->epoch != epoch_)
            return {false, false, "上报同意已关闭或通道已变化，没有开始网络请求。"};
        reply = network_.post(request, multipart.get());
        reply->setReadBufferSize(8192);
        multipart.release()->setParent(reply);
        activeReply_ = reply;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    qint64 received = 0;
    bool exceeded = false;
    bool timedOut = false;
    auto consume = [&] {
        // 只统计并丢弃回应，不保存/回显服务端提供的文本、URL或凭据。
        while (reply->bytesAvailable() > 0) {
            const auto bytes = reply->read((std::min)(reply->bytesAvailable(), qint64{8192}));
            received += bytes.size();
            if (received > maxReplyBytes_) { exceeded = true; reply->abort(); break; }
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, consume);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { timedOut = true; reply->abort(); loop.quit(); });
    timeout.start(timeoutMs_);
    if (!reply->isFinished()) loop.exec();
    timeout.stop();
    consume();
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto networkError = reply->error();
    activeReply_.clear();
    reply->deleteLater();

    if (exceeded) return {false, false, "上报回应超过容量；送达结果未知，已停止自动重试。"};
    if (timedOut) return {false, false, "上报等待超时；送达结果未知，已停止自动重试。"};
    if (status >= 200 && status < 300 && networkError == QNetworkReply::NoError) return {true, false, {}};
    if (!GateOpen(gate_, epoch_)) return {false, false, "上报请求已撤销；送达结果未知，已停止自动重试。"};
    if (status == 429) return {false, true, "服务端限流，按有界退避等待重试。"};
    if (status >= 300 && status < 400) return {false, false, "上报端点返回重定向，未向其他地址发送报告。"};
    if (status >= 400 && status < 500) return {false, false, "服务端拒绝上报，HTTP " + std::to_string(status) + "。"};
    if (status >= 500) return {false, false, "服务端返回错误；送达结果未知，已停止自动重试。"};
    if (networkError == QNetworkReply::ConnectionRefusedError || networkError == QNetworkReply::HostNotFoundError
        || networkError == QNetworkReply::ProxyConnectionRefusedError || networkError == QNetworkReply::ProxyNotFoundError)
        return {false, true, "上报通道无法连接，按有界退避等待重试。"};
    return {false, false, "上报通道中断；送达结果未知，已停止自动重试。"};
}

} // namespace War3Map::QtFeedback::Detail
