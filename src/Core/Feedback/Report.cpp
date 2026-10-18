#include "War3MapCore/Feedback/Report.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <array>
#include <exception>
#include <tuple>

namespace War3Map::Feedback {
namespace {

template<class T> Result<T> Failure(std::string code, std::string message) {
    return Result<T>::Failure("Feedback.Report", std::move(code), std::move(message));
}
bool ValidLimits(const ReportLimits& limits) {
    return limits.maxDescriptionBytes > 0 && limits.maxDescriptionBytes <= 64 * 1024
        && limits.maxDiagnosticBytes > 0 && limits.maxDiagnosticBytes <= 128 * 1024
        && limits.maxLogBytes > 0 && limits.maxLogBytes <= 1024 * 1024
        && limits.maxConversationBytes > 0 && limits.maxConversationBytes <= 64 * 1024
        && limits.maxStackFrames <= 128 && limits.maxScreenshotBytes > 0 && limits.maxScreenshotBytes <= 16 * 1024 * 1024
        && limits.maxReportJsonBytes > 0 && limits.maxReportJsonBytes <= 4 * 1024 * 1024;
}
bool ValidCommit(std::string_view commit, const PrivacyContext& context) {
    if (commit.empty()) return true;
    if (commit.size() < 7 || commit.size() > 64 || !std::all_of(commit.begin(), commit.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); })) return false;
    return std::none_of(context.secretValues.begin(), context.secretValues.end(), [&](const auto& secret) { return commit.find(secret) != std::string_view::npos; });
}
bool ProgramIssue(AgentIssue issue) {
    return (issue >= AgentIssue::InternalToolError && issue <= AgentIssue::UnexplainedRenderFailure)
        || issue == AgentIssue::ToolUsabilityGap || issue == AgentIssue::CompatibilityGap;
}

} // namespace

Status ValidateReportLimits(const ReportLimits& limits) {
    return ValidLimits(limits) ? Status::Ok() : Status::Failure("Feedback.Report", "FEEDBACK_REPORT_BUDGET", "反馈报告容量配置无效。");
}

std::string_view SourceName(ReportSource value) noexcept {
    switch (value) { case ReportSource::User: return "user"; case ReportSource::Agent: return "agent"; }
    return "invalid";
}
std::string_view KindName(ReportKind value) noexcept {
    switch (value) { case ReportKind::Problem: return "problem"; case ReportKind::Suggestion: return "suggestion"; }
    return "invalid";
}
std::string_view AgentIssueName(AgentIssue value) noexcept {
    switch (value) {
    case AgentIssue::Unspecified: return "unspecified";
    case AgentIssue::InternalToolError: return "internal-tool-error";
    case AgentIssue::UnhandledException: return "unhandled-exception";
    case AgentIssue::ApplicationSaveDefect: return "application-save-defect";
    case AgentIssue::AssertionFailure: return "assertion-failure";
    case AgentIssue::UnexplainedRenderFailure: return "unexplained-render-failure";
    case AgentIssue::UserMapError: return "user-map-error";
    case AgentIssue::ToolUsabilityGap: return "tool-usability-gap";
    case AgentIssue::CompatibilityGap: return "compatibility-gap";
    }
    return "invalid";
}

