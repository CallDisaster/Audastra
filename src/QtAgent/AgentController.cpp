#include "AgentController.h"
#include "AgentSessionStore.h"
#include "CredentialStore.h"
#include "ProjectMemoryService.h"
#include "ProviderCatalog.h"
#include "TokenMeter.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <cmath>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace War3Map::Agent {
namespace {

constexpr qsizetype kLineBudget = 8 * 1024 * 1024;
constexpr qint64 kLogBudget = 64 * 1024 * 1024;
constexpr qsizetype kPromptBudget = 65536;

QString DefaultHistoryDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("agent/history"));
}

QJsonObject ToolError(const QString& code, const QString& message) {
    return {{QStringLiteral("isError"), true}, {QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
}

// JSON 字符串逐值替换，密钥含引号/反斜杠时仍保持合法 JSON。
QJsonValue Redacted(const QJsonValue& value, const QString& secret) {
    if (value.isString()) {
        auto text = value.toString();
        if (!secret.isEmpty()) text.replace(secret, QStringLiteral("[已隐藏密钥]"));
        return text;
    }
    if (value.isArray()) {
        QJsonArray result;
        for (const auto& entry : value.toArray()) result.append(Redacted(entry, secret));
        return result;
    }
    if (value.isObject()) {
        QJsonObject result;
        const auto object = value.toObject();
        for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
            const auto key = iterator.key().toLower();
            if (key == QStringLiteral("apikey") || key == QStringLiteral("api_key")
                || key == QStringLiteral("authorization") || key == QStringLiteral("credentialref")
                || key == QStringLiteral("credential_ref")) continue;
            result.insert(iterator.key(), Redacted(iterator.value(), secret));
        }
        return result;
    }
    return value;
}

QJsonArray HistoryUiEvents(const QJsonArray& messages) {
    QJsonArray events;
    QHash<QString, QJsonObject> calls;
    qsizetype sequence = 0;
    for (const auto& value : messages) {
        const auto message = value.toObject();
        if (message.value(QStringLiteral("forge_injection")).toString() == QStringLiteral("compaction_summary")) {
            QString summary = AgentMessageText(message);
            summary.remove(QStringLiteral("<compacted-summary>"));
            summary.remove(QStringLiteral("</compacted-summary>"));
            events.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("context_compacted")},
                {QStringLiteral("removed_messages"), message.value(QStringLiteral("forge_compacted_count")).toInt()},
                {QStringLiteral("saved_tokens"), message.value(QStringLiteral("forge_saved_tokens")).toInteger()},
                {QStringLiteral("summary"), summary.trimmed()},
                {QStringLiteral("historical"), true},
            });
            continue;
        }
        if (IsHiddenAgentMessage(message)) continue;
        const auto role = message.value(QStringLiteral("role")).toString();
        const auto content = message.value(QStringLiteral("content"));
        QString text = content.isString() ? content.toString() : QString{};
        QString thinking;
        if (content.isArray()) for (const auto& block : content.toArray()) {
            const auto item = block.toObject();
            const QString blockType = item.value(QStringLiteral("type")).toString();
            if (blockType == QStringLiteral("text")) text += item.value(QStringLiteral("text")).toString();
            else if (blockType == QStringLiteral("thinking")) thinking += item.value(QStringLiteral("thinking")).toString();
            if (role == QStringLiteral("assistant") && blockType == QStringLiteral("toolCall")) {
                const auto originalId = item.value(QStringLiteral("id")).toString();
                QJsonObject call{{QStringLiteral("type"), QStringLiteral("tool_start")},
                    {QStringLiteral("id"), QStringLiteral("history-%1-%2").arg(++sequence).arg(originalId)},
                    {QStringLiteral("original_id"), originalId}, {QStringLiteral("historical"), true},
                    {QStringLiteral("tool_name"), item.value(QStringLiteral("name"))},
                    {QStringLiteral("details"), QJsonObject{{QStringLiteral("arguments"), item.value(QStringLiteral("arguments"))}}}};
                calls.insert(originalId, call); events.append(call);
            }
        }
        if (role == QStringLiteral("assistant") && !thinking.trimmed().isEmpty()) {
            events.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("thinking_block")},
                {QStringLiteral("text"), thinking},
                {QStringLiteral("historical"), true},
            });
        }
        if ((role == QStringLiteral("user") || role == QStringLiteral("assistant")) && !text.isEmpty())
            events.append(QJsonObject{{QStringLiteral("type"), role == QStringLiteral("user") ? QStringLiteral("user_message") : QStringLiteral("assistant_message")},
                {QStringLiteral("text"), text}, {QStringLiteral("historical"), true}});
        if (role == QStringLiteral("toolResult")) {
            auto call = calls.take(message.value(QStringLiteral("toolCallId")).toString());
            call.insert(QStringLiteral("type"), QStringLiteral("tool_end"));
            call.insert(QStringLiteral("success"), !message.value(QStringLiteral("isError")).toBool()
                && !message.value(QStringLiteral("details")).toObject().value(QStringLiteral("isError")).toBool());
            call.insert(QStringLiteral("historical"), true);
            call.insert(QStringLiteral("details"), QJsonObject{{QStringLiteral("result"), content}});
            events.append(call);
        }
    }
    return events;
}

QJsonObject EncodeCompositionEvent(const ContextCompositionSnapshot& comp) {
    const auto encodeItems = [](const QList<ContextNamedItem>& items) {
        QJsonArray arr;
        for (const auto& item : items) {
            arr.append(QJsonObject{
                {QStringLiteral("name"), item.name},
                {QStringLiteral("detail"), item.detail},
                {QStringLiteral("tokens"), item.tokens},
            });
        }
        return arr;
    };
    return {
        {QStringLiteral("type"), QStringLiteral("context_composition")},
        {QStringLiteral("messages_tokens"), comp.messagesTokens},
        {QStringLiteral("editor_tools_tokens"), comp.editorToolsTokens},
        {QStringLiteral("mcp_tools_tokens"), comp.mcpToolsTokens},
        {QStringLiteral("system_prompt_tokens"), comp.systemPromptTokens},
        {QStringLiteral("skills_tokens"), comp.skillsTokens},
        {QStringLiteral("mcp_instructions_tokens"), comp.mcpInstructionsTokens},
        {QStringLiteral("memory_files_tokens"), comp.memoryFilesTokens},
        {QStringLiteral("autocompact_threshold_ratio"), comp.autocompactThresholdRatio},
        {QStringLiteral("editor_tool_items"), encodeItems(comp.editorToolItems)},
        {QStringLiteral("mcp_tool_items"), encodeItems(comp.mcpToolItems)},
        {QStringLiteral("skill_items"), encodeItems(comp.skillItems)},
        {QStringLiteral("memory_items"), encodeItems(comp.memoryItems)},
    };
}

} // namespace

class AgentController::Impl {
public:
    explicit Impl(AgentController* owner) : q(owner), credentials(std::make_unique<CredentialStore>()), historyDirectory(DefaultHistoryDirectory()) {
        process = new QProcess(q);
        process->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
            arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            arguments->startupInfo->wShowWindow = SW_HIDE;
        });
