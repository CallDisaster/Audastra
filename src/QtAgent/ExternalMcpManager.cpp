#include "ExternalMcpManager.h"
#include "CredentialStore.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace War3Map::Agent {
namespace {

constexpr qsizetype kMaxStdioBufferBytes = 8 * 1024 * 1024;
constexpr int kMaxToolsPerServer = 512;

QString Slugify(const QString& input) {
    QString out;
    out.reserve(input.size());
    for (const QChar ch : input) {
        const ushort u = ch.unicode();
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_') {
            out.append(ch.toLower());
        } else {
            if (!out.endsWith(QLatin1Char('_'))) out.append(QLatin1Char('_'));
        }
    }
    while (out.startsWith(QLatin1Char('_'))) out.remove(0, 1);
    while (out.endsWith(QLatin1Char('_'))) out.chop(1);
    if (out.isEmpty()) out = QStringLiteral("server");
    return out.left(24);
}

bool LooksLikeSecretEnvKey(const QString& key) {
    const QString lower = key.toLower();
    return lower == QStringLiteral("api_key") || lower == QStringLiteral("apikey")
        || lower == QStringLiteral("secret") || lower == QStringLiteral("password")
        || lower == QStringLiteral("authorization") || lower == QStringLiteral("token")
        || lower.endsWith(QStringLiteral("_api_key")) || lower.endsWith(QStringLiteral("_token"))
        || lower.endsWith(QStringLiteral("_secret"));
}

QString RedactSecret(QString text, const QString& secret) {
    if (!secret.isEmpty()) text.replace(secret, QStringLiteral("[已隐藏密钥]"));
    return text;
}

QJsonValue RedactJson(const QJsonValue& value, const QString& secret) {
    if (secret.isEmpty()) return value;
    if (value.isString()) return RedactSecret(value.toString(), secret);
    if (value.isArray()) {
        QJsonArray arr;
        for (const auto& item : value.toArray()) arr.append(RedactJson(item, secret));
        return arr;
    }
    if (value.isObject()) {
        QJsonObject obj;
        const auto src = value.toObject();
        for (auto it = src.constBegin(); it != src.constEnd(); ++it) {
            obj.insert(it.key(), RedactJson(it.value(), secret));
        }
        return obj;
    }
    return value;
}

