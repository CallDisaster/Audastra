#pragma once

#include "War3MapCore/Feedback/Export.h"
#include "War3MapCore/Feedback/Report.h"

#include <memory>

namespace War3Map::Feedback {

enum class QueueState : std::uint8_t { Pending, Sent, Failed, Deleted };
enum class AttemptState : std::uint8_t {
    NoConsent, NoEndpoint, Unreviewed, NotDue, AlreadyComplete,
    Sent, RetryScheduled, Failed
};

struct QueueLimits {
    std::size_t maxEntries = 128;
    std::size_t maxStorageBytes = 64 * 1024 * 1024;
    std::size_t maxListEntries = 128;
    std::int64_t dedupWindowSeconds = 24 * 60 * 60;
    std::int64_t initialRetrySeconds = 30;
    ReportLimits report;
};

struct EntrySummary {
    std::string id;
    ReportSource source = ReportSource::User;
    ReportKind kind = ReportKind::Problem;
    QueueState state = QueueState::Pending;
    std::string errorCode;
    std::string toolName;
    std::string version;
    std::uint64_t occurrences = 1;
    std::int64_t firstSeenUnixSeconds = 0;
    std::int64_t lastSeenUnixSeconds = 0;
    std::uint32_t attempts = 0;
    std::optional<std::int64_t> nextAttemptUnixSeconds;
    std::string failureReason;
    bool hasPayload = true;
    bool previewConfirmed = false;
    bool hasLogs = false;
    bool hasConversation = false;
    bool hasScreenshot = false;
};

struct DraftResult {
    EntrySummary entry;
    bool deduplicated = false;
    bool consent = false;
    std::string message;
};

struct Preview {
    Delivery delivery;
    bool consent = false;
    bool confirmed = false;
    // 若有 PNG，完整预览应显示附件；不能只显示 JSON 就确认。
    bool requiresScreenshotPreview = false;
};

struct SendOutcome {
    AttemptState state = AttemptState::NoConsent;
    EntrySummary entry;
    std::string message;
};

// UI/CLI/MCP 共用服务。单进程互斥 + 独占文件租约，持久化只接受清理后白名单。
// Open 每次默认 consent=false；root 从本机设置恢复同意，P7 设置迁移不得导入。
class FeedbackQueue final {
public:
    [[nodiscard]] static Result<std::shared_ptr<FeedbackQueue>> Open(
        const std::filesystem::path& directory, PrivacyContext context = {}, QueueLimits limits = {});
    ~FeedbackQueue();
    FeedbackQueue(const FeedbackQueue&) = delete;
    FeedbackQueue& operator=(const FeedbackQueue&) = delete;

    [[nodiscard]] Result<DraftResult> Draft(const DraftInput& input, std::int64_t nowUnixSeconds);
    [[nodiscard]] Result<Preview> GetPreview(std::string_view id) const;
    [[nodiscard]] Status ConfirmPreview(std::string_view id, std::string_view expectedSha256);
    // 关闭立即阻止发送，并原子清空 Pending/Failed；写入失败如实返回，不假报清空。
    [[nodiscard]] Status SetConsent(bool enabled);
    [[nodiscard]] bool Consent() const;
    [[nodiscard]] std::optional<std::int64_t> LastSentUnixSeconds() const;
    [[nodiscard]] Result<std::vector<EntrySummary>> List(std::size_t offset = 0, std::size_t limit = 50) const;
    [[nodiscard]] Result<EntrySummary> GetStatus(std::string_view id) const;
    // 删除正文/附件并留下无内容 Deleted 状态，不保留原文。
    [[nodiscard]] Status Delete(std::string_view id);
    // 持久化inFlight后释放锁再发送；迟到结果不会复活已清空/删除的条目。
    // transport须自行有界，并在真正开始网络请求时复核宿主即时撤销gate。
    [[nodiscard]] Result<SendOutcome> TrySend(std::string_view id, std::string_view expectedPreviewSha256,
        std::int64_t nowUnixSeconds, IReportTransport& transport);
    // 显式导出无需同意或发送确认；调用方先用 GetPreview 给用户看完整内容。
    // 目标必须不存在，禁止覆盖任何已有文件。
    [[nodiscard]] Result<ExportResult> Export(std::string_view id, const std::filesystem::path& destination) const;

private:
    class Impl;
    explicit FeedbackQueue(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view StateName(QueueState value) noexcept;
[[nodiscard]] std::string_view AttemptName(AttemptState value) noexcept;

} // namespace War3Map::Feedback
