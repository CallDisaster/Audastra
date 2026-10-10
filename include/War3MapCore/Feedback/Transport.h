#pragma once

#include "War3MapCore/Types.h"

namespace War3Map::Feedback {

struct Delivery {
    std::string reportId;
    std::string reportJson;
    std::optional<std::string> cleanedLogs;
    std::vector<std::byte> userReviewedScreenshotPng;
    std::string previewSha256;
};

struct TransportResult {
    bool sent = false;
    bool retryable = false;
    std::string failureReason;
};

// 本段没有网络实现。接线方应在有界工作线程中调用，正文不能进入 URL 参数。
// Send期间Core不持队列锁。通道须在真正开始网络请求时复核可即时撤销的同意gate；
// 已开始请求的撤销不等于已撤回远端内容，结果未知时应返回retryable=false。
// 默认没有通道；Configured=false 时服务不会调用 Send。
class IReportTransport {
public:
    virtual ~IReportTransport() = default;
    [[nodiscard]] virtual bool Configured() const noexcept = 0;
    [[nodiscard]] virtual TransportResult Send(const Delivery& delivery) = 0;
};

} // namespace War3Map::Feedback
