#pragma once

#include "War3MapCore/Feedback/Privacy.h"

namespace War3Map::Feedback {

enum class ReportSource : std::uint8_t { User, Agent };
enum class ReportKind : std::uint8_t { Problem, Suggestion };
enum class AgentIssue : std::uint8_t {
    Unspecified, InternalToolError, UnhandledException, ApplicationSaveDefect,
    AssertionFailure, UnexplainedRenderFailure, UserMapError,
    // 程序工具本身不合理或缺能力（非崩溃）：仍属程序自身问题，不承载地图内容。
    ToolUsabilityGap, CompatibilityGap
};

struct ApplicationInfo {
    std::string version;
    std::string commit;
    std::string operatingSystem;
    std::string graphicsAdapter;
    [[nodiscard]] bool operator==(const ApplicationInfo&) const = default;
};

struct ScreenshotInput {
    std::vector<std::byte> png;
    // 必须匹配 PrepareScreenshot 输出，表明用户已查看最终附件。
    std::string userReviewedSha256;
};

struct DraftInput {
    ReportSource source = ReportSource::User;
    ReportKind kind = ReportKind::Problem;
    AgentIssue agentIssue = AgentIssue::Unspecified;
    ApplicationInfo application;
    std::string errorCode;
    std::string callSite;
    std::string toolName;
    std::string diagnostic;
    std::vector<std::string> callStack;
    std::string userDescription;
    bool includeLogs = false;
    std::string recentLogs;
    bool includeConversation = false;
    std::string conversationSnippet;
    bool includeScreenshot = false;
    ScreenshotInput screenshot;
    // 地图、触发器正文没有输入字段；外部 JSON 适配器必须拒绝未知字段。
};

struct ReportLimits {
    std::size_t maxDescriptionBytes = 16 * 1024;
    std::size_t maxDiagnosticBytes = 32 * 1024;
    std::size_t maxLogBytes = 256 * 1024;
    std::size_t maxConversationBytes = 16 * 1024;
    std::size_t maxStackFrames = 64;
    std::size_t maxScreenshotBytes = 4 * 1024 * 1024;
    std::size_t maxReportJsonBytes = 1024 * 1024;
};

[[nodiscard]] Status ValidateReportLimits(const ReportLimits& limits);

// 仅存经过清理的白名单字段。PNG 像素是用户显式审核的附件，不自动抓取。
struct ReportData {
    ReportSource source = ReportSource::User;
    ReportKind kind = ReportKind::Problem;
    AgentIssue agentIssue = AgentIssue::Unspecified;
    ApplicationInfo application;
    std::string errorCode;
    std::string callSite;
    std::string toolName;
    std::string diagnostic;
    std::vector<std::string> callStack;
    std::string userDescription;
    std::optional<std::string> logs;
    std::optional<std::string> conversationSnippet;
    std::optional<PreparedScreenshot> screenshot;
    [[nodiscard]] bool operator==(const ReportData&) const = default;
};

[[nodiscard]] Result<ReportData> CleanDraft(const DraftInput& input,
    const PrivacyContext& context = {}, ReportLimits limits = {});
[[nodiscard]] Status ValidateCleanReport(const ReportData& report,
    const PrivacyContext& context = {}, ReportLimits limits = {});
// 长度前缀绑定 errorCode/callSite/version，原始值只参与摘要，不持久化。
[[nodiscard]] Result<std::string> ProblemFingerprint(std::string_view errorCode,
    std::string_view callSite, std::string_view version);

[[nodiscard]] std::string_view SourceName(ReportSource value) noexcept;
[[nodiscard]] std::string_view KindName(ReportKind value) noexcept;
[[nodiscard]] std::string_view AgentIssueName(AgentIssue value) noexcept;

} // namespace War3Map::Feedback
