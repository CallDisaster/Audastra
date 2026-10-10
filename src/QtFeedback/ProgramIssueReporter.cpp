#include "ProgramIssueReporter.h"

#include <QDateTime>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace War3Map::QtFeedback {
namespace {
// 同一线程的反馈失败不得再次进入上报；不同线程仍由既有 Core 队列串行落盘。
thread_local bool reporting = false;
struct ReportGuard {
    ReportGuard() { reporting = true; }
    ~ReportGuard() { reporting = false; }
};

std::optional<std::size_t> RenderOperation(std::string_view operation) {
    constexpr std::array<std::string_view, 21> operations{
        std::string_view{"createRenderer"}, "attachNativeWindow", "getStatus", "resize",
        "submitTerrainMipScene", "submitTerrainScene", "submitMesh", "submitModelScene",
        "setEnvironment", "updateModelInstances", "setOverlayLines", "setCamera",
        "captureFrameRgba", "captureLayeredFrameRgba", "captureEditorPresentationFrameRgba",
        "getModelLayersTarget", "getViewInfo", "getEditorPresentationTarget",
        "submitEditorModelScene", "submitEditorVisibility", "getFrameMetrics"};
    const auto found = std::find(operations.begin(), operations.end(), operation);
    if (found == operations.end()) return std::nullopt;
    return static_cast<std::size_t>(found - operations.begin());
}
} // namespace

ProgramIssueReporter::ProgramIssueReporter(std::shared_ptr<Feedback::FeedbackQueue> queue,
    Feedback::ApplicationInfo application, Clock clock)
    : queue_(std::move(queue)), application_(std::move(application)), clock_(std::move(clock)) {}

std::optional<Feedback::DraftResult> ProgramIssueReporter::Draft(Feedback::AgentIssue issue,
    std::string_view code, std::string_view callSite, std::string_view diagnostic) noexcept {
    if (!queue_ || reporting) return std::nullopt;
    ReportGuard guard;
    try {
        Feedback::DraftInput input;
        input.source = Feedback::ReportSource::Agent;
        input.kind = Feedback::ReportKind::Problem;
        input.agentIssue = issue;
        input.application = application_;
        input.errorCode = code;
        input.callSite = callSite;
        input.diagnostic = diagnostic;
        const auto draft = queue_->Draft(input, QDateTime::currentSecsSinceEpoch());
        if (draft.Succeeded() && draft.value) return *draft.value;
    } catch (...) {
        // 起草失败保持原故障结果；不调用诊断钩子、网络、其他队列或重试。
    }
    return std::nullopt;
}

std::optional<Feedback::DraftResult> ProgramIssueReporter::UnhandledException(ExceptionBoundary boundary,
    bool standardException) noexcept {
    const auto site = boundary == ExceptionBoundary::Startup ? "application/startup"
        : boundary == ExceptionBoundary::Event ? "application/notify" : "map-save/worker";
    return Draft(Feedback::AgentIssue::UnhandledException,
        standardException ? "APPLICATION_UNHANDLED_CPP_EXCEPTION" : "APPLICATION_UNHANDLED_NONSTANDARD_EXCEPTION",
        site, standardException ? "程序边界捕获了未处理的 C++ 标准异常；当前运行或保存操作已失败。"
            : "程序边界捕获了未处理的非标准 C++ 异常；当前运行或保存操作已失败。");
}

bool ProgramIssueReporter::IsSaveDefect(std::string_view code) noexcept {
    constexpr std::array<std::string_view, 4> codes{std::string_view{"SAVE_TASK_ALREADY_EXECUTED"}, "SAVE_SOURCE_SNAPSHOT_INVALID",
        "SCRIPT_INPUT_MISSING", "SAVE_REVISION_BINDING_MISSING"};
    return std::find(codes.begin(), codes.end(), code) != codes.end();
}

std::optional<Feedback::DraftResult> ProgramIssueReporter::SaveDefect(std::string_view code) noexcept {
    if (code == "SAVE_INTERNAL_CPP_EXCEPTION") return UnhandledException(ExceptionBoundary::SaveWorker, true);
    if (code == "SAVE_INTERNAL_NONSTANDARD_EXCEPTION") return UnhandledException(ExceptionBoundary::SaveWorker, false);
    if (!IsSaveDefect(code)) return std::nullopt;
    return Draft(Feedback::AgentIssue::ApplicationSaveDefect, code, "map-save/host-invariant",
        "可信编辑器保存任务未满足程序内部不变量；保存被拒绝，当前编辑未签为保存成功。");
}

std::optional<Feedback::DraftResult> ProgramIssueReporter::RenderFailure(std::string_view operation,
    std::int32_t result, bool statusAvailable, std::int32_t statusCallResult,
    std::int32_t statusResult, std::string_view boundedMessage) noexcept {
    const auto slot = RenderOperation(operation);
    if (!slot || result == 0 || boundedMessage.size() > 512 || reporting) return std::nullopt;
    // 旧成功状态（硬件名称等）不解释当前拒绝；失败状态或明确失败 message 则沿原诊断展示。
    const bool priorSuccess = boundedMessage.starts_with("D3D11 WARP / Feature Level 11.0 / ")
        || boundedMessage.starts_with("D3D11 hardware / Feature Level 11.0 / ");
    if (statusAvailable && !boundedMessage.empty() && !priorSuccess) return std::nullopt;
    try {
        // 悬停等周期查询可能持续失败；每个允许操作只在成功落盘后限频，不储存插件正文。
        // 锁覆盖起草，避免并发调用同时越过同一槽；保存/异常入口不受此节流影响。
        std::scoped_lock lock(renderMutex_);
        const auto now = clock_ ? clock_() : std::chrono::steady_clock::now();
        auto& last = renderLastDraft_[*slot];
        if (last && now - *last < std::chrono::seconds(60)) return std::nullopt;
        const auto diagnostic = "插件操作 " + std::string(operation) + " 返回 " + std::to_string(result)
            + "；状态可读取=" + (statusAvailable ? "true" : "false")
            + "，状态接口返回=" + std::to_string(statusCallResult) + "，状态结果=" + std::to_string(statusResult)
            + "；插件未提供当前失败的具体诊断。";
        auto draft = Draft(Feedback::AgentIssue::UnexplainedRenderFailure, "RENDER_PLUGIN_UNEXPLAINED_FAILURE",
            "render-plugin/" + std::string(operation), diagnostic);
        if (draft) last = now;
        return draft;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace War3Map::QtFeedback
