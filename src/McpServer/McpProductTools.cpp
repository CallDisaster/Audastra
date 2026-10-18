#include "McpProductTools.h"
#include "../QtCommon/LocalConsentPreference.h"
#include "McpToolInternal.h"
#include "AudastraVersion.generated.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

#include <cmath>
#include <stdexcept>
#include <mutex>

namespace War3Map::Mcp {
namespace {
using namespace Feedback;
constexpr qsizetype kRequestBytes = 64 * 1024;
constexpr std::size_t kResponseBytes = 256 * 1024;
std::mutex updateStatusMutex;
QJsonObject applicationUpdateStatus;
QJsonObject TextSchema(int limit) { return {{"type", "string"}, {"maxLength", limit}}; }
QJsonObject Properties(const QString& name) {
    if (name == "feedback.draft") return {
        {"issue", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"internal_tool_error", "unhandled_exception",
            "application_save_defect", "assertion_failure", "unexplained_render_failure",
            "tool_usability_gap", "compatibility_gap"}}}},
        {"error_code", TextSchema(256)}, {"call_site", TextSchema(4096)}, {"tool_name", TextSchema(256)},
        {"diagnostic", TextSchema(32768)}, {"description", TextSchema(16384)},
        {"call_stack", QJsonObject{{"type", "array"}, {"maxItems", 64}, {"items", TextSchema(4096)}}}};
    if (name == "feedback.list") return {{"offset", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 128}}},
        {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}}}};
    if (name == "feedback.status") return {{"report_id", TextSchema(128)}};
    return {};
}
QJsonArray Required(const QString& name) {
    if (name == "feedback.draft") return {"issue", "error_code", "call_site", "tool_name", "diagnostic"};
    if (name == "feedback.status") return {"report_id"};
    return {};
}
std::string Text(const QJsonObject& object, const char* name, std::size_t maxBytes, bool optional = false) {
    if (optional && !object.contains(name)) return {};
    const auto value = object.value(name);
    if (!value.isString() || value.toString().contains(QChar::Null)) throw std::invalid_argument("string");
    const auto bytes = value.toString().toUtf8();
    if (static_cast<std::size_t>(bytes.size()) > maxBytes || QString::fromUtf8(bytes) != value.toString())
        throw std::invalid_argument("text budget/encoding");
    return bytes.toStdString();
}
std::size_t Index(const QJsonObject& object, const char* name, std::size_t defaultValue, bool nonzero) {
    if (!object.contains(name)) return defaultValue;
    const auto value = object.value(name);
    if (!value.isDouble()) throw std::invalid_argument("index");
    const double number = value.toDouble();
    if (!std::isfinite(number) || number != std::floor(number) || number < (nonzero ? 1 : 0) || number > 128)
        throw std::invalid_argument("index budget");
    return static_cast<std::size_t>(number);
}
QJsonObject Summary(const EntrySummary& entry) {
    QJsonObject result{{"report_id", QString::fromStdString(entry.id)}, {"source", QString::fromUtf8(SourceName(entry.source))},
        {"kind", QString::fromUtf8(KindName(entry.kind))}, {"state", QString::fromUtf8(StateName(entry.state))},
        {"error_code", QString::fromStdString(entry.errorCode)}, {"tool_name", QString::fromStdString(entry.toolName)},
        {"version", QString::fromStdString(entry.version)}, {"occurrences", QString::number(entry.occurrences)},
        {"first_seen_unix_seconds", QString::number(entry.firstSeenUnixSeconds)},
        {"last_seen_unix_seconds", QString::number(entry.lastSeenUnixSeconds)}, {"attempts", static_cast<int>(entry.attempts)},
        {"failure_reason", QString::fromStdString(entry.failureReason)}, {"has_payload", entry.hasPayload},
        {"preview_confirmed", entry.previewConfirmed}, {"has_logs", entry.hasLogs},
        {"has_conversation", entry.hasConversation}, {"has_screenshot", entry.hasScreenshot}};
    if (entry.nextAttemptUnixSeconds) result.insert("next_attempt_unix_seconds", QString::number(*entry.nextAttemptUnixSeconds));
    return result;
}
AgentIssue Issue(const std::string& value) {
    if (value == "internal_tool_error") return AgentIssue::InternalToolError;
    if (value == "unhandled_exception") return AgentIssue::UnhandledException;
    if (value == "application_save_defect") return AgentIssue::ApplicationSaveDefect;
    if (value == "assertion_failure") return AgentIssue::AssertionFailure;
    if (value == "unexplained_render_failure") return AgentIssue::UnexplainedRenderFailure;
    if (value == "tool_usability_gap") return AgentIssue::ToolUsabilityGap;
    if (value == "compatibility_gap") return AgentIssue::CompatibilityGap;
    throw std::invalid_argument("not an application issue");
}
QJsonObject Budget(QJsonObject body) {
    if (static_cast<std::size_t>(QJsonDocument(body).toJson(QJsonDocument::Compact).size()) > kResponseBytes)
        return Failure("FEEDBACK_RESPONSE_BUDGET", "反馈回执超出容量，请缩小页长。");
    return body;
}
} // namespace