Result<ReportData> CleanDraft(const DraftInput& input, const PrivacyContext& context, ReportLimits limits) {
    try {
        const auto limitStatus = ValidateReportLimits(limits);
        if (!limitStatus.Succeeded()) { Result<ReportData> failure; failure.diagnostics = limitStatus.diagnostics; return failure; }
        const auto contextStatus = ValidatePrivacyContext(context);
        if (!contextStatus.Succeeded()) { Result<ReportData> failure; failure.diagnostics = contextStatus.diagnostics; return failure; }
        if (SourceName(input.source) == "invalid" || KindName(input.kind) == "invalid" || AgentIssueName(input.agentIssue) == "invalid")
            return Failure<ReportData>("FEEDBACK_REPORT_ENUM", "反馈来源或类别无效。");
        if (input.source == ReportSource::Agent && (!ProgramIssue(input.agentIssue) || input.kind != ReportKind::Problem
            || input.errorCode.empty() || input.callSite.empty() || input.application.version.empty()))
            return Failure<ReportData>("FEEDBACK_AGENT_ISSUE_REQUIRED", "Agent 只可起草具有错误码、调用位置和版本的程序自身问题。");
        if (input.source == ReportSource::User && input.agentIssue != AgentIssue::Unspecified)
            return Failure<ReportData>("FEEDBACK_REPORT_ENUM", "用户反馈不接受 Agent 程序问题分类。");
        if (input.diagnostic.empty() && input.userDescription.empty() && input.errorCode.empty())
            return Failure<ReportData>("FEEDBACK_DRAFT_EMPTY", "反馈报告没有描述或诊断。");
        if (input.callStack.size() > limits.maxStackFrames || !ValidCommit(input.application.commit, context))
            return Failure<ReportData>("FEEDBACK_REPORT_METADATA", "反馈提交身份或调用栈容量无效。");
        ReportData report;
        report.source = input.source;
        report.kind = input.kind;
        report.agentIssue = input.agentIssue;
        report.application.commit = input.application.commit;
        auto clean = [&](std::string_view raw, std::size_t maxBytes, std::string& target) -> Status {
            PrivacyLimits privacy;
            privacy.maxTextBytes = maxBytes;
            auto value = CleanPrivateText(raw, context, privacy);
            if (!value.Succeeded()) return Status{value.diagnostics};
            target = std::move(value.value->text);
            return Status::Ok();
        };
        const auto pairs = std::array{
            std::tuple{std::string_view(input.application.version), std::size_t{128}, &report.application.version},
            std::tuple{std::string_view(input.application.operatingSystem), std::size_t{512}, &report.application.operatingSystem},
            std::tuple{std::string_view(input.application.graphicsAdapter), std::size_t{512}, &report.application.graphicsAdapter},
            std::tuple{std::string_view(input.errorCode), std::size_t{256}, &report.errorCode},
            std::tuple{std::string_view(input.callSite), std::size_t{4096}, &report.callSite},
            std::tuple{std::string_view(input.toolName), std::size_t{256}, &report.toolName},
            std::tuple{std::string_view(input.diagnostic), limits.maxDiagnosticBytes, &report.diagnostic},
            std::tuple{std::string_view(input.userDescription), limits.maxDescriptionBytes, &report.userDescription}};
        for (const auto& [raw, maxBytes, target] : pairs) {
            const auto status = clean(raw, maxBytes, *target);
            if (!status.Succeeded()) { Result<ReportData> failure; failure.diagnostics = status.diagnostics; return failure; }
        }
        for (const auto& frame : input.callStack) {
            std::string cleaned;
            const auto status = clean(frame, 4096, cleaned);
            if (!status.Succeeded()) { Result<ReportData> failure; failure.diagnostics = status.diagnostics; return failure; }
            report.callStack.push_back(std::move(cleaned));
        }
        if (input.includeLogs) {
            std::string cleaned;
            const auto status = clean(input.recentLogs, limits.maxLogBytes, cleaned);
            if (!status.Succeeded()) { Result<ReportData> failure; failure.diagnostics = status.diagnostics; return failure; }
            report.logs = std::move(cleaned);
        }
        if (input.includeConversation) {
            if (input.source != ReportSource::User) return Failure<ReportData>("FEEDBACK_CONVERSATION_USER_ONLY", "对话片段只能由用户明确选择，Agent 不可自动附带。");
            std::string cleaned;
            const auto status = clean(input.conversationSnippet, limits.maxConversationBytes, cleaned);
            if (!status.Succeeded()) { Result<ReportData> failure; failure.diagnostics = status.diagnostics; return failure; }
            report.conversationSnippet = std::move(cleaned);
        }
        if (input.includeScreenshot) {
            if (input.source != ReportSource::User) return Failure<ReportData>("FEEDBACK_SCREENSHOT_USER_ONLY", "截图只能由用户明确选择，Agent 不可自动附带。");
            auto screenshot = PrepareScreenshot(input.screenshot.png, limits.maxScreenshotBytes);
            if (!screenshot.Succeeded()) { Result<ReportData> failure; failure.diagnostics = screenshot.diagnostics; return failure; }
            if (input.screenshot.userReviewedSha256 != screenshot.value->sha256)
                return Failure<ReportData>("FEEDBACK_SCREENSHOT_REVIEW_REQUIRED", "用户必须预览并确认清理附加信息后的完整 PNG 附件。");
            report.screenshot = std::move(*screenshot.value);
        }
        return Result<ReportData>::Success(std::move(report));
    } catch (const std::exception&) {
        return Failure<ReportData>("FEEDBACK_REPORT_FAILED", "反馈报告清理失败，未保存原始内容。");
    }
}

Status ValidateCleanReport(const ReportData& report, const PrivacyContext& context, ReportLimits limits) {
    DraftInput input;
    input.source = report.source;
    input.kind = report.kind;
    input.agentIssue = report.agentIssue;
    input.application = report.application;
    input.errorCode = report.errorCode;
    input.callSite = report.callSite;
    input.toolName = report.toolName;
    input.diagnostic = report.diagnostic;
    input.callStack = report.callStack;
    input.userDescription = report.userDescription;
    if (report.logs) { input.includeLogs = true; input.recentLogs = *report.logs; }
    if (report.conversationSnippet) { input.includeConversation = true; input.conversationSnippet = *report.conversationSnippet; }
    if (report.screenshot) {
        input.includeScreenshot = true;
        input.screenshot.png = report.screenshot->png;
        input.screenshot.userReviewedSha256 = report.screenshot->sha256;
    }
    const auto cleaned = CleanDraft(input, context, limits);
    if (!cleaned.Succeeded()) return Status{cleaned.diagnostics};
    if (*cleaned.value != report) return Status::Failure("Feedback.Report", "FEEDBACK_UNCLEAN_REPORT", "反馈载荷未通过清理复核，不允许持久化、导出或发送。");
    return Status::Ok();
}

Result<std::string> ProblemFingerprint(std::string_view errorCode, std::string_view callSite, std::string_view version) {
    if (errorCode.empty() || callSite.empty() || version.empty() || errorCode.size() > 256 || callSite.size() > 4096 || version.size() > 128)
        return Failure<std::string>("FEEDBACK_FINGERPRINT_INPUT", "问题去重必须包含有界的错误码、调用位置和版本。");
    std::string identity = "audastra-feedback-problem-v1";
    for (auto value : {errorCode, callSite, version}) {
        identity += std::to_string(value.size());
        identity += ':';
        identity += value;
    }
    return Sha256Text(identity);
}

} // namespace War3Map::Feedback
