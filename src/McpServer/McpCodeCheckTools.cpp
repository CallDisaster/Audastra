#include "McpCodeCheckTools.h"
#include "McpToolInternal.h"
#include "McpWorkspace.h"

#include "War3MapCore/Trigger/TriggerCodeEditing.h"

#include <QJsonDocument>
#include <QUuid>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace War3Map::Mcp {
namespace {
constexpr std::size_t kTextBytes = 512 * 1024;
constexpr std::size_t kJsonBytes = 256 * 1024;
constexpr std::size_t kJobs = 16, kPending = 4;
struct Job {
    std::string id, binding, target;
    std::uint64_t generation = 0;
    std::stop_source cancellation;
    std::function<Services::CodeCheckResult(std::stop_token)> run;
    std::optional<Services::CodeCheckResult> result;
    bool running = false, done = false, failed = false;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
};
std::string Text(const QJsonObject& arguments, const char* key, std::size_t limit, std::string fallback = {}) {
    if (!arguments.contains(key)) return fallback;
    const auto value = arguments.value(key);
    if (!value.isString() || value.toString().contains(QChar::Null)) throw std::invalid_argument("text");
    const auto bytes = value.toString().toUtf8();
    if (static_cast<std::size_t>(bytes.size()) > limit || QString::fromUtf8(bytes) != value.toString())
        throw std::invalid_argument("text budget");
    return bytes.toStdString();
}
std::uint64_t Unsigned(const QJsonObject& arguments, const char* key, std::uint64_t fallback = 0) {
    if (!arguments.contains(key)) return fallback;
    const auto value = arguments.value(key);
    bool ok = false;
    const auto text = value.toString();
    const auto number = text.toULongLong(&ok);
    if (!value.isString() || !ok || QString::number(number) != text) throw std::invalid_argument("uint64");
    return number;
}
QJsonObject DiagnosticJson(const Services::CodeCheckDiagnostic& diagnostic) {
    QJsonObject output{{"severity", diagnostic.severity == DiagnosticSeverity::Error ? "error"
        : diagnostic.severity == DiagnosticSeverity::Warning ? "warning" : "info"},
        {"source", QString::fromStdString(diagnostic.source)}, {"code", QString::fromStdString(diagnostic.code)},
        {"message", QString::fromStdString(diagnostic.message)}, {"line", static_cast<qint64>(diagnostic.line)},
        {"byte_column", static_cast<qint64>(diagnostic.byteColumn)}, {"byte_length", static_cast<qint64>(diagnostic.byteLength)},
        {"origin_mapped", diagnostic.originMapped}, {"byte_range_exact", diagnostic.byteRangeExact}};
    // 落在 GUI 语句生成代码上：给出紧凑 T 位置（trigger_text 导出的行列）。
    if (const auto& at = diagnostic.trigger) {
        output.insert("t", QJsonObject{{"trigger_id", QString::number(at->triggerId)},
            {"trigger_name", QString::fromStdString(at->triggerName)}, {"eca_id", QString::number(at->ecaId)},
            {"function", QString::fromStdString(at->function)}, {"line", static_cast<qint64>(at->line)},
            {"column", static_cast<qint64>(at->column)}, {"generated", QString::fromStdString(at->generated)}});
    }
    return output;
}
} // namespace

struct CodeCheckToolSession::Impl {
    std::mutex mutex;
    std::condition_variable condition;
    std::map<std::string, std::shared_ptr<Job>> jobs;
    std::deque<std::shared_ptr<Job>> pending;
    std::thread worker;
    bool stopped = false, available = true;
    std::uint64_t generation = 0;