std::vector<ToolDefinition> ProductToolDefinitions() {
    std::vector<ToolDefinition> result;
    for (const auto* value : {"feedback.draft", "feedback.list", "feedback.status", "update.status"}) {
        const QString name = value;
        ToolDefinition tool;
        tool.name = name;
        tool.description = name == "feedback.draft"
            ? "起草程序自身问题的清理后本地报告；不附地图/触发器/对话/日志/截图，不发送。未同意时只留本地；用户可在反馈队列查看和导出。"
              "工具不合理或缺能力用 tool_usability_gap，与旧版地图/框架兼容缺口用 compatibility_gap；地图内容本身的问题不在此提交。"
            : name == "update.status" ? "只读当前版本、发布信任配置和宿主的检查状态；不检查网络、不触发下载或安装。"
            : "读取本地反馈队列的清理后状态；不发送、不修改同意项。";
        tool.inputSchema = {{"type", "object"}, {"properties", Properties(name)},
            {"required", Required(name)}, {"additionalProperties", false}};
        tool.permission = name == "feedback.draft" ? ToolPermission::Edit : ToolPermission::Query;
        tool.maxResponseBytes = kResponseBytes;
        tool.maxRequestIdBytes = 4096;
        result.push_back(std::move(tool));
    }
    return result;
}

QJsonObject InvokeProductTool(const QString& name, const QJsonObject& arguments, const ProductToolHost& host) {
    try {
        const auto properties = Properties(name);
        if (name != "feedback.draft" && name != "feedback.list" && name != "feedback.status" && name != "update.status")
            return Failure("PRODUCT_TOOL_UNKNOWN", "未知产品工具。");
        if (QJsonDocument(arguments).toJson(QJsonDocument::Compact).size() > kRequestBytes)
            return Failure("FEEDBACK_REQUEST_BUDGET", "反馈请求超出64 KiB。");
        for (auto it = arguments.begin(); it != arguments.end(); ++it) if (!properties.contains(it.key()))
            return Failure("FEEDBACK_ARGUMENT_UNKNOWN", "反馈请求含未知字段；地图、触发器或对话正文不能由此工具附带。");
        for (const auto& required : Required(name)) if (!arguments.contains(required.toString()))
            return Failure("FEEDBACK_ARGUMENT_REQUIRED", "反馈请求缺少必需字段。");
        if (name == "update.status") {
            auto result = host.updateStatus ? host.updateStatus() : QJsonObject{};
            result.insert("ok", true);
            result.insert("current_version", QString::fromUtf8(BuildInfo::Version));
            result.insert("commit", QString::fromUtf8(BuildInfo::Commit));
            result.insert("release_owner", QString::fromUtf8(BuildInfo::ReleaseOwner));
            result.insert("release_repository", QString::fromUtf8(BuildInfo::ReleaseRepository));
            result.insert("trusted_key_configured", !BuildInfo::UpdatePublicKeyHex.empty());
            result.insert("installation_available_to_agent", false);
            if (!result.contains("check_state")) result.insert("check_state", "not_checked");
            return Budget(std::move(result));
        }
        if (!host.feedback) return Failure("FEEDBACK_HOST_UNAVAILABLE", "反馈服务尚未配置。");
        auto queue = host.feedback();
        if (!queue.Succeeded()) return DiagnosticsJson(queue.diagnostics);
        if (name == "feedback.draft") {
            DraftInput draft;
            draft.source = ReportSource::Agent;
            draft.kind = ReportKind::Problem;
            draft.agentIssue = Issue(Text(arguments, "issue", 64));
            draft.application = {std::string(BuildInfo::Version), std::string(BuildInfo::Commit),
                QSysInfo::prettyProductName().toUtf8().toStdString(), {}};
            draft.errorCode = Text(arguments, "error_code", 256);
            draft.callSite = Text(arguments, "call_site", 4096);
            draft.toolName = Text(arguments, "tool_name", 256);
            draft.diagnostic = Text(arguments, "diagnostic", 32768);
            draft.userDescription = Text(arguments, "description", 16384, true);
            if (arguments.contains("call_stack")) {
                if (!arguments.value("call_stack").isArray() || arguments.value("call_stack").toArray().size() > 64)
                    throw std::invalid_argument("stack budget");
                for (const auto& frame : arguments.value("call_stack").toArray())
                    draft.callStack.push_back(Text(QJsonObject{{"frame", frame}}, "frame", 4096));
            }
            auto result = (*queue.value)->Draft(draft, host.nowUnixSeconds ? host.nowUnixSeconds() : QDateTime::currentSecsSinceEpoch());
            if (!result.Succeeded()) return DiagnosticsJson(result.diagnostics);
            return Budget({{"ok", true}, {"entry", Summary(result.value->entry)}, {"consent", result.value->consent},
                {"deduplicated", result.value->deduplicated}, {"sent", false}, {"message", QString::fromStdString(result.value->message)}});
        }
        if (name == "feedback.list") {
            const auto offset = Index(arguments, "offset", 0, false), limit = Index(arguments, "limit", 50, true);
            auto entries = (*queue.value)->List(offset, limit);
            if (!entries.Succeeded()) return DiagnosticsJson(entries.diagnostics);
            QJsonArray array;
            for (const auto& entry : *entries.value) array.push_back(Summary(entry));
            return Budget({{"ok", true}, {"entries", array}, {"offset", static_cast<int>(offset)},
                {"limit", static_cast<int>(limit)}, {"consent", (*queue.value)->Consent()}, {"sent", false}});
        }
        auto entry = (*queue.value)->GetStatus(Text(arguments, "report_id", 128));
        if (!entry.Succeeded()) return DiagnosticsJson(entry.diagnostics);
        return Budget({{"ok", true}, {"entry", Summary(*entry.value)}, {"consent", (*queue.value)->Consent()}, {"sent", false}});
    } catch (const std::invalid_argument&) {
        return Failure("FEEDBACK_ARGUMENT_INVALID", "反馈参数类型、内容、编码或容量无效。");
    } catch (const std::exception&) {
        return Failure("FEEDBACK_OPERATION_FAILED", "反馈操作未完成，未发送报告。");
    }
}

