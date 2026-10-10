#include "War3MapCore/Feedback/Queue.h"
#include "War3MapCore/Hash.h"
#include "FeedbackInternal.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>

namespace War3Map::Feedback {
namespace {

constexpr std::int64_t maxTimestamp = 253402300799;
bool TimeValid(std::int64_t now) { return now >= 0 && now <= maxTimestamp; }
bool ValidLimits(const QueueLimits& limits) {
    return limits.maxEntries > 0 && limits.maxEntries <= 256 && limits.maxStorageBytes >= 1024 && limits.maxStorageBytes <= 64 * 1024 * 1024
        && limits.maxListEntries > 0 && limits.maxListEntries <= 256 && limits.dedupWindowSeconds == 24 * 60 * 60
        && limits.initialRetrySeconds > 0 && limits.initialRetrySeconds <= 3600;
}
template<class T> Result<T> Failure(std::string code, std::string message) {
    return Result<T>::Failure("Feedback.Queue", std::move(code), std::move(message));
}
template<class T> Result<T> FromStatus(const Status& status) { Result<T> failure; failure.diagnostics = status.diagnostics; return failure; }
std::string Id(std::uint64_t sequence) {
    std::ostringstream stream;
    stream << "feedback-" << std::hex << std::setw(16) << std::setfill('0') << sequence;
    return stream.str();
}
std::size_t Find(const Detail::Snapshot& state, std::string_view id) {
    for (std::size_t i = 0; i < state.entries.size(); ++i) if (state.entries[i].summary.id == id) return i;
    return state.entries.size();
}
void RemovePayload(Detail::Entry& entry) {
    entry.report.reset();
    entry.confirmedPreviewSha256.clear();
    entry.deliveryInFlight = false;
    entry.summary.nextAttemptUnixSeconds.reset();
}

} // namespace

class FeedbackQueue::Impl {
public:
    mutable std::mutex mutex;
    PrivacyContext context;
    QueueLimits limits;
    std::unique_ptr<Detail::QueueStorage> storage;
    Detail::Snapshot snapshot;
    bool consent = false;
    std::uint64_t consentGeneration = 0;