    static void Run(const std::shared_ptr<Impl>& state) noexcept {
        try {
            for (;;) {
                std::shared_ptr<Job> job;
                {
                    std::unique_lock lock(state->mutex);
                    state->condition.wait(lock, [&] { return state->stopped || !state->pending.empty(); });
                    if (state->stopped) return;
                    job = state->pending.front(); state->pending.pop_front();
                    if (job->done) continue;
                    job->running = true;
                }
                try {
                    auto checked = job->run(job->cancellation.get_token());
                    checked.generation = job->generation; checked.contextBinding = job->binding;
                    std::lock_guard lock(state->mutex);
                    job->result.emplace(std::move(checked)); job->run = {}; job->running = false; job->done = true;
                } catch (...) {
                    std::lock_guard lock(state->mutex);
                    job->run = {}; job->running = false; job->done = true; job->failed = true;
                }
            }
        } catch (...) {
            try {
                std::lock_guard lock(state->mutex); state->available = false;
                for (auto& [id, job] : state->jobs) { (void)id; job->cancellation.request_stop(); job->failed = true; job->done = true; }
            } catch (...) {}
        }
    }
    void Prune(bool needSlot = false) {
        const auto now = std::chrono::steady_clock::now();
        for (auto at = jobs.begin(); at != jobs.end();) {
            if (at->second->done && now - at->second->created > std::chrono::minutes(5)) at = jobs.erase(at);
            else ++at;
        }
        while (needSlot && jobs.size() >= kJobs) {
            auto oldest = jobs.end();
            for (auto at = jobs.begin(); at != jobs.end(); ++at)
                if (at->second->done && (oldest == jobs.end() || at->second->created < oldest->second->created)) oldest = at;
            if (oldest == jobs.end()) break;
            jobs.erase(oldest);
        }
        std::erase_if(pending, [](const auto& job) { return job->done; });
    }
};

CodeCheckToolSession::CodeCheckToolSession() : impl_(std::make_shared<Impl>()) {
    try { impl_->worker = std::thread([state = impl_] { Impl::Run(state); }); }
    catch (...) { impl_->available = false; }
}
CodeCheckToolSession::~CodeCheckToolSession() {
    {
        std::lock_guard lock(impl_->mutex); impl_->stopped = true;
        for (auto& [id, job] : impl_->jobs) { (void)id; job->cancellation.request_stop(); }
        for (auto& job : impl_->pending) { job->run = {}; job->done = true; }
        impl_->pending.clear();
    }
    impl_->condition.notify_all();
    // 后台只拥有state/job/不可变来源；窗口销毁不join，也不等待磁盘或编译检查。
    if (impl_->worker.joinable()) impl_->worker.detach();
}

QJsonObject CodeCheckToolSession::Invoke(const QJsonObject& arguments, const CodeCheckToolHost& host) {
    try {
        if (QJsonDocument(arguments).toJson(QJsonDocument::Compact).size() > static_cast<qsizetype>(kTextBytes + 8192))
            return Failure("CODE_CHECK_REQUEST_BUDGET", "代码检查请求超出512KiB正文预算。");
        const auto properties = CodeCheckToolDefinition().inputSchema.value("properties").toObject();
        for (auto it = arguments.begin(); it != arguments.end(); ++it) if (!properties.contains(it.key()))
            return Failure("CODE_CHECK_ARGUMENT_UNKNOWN", "代码检查请求含未知字段。");
        const auto action = Text(arguments, "action", 16, "submit");
        if (action == "submit") {
            CodeCheckToolInput input;
            const auto language = Text(arguments, "language", 16, "jass");
            if (language == "jass") input.language = Services::CodeLanguage::Jass;
            else if (language == "vjass") input.language = Services::CodeLanguage::VJass;
            else if (language == "lua53") input.language = Services::CodeLanguage::Lua53;
            else return Failure("CODE_CHECK_LANGUAGE_INVALID", "仅支持jass、vjass或lua53。");
            const auto kind = Text(arguments, "source_kind", 16, "text");
            if (kind != "text" && kind != "block") return Failure("CODE_CHECK_SOURCE_INVALID", "来源须为text或block。");
            input.block = kind == "block"; input.hasText = arguments.contains("text");
            input.text = Text(arguments, "text", kTextBytes);
            input.sourceName = Text(arguments, "source_name", 1024, language == "lua53" ? "MCP/input.lua" : "MCP/input.j");
            const auto block = Text(arguments, "block_kind", 16, "header");
            if (block != "header" && block != "trigger") return Failure("CODE_CHECK_BLOCK_INVALID", "代码块须为header或trigger。");
            input.target = {block == "header" ? Trigger::CodeBlockKind::GlobalHeader : Trigger::CodeBlockKind::CustomTrigger,
                Unsigned(arguments, "trigger_id")};
            if ((!input.block && !input.hasText) || (input.block && block == "trigger" && input.target.triggerId == 0))
                return Failure("CODE_CHECK_SOURCE_REQUIRED", "文本请求须提供text，触发器代码块须提供稳定trigger_id。");
            if (!host.capture) return Failure("CODE_CHECK_HOST_UNAVAILABLE", "宿主没有代码来源冻结入口。");
            const auto generation = ReserveGeneration();
            if (!generation.Succeeded() || !generation.value) return Failure(generation);
            input.generation = *generation.value;
            auto captured = host.capture(std::move(input));
            if (!captured.Succeeded() || !captured.value) return Failure(captured);
            return Enqueue(std::move(*captured.value), *generation.value);
        }
        if (action != "status" && action != "cancel") return Failure("CODE_CHECK_ACTION_INVALID", "action须为submit、status或cancel。");
        return Report(arguments, host.currentBinding ? host.currentBinding() : std::string{});
    } catch (const std::exception&) { return Failure("CODE_CHECK_ARGUMENT_INVALID", "代码检查字段、预算或工作项无效，未执行作者代码。"); }
}