Result<std::shared_ptr<FeedbackQueue>> ApplicationFeedbackQueue() {
    static std::shared_ptr<FeedbackQueue> queue;
    if (queue) return Result<std::shared_ptr<FeedbackQueue>>::Success(queue);
    // 仅对系统选定的既有应用数据根解析别名；队列自身随后仍检查普通路径和独占租约。
    const auto generic = QFileInfo(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).canonicalFilePath();
    if (generic.isEmpty()) return Result<std::shared_ptr<FeedbackQueue>>::Failure("Feedback", "FEEDBACK_DATA_DIRECTORY", "无法定位本机应用数据目录。");
    PrivacyContext context;
    context.paths.push_back({PathRole::User, QDir::homePath().toUtf8().toStdString()});
    QSettings preferences(QSettings::defaultFormat(), QSettings::UserScope, "CallDisaster", "Audastra");
    const auto gameRoot = preferences.value("war3/gameRoot").toString();
    const auto ydweRoot = preferences.value("ui/dataRoot").toString();
    if (!gameRoot.isEmpty()) context.paths.push_back({PathRole::Game, gameRoot.toUtf8().toStdString()});
    if (!ydweRoot.isEmpty()) context.paths.push_back({PathRole::Ydwe, ydweRoot.toUtf8().toStdString()});
    auto opened = FeedbackQueue::Open(std::filesystem::path(QDir(generic).filePath("CallDisaster/Audastra/feedback").toStdWString()), context);
    if (!opened.Succeeded()) return opened;
    if (QtCommon::ReadExplicitLocalConsent(preferences, QStringLiteral("feedback/consent"))) {
        const auto restored = (*opened.value)->SetConsent(true);
        if (!restored.Succeeded()) return {{}, restored.diagnostics};
    }
    queue = *opened.value;
    return opened;
}
ProductToolHost DefaultProductToolHost() {
    return {[] { return ApplicationFeedbackQueue(); }, [] { return QDateTime::currentSecsSinceEpoch(); }, [] {
        const std::lock_guard lock(updateStatusMutex);
        return applicationUpdateStatus;
    }};
}
void PublishApplicationUpdateStatus(QJsonObject status) {
    const std::lock_guard lock(updateStatusMutex);
    applicationUpdateStatus = std::move(status);
}
void RegisterProductTools(McpServer& server, ProductToolHost host) {
    for (auto definition : ProductToolDefinitions()) {
        const auto name = definition.name;
        definition.handler = [name, host](const QJsonObject& arguments) { return InvokeProductTool(name, arguments, host); };
        server.RegisterTool(std::move(definition));
    }
}
} // namespace War3Map::Mcp