    Status Commit(Detail::Snapshot candidate) {
        const auto encoded = Detail::EncodeSnapshot(candidate, context, limits);
        if (!encoded.Succeeded()) return Status{encoded.diagnostics};
        const auto status = storage->Save(std::as_bytes(std::span(encoded.value->data(), encoded.value->size())));
        if (status.Succeeded()) snapshot = std::move(candidate);
        return status;
    }
};

FeedbackQueue::FeedbackQueue(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FeedbackQueue::~FeedbackQueue() = default;

Result<std::shared_ptr<FeedbackQueue>> FeedbackQueue::Open(const std::filesystem::path& directory, PrivacyContext context, QueueLimits limits) {
    try {
        if (!ValidLimits(limits)) return Failure<std::shared_ptr<FeedbackQueue>>("FEEDBACK_QUEUE_LIMITS", "反馈队列容量、24 小时去重窗口或重试配置无效。");
        const auto reportLimits = ValidateReportLimits(limits.report);
        if (!reportLimits.Succeeded()) return FromStatus<std::shared_ptr<FeedbackQueue>>(reportLimits);
        const auto contextStatus = ValidatePrivacyContext(context);
        if (!contextStatus.Succeeded()) return FromStatus<std::shared_ptr<FeedbackQueue>>(contextStatus);
        auto storage = Detail::QueueStorage::Open(directory, limits.maxStorageBytes);
        if (!storage.Succeeded()) { Result<std::shared_ptr<FeedbackQueue>> failure; failure.diagnostics = storage.diagnostics; return failure; }
        auto existing = (*storage.value)->Load();
        if (!existing.Succeeded()) { Result<std::shared_ptr<FeedbackQueue>> failure; failure.diagnostics = existing.diagnostics; return failure; }
        auto impl = std::make_unique<Impl>();
        impl->context = std::move(context);
        impl->limits = limits;
        impl->storage = std::move(*storage.value);
        if (existing.value->has_value()) {
            auto decoded = Detail::DecodeSnapshot(**existing.value, impl->context, limits);
            if (!decoded.Succeeded()) { Result<std::shared_ptr<FeedbackQueue>> failure; failure.diagnostics = decoded.diagnostics; return failure; }
            impl->snapshot = std::move(*decoded.value);
            auto recovered = impl->snapshot;
            bool interrupted = false;
            for (auto& entry : recovered.entries) if (entry.deliveryInFlight) {
                entry.deliveryInFlight = false;
                entry.summary.state = QueueState::Failed;
                entry.summary.failureReason = "上次发送未完整返回，送达结果未知；已停止自动重试。";
                entry.summary.nextAttemptUnixSeconds.reset();
                interrupted = true;
            }
            if (interrupted) {
                const auto status = impl->Commit(std::move(recovered));
                if (!status.Succeeded()) return FromStatus<std::shared_ptr<FeedbackQueue>>(status);
            }
        } else {
            const auto status = impl->Commit(impl->snapshot);
            if (!status.Succeeded()) return FromStatus<std::shared_ptr<FeedbackQueue>>(status);
        }
        return Result<std::shared_ptr<FeedbackQueue>>::Success(std::shared_ptr<FeedbackQueue>(new FeedbackQueue(std::move(impl))));
    } catch (const std::exception&) {
        return Failure<std::shared_ptr<FeedbackQueue>>("FEEDBACK_QUEUE_OPEN_FAILED", "反馈队列初始化失败，没有发出报告。");
    }
}

Result<DraftResult> FeedbackQueue::Draft(const DraftInput& input, std::int64_t nowUnixSeconds) {
    std::scoped_lock lock(impl_->mutex);
    if (!TimeValid(nowUnixSeconds)) return Failure<DraftResult>("FEEDBACK_TIME_INVALID", "反馈起草时间无效。");
    const auto cleaned = CleanDraft(input, impl_->context, impl_->limits.report);
    if (!cleaned.Succeeded()) { Result<DraftResult> failure; failure.diagnostics = cleaned.diagnostics; return failure; }
    std::string fingerprint;
    if (input.source == ReportSource::Agent) {
        const auto hash = ProblemFingerprint(input.errorCode, input.callSite, input.application.version);
        if (!hash.Succeeded()) { Result<DraftResult> failure; failure.diagnostics = hash.diagnostics; return failure; }
        fingerprint = *hash.value;
    }
    auto candidate = impl_->snapshot;
    if (!fingerprint.empty()) {
        for (auto iterator = candidate.entries.rbegin(); iterator != candidate.entries.rend(); ++iterator) {
            auto& entry = *iterator;
            if (entry.summary.state == QueueState::Deleted || entry.problemFingerprint != fingerprint) continue;
            if (nowUnixSeconds < entry.summary.lastSeenUnixSeconds) return Failure<DraftResult>("FEEDBACK_CLOCK_REVERSED", "反馈时钟早于该问题最近记录，未误计去重窗口。");
            if (nowUnixSeconds - entry.summary.firstSeenUnixSeconds >= impl_->limits.dedupWindowSeconds) break;
            if (entry.deliveryInFlight) return Failure<DraftResult>("FEEDBACK_DELIVERY_IN_FLIGHT", "该问题发送结果未完整返回，未改写正在处理的报告。");
            if (entry.summary.occurrences == (std::numeric_limits<std::uint64_t>::max)())
                return Failure<DraftResult>("FEEDBACK_COUNT_LIMIT", "反馈问题计数超过容量。");
            ++entry.summary.occurrences;
            entry.summary.lastSeenUnixSeconds = nowUnixSeconds;
            entry.confirmedPreviewSha256.clear();
            // 同一问题只存第一份清理后正文；计数和预览身份变化，不暗中替换诊断/附件。
            const auto summary = Detail::Summarize(entry);
            const auto status = impl_->Commit(std::move(candidate));
            if (!status.Succeeded()) return FromStatus<DraftResult>(status);
            return Result<DraftResult>::Success(DraftResult{summary, true, impl_->consent,
                impl_->consent ? "24 小时内重复问题计数已更新；未自动重复上报。" : "未同意，不会发送；重复问题仅在本地计数。"});
        }
    }
    if (candidate.nextSequence == (std::numeric_limits<std::uint64_t>::max)())
        return Failure<DraftResult>("FEEDBACK_ID_LIMIT", "反馈队列编号超过容量。");
    if (candidate.entries.size() >= impl_->limits.maxEntries) {
        const auto removable = std::find_if(candidate.entries.begin(), candidate.entries.end(), [](const auto& entry) {
            return entry.summary.state == QueueState::Sent || entry.summary.state == QueueState::Deleted;
        });
        if (removable == candidate.entries.end()) return Failure<DraftResult>("FEEDBACK_QUEUE_FULL", "本地反馈队列已满，请先删除或处理现有报告。");
        candidate.entries.erase(removable);
    }
    Detail::Entry entry;
    entry.summary.id = Id(candidate.nextSequence++);
    entry.summary.source = cleaned.value->source;
    entry.summary.kind = cleaned.value->kind;
    entry.summary.errorCode = cleaned.value->errorCode;
    entry.summary.toolName = cleaned.value->toolName;
    entry.summary.version = cleaned.value->application.version;
    entry.summary.firstSeenUnixSeconds = nowUnixSeconds;
    entry.summary.lastSeenUnixSeconds = nowUnixSeconds;
    entry.report = *cleaned.value;
    entry.problemFingerprint = std::move(fingerprint);
    const auto summary = Detail::Summarize(entry);
    candidate.entries.push_back(std::move(entry));
    const auto status = impl_->Commit(std::move(candidate));
    if (!status.Succeeded()) return FromStatus<DraftResult>(status);
    return Result<DraftResult>::Success(DraftResult{summary, false, impl_->consent,
        impl_->consent ? "报告已在本地起草，发送前须确认完整预览。" : "未同意，不会发送；可预览并导出本地报告包。"});
}

Result<Preview> FeedbackQueue::GetPreview(std::string_view id) const {
    std::scoped_lock lock(impl_->mutex);
    const auto index = Find(impl_->snapshot, id);
    if (index == impl_->snapshot.entries.size()) return Failure<Preview>("FEEDBACK_NOT_FOUND", "反馈报告不存在。");
    const auto& entry = impl_->snapshot.entries[index];
    auto delivery = Detail::MakeDelivery(entry, impl_->context, impl_->limits);
    if (!delivery.Succeeded()) { Result<Preview> failure; failure.diagnostics = delivery.diagnostics; return failure; }
    const bool confirmed = entry.confirmedPreviewSha256 == delivery.value->previewSha256;
    const bool screenshot = !delivery.value->userReviewedScreenshotPng.empty();
    return Result<Preview>::Success(Preview{std::move(*delivery.value), impl_->consent, confirmed, screenshot});
}

Status FeedbackQueue::ConfirmPreview(std::string_view id, std::string_view expectedSha256) {
    std::scoped_lock lock(impl_->mutex);
    const auto index = Find(impl_->snapshot, id);
    if (index == impl_->snapshot.entries.size()) return Status::Failure("Feedback.Queue", "FEEDBACK_NOT_FOUND", "反馈报告不存在。");
    const auto& entry = impl_->snapshot.entries[index];
    if (entry.deliveryInFlight) return Status::Failure("Feedback.Queue", "FEEDBACK_DELIVERY_IN_FLIGHT", "反馈发送结果尚未完整返回。");
    const auto delivery = Detail::MakeDelivery(entry, impl_->context, impl_->limits);
    if (!delivery.Succeeded()) return Status{delivery.diagnostics};
    if (expectedSha256 != delivery.value->previewSha256) return Status::Failure("Feedback.Queue", "FEEDBACK_PREVIEW_CHANGED", "反馈正文或附件已经变化，请重新查看完整预览。");
    auto candidate = impl_->snapshot;
    candidate.entries[index].confirmedPreviewSha256 = delivery.value->previewSha256;
    return impl_->Commit(std::move(candidate));
}

Status FeedbackQueue::SetConsent(bool enabled) {
    std::scoped_lock lock(impl_->mutex);
    if (enabled) { impl_->consent = true; return Status::Ok(); }
    // 即使清队列写盘失败，发送权限也立即关闭。
    impl_->consent = false;
    if (impl_->consentGeneration < (std::numeric_limits<std::uint64_t>::max)()) ++impl_->consentGeneration;
    auto candidate = impl_->snapshot;
    std::erase_if(candidate.entries, [](const auto& entry) { return entry.summary.state == QueueState::Pending || entry.summary.state == QueueState::Failed; });
    return impl_->Commit(std::move(candidate));
}

bool FeedbackQueue::Consent() const { std::scoped_lock lock(impl_->mutex); return impl_->consent; }
std::optional<std::int64_t> FeedbackQueue::LastSentUnixSeconds() const { std::scoped_lock lock(impl_->mutex); return impl_->snapshot.lastSentUnixSeconds; }

Result<std::vector<EntrySummary>> FeedbackQueue::List(std::size_t offset, std::size_t limit) const {
    std::scoped_lock lock(impl_->mutex);
    if (limit == 0 || limit > impl_->limits.maxListEntries) return Failure<std::vector<EntrySummary>>("FEEDBACK_LIST_BUDGET", "反馈列表请求超过允许范围。");
    std::vector<EntrySummary> result;
    if (offset < impl_->snapshot.entries.size()) {
        const auto count = (std::min)(limit, impl_->snapshot.entries.size() - offset);
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) result.push_back(Detail::Summarize(impl_->snapshot.entries[offset + i]));
    }
    return Result<std::vector<EntrySummary>>::Success(std::move(result));
}

Result<EntrySummary> FeedbackQueue::GetStatus(std::string_view id) const {
    std::scoped_lock lock(impl_->mutex);
    const auto index = Find(impl_->snapshot, id);
    return index == impl_->snapshot.entries.size() ? Failure<EntrySummary>("FEEDBACK_NOT_FOUND", "反馈报告不存在。")
        : Result<EntrySummary>::Success(Detail::Summarize(impl_->snapshot.entries[index]));
}

Status FeedbackQueue::Delete(std::string_view id) {
    std::scoped_lock lock(impl_->mutex);
    const auto index = Find(impl_->snapshot, id);
    if (index == impl_->snapshot.entries.size()) return Status::Failure("Feedback.Queue", "FEEDBACK_NOT_FOUND", "反馈报告不存在。");
    auto candidate = impl_->snapshot;
    auto& entry = candidate.entries[index];
    RemovePayload(entry);
    entry.problemFingerprint.clear();
    entry.summary.state = QueueState::Deleted;
    entry.summary.errorCode.clear();
    entry.summary.toolName.clear();
    entry.summary.version.clear();
    entry.summary.failureReason.clear();
    entry.summary.attempts = 0;
    return impl_->Commit(std::move(candidate));
}

Result<SendOutcome> FeedbackQueue::TrySend(std::string_view id, std::string_view expectedPreviewSha256,
    std::int64_t nowUnixSeconds, IReportTransport& transport) {
    // 只串行化持久化事务。网络期间释放锁，UI/MCP能查询、起草或撤同意。
    // 同条inFlight拒绝二次发送；迟到回执只作用于同一次已持久化的发送身份。
    std::unique_lock lock(impl_->mutex);
    if (!TimeValid(nowUnixSeconds)) return Failure<SendOutcome>("FEEDBACK_TIME_INVALID", "反馈发送时间无效。");
    const auto index = Find(impl_->snapshot, id);
    if (index == impl_->snapshot.entries.size()) return Failure<SendOutcome>("FEEDBACK_NOT_FOUND", "反馈报告不存在。");
    const auto& entry = impl_->snapshot.entries[index];
    auto blocked = [&](AttemptState state, std::string message) { return Result<SendOutcome>::Success(SendOutcome{state, Detail::Summarize(entry), std::move(message)}); };
    if (!impl_->consent) return blocked(AttemptState::NoConsent, "未同意，不会发送。");
    if (entry.summary.state != QueueState::Pending || entry.deliveryInFlight) return blocked(AttemptState::AlreadyComplete, "该报告不处于可自动发送状态。");
    if (nowUnixSeconds < entry.summary.lastSeenUnixSeconds) return Failure<SendOutcome>("FEEDBACK_CLOCK_REVERSED", "反馈发送时间早于报告最近记录。");
    auto delivery = Detail::MakeDelivery(entry, impl_->context, impl_->limits);
    if (!delivery.Succeeded()) { Result<SendOutcome> failure; failure.diagnostics = delivery.diagnostics; return failure; }
    if (entry.confirmedPreviewSha256 != delivery.value->previewSha256 || expectedPreviewSha256 != delivery.value->previewSha256)
        return blocked(AttemptState::Unreviewed, "必须确认当前正文、日志、对话及截图的完整预览后才会发送。");
    if (entry.summary.nextAttemptUnixSeconds && nowUnixSeconds < *entry.summary.nextAttemptUnixSeconds)
        return blocked(AttemptState::NotDue, "尚未到下一次有界重试时间。");
    if (!transport.Configured()) return blocked(AttemptState::NoEndpoint, "未配置上报通道；只可导出本地报告包。");
    auto candidate = impl_->snapshot;
    candidate.entries[index].deliveryInFlight = true;
    ++candidate.entries[index].summary.attempts;
    candidate.entries[index].summary.nextAttemptUnixSeconds.reset();
    const auto started = impl_->Commit(std::move(candidate));
    if (!started.Succeeded()) return FromStatus<SendOutcome>(started);
    const auto activeId = delivery.value->reportId;
    const auto activeAttempt = impl_->snapshot.entries[index].summary.attempts;
    const auto consentGeneration = impl_->consentGeneration;
    lock.unlock();
    TransportResult transported;
    try { transported = transport.Send(*delivery.value); }
    catch (...) { transported = {false, false, "发送通道未完整返回，送达结果未知；已停止自动重试。"}; }
    if (transported.sent && (transported.retryable || !transported.failureReason.empty()))
        transported = {false, false, "发送通道返回相互冲突的结果，送达状态未知；已停止自动重试。"};
    lock.lock();
    const auto finalIndex = Find(impl_->snapshot, activeId);
    if (finalIndex == impl_->snapshot.entries.size()
        || impl_->snapshot.entries[finalIndex].summary.state != QueueState::Pending
        || !impl_->snapshot.entries[finalIndex].deliveryInFlight
        || impl_->snapshot.entries[finalIndex].summary.attempts != activeAttempt
        || impl_->snapshot.entries[finalIndex].confirmedPreviewSha256 != delivery.value->previewSha256) {
        // 用户已经清空/删除。只记录已确认的最近上报时间，不恢复正文、墓碑或重试。
        if (transported.sent) {
            auto receiptOnly = impl_->snapshot;
            receiptOnly.lastSentUnixSeconds = (std::max)(receiptOnly.lastSentUnixSeconds.value_or(0), nowUnixSeconds);
            const auto receipt = impl_->Commit(std::move(receiptOnly));
            if (!receipt.Succeeded()) return FromStatus<SendOutcome>(receipt);
        }
        return Failure<SendOutcome>("FEEDBACK_DELIVERY_WITHDRAWN", transported.sent
            ? "报告在发送期间被关闭或删除；通道已确认送达，本地没有恢复报告内容。"
            : "报告在发送期间被关闭或删除；未恢复报告内容，未安排自动重试。");
    }
    auto finalState = impl_->snapshot;
    auto& final = finalState.entries[finalIndex];
    final.deliveryInFlight = false;
    AttemptState outcome;
    std::string message;
    if (transported.sent) {
        final.summary.state = QueueState::Sent;
        final.summary.failureReason.clear();
        RemovePayload(final);
        finalState.lastSentUnixSeconds = (std::max)(finalState.lastSentUnixSeconds.value_or(0), nowUnixSeconds);
        outcome = AttemptState::Sent;
        message = "报告已发送；本地正文和附件已移除。";
    } else {
        auto reason = CleanPrivateText(transported.failureReason, impl_->context, PrivacyLimits{4096, 128, 4096});
        final.summary.failureReason = reason.Succeeded() && !reason.value->text.empty() ? reason.value->text : "上报通道失败，未记录未经清理的失败详情。";
        const auto delay = impl_->limits.initialRetrySeconds * (std::int64_t{1} << (final.summary.attempts - 1));
        if (transported.retryable && impl_->consent && consentGeneration == impl_->consentGeneration
            && final.summary.attempts < 3 && nowUnixSeconds <= maxTimestamp - delay) {
            final.summary.state = QueueState::Pending;
            final.summary.nextAttemptUnixSeconds = nowUnixSeconds + delay;
            outcome = AttemptState::RetryScheduled;
            message = "发送失败，已安排有界指数退避；总尝试最多三次。";
        } else {
            final.summary.state = QueueState::Failed;
            final.summary.nextAttemptUnixSeconds.reset();
            outcome = AttemptState::Failed;
            message = "发送失败，自动尝试已停止。";
        }
    }
    const auto summary = Detail::Summarize(final);
    const auto committed = impl_->Commit(std::move(finalState));
    // 若回执写盘失败，保留已经持久化的 inFlight，禁止把未知结果自动再发一次。
    if (!committed.Succeeded()) return FromStatus<SendOutcome>(committed);
    return Result<SendOutcome>::Success(SendOutcome{outcome, summary, std::move(message)});
}

Result<ExportResult> FeedbackQueue::Export(std::string_view id, const std::filesystem::path& destination) const {
    std::scoped_lock lock(impl_->mutex);
    const auto index = Find(impl_->snapshot, id);
    if (index == impl_->snapshot.entries.size()) return Failure<ExportResult>("FEEDBACK_NOT_FOUND", "反馈报告不存在。");
    const auto delivery = Detail::MakeDelivery(impl_->snapshot.entries[index], impl_->context, impl_->limits);
    if (!delivery.Succeeded()) { Result<ExportResult> failure; failure.diagnostics = delivery.diagnostics; return failure; }
    const auto zip = Detail::MakeReportZip(*delivery.value);
    if (!zip.Succeeded()) { Result<ExportResult> failure; failure.diagnostics = zip.diagnostics; return failure; }
    const auto hash = Sha256(*zip.value);
    if (!hash.Succeeded()) { Result<ExportResult> failure; failure.diagnostics = hash.diagnostics; return failure; }
    const auto written = Detail::WriteNewExport(destination, *zip.value);
    if (!written.Succeeded()) return FromStatus<ExportResult>(written);
    std::error_code error;
    const auto absolute = std::filesystem::absolute(destination, error).lexically_normal();
    if (error) return Failure<ExportResult>("FEEDBACK_EXPORT_PATH_INVALID", "报告包已写出，但目标路径无法回报。");
    return Result<ExportResult>::Success(ExportResult{absolute, static_cast<std::uint64_t>(zip.value->size()), *hash.value});
}

} // namespace War3Map::Feedback