Result<std::uint64_t> CodeCheckToolSession::ReserveGeneration() {
    std::lock_guard lock(impl_->mutex); impl_->Prune(true);
    if (!impl_->available || impl_->stopped)
        return Result<std::uint64_t>::Failure("MCP", "CODE_CHECK_WORKER_UNAVAILABLE", "后台检查工作线程不可用。");
    if (impl_->pending.size() >= kPending || impl_->jobs.size() >= kJobs)
        return Result<std::uint64_t>::Failure("MCP", "CODE_CHECK_QUEUE_FULL", "检查队列已满；取消旧请求或等待完成。");
    if (impl_->generation == (std::numeric_limits<std::uint64_t>::max)())
        return Result<std::uint64_t>::Failure("MCP", "CODE_CHECK_GENERATION_EXHAUSTED", "检查请求修订序号已耗尽，请重新建立会话。");
    return Result<std::uint64_t>::Success(++impl_->generation);
}

QJsonObject CodeCheckToolSession::Enqueue(CodeCheckToolWork work, std::uint64_t generation) {
    if (!work.run || work.binding.size() > 4096 || work.targetKey.size() > 4096)
        return Failure("CODE_CHECK_CAPTURE_INVALID", "宿主未冻结有界的完整工作项。");
    auto job = std::make_shared<Job>();
    job->id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    job->binding = work.binding; job->target = work.targetKey; job->generation = generation;
    job->run = std::move(work.run);
    {
        std::lock_guard lock(impl_->mutex); impl_->Prune(true);
        if (!impl_->available || impl_->stopped || impl_->pending.size() >= kPending || impl_->jobs.size() >= kJobs)
            return Failure("CODE_CHECK_QUEUE_FULL", "后台检查已停止或队列已满。");
        for (auto& [id, old] : impl_->jobs) {
            (void)id;
            if (!old->done && old->binding == job->binding && old->target == job->target) {
                old->cancellation.request_stop();
                if (!old->running) { old->run = {}; old->done = true; }
            }
        }
        impl_->Prune(true); impl_->jobs.emplace(job->id, job); impl_->pending.push_back(job);
    }
    impl_->condition.notify_one();
    return {{"ok", true}, {"schema_version", 1}, {"request_id", QString::fromStdString(job->id)},
        {"state", "queued"}, {"generation", QString::number(job->generation)},
        {"context_binding", QString::fromStdString(job->binding)}, {"executes_code", false}, {"writes_map", false}};
}