#endif
        startupTimer = new QTimer(q); startupTimer->setSingleShot(true);
        stopTimer = new QTimer(q); stopTimer->setSingleShot(true);
        QObject::connect(startupTimer, &QTimer::timeout, q, [this] { Failure(QStringLiteral("PI 辅助进程未在 15 秒内准备完成。")); });
        QObject::connect(stopTimer, &QTimer::timeout, q, [this] {
            if (!toolExecuting && process->state() != QProcess::NotRunning) {
                process->kill();
                Complete(false, true, QStringLiteral("已停止辅助进程，已完成的地图改动保留。"));
            }
        });
        QObject::connect(process, &QProcess::readyReadStandardOutput, q, [this] { ReadOutput(); });
        QObject::connect(process, &QProcess::readyReadStandardError, q, [this] {
            // Node 的错误栈可能带站点回显，原始 stderr 不进入用户日志。
            process->setReadChannel(QProcess::StandardError);
            while (process->bytesAvailable() > 0) process->read(65536);
            process->setReadChannel(QProcess::StandardOutput);
        });
        QObject::connect(process, &QProcess::errorOccurred, q, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) Failure(QStringLiteral("无法启动 PI 辅助进程，请检查 Node 与运行时文件。"));
        });
        QObject::connect(process, &QProcess::finished, q, [this](int code, QProcess::ExitStatus status) {
            ReadOutput(); ready = false; startupTimer->stop(); stopTimer->stop();
            if (busy) {
                if (stopRequested) Complete(false, true, QStringLiteral("已停止，保留已完成改动。"));
                else Failure(QStringLiteral("PI 辅助进程意外退出（代码 %1，%2）；地图和已完成改动保留。")
                    .arg(code).arg(status == QProcess::NormalExit ? QStringLiteral("正常退出") : QStringLiteral("进程异常")));
            }
        });
    }

    QString ComposeFullSystemPrompt() const {
        QString full = systemPrompt;
        if (!skillsPrompt.isEmpty()) full += QStringLiteral("\n") + skillsPrompt;
        if (mcpInstructionsProvider) {
            const QString mcpInst = mcpInstructionsProvider().trimmed();
            if (!mcpInst.isEmpty()) full += QStringLiteral("\n\n# 外部 MCP 服务说明\n") + mcpInst;
        }
        if (memoryService) {
            const QString memSection = memoryService->BuildPromptSection();
            if (!memSection.isEmpty()) full += memSection;
        }
        full += QStringLiteral("\n当前地图：") + mapTitle
            + QStringLiteral("\n恢复历史可能已过期，修改前重新查询当前身份与修订。");
        return full;
    }

    ContextCompositionSnapshot BuildCompositionSnapshot(const QJsonArray& activeMessages) const {
        ContextCompositionSnapshot snap;
        const QString baseSys = systemPrompt + QStringLiteral("\n当前地图：") + mapTitle
            + QStringLiteral("\n恢复历史可能已过期，修改前重新查询当前身份与修订。");
        snap.systemPromptTokens = TokenMeter::EstimateText(baseSys);

        if (!skillItems.isEmpty()) {
            snap.skillItems = skillItems;
            for (const auto& item : skillItems) snap.skillsTokens += item.tokens;
        } else if (!skillsPrompt.isEmpty()) {
            snap.skillsTokens = TokenMeter::EstimateText(skillsPrompt);
        }

        if (mcpInstructionsProvider) {
            const QString inst = mcpInstructionsProvider().trimmed();
            if (!inst.isEmpty()) snap.mcpInstructionsTokens = TokenMeter::EstimateText(inst);
        }

        if (memoryService) {
            for (const auto& mem : memoryService->Snapshots()) {
                snap.memoryItems.push_back({mem.displayName, mem.filePath, mem.tokens});
                snap.memoryFilesTokens += mem.tokens;
            }
            for (const auto& memToolVal : ProjectMemoryService::ToolDefinitions()) {
                const auto tool = memToolVal.toObject();
                const QString name = tool.value(QStringLiteral("name")).toString();
                if (settings.disabledTools.contains(name)) continue;
                const qint64 t = TokenMeter::EstimateTool(tool);
                snap.editorToolsTokens += t;
                snap.editorToolItems.push_back({name, QStringLiteral("记忆工具"), t});
            }
        }

        if (toolsProvider) {
            try {
                for (const auto& val : toolsProvider()) {
                    const auto tool = val.toObject();
                    const QString name = tool.value(QStringLiteral("name")).toString();
                    if (name.isEmpty() || settings.disabledTools.contains(name)) continue;
                    const qint64 t = TokenMeter::EstimateTool(tool);
                    snap.editorToolsTokens += t;
                    snap.editorToolItems.push_back({name, QStringLiteral("内置工具"), t});
                }
            } catch (...) {}
        }

        if (externalToolsProvider) {
            try {
                for (const auto& val : externalToolsProvider()) {
                    const auto tool = val.toObject();
                    const QString name = tool.value(QStringLiteral("name")).toString();
                    if (name.isEmpty() || settings.disabledTools.contains(name)) continue;
                    const qint64 t = TokenMeter::EstimateTool(tool);
                    snap.mcpToolsTokens += t;
                    snap.mcpToolItems.push_back({name, tool.value(QStringLiteral("description")).toString().section(QLatin1Char('\n'), 0, 0), t});
                }
            } catch (...) {}
        }

        for (const auto& val : activeMessages) {
            snap.messagesTokens += TokenMeter::EstimateMessage(val.toObject());
        }
        return snap;
    }

    void PublishContextComposition() {
        const QJsonArray& active = pendingState && !candidateHistory.isEmpty()
            ? candidateHistory
            : (currentSession ? currentSession->messages : history);
        Publish(EncodeCompositionEvent(BuildCompositionSnapshot(active)));
    }

    void FlushStreamingTranscriptBuffers() {
        if (!activeThinkingBuffer.trimmed().isEmpty()) {
            activeTranscriptLog.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("thinking_block")},
                {QStringLiteral("text"), activeThinkingBuffer},
                {QStringLiteral("historical"), true},
            });
            activeThinkingBuffer.clear();
        }
        if (!activeAssistantBuffer.isEmpty()) {
            activeTranscriptLog.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("assistant_message")},
                {QStringLiteral("text"), activeAssistantBuffer},
                {QStringLiteral("historical"), true},
            });
            activeAssistantBuffer.clear();
        }
    }

    bool SelectExisting(QString& error) {
        if (currentSession && currentSession->mapIdentity == sessionMap) return true;
        const AgentSessionStore store(historyDirectory);
        if (!store.MigrateLegacy(sessionMap, &error, mapIdentity)) return false;
        const auto sessions = store.List(sessionMap, &error);
        if (!error.isEmpty()) return false;
        if (sessions.isEmpty()) return true;
        auto selected = sessions.front();
        for (const auto& session : sessions) if (session.id == selectedSessions.value(sessionMap)) { selected = session; break; }
        currentSession = selected;
        selectedSessions.insert(sessionMap, selected.id);
        return true;
    }

    QJsonArray LoadHistory(QString& error) {
        if (!SelectExisting(error)) return {};
        const AgentSessionStore store(historyDirectory);
        if (!currentSession) currentSession = store.Create(sessionMap, &error);
        if (!currentSession || !error.isEmpty()) return {};
        const auto stored = store.Read(sessionMap, currentSession->id, &error);
        if (!stored) return {};
        currentSession = *stored;
        selectedSessions.insert(sessionMap, stored->id);
        history = stored->messages;
        return history;
    }

    void RestoreSession() {
        Publish({{QStringLiteral("type"), QStringLiteral("conversation_reset")}});
        if (!currentSession || (currentSession->messages.isEmpty() && currentSession->transcriptLog.isEmpty())) {
            PublishContextComposition();
            return;
        }
        const QJsonArray uiEvents = !currentSession->transcriptLog.isEmpty()
            ? currentSession->transcriptLog
            : HistoryUiEvents(currentSession->messages);
        QJsonObject restored{{QStringLiteral("type"), QStringLiteral("history_restored")},
            {QStringLiteral("session_id"), currentSession->id},
            {QStringLiteral("text"), QStringLiteral("已恢复此地图的完整历史；继续编辑前会重新查询当前修订。")},
            {QStringLiteral("messages"), currentSession->messages},
            {QStringLiteral("ui_events"), uiEvents}};
        if (QJsonDocument(restored).toJson(QJsonDocument::Compact).size() > kLineBudget - 4096) {
            restored.remove(QStringLiteral("ui_events"));
            restored.insert(QStringLiteral("ui_events_omitted"), true);
        }
        Publish(restored);
        if (!currentSession->lastUsage.isEmpty()) {
            Publish(currentSession->lastUsage);
        }
        PublishContextComposition();
        memoryBinding = currentSession->id;
    }

    void RefreshConversations(bool restore = false) {
        if (busy) return;
        QString error;
        if (!SelectExisting(error)) {
            Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"), error}}); return;
        }
        QString listWarning;
        const auto sessions = AgentSessionStore(historyDirectory).List(sessionMap, &error, &listWarning);
        if (!error.isEmpty()) {
            Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"), error}}); return;
        }
        QJsonArray rows;
        for (const auto& session : sessions) rows.append(QJsonObject{{QStringLiteral("id"), session.id},
            {QStringLiteral("title"), session.title}, {QStringLiteral("updated_at"), session.updatedAt},
            {QStringLiteral("provider_id"), session.providerId}, {QStringLiteral("model_id"), session.modelId}});
        Publish({{QStringLiteral("type"), QStringLiteral("conversations")}, {QStringLiteral("sessions"), rows},
            {QStringLiteral("map_identity"), mapIdentity},
            {QStringLiteral("selected_id"), currentSession ? currentSession->id : QString()}});
        if (restore) RestoreSession();
        else PublishContextComposition();
    }

    bool SaveHistory() {
        if (discardState) return true;
        if (!pendingState || !IsCompleteAgentTranscript(candidateHistory) || !currentSession || currentSession->id != taskBinding) return false;
        auto session = *currentSession;
        session.providerId = taskProvider; session.modelId = taskModel;
        session.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        session.messages = candidateHistory;
        if (!activeTranscriptLog.isEmpty()) session.transcriptLog = activeTranscriptLog;
        if (!activeLastUsage.isEmpty()) session.lastUsage = activeLastUsage;
        if (session.title == QStringLiteral("新会话")) {
            for (const auto& value : session.transcriptLog) {
                const auto ev = value.toObject();
                if (ev.value(QStringLiteral("type")).toString() != QStringLiteral("user_message")) continue;
                const auto text = ev.value(QStringLiteral("text")).toString().simplified();
                if (!text.isEmpty()) { session.title = text.left(30); break; }
            }
            if (session.title == QStringLiteral("新会话")) {
                for (const auto& value : candidateHistory) {
                    const auto message = value.toObject();
                    if (message.value(QStringLiteral("role")).toString() != QStringLiteral("user") || IsHiddenAgentMessage(message)) continue;
                    const auto text = AgentMessageText(message).simplified();
                    if (!text.isEmpty()) { session.title = text.left(30); break; }
                }
            }
        }
        if (!AgentSessionStore(historyDirectory).Save(session)) return false;
        currentSession = std::move(session); memoryBinding = taskBinding; history = candidateHistory;
        return true;
    }

    void Publish(const QJsonObject& object) {
        auto event = Redacted(object, secret).toObject();
        if (!event.contains(QStringLiteral("map_identity")) && !(busy ? taskMap : mapIdentity).isEmpty())
            event.insert(QStringLiteral("map_identity"), busy ? taskMap : mapIdentity);
        if (!runId.isEmpty()) event.insert(QStringLiteral("run_id"), runId);
        event.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        event.insert(QStringLiteral("sequence"), ++eventSequence);
        if (eventLog.isOpen()) {
            const auto bytes = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
            if (bytes.size() > kLineBudget || loggedBytes + bytes.size() > kLogBudget
                || eventLog.write(bytes) != bytes.size() || !eventLog.flush()) {
                eventLog.close(); recordingFailed = true;
                if (event.value(QStringLiteral("type")).toString() == QStringLiteral("run_end")) event.insert(QStringLiteral("success"), false);
                emit q->Event({{QStringLiteral("type"), QStringLiteral("error")},
                    {QStringLiteral("text"), QStringLiteral("本地任务记录无法继续写入，已请求停止；现有改动保留。")}});
                Stop();
            } else loggedBytes += bytes.size();
        }
        emit q->Event(event);
    }

    void Failure(const QString& text) {
        if (busy) {
            FlushStreamingTranscriptBuffers();
            activeTranscriptLog.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("error")},
                {QStringLiteral("text"), Redacted(text, secret).toString()},
                {QStringLiteral("historical"), true},
            });
        }
        Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"), text}});
        if (busy) Complete(false, stopRequested, QStringLiteral("任务未完成，保留已有地图改动和已记录结果。"));
        pendingStart = {}; ready = false;
        eventLog.close(); secret.fill(QChar(0)); secret.clear();
        if (!toolExecuting && process->state() != QProcess::NotRunning) process->kill();
    }

    void Complete(bool success, bool cancelled, const QString& text) {
        if (!busy || completing) return;
        completing = true;
        startupTimer->stop(); stopTimer->stop();
        FlushStreamingTranscriptBuffers();
        if (!recordingFailed) {
            if (!SaveHistory() && success) {
                Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"),
                    QStringLiteral("本次地图操作已结束，但完整会话未能保存；先前历史和已完成地图改动保留。")}});
                success = false;
            }
        }
        if (compactingTask) {
            Publish({{QStringLiteral("type"), QStringLiteral("compact_end")},
                {QStringLiteral("success"), success && !recordingFailed},
                {QStringLiteral("cancelled"), cancelled},
                {QStringLiteral("text"), text}});
        } else {
            Publish({{QStringLiteral("type"), QStringLiteral("run_end")},
                {QStringLiteral("success"), success && !recordingFailed},
                {QStringLiteral("cancelled"), cancelled},
                {QStringLiteral("step_limit_reached"), stepLimitReached},
                {QStringLiteral("text"), text}});
        }
        busy = false; compactingTask = false; stepLimitReached = false;
        taskSent = false; pendingStart = {}; candidateHistory = {}; pendingState = false;
        activeTranscriptLog = {}; activeLastUsage = {};
        activeThinkingBuffer.clear(); activeAssistantBuffer.clear();
        eventLog.close(); secret.fill(QChar(0)); secret.clear();
        toolIds.clear(); dispatchedIds.clear();
        completing = false;
        emit q->BusyChanged(false);
        RefreshConversations();
    }

    bool ResolveRuntime(QString& error) {
        nodePath = configuredNode; workerPath = configuredWorker;
        const QDir deployed(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("agent-runtime")));
        if (nodePath.isEmpty()) {
            const auto candidate = deployed.filePath(QStringLiteral("node.exe"));
            nodePath = QFileInfo::exists(candidate) ? candidate : QStandardPaths::findExecutable(QStringLiteral("node"));
        }
        if (workerPath.isEmpty()) {
            const auto candidate = deployed.filePath(QStringLiteral("worker.bundle.mjs"));
            if (QFileInfo::exists(candidate)) workerPath = candidate;
#ifdef WAR3MAP_AGENT_RUNTIME_SOURCE_DIR
            else workerPath = QDir(QString::fromUtf8(WAR3MAP_AGENT_RUNTIME_SOURCE_DIR)).filePath(QStringLiteral("worker.mjs"));
#endif
        }
        if (nodePath.isEmpty() || !QFileInfo(nodePath).isFile() || workerPath.isEmpty() || !QFileInfo(workerPath).isFile()) {
            error = QStringLiteral("PI 运行时尚未部署：需要 agent-runtime/node.exe 和 worker.bundle.mjs。开发环境可使用已固定的源码运行时。");
            return false;
        }
        nodePath = QFileInfo(nodePath).absoluteFilePath(); workerPath = QFileInfo(workerPath).absoluteFilePath();
        return true;
    }

    bool CollectEnabledTools(bool toolsSupported, QJsonArray& enabledTools, QString& error) {
        enabledTools = {};
        toolIds.clear(); toolTitles.clear();
        if (!toolsSupported) return true;
        QJsonArray combined;
        try {
            if (toolsProvider) combined = toolsProvider();
            if (memoryService) {
                for (const auto& t : ProjectMemoryService::ToolDefinitions()) combined.append(t);
            }
            if (externalToolsProvider) {
                for (const auto& t : externalToolsProvider()) combined.append(t);
            }
        } catch (...) {
            error = QStringLiteral("无法读取编辑器或 MCP 工具目录。");
            return false;
        }
        for (const auto& value : combined) {
            if (!value.isObject()) { error = QStringLiteral("编辑器工具目录含无效条目。"); return false; }
            const auto tool = value.toObject();
            const auto name = tool.value(QStringLiteral("name")).toString();
            if (name.isEmpty() || toolIds.contains(name) || !tool.value(QStringLiteral("inputSchema")).isObject()) {
                error = QStringLiteral("编辑器工具目录的名称或参数 schema 无效。"); return false;
            }
            if (settings.disabledTools.contains(name)) continue;
            toolIds.insert(name);
            toolTitles.insert(name, tool.value(QStringLiteral("description")).toString(name).section(QLatin1Char('\n'), 0, 0));
            enabledTools.append(tool);
        }
        return true;
    }

    void EnsureWorkerStarted() {
        if (ready && process->state() == QProcess::Running) {
            SendStart();
            return;
        }
        output.clear(); ready = false;
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto& name : {QStringLiteral("OPENAI_API_KEY"), QStringLiteral("ANTHROPIC_API_KEY"),
            QStringLiteral("GOOGLE_API_KEY"), QStringLiteral("GEMINI_API_KEY"), QStringLiteral("DEEPSEEK_API_KEY")}) environment.remove(name);
        process->setProcessEnvironment(environment);
        process->setProgram(nodePath); process->setArguments({workerPath});
        process->setWorkingDirectory(QFileInfo(workerPath).absolutePath());
        startupTimer->start(15000); process->start();
    }

    void Start(QString prompt, const QString& providerId, const QString& modelId) {
        if (busy) { Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"), QStringLiteral("当前任务仍在运行，请先停止或等待结束。")}}); return; }
        if (!settings.aiEnabled) { Failure(QStringLiteral("内置 AI 已关闭，请先在设置中启用。")); return; }
        if (prompt.trimmed().isEmpty() || prompt.size() > kPromptBudget) { Failure(QStringLiteral("任务说明为空或超过 65536 字符。")); return; }
        const ProviderSettings* provider = nullptr;
        const ModelSettings* model = nullptr;
        for (const auto& candidate : settings.providers) if (candidate.id == providerId && candidate.enabled) {
            provider = &candidate;
            for (const auto& candidateModel : candidate.models) if (candidateModel.id == modelId && candidateModel.enabled) { model = &candidateModel; break; }
            break;
        }
        if (provider == nullptr || model == nullptr) { Failure(QStringLiteral("所选提供商或模型未启用。")); return; }
        QString error;
        if (ProviderCatalog::Endpoint(*provider, &error).isEmpty()) { Failure(error); return; }
        const auto context = ResolveContextWindow(*model);
        if (context.tokens < 1024) { Failure(QStringLiteral("PI 会话上下文至少需要 1024 tokens。")); return; }
        if (!ResolveRuntime(error)) { Failure(error); return; }
        const auto priorBinding = memoryBinding;
        auto messages = LoadHistory(error);
        if (!error.isEmpty()) { Failure(error); return; }
        const auto binding = currentSession->id;
        const bool restoreHistory = priorBinding != binding;
        secret = credentials->Read(provider->credentialRef, &error);
        if (!error.isEmpty()) { Failure(error); return; }

        QJsonArray enabledTools;
        if (!CollectEnabledTools(model->toolsSupported, enabledTools, error)) { Failure(error); return; }

        runId = QUuid::createUuid().toString(QUuid::WithoutBraces); eventSequence = 0;
        taskMap = mapIdentity; taskProvider = providerId; taskModel = modelId; taskBinding = binding;
        taskProviderSettings = *provider;
        compactingTask = false; stepLimitReached = false;
        discardState = false; stopRequested = false; recordingFailed = false;
        dispatchedIds.clear(); toolDurations.clear(); loggedBytes = 0;
        activeThinkingBuffer.clear(); activeAssistantBuffer.clear();

        // 初始化持久完整事件流（旧 v2 会话无 transcript_log 时从 messages 无损提取初值），并立即记录本轮用户原始提问。
        activeTranscriptLog = !currentSession->transcriptLog.isEmpty()
            ? currentSession->transcriptLog
            : HistoryUiEvents(messages);
        activeLastUsage = currentSession->lastUsage;

        const auto logDirectory = QDir(historyDirectory).filePath(QStringLiteral("events"));
        if (!QDir().mkpath(logDirectory)) { Failure(QStringLiteral("无法创建本地 AI 任务记录目录，未启动模型请求。")); return; }
        eventLog.setFileName(QDir(logDirectory).filePath(runId + QStringLiteral(".jsonl")));
        if (!eventLog.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { Failure(QStringLiteral("无法创建本地 AI 任务记录，未启动模型请求。")); return; }

        const QString fullSystemPrompt = ComposeFullSystemPrompt();
        pendingStart = {{QStringLiteral("type"), QStringLiteral("start")},
            {QStringLiteral("sessionId"), binding},
            {QStringLiteral("prompt"), prompt},
            {QStringLiteral("systemPrompt"), fullSystemPrompt},
            {QStringLiteral("maxSteps"), settings.maxAutoSteps},
            {QStringLiteral("provider"), QJsonObject{{QStringLiteral("id"), provider->id}, {QStringLiteral("name"), provider->name},
                {QStringLiteral("protocol"), provider->protocol}, {QStringLiteral("baseUrl"), provider->baseUrl}, {QStringLiteral("apiKey"), secret}}},
            {QStringLiteral("model"), QJsonObject{{QStringLiteral("id"), model->id}, {QStringLiteral("name"), model->name},
                {QStringLiteral("contextWindow"), context.tokens}, {QStringLiteral("maxOutputTokens"), model->maxOutputTokens},
                {QStringLiteral("imagesSupported"), model->imagesSupported}}},
            {QStringLiteral("tools"), enabledTools}, {QStringLiteral("messages"), messages}};
        if (QJsonDocument(pendingStart).toJson(QJsonDocument::Compact).size() > kLineBudget) {
            pendingStart = {}; Failure(QStringLiteral("任务与历史超过 8 MiB IPC 预算，未发送模型请求。")); return;
        }

        busy = true; taskSent = false; emit q->BusyChanged(true);
        if (restoreHistory && (!messages.isEmpty() || !activeTranscriptLog.isEmpty())) {
            QJsonObject restored{{QStringLiteral("type"), QStringLiteral("history_restored")},
                {QStringLiteral("text"), QStringLiteral("已恢复此地图的完整历史；继续编辑前会重新查询当前修订。")},
                {QStringLiteral("messages"), messages}, {QStringLiteral("ui_events"), activeTranscriptLog}};
            if (QJsonDocument(restored).toJson(QJsonDocument::Compact).size() > kLineBudget - 4096) {
                restored.remove(QStringLiteral("ui_events"));
                restored.insert(QStringLiteral("ui_events_omitted"), true);
            }
            Publish(restored);
        }

        // 提交即落盘：在向模型发起请求之前先把用户长提示词写入磁盘检查点，确保中途失败/停止/断网也绝不丢失！
        const auto redactedPrompt = Redacted(prompt, secret).toString();
        activeTranscriptLog.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("user_message")},
            {QStringLiteral("text"), redactedPrompt},
            {QStringLiteral("historical"), true},
            {QStringLiteral("provider_id"), providerId},
            {QStringLiteral("model_id"), modelId},
        });
        candidateHistory = messages;
        candidateHistory.append(QJsonObject{
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), redactedPrompt}}}},
            {QStringLiteral("timestamp"), static_cast<double>(QDateTime::currentMSecsSinceEpoch())},
            {QStringLiteral("forge_provider_id"), providerId},
            {QStringLiteral("forge_model_id"), modelId},
        });
        pendingState = true;
        (void)SaveHistory();

        memoryBinding = binding;
        Publish({{QStringLiteral("type"), QStringLiteral("user_message")}, {QStringLiteral("text"), prompt}});
        PublishContextComposition();
        if (!busy) return;
        EnsureWorkerStarted();
    }

    void Compact(const QString& providerId, const QString& modelId) {
        if (busy) {
            Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"), QStringLiteral("任务运行中无法手动压缩，请等待本轮结束。")}});
            return;
        }
        QString error;
        auto messages = LoadHistory(error);
        if (!error.isEmpty()) { Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"), error}}); return; }
        if (!currentSession || messages.size() < 2) {
            Publish({{QStringLiteral("type"), QStringLiteral("compact_end")}, {QStringLiteral("success"), true},
                {QStringLiteral("compacted"), false}, {QStringLiteral("text"), QStringLiteral("当前会话内容较短，无需压缩。")}});
            return;
        }
        if (!ResolveRuntime(error)) { Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"), error}}); return; }

        const ProviderSettings* provider = nullptr;
        const ModelSettings* model = nullptr;
        for (const auto& candidate : settings.providers) if (candidate.id == providerId && candidate.enabled) {
            provider = &candidate;
            for (const auto& cm : candidate.models) if (cm.id == modelId && cm.enabled) { model = &cm; break; }
            break;
        }
        qint64 contextTokens = 262144;
        QJsonObject providerObj, modelObj;
        secret.clear();
        if (provider != nullptr && model != nullptr && !ProviderCatalog::Endpoint(*provider, &error).isEmpty()) {
            secret = credentials->Read(provider->credentialRef, &error);
            if (error.isEmpty()) {
                contextTokens = ResolveContextWindow(*model).tokens;
                providerObj = {{QStringLiteral("id"), provider->id}, {QStringLiteral("name"), provider->name},
                    {QStringLiteral("protocol"), provider->protocol}, {QStringLiteral("baseUrl"), provider->baseUrl}, {QStringLiteral("apiKey"), secret}};
                modelObj = {{QStringLiteral("id"), model->id}, {QStringLiteral("name"), model->name},
                    {QStringLiteral("contextWindow"), contextTokens}, {QStringLiteral("maxOutputTokens"), model->maxOutputTokens}};
            }
        }
        QJsonArray enabledTools;
        (void)CollectEnabledTools(true, enabledTools, error);

        runId = QUuid::createUuid().toString(QUuid::WithoutBraces); eventSequence = 0;
        taskMap = mapIdentity;
        taskProvider = provider ? provider->id : currentSession->providerId;
        taskModel = model ? model->id : currentSession->modelId;
        taskBinding = currentSession->id;
        if (provider) taskProviderSettings = *provider;
        compactingTask = true; stepLimitReached = false;
        discardState = false; stopRequested = false; recordingFailed = false;
        activeThinkingBuffer.clear(); activeAssistantBuffer.clear();
        activeTranscriptLog = !currentSession->transcriptLog.isEmpty() ? currentSession->transcriptLog : HistoryUiEvents(messages);
        activeLastUsage = currentSession->lastUsage;
        candidateHistory = messages; pendingState = false;

        pendingStart = {{QStringLiteral("type"), QStringLiteral("compact")},
            {QStringLiteral("sessionId"), currentSession->id},
            {QStringLiteral("systemPrompt"), ComposeFullSystemPrompt()},
            {QStringLiteral("provider"), providerObj},
            {QStringLiteral("model"), modelObj.isEmpty() ? QJsonObject{{QStringLiteral("contextWindow"), contextTokens}} : modelObj},
            {QStringLiteral("tools"), enabledTools},
            {QStringLiteral("messages"), messages}};
        if (QJsonDocument(pendingStart).toJson(QJsonDocument::Compact).size() > kLineBudget) {
            pendingStart = {};
            Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"), QStringLiteral("会话超过 8 MiB IPC 预算。")}});
            return;
        }
        busy = true; taskSent = false; emit q->BusyChanged(true);
        Publish({{QStringLiteral("type"), QStringLiteral("compaction_status")}, {QStringLiteral("active"), true}, {QStringLiteral("text"), QStringLiteral("正在整理与压缩会话上下文…")}});
        EnsureWorkerStarted();
    }

    bool Send(const QJsonObject& object) {
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        if (bytes.size() > kLineBudget || process->bytesToWrite() + bytes.size() > 2 * kLineBudget
            || process->state() != QProcess::Running || process->write(bytes) != bytes.size()) {
            Failure(QStringLiteral("PI 进程通信失败，未发送不完整请求；已有地图改动保留。")); return false;
        }
        return true;
    }

    void SendStart() {
        if (!busy || taskSent || pendingStart.isEmpty()) return;
        if (Send(pendingStart)) { taskSent = true; pendingStart = {}; }
    }

    void ReadOutput() {
        process->setReadChannel(QProcess::StandardOutput);
        while (process->bytesAvailable() > 0) {
            output.append(process->read(65536));
            for (;;) {
                const auto newline = output.indexOf('\n');
                if (newline < 0) break;
                if (newline > kLineBudget) { output.clear(); Failure(QStringLiteral("PI 输出超过 8 MiB 单行预算。")); return; }
                auto line = output.left(newline); output.remove(0, newline + 1);
                if (line.endsWith('\r')) line.chop(1);
                if (line.isEmpty()) continue;
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(line, &error);
                if (error.error != QJsonParseError::NoError || !document.isObject()) { Failure(QStringLiteral("PI 输出不是有效 NDJSON，任务停止。")); return; }
                Handle(document.object());
            }
            if (output.size() > kLineBudget) { output.clear(); Failure(QStringLiteral("PI 输出缺少行边界或超过读取预算。")); return; }
        }
    }

    void UpdateCandidateMessages(const QJsonArray& rawMessages) {
        QJsonArray next;
        for (const auto& value : Redacted(rawMessages, secret).toArray()) {
            auto message = value.toObject();
            if (!message.contains(QStringLiteral("forge_provider_id"))) message.insert(QStringLiteral("forge_provider_id"), taskProvider);
            if (!message.contains(QStringLiteral("forge_model_id"))) message.insert(QStringLiteral("forge_model_id"), taskModel);
            if (IsHiddenAgentMessage(message)) message.insert(QStringLiteral("forge_hidden"), true);
            next.append(message);
        }
        if (IsCompleteAgentTranscript(next) && !next.isEmpty()) {
            candidateHistory = std::move(next);
            pendingState = true;
        }
    }

    void Handle(QJsonObject event) {
        const auto type = event.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("ready")) {
            if (ready || event.value(QStringLiteral("runtime")).toString() != QStringLiteral("pi")
                || event.value(QStringLiteral("version")).toString() != QStringLiteral("1.0.0")) {
                Failure(QStringLiteral("PI 运行时准备身份不匹配。")); return;
            }
            startupTimer->stop(); ready = true; SendStart(); return;
        }
        if (!busy) return;
        if (type == QStringLiteral("tool_call")) {
            const auto id = event.value(QStringLiteral("id")).toString();
            if (id.isEmpty() || dispatchedIds.contains(id) || !event.value(QStringLiteral("arguments")).isObject()) {
                Failure(QStringLiteral("PI 工具请求身份重复或参数无效。")); return;
            }
            dispatchedIds.insert(id);
            QTimer::singleShot(0, q, [this, event] { Dispatch(event); });
            return;
        }
        if (type == QStringLiteral("turn_checkpoint")) {
            if (event.value(QStringLiteral("messages")).isArray()) {
                UpdateCandidateMessages(event.value(QStringLiteral("messages")).toArray());
                FlushStreamingTranscriptBuffers();
                if (!recordingFailed) (void)SaveHistory();
                PublishContextComposition();
            }
            return;
        }
        if (type == QStringLiteral("session_state")) {
            if (!event.value(QStringLiteral("messages")).isArray()) { Failure(QStringLiteral("PI 返回无效会话状态。")); return; }
            if (event.value(QStringLiteral("step_limit_reached")).toBool()) stepLimitReached = true;
            UpdateCandidateMessages(event.value(QStringLiteral("messages")).toArray());
            Publish(event);
            PublishContextComposition();
            return;
        }
        if (type == QStringLiteral("compact_end")) {
            const bool success = event.value(QStringLiteral("success")).toBool();
            Complete(success && !stopRequested, stopRequested,
                event.value(QStringLiteral("text")).toString(QStringLiteral("压缩已完成。")));
            return;
        }
        if (type == QStringLiteral("run_end")) {
            const bool success = event.value(QStringLiteral("success")).toBool();
            Complete(success && !stopRequested, stopRequested,
                event.value(QStringLiteral("text")).toString(QStringLiteral("任务结束。")));
            return;
        }
        if (type == QStringLiteral("thinking_delta")) {
            activeThinkingBuffer += Redacted(event.value(QStringLiteral("text")), secret).toString();
            Publish(event);
            return;
        }
        if (type == QStringLiteral("assistant_delta")) {
            if (!activeThinkingBuffer.trimmed().isEmpty()) {
                activeTranscriptLog.append(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("thinking_block")},
                    {QStringLiteral("text"), activeThinkingBuffer},
                    {QStringLiteral("historical"), true},
                });
                activeThinkingBuffer.clear();
            }
            activeAssistantBuffer += Redacted(event.value(QStringLiteral("text")), secret).toString();
            Publish(event);
            return;
        }
        if (type == QStringLiteral("context_compacted")) {
            FlushStreamingTranscriptBuffers();
            auto recorded = Redacted(event, secret).toObject();
            recorded.insert(QStringLiteral("historical"), true);
            activeTranscriptLog.append(recorded);
            // 压缩后清掉旧高水位缓存指标，避免旧的 100k 用量掩盖已压缩表层。
            activeLastUsage = {};
            Publish(event);
            return;
        }
        if (type == QStringLiteral("usage")) {
            if (event.value(QStringLiteral("available")).toBool()) {
                activeLastUsage = Redacted(event, secret).toObject();
            }
            Publish(event);
            return;
        }
        if (type == QStringLiteral("tool_start") || type == QStringLiteral("tool_end")) {
            FlushStreamingTranscriptBuffers();
            const auto name = event.value(QStringLiteral("tool_name")).toString();
            if (toolTitles.contains(name)) event.insert(QStringLiteral("title"), toolTitles.value(name));
            const auto id = event.value(QStringLiteral("id")).toString();
            if (toolDurations.contains(id)) event.insert(QStringLiteral("duration_ms"), toolDurations.take(id));
            auto histEvent = Redacted(event, secret).toObject();
            histEvent.insert(QStringLiteral("historical"), true);
            activeTranscriptLog.append(histEvent);
        }
        Publish(event);
    }

    void Dispatch(const QJsonObject& call) {
        if (!busy || !taskSent) return;
        const auto id = call.value(QStringLiteral("id")).toString();
        const auto name = call.value(QStringLiteral("name")).toString();
        QJsonObject result;
        if (stopRequested || taskMap != mapIdentity) result = ToolError(QStringLiteral("AGENT_TASK_STOPPED"), QStringLiteral("任务已停止或切换地图，未执行此工具。"));
        else if (!toolIds.contains(name) || settings.disabledTools.contains(name)) result = ToolError(QStringLiteral("AGENT_TOOL_DISABLED"), QStringLiteral("此工具未提供或已关闭。"));
        else if (memoryService && ProjectMemoryService::IsMemoryTool(name)) {
            toolExecuting = true;
            QElapsedTimer elapsed; elapsed.start();
            result = memoryService->InvokeTool(name, call.value(QStringLiteral("arguments")).toObject(), settings.allowEdits);
            toolExecuting = false;
            toolDurations.insert(id, elapsed.elapsed());
        } else if (!dispatcher) result = ToolError(QStringLiteral("AGENT_TOOL_UNAVAILABLE"), QStringLiteral("编辑器工具服务尚未连接。"));
        else {
            toolExecuting = true;
            QElapsedTimer elapsed; elapsed.start();
            const QPointer<AgentController> guard(q);
            try { result = dispatcher(name, call.value(QStringLiteral("arguments")).toObject()); }
            catch (...) { result = ToolError(QStringLiteral("AGENT_TOOL_EXCEPTION"), QStringLiteral("工具服务抛出异常，请查询当前地图状态；已有改动保留。")); }
            if (guard.isNull()) return;
            toolExecuting = false; toolDurations.insert(id, elapsed.elapsed());
        }
        if (result.value(QStringLiteral("error")).isObject()) result.insert(QStringLiteral("isError"), true);
        if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > kLineBudget - 4096)
            result = ToolError(QStringLiteral("AGENT_TOOL_RESULT_BUDGET"), QStringLiteral("工具结果超过 IPC 预算；操作可能已完成，请分页重新查询当前状态。"));
        auto visible = result.value(QStringLiteral("changes"));
        if (visible.isUndefined()) visible = result.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("changes"));
        if (visible.isObject()) {
            auto change = visible.toObject(); change.insert(QStringLiteral("type"), QStringLiteral("changes")); change.insert(QStringLiteral("tool_call_id"), id); Publish(change);
            auto histChange = Redacted(change, secret).toObject();
            histChange.insert(QStringLiteral("historical"), true);
            activeTranscriptLog.append(histChange);
        }
        Publish({{QStringLiteral("type"), QStringLiteral("tool_update")}, {QStringLiteral("id"), id},
            {QStringLiteral("tool_name"), name}, {QStringLiteral("details"), QJsonObject{{QStringLiteral("result"), result}}}});
        if (!Send({{QStringLiteral("type"), QStringLiteral("tool_result")}, {QStringLiteral("id"), id}, {QStringLiteral("result"), result}})) return;
        if (stopRequested) SendStop();
    }

    void SendStop() {
        if (!busy || toolExecuting) return;
        if (taskSent) {
            if (Send({{QStringLiteral("type"), QStringLiteral("stop")}})) stopTimer->start(5000);
        } else {
            Complete(false, true, QStringLiteral("已取消启动，未发送模型请求。"));
            if (process->state() != QProcess::NotRunning) process->kill();
        }
    }

    void Stop() {
        if (!busy || completing) return;
        stopRequested = true;
        if (!toolExecuting) SendStop();
    }

    void NewConversation() {
        if (busy) {
            Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"),
                QStringLiteral("当前任务仍在运行，请先停止并等待结束，再开启新会话。")}});
            return;
        }
        QString error;
        const auto session = AgentSessionStore(historyDirectory).Create(sessionMap, &error);
        if (!session) { Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"), error}}); return; }
        currentSession = *session; selectedSessions.insert(sessionMap, session->id);
        memoryBinding = session->id; history = {};
        Publish({{QStringLiteral("type"), QStringLiteral("conversation_reset")}});
        RefreshConversations();
    }

    bool SessionOperationAllowed() {
        if (!busy) return true;
        Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"),
            QStringLiteral("任务运行中不能更改会话，请先停止并等待结束。")}});
        return false;
    }

    void SwitchConversation(const QString& id) {
        if (!SessionOperationAllowed()) return;
        QString error;
        const auto session = AgentSessionStore(historyDirectory).Read(sessionMap, id, &error);
        if (!session) { Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"), error}}); return; }
        currentSession = *session; history = session->messages; selectedSessions.insert(sessionMap, id);
        RestoreSession(); RefreshConversations();
    }

    void RenameConversation(const QString& id, const QString& title) {
        if (!SessionOperationAllowed()) return;
        QString error;
        if (!AgentSessionStore(historyDirectory).Rename(sessionMap, id, title, &error)) {
            Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"), error}}); return;
        }
        if (currentSession && currentSession->id == id) currentSession->title = title.trimmed();
        RefreshConversations();
    }

    void ArchiveConversation(const QString& id) {
        if (!SessionOperationAllowed()) return;
        QString error;
        if (!AgentSessionStore(historyDirectory).Archive(sessionMap, id, &error)) {
            Publish({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("text"), error}}); return;
        }
        const bool active = currentSession && currentSession->id == id;
        if (active) { currentSession.reset(); memoryBinding.clear(); history = {}; selectedSessions.remove(sessionMap); }
        RefreshConversations(active);
    }

    AgentController* q;
    AgentSettings settings;
    std::unique_ptr<CredentialStore> credentials;
    std::function<QJsonObject(QString, QJsonObject)> dispatcher;
    std::function<QJsonArray()> toolsProvider;
    std::function<QJsonArray()> externalToolsProvider;
    std::function<QString()> mcpInstructionsProvider;
    QPointer<ProjectMemoryService> memoryService;
    QString systemPrompt = QStringLiteral("你是 Audastra 地图助手。新增与优化逻辑默认写成作者可编辑的触发器（T，传统事件/条件/动作）。通过提供的工具查询、编辑和保存，不能伪造修改或验收。");
    QString skillsPrompt;
    QList<ContextNamedItem> skillItems;
    QString mapTitle, mapIdentity, sessionMap, taskMap, taskProvider, taskModel, taskBinding, runId;
    ProviderSettings taskProviderSettings;
    QString configuredNode, configuredWorker, nodePath, workerPath, historyDirectory, memoryBinding, secret;
    QProcess* process = nullptr;
    QTimer *startupTimer = nullptr, *stopTimer = nullptr;
    QByteArray output;
    QJsonObject pendingStart, activeLastUsage;
    QJsonArray history, candidateHistory, activeTranscriptLog;
    QString activeThinkingBuffer, activeAssistantBuffer;
    std::optional<AgentSession> currentSession;
    QHash<QString, QString> selectedSessions;
    QSet<QString> toolIds, dispatchedIds;
    QHash<QString, QString> toolTitles;
    QHash<QString, qint64> toolDurations;
    QFile eventLog;
    qint64 loggedBytes = 0, eventSequence = 0;
    bool ready = false, busy = false, taskSent = false, stopRequested = false, toolExecuting = false,
        pendingState = false, discardState = false, recordingFailed = false, completing = false,
        compactingTask = false, stepLimitReached = false;
};

