#include "FeedbackInternal.h"
#include "../Serialization/StrictJson.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>

namespace War3Map::Feedback {

std::string_view StateName(QueueState value) noexcept {
    switch (value) {
    case QueueState::Pending: return "pending";
    case QueueState::Sent: return "sent";
    case QueueState::Failed: return "failed";
    case QueueState::Deleted: return "deleted";
    }
    return "invalid";
}
std::string_view AttemptName(AttemptState value) noexcept {
    switch (value) {
    case AttemptState::NoConsent: return "no-consent";
    case AttemptState::NoEndpoint: return "no-endpoint";
    case AttemptState::Unreviewed: return "unreviewed";
    case AttemptState::NotDue: return "not-due";
    case AttemptState::AlreadyComplete: return "already-complete";
    case AttemptState::Sent: return "sent";
    case AttemptState::RetryScheduled: return "retry-scheduled";
    case AttemptState::Failed: return "failed";
    }
    return "invalid";
}

namespace Detail {
namespace {

using Json = nlohmann::json;
constexpr std::int64_t maxTimestamp = 253402300799;

template<class T> Result<T> Failure(std::string code, std::string message) {
    return Result<T>::Failure("Feedback.Queue", std::move(code), std::move(message));
}
void Require(bool ok) { if (!ok) throw std::runtime_error("反馈白名单或状态合同无效。"); }
void Keys(const Json& object, std::initializer_list<std::string_view> names) {
    Require(object.is_object() && object.size() == names.size());
    for (auto name : names) Require(object.contains(std::string(name)));
}
std::string String(const Json& value, std::size_t maxBytes) {
    Require(value.is_string());
    const auto& text = value.get_ref<const std::string&>();
    Require(text.size() <= maxBytes);
    return text;
}
std::uint64_t Unsigned(const Json& value) {
    Require(value.is_number_integer());
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    const auto signedValue = value.get<std::int64_t>();
    Require(signedValue >= 0);
    return static_cast<std::uint64_t>(signedValue);
}
std::int64_t Time(const Json& value) {
    const auto number = Unsigned(value);
    Require(number <= static_cast<std::uint64_t>(maxTimestamp));
    return static_cast<std::int64_t>(number);
}
std::optional<std::int64_t> OptionalTime(const Json& value) { return value.is_null() ? std::nullopt : std::optional{Time(value)}; }
std::optional<std::string> OptionalString(const Json& value, std::size_t maxBytes) {
    return value.is_null() ? std::nullopt : std::optional{String(value, maxBytes)};
}
bool Boolean(const Json& value) { Require(value.is_boolean()); return value.get<bool>(); }
bool IsSha(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::uint64_t Sequence(std::string_view id) {
    Require(id.size() == 25 && id.starts_with("feedback-"));
    std::uint64_t sequence = 0;
    const auto result = std::from_chars(id.data() + 9, id.data() + id.size(), sequence, 16);
    Require(result.ec == std::errc{} && result.ptr == id.data() + id.size() && sequence > 0);
    return sequence;
}
template<class E> E Enum(std::string_view text, std::initializer_list<std::pair<std::string_view, E>> values) {
    for (const auto& [name, value] : values) if (name == text) return value;
    throw std::runtime_error("反馈枚举无效。");
}
ReportSource Source(const Json& value) {
    return Enum<ReportSource>(String(value, 16), {{"user", ReportSource::User}, {"agent", ReportSource::Agent}});
}
ReportKind Kind(const Json& value) {
    return Enum<ReportKind>(String(value, 16), {{"problem", ReportKind::Problem}, {"suggestion", ReportKind::Suggestion}});
}
QueueState State(const Json& value) {
    return Enum<QueueState>(String(value, 16), {{"pending", QueueState::Pending}, {"sent", QueueState::Sent}, {"failed", QueueState::Failed}, {"deleted", QueueState::Deleted}});
}
AgentIssue Issue(const Json& value) {
    return Enum<AgentIssue>(String(value, 64), {{"unspecified", AgentIssue::Unspecified}, {"internal-tool-error", AgentIssue::InternalToolError},
        {"unhandled-exception", AgentIssue::UnhandledException}, {"application-save-defect", AgentIssue::ApplicationSaveDefect},
        {"assertion-failure", AgentIssue::AssertionFailure}, {"unexplained-render-failure", AgentIssue::UnexplainedRenderFailure}, {"user-map-error", AgentIssue::UserMapError},
        {"tool-usability-gap", AgentIssue::ToolUsabilityGap}, {"compatibility-gap", AgentIssue::CompatibilityGap}});
}
std::string HexBytes(std::span<const std::byte> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (auto b : bytes) { const auto value = std::to_integer<unsigned>(b); hex.push_back(digits[value >> 4]); hex.push_back(digits[value & 15]); }
    return hex;
}
std::vector<std::byte> BytesFromHex(std::string_view hex, std::size_t maxBytes) {
    Require(hex.size() % 2 == 0 && hex.size() / 2 <= maxBytes);
    std::vector<std::byte> bytes;
    bytes.reserve(hex.size() / 2);
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const auto a = digit(hex[i]), b = digit(hex[i + 1]);
        Require(a >= 0 && b >= 0);
        bytes.push_back(static_cast<std::byte>((a << 4) | b));
    }
    return bytes;
}

Json ReportObject(const ReportData& report, bool storage) {
    Json result{{"schema", "audastra-feedback-report-v1"}, {"source", SourceName(report.source)}, {"kind", KindName(report.kind)},
        {"agentIssue", AgentIssueName(report.agentIssue)}, {"application", {{"version", report.application.version}, {"commit", report.application.commit},
            {"operatingSystem", report.application.operatingSystem}, {"graphicsAdapter", report.application.graphicsAdapter}}},
        {"errorCode", report.errorCode}, {"callSite", report.callSite}, {"toolName", report.toolName}, {"diagnostic", report.diagnostic},
        {"callStack", report.callStack}, {"userDescription", report.userDescription}, {"logs", report.logs ? Json(*report.logs) : Json(nullptr)},
        {"conversationSnippet", report.conversationSnippet ? Json(*report.conversationSnippet) : Json(nullptr)}, {"screenshot", nullptr}};
    if (report.screenshot) {
        Json screenshot{{"file", "screenshot.png"}, {"bytes", report.screenshot->png.size()}, {"sha256", report.screenshot->sha256},
            {"pixelPrivacy", "截图像素由用户审核，未自动脱敏"}};
        if (storage) screenshot["pngHex"] = HexBytes(report.screenshot->png);
        result["screenshot"] = std::move(screenshot);
    }
    return result;
}
ReportData ParseReport(const Json& json, const ReportLimits& limits) {
    Keys(json, {"schema", "source", "kind", "agentIssue", "application", "errorCode", "callSite", "toolName", "diagnostic", "callStack", "userDescription", "logs", "conversationSnippet", "screenshot"});
    Require(String(json.at("schema"), 64) == "audastra-feedback-report-v1");
    ReportData report;
    report.source = Source(json.at("source"));
    report.kind = Kind(json.at("kind"));
    report.agentIssue = Issue(json.at("agentIssue"));
    const auto& app = json.at("application");
    Keys(app, {"version", "commit", "operatingSystem", "graphicsAdapter"});
    report.application = {String(app.at("version"), 128), String(app.at("commit"), 64), String(app.at("operatingSystem"), 512), String(app.at("graphicsAdapter"), 512)};
    report.errorCode = String(json.at("errorCode"), 256);
    report.callSite = String(json.at("callSite"), 4096);
    report.toolName = String(json.at("toolName"), 256);
    report.diagnostic = String(json.at("diagnostic"), limits.maxDiagnosticBytes);
    report.userDescription = String(json.at("userDescription"), limits.maxDescriptionBytes);
    const auto& stack = json.at("callStack");
    Require(stack.is_array() && stack.size() <= limits.maxStackFrames);
    for (const auto& value : stack) report.callStack.push_back(String(value, 4096));
    report.logs = OptionalString(json.at("logs"), limits.maxLogBytes);
    report.conversationSnippet = OptionalString(json.at("conversationSnippet"), limits.maxConversationBytes);
    if (!json.at("screenshot").is_null()) {
        const auto& screenshot = json.at("screenshot");
        Keys(screenshot, {"file", "bytes", "sha256", "pixelPrivacy", "pngHex"});
        Require(String(screenshot.at("file"), 32) == "screenshot.png"
            && String(screenshot.at("pixelPrivacy"), 128) == "截图像素由用户审核，未自动脱敏");
        PreparedScreenshot png{BytesFromHex(String(screenshot.at("pngHex"), limits.maxScreenshotBytes * 2), limits.maxScreenshotBytes), String(screenshot.at("sha256"), 64)};
        Require(Unsigned(screenshot.at("bytes")) == png.png.size() && IsSha(png.sha256));
        report.screenshot = std::move(png);
    }
    return report;
}

} // namespace

EntrySummary Summarize(const Entry& entry) {
    auto summary = entry.summary;
    summary.hasPayload = entry.report.has_value();
    summary.previewConfirmed = !entry.confirmedPreviewSha256.empty();
    summary.hasLogs = entry.report && entry.report->logs.has_value();
    summary.hasConversation = entry.report && entry.report->conversationSnippet.has_value();
    summary.hasScreenshot = entry.report && entry.report->screenshot.has_value();
    return summary;
}

Status ValidateEntry(const Entry& entry, const PrivacyContext& context, const QueueLimits& limits) {
    try {
        const auto& s = entry.summary;
        (void)Sequence(s.id);
        Require(SourceName(s.source) != "invalid" && KindName(s.kind) != "invalid" && StateName(s.state) != "invalid"
            && s.occurrences > 0 && s.firstSeenUnixSeconds >= 0 && s.lastSeenUnixSeconds >= s.firstSeenUnixSeconds
            && s.lastSeenUnixSeconds <= maxTimestamp && s.attempts <= 3
            && (!s.nextAttemptUnixSeconds || (*s.nextAttemptUnixSeconds >= 0 && *s.nextAttemptUnixSeconds <= maxTimestamp))
            && (entry.confirmedPreviewSha256.empty() || IsSha(entry.confirmedPreviewSha256)));
        if (s.state == QueueState::Deleted) Require(!entry.report && !entry.deliveryInFlight && entry.confirmedPreviewSha256.empty()
            && entry.problemFingerprint.empty() && s.errorCode.empty() && s.toolName.empty() && s.version.empty() && s.failureReason.empty());
        else Require(s.source == ReportSource::Agent ? IsSha(entry.problemFingerprint) : entry.problemFingerprint.empty());
        if (s.state == QueueState::Sent || s.state == QueueState::Deleted) Require(!entry.report && !entry.deliveryInFlight
            && entry.confirmedPreviewSha256.empty() && !s.nextAttemptUnixSeconds);
        else Require(entry.report.has_value());
        if (s.state == QueueState::Sent) Require(s.attempts > 0 && s.failureReason.empty());
        if (s.state == QueueState::Failed) Require(s.attempts > 0 && !s.nextAttemptUnixSeconds && !entry.deliveryInFlight && !s.failureReason.empty());
        if (s.state == QueueState::Pending) Require(s.attempts < 3 || entry.deliveryInFlight);
        if (entry.deliveryInFlight) Require(s.state == QueueState::Pending && s.attempts > 0 && !s.nextAttemptUnixSeconds && !entry.confirmedPreviewSha256.empty());
        if (s.state == QueueState::Pending && s.attempts > 0 && !entry.deliveryInFlight) Require(s.nextAttemptUnixSeconds.has_value());
        if (s.state == QueueState::Pending && s.attempts == 0) Require(!s.nextAttemptUnixSeconds && s.failureReason.empty());
        for (const auto* text : {&s.errorCode, &s.toolName, &s.version, &s.failureReason}) {
            const auto clean = CleanPrivateText(*text, context);
            Require(clean.Succeeded() && clean.value->text == *text);
        }
        if (entry.report) {
            const auto clean = ValidateCleanReport(*entry.report, context, limits.report);
            if (!clean.Succeeded()) return clean;
            Require(entry.report->source == s.source && entry.report->kind == s.kind && entry.report->errorCode == s.errorCode
                && entry.report->toolName == s.toolName && entry.report->application.version == s.version);
        }
        return Status::Ok();
    } catch (const std::exception&) {
        return Status::Failure("Feedback.Queue", "FEEDBACK_QUEUE_INVALID", "反馈队列条目不符合白名单或状态合同，未恢复不确定内容。");
    }
}

Result<Delivery> MakeDelivery(const Entry& entry, const PrivacyContext& context, const QueueLimits& limits) {
    try {
        const auto status = ValidateEntry(entry, context, limits);
        if (!status.Succeeded()) { Result<Delivery> failure; failure.diagnostics = status.diagnostics; return failure; }
        if (!entry.report) return Failure<Delivery>("FEEDBACK_PAYLOAD_REMOVED", "反馈正文已经移除，不能预览、导出或发送。");
        Json json = ReportObject(*entry.report, false);
        json["reportId"] = entry.summary.id;
        json["occurrences"] = entry.summary.occurrences;
        json["firstSeenUnixSeconds"] = entry.summary.firstSeenUnixSeconds;
        json["lastSeenUnixSeconds"] = entry.summary.lastSeenUnixSeconds;
        Delivery delivery;
        delivery.reportId = entry.summary.id;
        delivery.reportJson = json.dump();
        if (delivery.reportJson.size() > limits.report.maxReportJsonBytes)
            return Failure<Delivery>("FEEDBACK_REPORT_JSON_BUDGET", "反馈预览报告超过容量，未输出部分报告。");
        delivery.cleanedLogs = entry.report->logs;
        if (entry.report->screenshot) delivery.userReviewedScreenshotPng = entry.report->screenshot->png;
        const auto hash = Sha256Text(delivery.reportJson);
        if (!hash.Succeeded()) { Result<Delivery> failure; failure.diagnostics = hash.diagnostics; return failure; }
        delivery.previewSha256 = *hash.value;
        return Result<Delivery>::Success(std::move(delivery));
    } catch (const std::exception&) {
        return Failure<Delivery>("FEEDBACK_PREVIEW_FAILED", "反馈完整预览生成失败，未返回不确定载荷。");
    }
}

Result<std::string> EncodeSnapshot(const Snapshot& snapshot, const PrivacyContext& context, const QueueLimits& limits) {
    try {
        Require(snapshot.nextSequence > 0 && snapshot.entries.size() <= limits.maxEntries
            && (!snapshot.lastSentUnixSeconds || (*snapshot.lastSentUnixSeconds >= 0 && *snapshot.lastSentUnixSeconds <= maxTimestamp)));
        Json entries = Json::array();
        std::set<std::string> ids;
        for (const auto& entry : snapshot.entries) {
            const auto status = ValidateEntry(entry, context, limits);
            if (!status.Succeeded()) { Result<std::string> failure; failure.diagnostics = status.diagnostics; return failure; }
            const auto& s = entry.summary;
            Require(Sequence(s.id) < snapshot.nextSequence && ids.insert(s.id).second);
            if (entry.report) {
                auto delivery = MakeDelivery(entry, context, limits);
                if (!delivery.Succeeded()) { Result<std::string> failure; failure.diagnostics = delivery.diagnostics; return failure; }
                Require(entry.confirmedPreviewSha256.empty() || delivery.value->previewSha256 == entry.confirmedPreviewSha256);
            }
            entries.push_back(Json{{"id", s.id}, {"source", SourceName(s.source)}, {"kind", KindName(s.kind)}, {"state", StateName(s.state)},
                {"errorCode", s.errorCode}, {"toolName", s.toolName}, {"version", s.version}, {"occurrences", s.occurrences},
                {"firstSeenUnixSeconds", s.firstSeenUnixSeconds}, {"lastSeenUnixSeconds", s.lastSeenUnixSeconds}, {"attempts", s.attempts},
                {"nextAttemptUnixSeconds", s.nextAttemptUnixSeconds ? Json(*s.nextAttemptUnixSeconds) : Json(nullptr)}, {"failureReason", s.failureReason},
                {"problemFingerprint", entry.problemFingerprint}, {"confirmedPreviewSha256", entry.confirmedPreviewSha256},
                {"deliveryInFlight", entry.deliveryInFlight}, {"report", entry.report ? ReportObject(*entry.report, true) : Json(nullptr)}});
        }
        Json root{{"schema", "audastra-feedback-queue-v1"}, {"nextSequence", snapshot.nextSequence},
            {"lastSentUnixSeconds", snapshot.lastSentUnixSeconds ? Json(*snapshot.lastSentUnixSeconds) : Json(nullptr)}, {"entries", std::move(entries)}};
        auto text = root.dump();
        if (text.size() > limits.maxStorageBytes) return Failure<std::string>("FEEDBACK_QUEUE_BUDGET", "反馈队列超过容量，未替换当前文件。");
        return Result<std::string>::Success(std::move(text));
    } catch (const std::exception&) {
        return Failure<std::string>("FEEDBACK_QUEUE_INVALID", "反馈队列不符合持久化合同，未写出部分状态。");
    }
}

Result<Snapshot> DecodeSnapshot(std::span<const std::byte> bytes, const PrivacyContext& context, const QueueLimits& limits) {
    Serialization::StrictJsonLimits jsonLimits;
    jsonLimits.maxBytes = limits.maxStorageBytes;
    jsonLimits.maxDepth = 16;
    jsonLimits.maxNodes = 131072;
    jsonLimits.maxStringBytes = (std::max)(limits.report.maxScreenshotBytes * 2, limits.report.maxLogBytes);
    jsonLimits.rejectFloatingPoint = true;
    const auto parsed = Serialization::ParseStrictJson(bytes, jsonLimits);
    if (!parsed.Succeeded()) { Result<Snapshot> failure; failure.diagnostics = parsed.diagnostics; return failure; }
    try {
        const auto& root = *parsed.value;
        Keys(root, {"schema", "nextSequence", "lastSentUnixSeconds", "entries"});
        Require(String(root.at("schema"), 64) == "audastra-feedback-queue-v1");
        Snapshot snapshot;
        snapshot.nextSequence = Unsigned(root.at("nextSequence"));
        snapshot.lastSentUnixSeconds = OptionalTime(root.at("lastSentUnixSeconds"));
        const auto& entries = root.at("entries");
        Require(entries.is_array() && entries.size() <= limits.maxEntries);
        for (const auto& json : entries) {
            Keys(json, {"id", "source", "kind", "state", "errorCode", "toolName", "version", "occurrences", "firstSeenUnixSeconds", "lastSeenUnixSeconds",
                "attempts", "nextAttemptUnixSeconds", "failureReason", "problemFingerprint", "confirmedPreviewSha256", "deliveryInFlight", "report"});
            Entry entry;
            auto& s = entry.summary;
            s.id = String(json.at("id"), 64);
            s.source = Source(json.at("source"));
            s.kind = Kind(json.at("kind"));
            s.state = State(json.at("state"));
            s.errorCode = String(json.at("errorCode"), 256);
            s.toolName = String(json.at("toolName"), 256);
            s.version = String(json.at("version"), 128);
            s.occurrences = Unsigned(json.at("occurrences"));
            s.firstSeenUnixSeconds = Time(json.at("firstSeenUnixSeconds"));
            s.lastSeenUnixSeconds = Time(json.at("lastSeenUnixSeconds"));
            const auto attempts = Unsigned(json.at("attempts"));
            Require(attempts <= 3);
            s.attempts = static_cast<std::uint32_t>(attempts);
            s.nextAttemptUnixSeconds = OptionalTime(json.at("nextAttemptUnixSeconds"));
            s.failureReason = String(json.at("failureReason"), 4096);
            entry.problemFingerprint = String(json.at("problemFingerprint"), 64);
            entry.confirmedPreviewSha256 = String(json.at("confirmedPreviewSha256"), 64);
            entry.deliveryInFlight = Boolean(json.at("deliveryInFlight"));
            if (!json.at("report").is_null()) entry.report = ParseReport(json.at("report"), limits.report);
            snapshot.entries.push_back(std::move(entry));
        }
        const auto validated = EncodeSnapshot(snapshot, context, limits);
        if (!validated.Succeeded()) { Result<Snapshot> failure; failure.diagnostics = validated.diagnostics; return failure; }
        return Result<Snapshot>::Success(std::move(snapshot));
    } catch (const std::exception&) {
        return Failure<Snapshot>("FEEDBACK_QUEUE_INVALID", "反馈队列包含未知字段、无效类型或不完整状态，原文件保持原样。");
    }
}

} // namespace Detail
} // namespace War3Map::Feedback