bool CodeCheckToolSession::Pending(const std::string& requestId) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->jobs.find(requestId);
    return found != impl_->jobs.end() && !found->second->done && !found->second->cancellation.stop_requested();
}

QJsonObject CodeCheckToolSession::Report(const QJsonObject& arguments, const std::string& currentBinding) {
    try {
        const auto action = Text(arguments, "action", 16, "status");
        if (action != "status" && action != "cancel") return Failure("CODE_CHECK_ACTION_INVALID", "action须为status或cancel。");
        const auto id = Text(arguments, "request_id", 128);
        std::lock_guard lock(impl_->mutex);
        impl_->Prune();
        const auto found = impl_->jobs.find(id);
        if (found == impl_->jobs.end()) return Failure("CODE_CHECK_REQUEST_UNKNOWN", "检查请求不存在或已超过5分钟保留期。");
        auto& job = *found->second;
        const bool stale = !job.binding.empty() && currentBinding != job.binding;
        if ((!job.done && action == "cancel") || stale) {
            job.cancellation.request_stop();
            if (!job.running) { job.run = {}; job.done = true; }
        }
        QJsonObject output{{"ok", true}, {"schema_version", 1}, {"request_id", QString::fromStdString(job.id)},
            {"generation", QString::number(job.generation)}, {"context_binding", QString::fromStdString(job.binding)},
            {"stale", stale}, {"executes_code", false}, {"writes_map", false}};
        output.insert("state", stale ? "stale" : job.cancellation.stop_requested() ? "cancelled"
            : !job.done ? job.running ? "running" : "queued" : job.failed ? "failed" : "complete");
        if (job.cancellation.stop_requested()) {
            output.insert("completed", false); output.insert("passed", false); output.insert("cancelled", true);
            return output;
        }
        if (!job.done || stale || !job.result) return output;
        const auto& result = *job.result;
        output.insert("completed", result.completed); output.insert("passed", result.Succeeded());
        output.insert("syntax_checked", result.syntaxChecked); output.insert("semantic_checked", result.semanticChecked);
        output.insert("context_complete", result.contextComplete); output.insert("cancelled", result.cancelled);
        output.insert("navigation_sources_complete", result.externalSourcesComplete);
        output.insert("reason", QString::fromStdString(result.reason));
        const auto offset = Unsigned(arguments, "offset");
        const auto limit = Unsigned(arguments, "limit", 64);
        if (limit < 1 || limit > 128 || offset > result.diagnostics.size()) return Failure("CODE_CHECK_PAGE_INVALID", "诊断页长须1..128，offset不能超出结果。");
        QJsonArray diagnostics;
        std::size_t bytes = static_cast<std::size_t>(QJsonDocument(output).toJson(QJsonDocument::Compact).size()) + 4096;
        for (std::size_t at = static_cast<std::size_t>(offset); at < result.diagnostics.size() && static_cast<std::size_t>(diagnostics.size()) < limit; ++at) {
            const auto item = DiagnosticJson(result.diagnostics[at]);
            const auto size = static_cast<std::size_t>(QJsonDocument(item).toJson(QJsonDocument::Compact).size()) + 1;
            if (size > kJsonBytes - bytes) break;
            bytes += size; diagnostics.append(item);
        }
        output.insert("diagnostics", diagnostics); output.insert("diagnostic_count", QString::number(result.diagnostics.size()));
        output.insert("next_offset", QString::number(offset + diagnostics.size()));
        output.insert("diagnostics_complete", offset + diagnostics.size() == result.diagnostics.size());
        return output;
    } catch (const std::exception&) { return Failure("CODE_CHECK_ARGUMENT_INVALID", "代码检查字段、预算或工作项无效，未执行作者代码。"); }
}