AgentController::AgentController(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>(this)) {}

AgentController::~AgentController() {
    impl_->Stop();
    impl_->secret.fill(QChar(0));
    if (impl_->process->state() != QProcess::NotRunning) {
        impl_->process->kill(); impl_->process->waitForFinished(1000);
    }
}

void AgentController::SetSettings(const AgentSettings& settings) {
    impl_->settings = settings;
    if (!impl_->busy) {
        impl_->PublishContextComposition();
        return;
    }
    bool available = settings.aiEnabled;
    bool found = false;
    for (const auto& provider : settings.providers) if (provider.id == impl_->taskProvider && provider.enabled
        && provider.baseUrl == impl_->taskProviderSettings.baseUrl && provider.protocol == impl_->taskProviderSettings.protocol
        && provider.credentialRef == impl_->taskProviderSettings.credentialRef)
        for (const auto& model : provider.models) if (model.id == impl_->taskModel && model.enabled) found = true;
    if (!available || !found) impl_->Stop();
}

void AgentController::SetActiveMap(QString title, QString identity, QString conversationIdentity) {
    if (conversationIdentity.isEmpty()) conversationIdentity = identity;
    const bool runtimeChanged = impl_->mapIdentity != identity;
    if (runtimeChanged) {
        impl_->memoryBinding.clear(); impl_->history = {}; impl_->currentSession.reset(); impl_->discardState = true;
        impl_->Stop();
    }
    const bool changed = impl_->sessionMap != conversationIdentity;
    if (changed) { impl_->currentSession.reset(); impl_->memoryBinding.clear(); impl_->history = {}; }
    impl_->mapTitle = std::move(title); impl_->mapIdentity = std::move(identity);
    impl_->sessionMap = std::move(conversationIdentity);
    if (changed || runtimeChanged) impl_->RefreshConversations(true);
}