QJsonObject McpToolError(const QString& code, const QString& message) {
    return {{QStringLiteral("isError"), true}, {QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
}

bool ConfigEquals(const ExternalMcpServerSettings& a, const ExternalMcpServerSettings& b) {
    return a.id == b.id && a.name == b.name && a.enabled == b.enabled
        && a.transport == b.transport && a.command == b.command && a.args == b.args
        && a.cwd == b.cwd && a.env == b.env && a.url == b.url
        && a.credentialRef == b.credentialRef;
}

// 从 HTTP JSON 或 Streamable HTTP SSE 响应体提取目标 JSON-RPC 响应对象。
std::optional<QJsonObject> ParseHttpJsonRpcBody(const QByteArray& raw, qint64 expectedId) {
    const QByteArray trimmed = raw.trimmed();
    if (trimmed.startsWith('{')) {
        QJsonParseError err;
        const auto doc = QJsonDocument::fromJson(trimmed, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) return doc.object();
    }
    std::optional<QJsonObject> fallback;
    for (const QByteArray& line : raw.split('\n')) {
        QByteArray clean = line.trimmed();
        if (clean.startsWith("data:")) clean = clean.mid(5).trimmed();
        if (!clean.startsWith('{')) continue;
        QJsonParseError err;
        const auto doc = QJsonDocument::fromJson(clean, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;
        const auto obj = doc.object();
        if (obj.value(QStringLiteral("id")).toInteger(-1) == expectedId) return obj;
        fallback = obj;
    }
    return fallback;
}

} // namespace

class ExternalMcpManager::Impl {
public:
    struct PendingRequest {
        std::function<void(const QJsonObject&)> onResponse;
        std::function<void(const QString&)> onError;
    };

    struct ServerRuntime {
        ExternalMcpServerSettings config;
        QString slug;
        QString secret;
        QString sessionId; // Streamable HTTP Mcp-Session-Id
        QString instructions;
        QString error;
        bool connecting = false;
        bool connected = false;
        int autoReconnectAttempts = 0;
        qint64 nextRpcId = 1;
        QProcess* process = nullptr;
        QTimer* reconnectTimer = nullptr;
        QByteArray stdoutBuffer;
        QHash<qint64, PendingRequest> pending;
        QList<DiscoveredExternalMcpTool> tools;
    };

    explicit Impl(ExternalMcpManager* owner)
        : q(owner), fallbackCredentials(std::make_unique<CredentialStore>()), credentialStore(fallbackCredentials.get()) {
        network = new QNetworkAccessManager(q);
#ifdef Q_OS_WIN
        jobHandle = CreateJobObjectW(nullptr, nullptr);
        if (jobHandle != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(jobHandle, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                CloseHandle(jobHandle);
                jobHandle = nullptr;
            }
        }
#endif
    }

    ~Impl() {
        StopAll(false);
#ifdef Q_OS_WIN
        if (jobHandle != nullptr) {
            CloseHandle(jobHandle);
            jobHandle = nullptr;
        }
#endif
    }

    void StopServer(ServerRuntime& rt, const QString& reason = {}) {
        if (rt.reconnectTimer) rt.reconnectTimer->stop();
        const auto pendingCopy = std::exchange(rt.pending, {});
        for (auto it = pendingCopy.constBegin(); it != pendingCopy.constEnd(); ++it) {
            if (it->onError) it->onError(reason.isEmpty() ? QStringLiteral("MCP 连接已关闭。") : reason);
        }
        if (rt.process) {
            rt.process->disconnect(q);
            if (rt.process->state() != QProcess::NotRunning) {
                rt.process->kill();
                rt.process->waitForFinished(500);
            }
            rt.process->deleteLater();
            rt.process = nullptr;
        }
        rt.stdoutBuffer.clear();
        rt.connecting = false;
        rt.connected = false;
        rt.sessionId.clear();
        rt.secret.fill(QChar(0));
        rt.secret.clear();
        if (!reason.isEmpty()) rt.error = reason;
    }

    void StopAll(bool notify = true) {
        for (auto it = servers.begin(); it != servers.end(); ++it) {
            StopServer(*it.value());
        }
        servers.clear();
        serverOrder.clear();
        if (notify) emit q->StatusChanged();
    }

    QString UniqueSlugFor(const ExternalMcpServerSettings& cfg, const QSet<QString>& usedSlugs) const {
        const QString base = Slugify(cfg.name.isEmpty() ? cfg.id : cfg.name);
        QString candidate = base;
        int suffix = 2;
        while (usedSlugs.contains(candidate)) {
            candidate = QStringLiteral("%1_%2").arg(base).arg(suffix++);
        }
        return candidate;
    }

    void ApplySettings(const AgentSettings& nextSettings) {
        settings = nextSettings;
        QSet<QString> nextIds;
        for (const auto& s : settings.externalMcpServers) nextIds.insert(s.id);

        // 移除已删除的服务器。
        for (const QString& existingId : serverOrder) {
            if (!nextIds.contains(existingId)) {
                if (auto rt = servers.take(existingId)) StopServer(*rt);
            }
        }
        serverOrder.clear();
        QSet<QString> usedSlugs;
        for (const auto& cfg : settings.externalMcpServers) {
            serverOrder.push_back(cfg.id);
            auto existing = servers.value(cfg.id);
            if (!existing) {
                existing = std::make_shared<ServerRuntime>();
                existing->config = cfg;
                existing->slug = UniqueSlugFor(cfg, usedSlugs);
                existing->reconnectTimer = new QTimer(q);
                existing->reconnectTimer->setSingleShot(true);
                const QString id = cfg.id;
                QObject::connect(existing->reconnectTimer, &QTimer::timeout, q, [this, id] {
                    if (auto target = servers.value(id)) ConnectServer(*target, false);
                });
                servers.insert(cfg.id, existing);
                usedSlugs.insert(existing->slug);
                if (cfg.enabled) ConnectServer(*existing, true);
            } else {
                usedSlugs.insert(existing->slug);
                const bool endpointChanged = !ConfigEquals(existing->config, cfg);
                existing->config = cfg;
                if (!cfg.enabled) {
                    StopServer(*existing);
                    existing->tools.clear();
                    existing->instructions.clear();
                    existing->error.clear();
                } else if (endpointChanged || (!existing->connected && !existing->connecting && existing->error.isEmpty())) {
                    ConnectServer(*existing, true);
                }
            }
        }
        emit q->StatusChanged();
    }

    void ConnectServer(ServerRuntime& rt, bool resetBackoff) {
        StopServer(rt);
        rt.error.clear();
        rt.tools.clear();
        rt.instructions.clear();
        if (resetBackoff) rt.autoReconnectAttempts = 0;
        if (!rt.config.enabled) {
            emit q->StatusChanged();
            return;
        }
        if (!rt.config.credentialRef.isEmpty() && credentialStore != nullptr) {
            QString credError;
            rt.secret = credentialStore->Read(rt.config.credentialRef, &credError);
            if (!credError.isEmpty()) {
                rt.error = credError;
                emit q->StatusChanged();
                return;
            }
        }
        rt.connecting = true;
        emit q->StatusChanged();

        if (rt.config.transport == QStringLiteral("stdio")) {
            StartStdioProcess(rt);
        } else if (rt.config.transport == QStringLiteral("http")) {
            BeginHandshake(rt);
        } else {
            rt.connecting = false;
            rt.error = QStringLiteral("不支持的 MCP 传输类型。");
            emit q->StatusChanged();
        }
    }

    void StartStdioProcess(ServerRuntime& rt) {
        auto* proc = new QProcess(q);
        rt.process = proc;
        proc->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
        proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
            arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            arguments->startupInfo->wShowWindow = SW_HIDE;
        });
#endif
        auto env = QProcessEnvironment::systemEnvironment();
        for (const auto& hostKey : {QStringLiteral("OPENAI_API_KEY"), QStringLiteral("ANTHROPIC_API_KEY"),
                 QStringLiteral("GOOGLE_API_KEY"), QStringLiteral("GEMINI_API_KEY"), QStringLiteral("DEEPSEEK_API_KEY")}) {
            env.remove(hostKey);
        }
        for (const auto& kv : rt.config.env) env.insert(kv.first, kv.second);
        if (!rt.secret.isEmpty() && !env.contains(QStringLiteral("MCP_AUTH_TOKEN"))) {
            env.insert(QStringLiteral("MCP_AUTH_TOKEN"), rt.secret);
        }
        proc->setProcessEnvironment(env);
        if (!rt.config.cwd.trimmed().isEmpty()) proc->setWorkingDirectory(rt.config.cwd.trimmed());
        proc->setProgram(rt.config.command.trimmed());
        proc->setArguments(rt.config.args);

        const QString serverId = rt.config.id;
        QObject::connect(proc, &QProcess::started, q, [this, serverId] {
            auto target = servers.value(serverId);
            if (!target || !target->process) return;
#ifdef Q_OS_WIN
            if (jobHandle != nullptr && target->process->processId() > 0) {
                HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                    static_cast<DWORD>(target->process->processId()));
                if (child != nullptr) {
                    (void)AssignProcessToJobObject(jobHandle, child);
                    CloseHandle(child);
                }
            }
#endif
            BeginHandshake(*target);
        });
        QObject::connect(proc, &QProcess::readyReadStandardOutput, q, [this, serverId] {
            if (auto target = servers.value(serverId)) ReadStdioOutput(*target);
        });
        QObject::connect(proc, &QProcess::readyReadStandardError, q, [this, serverId] {
            auto target = servers.value(serverId);
            if (!target || !target->process) return;
            target->process->setReadChannel(QProcess::StandardError);
            while (target->process->bytesAvailable() > 0) target->process->read(32768);
            target->process->setReadChannel(QProcess::StandardOutput);
        });
        QObject::connect(proc, &QProcess::errorOccurred, q, [this, serverId](QProcess::ProcessError err) {
            auto target = servers.value(serverId);
            if (!target) return;
            if (err == QProcess::FailedToStart) {
                StopServer(*target, QStringLiteral("无法启动 MCP 进程：%1").arg(target->config.command));
                emit q->StatusChanged();
            }
        });
        QObject::connect(proc, &QProcess::finished, q, [this, serverId](int exitCode, QProcess::ExitStatus) {
            auto target = servers.value(serverId);
            if (!target) return;
            const bool wasConnected = target->connected;
            StopServer(*target, QStringLiteral("MCP 进程已退出（代码 %1）").arg(exitCode));
            if (target->config.enabled && wasConnected && target->autoReconnectAttempts < 2) {
                ++target->autoReconnectAttempts;
                target->reconnectTimer->start(target->autoReconnectAttempts == 1 ? 1500 : 4000);
            }
            emit q->StatusChanged();
        });
        proc->start();
    }

    void ReadStdioOutput(ServerRuntime& rt) {
        if (!rt.process) return;
        rt.process->setReadChannel(QProcess::StandardOutput);
        rt.stdoutBuffer.append(rt.process->readAllStandardOutput());
        if (rt.stdoutBuffer.size() > kMaxStdioBufferBytes) {
            StopServer(rt, QStringLiteral("MCP 进程输出超过 8 MiB 单帧预算，已断开。"));
            emit q->StatusChanged();
            return;
        }
        for (;;) {
            // 兼容 Content-Length 头帧与标准 NDJSON 行帧。
            if (rt.stdoutBuffer.startsWith("Content-Length:")) {
                const auto headerEnd = rt.stdoutBuffer.indexOf("\r\n\r\n");
                if (headerEnd < 0) break;
                const auto firstLine = rt.stdoutBuffer.left(rt.stdoutBuffer.indexOf("\r\n"));
                const qint64 bodyLen = firstLine.mid(15).trimmed().toLongLong();
                if (bodyLen < 0 || bodyLen > kMaxStdioBufferBytes) {
                    StopServer(rt, QStringLiteral("MCP Content-Length 头无效。"));
                    emit q->StatusChanged();
                    return;
                }
                if (rt.stdoutBuffer.size() < headerEnd + 4 + bodyLen) break;
                const QByteArray payload = rt.stdoutBuffer.mid(headerEnd + 4, bodyLen);
                rt.stdoutBuffer.remove(0, headerEnd + 4 + bodyLen);
                DispatchIncomingJson(rt, payload);
                continue;
            }
            const auto nl = rt.stdoutBuffer.indexOf('\n');
            if (nl < 0) break;
            QByteArray line = rt.stdoutBuffer.left(nl).trimmed();
            rt.stdoutBuffer.remove(0, nl + 1);
            if (line.isEmpty()) continue;
            DispatchIncomingJson(rt, line);
        }
    }

    void DispatchIncomingJson(ServerRuntime& rt, const QByteArray& bytes) {
        QJsonParseError err;
        const auto doc = QJsonDocument::fromJson(bytes, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
        const auto obj = doc.object();
        if (!obj.contains(QStringLiteral("id"))) return;
        const qint64 id = obj.value(QStringLiteral("id")).toInteger(-1);
        auto pending = rt.pending.take(id);
        if (pending.onResponse) pending.onResponse(obj);
    }

    void SendRpc(ServerRuntime& rt, const QString& method, const QJsonObject& params,
        bool isNotification, int timeoutMs,
        std::function<void(const QJsonObject&)> onResponse = {},
        std::function<void(const QString&)> onError = {}) {
        QJsonObject frame{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), method}};
        if (!params.isEmpty()) frame.insert(QStringLiteral("params"), params);
        const qint64 id = isNotification ? -1 : rt.nextRpcId++;
        if (!isNotification) frame.insert(QStringLiteral("id"), id);

        const QString serverId = rt.config.id;
        if (rt.config.transport == QStringLiteral("stdio")) {
            if (!rt.process || rt.process->state() != QProcess::Running) {
                if (onError) onError(QStringLiteral("stdio MCP 进程未运行。"));
                return;
            }
            const QByteArray line = QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
            if (!isNotification) {
                rt.pending.insert(id, {std::move(onResponse), std::move(onError)});
                QTimer::singleShot(timeoutMs, q, [this, serverId, id] {
                    auto target = servers.value(serverId);
                    if (!target || !target->pending.contains(id)) return;
                    auto expired = target->pending.take(id);
                    if (expired.onError) expired.onError(QStringLiteral("MCP 请求超时。"));
                });
            }
            if (rt.process->write(line) != line.size()) {
                auto failed = rt.pending.take(id);
                if (failed.onError) failed.onError(QStringLiteral("无法写入 stdio MCP 管道。"));
            }
            return;
        }

        // Streamable HTTP JSON-RPC 2.0
        QNetworkRequest request{QUrl(rt.config.url)};
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Accept", "application/json, text/event-stream");
        if (!rt.secret.isEmpty()) {
            const QByteArray headerVal = rt.secret.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive)
                ? rt.secret.toUtf8()
                : (QByteArrayLiteral("Bearer ") + rt.secret.toUtf8());
            request.setRawHeader("Authorization", headerVal);
        }
        if (!rt.sessionId.isEmpty()) {
            request.setRawHeader("Mcp-Session-Id", rt.sessionId.toUtf8());
        }
        QNetworkReply* reply = network->post(request, QJsonDocument(frame).toJson(QJsonDocument::Compact));
        auto* timer = new QTimer(reply);
        timer->setSingleShot(true);
        QObject::connect(timer, &QTimer::timeout, reply, [reply] { reply->abort(); });
        timer->start(timeoutMs);

        QObject::connect(reply, &QNetworkReply::finished, q,
            [this, serverId, reply, id, isNotification, onResponse = std::move(onResponse), onError = std::move(onError)] {
                reply->deleteLater();
                auto target = servers.value(serverId);
                if (!target) return;
                const QByteArray sid = reply->rawHeader("Mcp-Session-Id");
                if (!sid.isEmpty()) target->sessionId = QString::fromUtf8(sid);
                if (reply->error() != QNetworkReply::NoError) {
                    if (onError) onError(RedactSecret(reply->errorString(), target->secret));
                    return;
                }
                if (isNotification) return;
                const QByteArray body = reply->read(kMaxStdioBufferBytes);
                const auto parsed = ParseHttpJsonRpcBody(body, id);
                if (!parsed) {
                    if (onError) onError(QStringLiteral("HTTP MCP 返回了无法解析的 JSON-RPC 响应。"));
                    return;
                }
                if (onResponse) onResponse(*parsed);
            });
    }

    void BeginHandshake(ServerRuntime& rt) {
        const QString serverId = rt.config.id;
        const QJsonObject initParams{
            {QStringLiteral("protocolVersion"), QStringLiteral("2025-03-26")},
            {QStringLiteral("capabilities"), QJsonObject{}},
            {QStringLiteral("clientInfo"), QJsonObject{
                {QStringLiteral("name"), QStringLiteral("Audastra")},
                {QStringLiteral("version"), QStringLiteral("0.5.8")},
            }},
        };
        SendRpc(rt, QStringLiteral("initialize"), initParams, false, 12000,
            [this, serverId](const QJsonObject& response) {
                auto target = servers.value(serverId);
                if (!target) return;
                if (response.contains(QStringLiteral("error"))) {
                    const QString msg = response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString(QStringLiteral("握手被拒绝"));
                    StopServer(*target, RedactSecret(msg, target->secret));
                    emit q->StatusChanged();
                    return;
                }
                const QJsonObject result = response.value(QStringLiteral("result")).toObject();
                target->instructions = result.value(QStringLiteral("instructions")).toString().trimmed();
                SendRpc(*target, QStringLiteral("notifications/initialized"), {}, true, 5000);
                FetchToolsPage(*target, QString{});
            },
            [this, serverId](const QString& err) {
                auto target = servers.value(serverId);
                if (!target) return;
                StopServer(*target, err);
                emit q->StatusChanged();
            });
    }

    void FetchToolsPage(ServerRuntime& rt, const QString& cursor) {
        const QString serverId = rt.config.id;
        QJsonObject params;
        if (!cursor.isEmpty()) params.insert(QStringLiteral("cursor"), cursor);
        SendRpc(rt, QStringLiteral("tools/list"), params, false, 12000,
            [this, serverId](const QJsonObject& response) {
                auto target = servers.value(serverId);
                if (!target) return;
                if (response.contains(QStringLiteral("error"))) {
                    const QString msg = response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString(QStringLiteral("读取工具列表失败"));
                    StopServer(*target, RedactSecret(msg, target->secret));
                    emit q->StatusChanged();
                    return;
                }
                const QJsonObject result = response.value(QStringLiteral("result")).toObject();
                for (const auto& item : result.value(QStringLiteral("tools")).toArray()) {
                    if (target->tools.size() >= kMaxToolsPerServer) break;
                    const auto obj = item.toObject();
                    const QString remoteName = obj.value(QStringLiteral("name")).toString().trimmed();
                    if (remoteName.isEmpty()) continue;
                    DiscoveredExternalMcpTool tool;
                    tool.serverId = target->config.id;
                    tool.serverName = target->config.name;
                    tool.remoteName = remoteName;
                    tool.qualifiedName = QStringLiteral("mcp__%1__%2").arg(target->slug, remoteName);
                    tool.description = obj.value(QStringLiteral("description")).toString(remoteName);
                    tool.inputSchema = obj.value(QStringLiteral("inputSchema")).toObject();
                    if (tool.inputSchema.isEmpty()) {
                        tool.inputSchema = {{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), QJsonObject{}}};
                    }
                    tool.minimumPermission = target->config.minimumPermission;
                    target->tools.push_back(std::move(tool));
                }
                const QString nextCursor = result.value(QStringLiteral("nextCursor")).toString();
                if (!nextCursor.isEmpty() && target->tools.size() < kMaxToolsPerServer) {
                    FetchToolsPage(*target, nextCursor);
                    return;
                }
                target->connecting = false;
                target->connected = true;
                target->error.clear();
                target->autoReconnectAttempts = 0;
                emit q->StatusChanged();
            },
            [this, serverId](const QString& err) {
                auto target = servers.value(serverId);
                if (!target) return;
                StopServer(*target, err);
                emit q->StatusChanged();
            });
    }

    ExternalMcpManager* q;
    AgentSettings settings;
    std::unique_ptr<CredentialStore> fallbackCredentials;
    CredentialStore* credentialStore = nullptr;
    QNetworkAccessManager* network = nullptr;
    QStringList serverOrder;
    QHash<QString, std::shared_ptr<ServerRuntime>> servers;