ToolDefinition CodeCheckToolDefinition() {
    ToolDefinition tool;
    tool.name = "code.check"; tool.description = "异步检查JASS/vJass/Lua5.3；submit冻结后后台运行，status分页读取同代码编辑器诊断，cancel取消。只编译检查，不执行代码或写地图；不完整上下文明示仅语法检查。";
    const QJsonObject uintSchema{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}};
    tool.inputSchema = {{"type", "object"}, {"additionalProperties", false}, {"properties", QJsonObject{
        {"action", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"submit", "status", "cancel"}}}},
        {"language", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"jass", "vjass", "lua53"}}}},
        {"source_kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"text", "block"}}}},
        {"text", QJsonObject{{"type", "string"}, {"maxLength", static_cast<int>(kTextBytes)}}},
        {"source_name", QJsonObject{{"type", "string"}, {"maxLength", 1024}}},
        {"block_kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"header", "trigger"}}}},
        {"trigger_id", uintSchema}, {"request_id", QJsonObject{{"type", "string"}, {"maxLength", 128}}},
        {"offset", uintSchema}, {"limit", uintSchema},
        {"project_identity", QJsonObject{{"type", "string"}, {"maxLength", 128}}}, {"expected_revision", QJsonObject{{"type", "object"}}}}}};
    tool.permission = ToolPermission::Query; tool.maxResponseBytes = kJsonBytes + 8192; tool.maxRequestIdBytes = 4096;
    return tool;
}

void RegisterCodeCheckTool(McpServer& server) {
    auto session = std::make_shared<CodeCheckToolSession>();
    auto definition = CodeCheckToolDefinition();
    definition.handler = [&server, session](const QJsonObject& arguments) -> QJsonValue {
        CodeCheckToolHost host;
        host.currentBinding = [&server] {
            const auto* workspace = server.Workspace();
            return workspace ? server.DocumentId() + ":" + std::to_string(server.DocumentGeneration()) + ":"
                + std::to_string(workspace->Revision().generation) + ":" + workspace->Revision().sourceSha256 : std::string{};
        };
        host.capture = [&server, &arguments, binding = host.currentBinding()](CodeCheckToolInput input) -> Result<CodeCheckToolWork> {
            if (input.block) {
                auto expected = ParseRevision(arguments);
                if (!expected.Succeeded() || !expected.value) return {{}, std::move(expected.diagnostics)};
                auto* workspace = server.Workspace();
                if (!workspace || workspace->Revision() != *expected.value) return Result<CodeCheckToolWork>::Failure(
                    "MCP", "MCP_REVISION_CONFLICT", "代码块请求的expected_revision已过期。");
                const auto& document = workspace->VisibleDocument();
                const Trigger::RawText* source = nullptr;
                if (input.target.kind == Trigger::CodeBlockKind::GlobalHeader) source = &document.wct.customCode;
                else for (const auto& trigger : document.triggers) if (trigger.id == input.target.triggerId && trigger.customTextFlag != 0) { source = &trigger.customText; break; }
                if (!source) return Result<CodeCheckToolWork>::Failure("MCP", "CODE_CHECK_BLOCK_MISSING", "代码块不存在或仍为GUI触发器。");
                if (!input.hasText) input.text = Trigger::TriggerCodeEditor::NormalizedText(*source);
                input.sourceName = input.target.kind == Trigger::CodeBlockKind::GlobalHeader ? "WCT/header.j" : "WCT/trigger-" + std::to_string(input.target.triggerId) + ".j";
            }
            Services::CodeCheckRequest request;
            request.language = input.language; request.source = {input.sourceName, std::move(input.text)};
            request.generation = input.generation; request.context.binding = binding;
            request.context.reason = "展开目录会话未冻结整图保存声明，仅完成正文语法检查。";
            CodeCheckToolWork work{binding, input.sourceName,
                [request = std::move(request)](std::stop_token cancel) { return Services::CheckCode(request, cancel); }};
            return Result<CodeCheckToolWork>::Success(std::move(work));
        };
        return session->Invoke(arguments, host);
    };
    server.RegisterTool(std::move(definition));
}
} // namespace War3Map::Mcp