void AgentController::SetToolDispatcher(std::function<QJsonObject(QString, QJsonObject)> dispatcher) { impl_->dispatcher = std::move(dispatcher); }
void AgentController::SetToolsProvider(std::function<QJsonArray()> provider) { impl_->toolsProvider = std::move(provider); }
void AgentController::SetSystemPrompt(QString prompt) { impl_->systemPrompt = std::move(prompt); }
void AgentController::SetSkillsPrompt(QString skillsPrompt, QList<ContextNamedItem> skillItems) {
    impl_->skillsPrompt = std::move(skillsPrompt);
    impl_->skillItems = std::move(skillItems);
}
void AgentController::SetExternalToolsProvider(std::function<QJsonArray()> provider) {
    impl_->externalToolsProvider = std::move(provider);
}
void AgentController::SetMcpInstructionsProvider(std::function<QString()> provider) {
    impl_->mcpInstructionsProvider = std::move(provider);
}
void AgentController::SetMemoryService(ProjectMemoryService* memory) {
    if (impl_->memoryService == memory) return;
    if (impl_->memoryService) disconnect(impl_->memoryService, nullptr, this, nullptr);
    impl_->memoryService = memory;
    if (memory) {
        connect(memory, &ProjectMemoryService::MemoryChanged, this, [this] {
            if (!impl_->busy) impl_->PublishContextComposition();
        });
    }
}