#ifdef Q_OS_WIN
    HANDLE jobHandle = nullptr;
#endif
};

ExternalMcpManager::ExternalMcpManager(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>(this)) {}
ExternalMcpManager::~ExternalMcpManager() = default;

void ExternalMcpManager::SetCredentialStore(CredentialStore* store) {
    if (store != nullptr) impl_->credentialStore = store;
}

void ExternalMcpManager::ApplySettings(const AgentSettings& settings) {
    impl_->ApplySettings(settings);
}

void ExternalMcpManager::Reconnect(const QString& serverId) {
    if (auto rt = impl_->servers.value(serverId)) {
        impl_->ConnectServer(*rt, true);
    }
}

void ExternalMcpManager::ReconnectAll() {
    for (const QString& id : impl_->serverOrder) {
        if (auto rt = impl_->servers.value(id); rt && rt->config.enabled) {
            impl_->ConnectServer(*rt, true);
        }
    }
}

void ExternalMcpManager::StopAll() {
    impl_->StopAll(true);
}

QList<ExternalMcpRuntimeStatus> ExternalMcpManager::Statuses() const {
    QList<ExternalMcpRuntimeStatus> list;
    for (const QString& id : impl_->serverOrder) {
        const auto rt = impl_->servers.value(id);
        if (!rt) continue;
        ExternalMcpRuntimeStatus st;
        st.id = rt->config.id;
        st.name = rt->config.name;
        st.transport = rt->config.transport;
        st.enabled = rt->config.enabled;
        st.connected = rt->connected;
        st.connecting = rt->connecting;
        st.blockedByPermission = rt->config.enabled && !ExternalMcpPermissionSatisfied(rt->config.minimumPermission, impl_->settings);
        st.toolCount = static_cast<int>(rt->tools.size());
        int disabled = 0;
        for (const auto& t : rt->tools) {
            if (impl_->settings.disabledTools.contains(t.qualifiedName)) ++disabled;
        }
        st.disabledToolCount = disabled;
        st.instructions = rt->instructions;
        st.error = rt->error;
        list.push_back(std::move(st));
    }
    return list;
}

QList<DiscoveredExternalMcpTool> ExternalMcpManager::DiscoveredTools() const {
    QList<DiscoveredExternalMcpTool> result;
    for (const QString& id : impl_->serverOrder) {
        const auto rt = impl_->servers.value(id);
        if (!rt || !rt->config.enabled || !rt->connected) continue;
        result.append(rt->tools);
    }
    return result;
}

QStringList ExternalMcpManager::AllDiscoveredToolNames() const {
    QStringList names;
    for (const auto& tool : DiscoveredTools()) names.push_back(tool.qualifiedName);
    return names;
}

QJsonArray ExternalMcpManager::EnabledToolDefinitions() const {
    QJsonArray defs;
    for (const QString& id : impl_->serverOrder) {
        const auto rt = impl_->servers.value(id);
        if (!rt || !rt->config.enabled || !rt->connected) continue;
        if (!ExternalMcpPermissionSatisfied(rt->config.minimumPermission, impl_->settings)) continue;
        for (const auto& tool : rt->tools) {
            if (impl_->settings.disabledTools.contains(tool.qualifiedName)) continue;
            defs.append(QJsonObject{
                {QStringLiteral("name"), tool.qualifiedName},
                {QStringLiteral("description"), QStringLiteral("[%1] %2").arg(tool.serverName, tool.description)},
                {QStringLiteral("inputSchema"), tool.inputSchema},
            });
        }
    }
    return defs;
}