void AgentController::SetRuntimePaths(QString nodePath, QString workerPath) {
    if (impl_->busy) return;
    impl_->configuredNode = std::move(nodePath); impl_->configuredWorker = std::move(workerPath);
    if (impl_->process->state() != QProcess::NotRunning) {
        impl_->process->kill(); impl_->process->waitForFinished(1000);
    }
    impl_->ready = false;
}

void AgentController::SetCredentialDirectory(QString directory) {
    if (!impl_->busy) impl_->credentials = std::make_unique<CredentialStore>(std::move(directory));
}

void AgentController::SetHistoryDirectory(QString directory) {
    if (impl_->busy || directory.isEmpty()) return;
    impl_->historyDirectory = std::move(directory); impl_->memoryBinding.clear(); impl_->history = {};
    impl_->currentSession.reset(); impl_->selectedSessions.clear();
}

void AgentController::RefreshConversations() { impl_->RefreshConversations(true); }
void AgentController::RefreshContextComposition() { if (!impl_->busy) impl_->PublishContextComposition(); }

bool AgentController::IsBusy() const { return impl_->busy; }
qint64 AgentController::RuntimeProcessId() const { return impl_->process->processId(); }
QString AgentController::CurrentFeedbackConversationSnippet(qsizetype maxUtf8Bytes) const {
    if (maxUtf8Bytes <= 0 || maxUtf8Bytes > 16384 || !impl_->currentSession) return {};
    QStringList snippets;
    qsizetype remaining = maxUtf8Bytes;
    const auto clipUtf8 = [](QString body, qsizetype budget) {
        if (body.toUtf8().size() > budget) {
            qsizetype lower = 0, upper = body.size();
            while (lower < upper) {
                const auto middle = lower + (upper - lower + 1) / 2;
                if (body.left(middle).toUtf8().size() <= budget) lower = middle;
                else upper = middle - 1;
            }
            if (lower > 0 && body[lower - 1].isHighSurrogate()) --lower;
            body.truncate(lower);
        }
        return body;
    };
    if (!impl_->currentSession->transcriptLog.isEmpty()) {
        const auto& log = impl_->currentSession->transcriptLog;
        for (auto it = log.constEnd(); it != log.constBegin() && remaining > 0;) {
            const auto ev = (--it)->toObject();
            const QString type = ev.value(QStringLiteral("type")).toString();
            if (type != QStringLiteral("user_message") && type != QStringLiteral("assistant_message")) continue;
            QString body = ev.value(QStringLiteral("text")).toString();
            if (body.isEmpty()) continue;
            const QString prefix = type == QStringLiteral("user_message") ? QStringLiteral("用户：") : QStringLiteral("助手：");
            const auto prefixBytes = prefix.toUtf8().size();
            if (remaining <= prefixBytes + 1) break;
            body = clipUtf8(std::move(body), remaining - prefixBytes - 1);
            const auto snippet = prefix + body + QChar('\n');
            remaining -= snippet.toUtf8().size();
            snippets.prepend(snippet);
        }
        return snippets.join(QString());
    }
    const auto& messages = impl_->currentSession->messages;
    for (auto it = messages.constEnd(); it != messages.constBegin() && remaining > 0;) {
        const auto message = (--it)->toObject();
        if (IsHiddenAgentMessage(message)) continue;
        const auto role = message.value(QStringLiteral("role")).toString();
        if (role != QStringLiteral("user") && role != QStringLiteral("assistant")) continue;
        QString body = AgentMessageText(message);
        if (body.isEmpty()) continue;
        const QString prefix = role == QStringLiteral("user") ? QStringLiteral("用户：") : QStringLiteral("助手：");
        const auto prefixBytes = prefix.toUtf8().size();
        if (remaining <= prefixBytes + 1) break;
        body = clipUtf8(std::move(body), remaining - prefixBytes - 1);
        const auto snippet = prefix + body + QChar('\n');
        remaining -= snippet.toUtf8().size();
        snippets.prepend(snippet);
    }
    return snippets.join(QString());
}
void AgentController::StartPrompt(QString text, QString providerId, QString modelId) { impl_->Start(std::move(text), providerId, modelId); }
void AgentController::CompactConversation(QString providerId, QString modelId) { impl_->Compact(std::move(providerId), std::move(modelId)); }
void AgentController::Stop() { impl_->Stop(); }
void AgentController::NewConversation() { impl_->NewConversation(); }
void AgentController::SwitchConversation(QString id) { impl_->SwitchConversation(id); }
void AgentController::RenameConversation(QString id, QString title) { impl_->RenameConversation(id, title); }
void AgentController::ArchiveConversation(QString id) { impl_->ArchiveConversation(id); }

} // namespace War3Map::Agent