QString ExternalMcpManager::CombinedServerInstructions() const {
    QStringList parts;
    for (const QString& id : impl_->serverOrder) {
        const auto rt = impl_->servers.value(id);
        if (!rt || !rt->config.enabled || !rt->connected || rt->instructions.isEmpty()) continue;
        if (!ExternalMcpPermissionSatisfied(rt->config.minimumPermission, impl_->settings)) continue;
        parts.push_back(QStringLiteral("## MCP 服务说明：%1\n%2").arg(rt->config.name, rt->instructions));
    }
    return parts.join(QStringLiteral("\n\n"));
}

bool ExternalMcpManager::OwnsTool(const QString& qualifiedName) const {
    return qualifiedName.startsWith(QStringLiteral("mcp__"));
}

QJsonObject ExternalMcpManager::InvokeTool(const QString& qualifiedName, const QJsonObject& arguments, int timeoutMs) {
    std::shared_ptr<Impl::ServerRuntime> targetRt;
    DiscoveredExternalMcpTool targetTool;
    bool found = false;
    for (const QString& id : impl_->serverOrder) {
        const auto rt = impl_->servers.value(id);
        if (!rt) continue;
        for (const auto& tool : rt->tools) {
            if (tool.qualifiedName == qualifiedName) {
                targetRt = rt;
                targetTool = tool;
                found = true;
                break;
            }
        }
        if (found) break;
    }
    if (!found || !targetRt || !targetRt->config.enabled || !targetRt->connected) {
        return McpToolError(QStringLiteral("MCP_TOOL_UNAVAILABLE"), QStringLiteral("外部 MCP 服务器未连接或该工具不可用。"));
    }
    if (!ExternalMcpPermissionSatisfied(targetRt->config.minimumPermission, impl_->settings)) {
        return McpToolError(QStringLiteral("MCP_PERMISSION_DENIED"),
            QStringLiteral("当前会话权限低于外部 MCP 服务器“%1”所需的最低权限档位（%2）。")
                .arg(targetRt->config.name, targetRt->config.minimumPermission));
    }
    if (impl_->settings.disabledTools.contains(qualifiedName)) {
        return McpToolError(QStringLiteral("MCP_TOOL_DISABLED"), QStringLiteral("该外部 MCP 工具已在设置中禁用。"));
    }

    QJsonObject outcome;
    bool finished = false;
    QEventLoop loop;
    const QPointer<ExternalMcpManager> guard(this);
    const QString secretCopy = targetRt->secret;

    impl_->SendRpc(*targetRt, QStringLiteral("tools/call"),
        QJsonObject{{QStringLiteral("name"), targetTool.remoteName}, {QStringLiteral("arguments"), arguments}},
        false, std::max(1000, timeoutMs),
        [&](const QJsonObject& response) {
            if (response.contains(QStringLiteral("error"))) {
                const auto errObj = response.value(QStringLiteral("error")).toObject();
                outcome = McpToolError(QStringLiteral("MCP_REMOTE_ERROR"),
                    RedactSecret(errObj.value(QStringLiteral("message")).toString(QStringLiteral("外部 MCP 工具返回错误。")), secretCopy));
            } else {
                outcome = RedactJson(response.value(QStringLiteral("result")), secretCopy).toObject();
            }
            finished = true;
            loop.quit();
        },
        [&](const QString& errText) {
            outcome = McpToolError(QStringLiteral("MCP_TRANSPORT_ERROR"), RedactSecret(errText, secretCopy));
            finished = true;
            loop.quit();
        });

    if (!finished && guard) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    if (!guard) {
        return McpToolError(QStringLiteral("MCP_MANAGER_CLOSED"), QStringLiteral("外部 MCP 管理器已关闭。"));
    }
    return outcome;
}

QList<ExternalMcpServerSettings> ExternalMcpManager::ParseMcpServersJson(
    const QString& jsonText, QHash<QString, QString>* extractedSecrets, QString* error) {
    if (error != nullptr) error->clear();
    if (extractedSecrets != nullptr) extractedSecrets->clear();
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error != nullptr) *error = QStringLiteral("无效的 JSON 对象，请粘贴包含 mcpServers 的配置。");
        return {};
    }
    QJsonObject serversMap = doc.object();
    if (serversMap.value(QStringLiteral("mcpServers")).isObject()) {
        serversMap = serversMap.value(QStringLiteral("mcpServers")).toObject();
    } else if (serversMap.value(QStringLiteral("servers")).isObject()) {
        serversMap = serversMap.value(QStringLiteral("servers")).toObject();
    }
    if (serversMap.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("未在 JSON 中找到任何 MCP 服务器条目。");
        return {};
    }
    QList<ExternalMcpServerSettings> parsed;
    for (auto it = serversMap.constBegin(); it != serversMap.constEnd(); ++it) {
        if (!it.value().isObject()) continue;
        const QJsonObject item = it.value().toObject();
        ExternalMcpServerSettings server;
        server.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        server.name = it.key().trimmed().left(256);
        server.enabled = !item.value(QStringLiteral("disabled")).toBool(false);
        server.minimumPermission = QStringLiteral("full");

        const QString url = item.value(QStringLiteral("url")).toString(item.value(QStringLiteral("serverUrl")).toString()).trimmed();
        const QString command = item.value(QStringLiteral("command")).toString().trimmed();
        QString extractedSecret;

        if (!url.isEmpty()) {
            server.transport = QStringLiteral("http");
            server.url = url;
            const QJsonObject headers = item.value(QStringLiteral("headers")).toObject();
            for (auto hit = headers.constBegin(); hit != headers.constEnd(); ++hit) {
                if (hit.key().compare(QStringLiteral("Authorization"), Qt::CaseInsensitive) == 0 && hit.value().isString()) {
                    QString bearer = hit.value().toString().trimmed();
                    if (bearer.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive)) bearer = bearer.mid(7).trimmed();
                    extractedSecret = bearer;
                }
            }
        } else if (!command.isEmpty()) {
            server.transport = QStringLiteral("stdio");
            server.command = command;
            for (const auto& arg : item.value(QStringLiteral("args")).toArray()) {
                if (arg.isString()) server.args.push_back(arg.toString());
            }
            server.cwd = item.value(QStringLiteral("cwd")).toString().trimmed();
            const QJsonObject envObj = item.value(QStringLiteral("env")).toObject();
            for (auto eit = envObj.constBegin(); eit != envObj.constEnd(); ++eit) {
                if (!eit.value().isString()) continue;
                if (LooksLikeSecretEnvKey(eit.key())) {
                    if (extractedSecret.isEmpty()) extractedSecret = eit.value().toString();
                } else {
                    server.env.push_back({eit.key(), eit.value().toString()});
                }
            }
        } else {
            if (error != nullptr) *error = QStringLiteral("服务器“%1”缺少 command 或 url 字段。").arg(it.key());
            return {};
        }
        if (!extractedSecret.isEmpty() && extractedSecrets != nullptr) {
            extractedSecrets->insert(server.id, extractedSecret);
        }
        parsed.push_back(std::move(server));
    }
    if (parsed.isEmpty() && error != nullptr) *error = QStringLiteral("未能解析出有效的 MCP 服务器配置。");
    return parsed;
}

} // namespace War3Map::Agent
