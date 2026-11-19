#include "ExternalMcpManager.h"
#include "CredentialStore.h"
#include "ProjectMemoryService.h"

#include "AudastraVersion.generated.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace War3Map::Agent {
namespace {

constexpr qsizetype kMaxStdioBufferBytes = 8 * 1024 * 1024;
constexpr qint64 kMaxHttpBodyBytes = 8 * 1024 * 1024;
constexpr qsizetype kMaxToolsPerServer = 512;
constexpr int kMaxToolListPages = 32;
constexpr int kMaxAutoReconnects = 2;
constexpr qint64 kStableConnectionMs = 60 * 1000;
constexpr qsizetype kMaxInstructionBytes = 4096;
constexpr qsizetype kMaxStoredInstructionChars = 16384;
constexpr qsizetype kMaxQualifiedToolName = 64;
constexpr qsizetype kMaxReadableSlug = 16;
constexpr qsizetype kMaxRemoteMessageChars = 2000;

// 服务器名的 ASCII 可读片段（小写字母数字，其他字符折叠为单个下划线）。
QString ReadableSlugPart(const QString& name) {
    QString out;
    for (const QChar ch : name) {
        const char16_t u = ch.unicode();
        if ((u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9')) {
            out.append(ch);
        } else if (u >= u'A' && u <= u'Z') {
            out.append(ch.toLower());
        } else if (!out.isEmpty() && !out.endsWith(QLatin1Char('_'))) {
            out.append(QLatin1Char('_'));
        }
    }
    out = out.left(kMaxReadableSlug);
    while (out.endsWith(QLatin1Char('_'))) out.chop(1);
    return out;
}

// 由服务器 ID 决定的稳定片段：可读名 + ID 的 SHA-256 前 8 位；中文名、重排、增删服务器都不会让工具名漂移到别的服务器。
QString ServerSlugFor(const ExternalMcpServerSettings& server) {
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(server.id.toUtf8(), QCryptographicHash::Sha256).toHex().left(8));
    const QString readable = ReadableSlugPart(server.name);
    return readable.isEmpty() ? hash : readable + QLatin1Char('_') + hash;
}

// 模型接口只接受 [A-Za-z0-9_-]，其余字符替换为下划线。
QString ToolNamePart(const QString& remoteName) {
    QString out;
    out.reserve(remoteName.size());
    for (const QChar ch : remoteName) {
        const char16_t u = ch.unicode();
        const bool allowed = (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9')
            || u == u'_' || u == u'-';
        out.append(allowed ? ch : QLatin1Char('_'));
    }
    return out.isEmpty() ? QStringLiteral("tool") : out;
}

QString BuildQualifiedName(const QString& slug, const QString& remoteName, const QSet<QString>& used) {
    const QString prefix = QStringLiteral("mcp__") + slug + QStringLiteral("__");
    const qsizetype room = std::max<qsizetype>(1, kMaxQualifiedToolName - prefix.size());
    const QString part = ToolNamePart(remoteName);
    QString candidate = prefix + part.left(room);
    for (int suffix = 2; used.contains(candidate); ++suffix) {
        const QString tail = QLatin1Char('_') + QString::number(suffix);
        candidate = prefix + part.left(std::max<qsizetype>(1, room - tail.size())) + tail;
    }
    return candidate;
}

QString HostOf(const QString& url) {
    const QString host = QUrl(url).host();
    return host.isEmpty() ? QStringLiteral("未知主机") : host;
}

QString TruncateUtf8(const QString& text, qsizetype maxBytes, bool* truncated) {
    const QByteArray utf8 = text.toUtf8();
    if (truncated != nullptr) *truncated = utf8.size() > maxBytes;
    if (utf8.size() <= maxBytes) return text;
    qsizetype cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(utf8.at(cut)) & 0xC0) == 0x80) --cut;
    return QString::fromUtf8(utf8.left(cut));
}

QString RedactText(QString text, const QString& secret, const QString& url) {
    if (!secret.isEmpty()) text.replace(secret, QStringLiteral("[已隐藏密钥]"));
    // 远端错误可能回显完整地址（路径或查询里可能有密钥），只保留主机名。
    if (!url.isEmpty()) text.replace(url, HostOf(url));
    return text;
}

QJsonValue RedactJson(const QJsonValue& value, const QString& secret) {
    if (secret.isEmpty()) return value;
    if (value.isString()) return RedactText(value.toString(), secret, {});
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

// 只比较影响连接的字段；改名只更新工具隔离名，不重连。
bool ConnectionConfigEquals(const ExternalMcpServerSettings& a, const ExternalMcpServerSettings& b) {
    return a.transport == b.transport && a.command == b.command && a.args == b.args
        && a.cwd == b.cwd && a.env == b.env && a.url == b.url
        && a.credentialRef == b.credentialRef && a.credentialEnv == b.credentialEnv
        && a.authorizationRaw == b.authorizationRaw;
}

bool IsResponseFor(const QJsonObject& object, qint64 expectedId) {
    const QJsonValue id = object.value(QStringLiteral("id"));
    return !object.contains(QStringLiteral("method")) && id.isDouble() && id.toInteger(-1) == expectedId;
}

// 从 HTTP JSON 或 Streamable HTTP SSE 响应体中取出 id 匹配的 JSON-RPC 响应；没有匹配项时不拿其他对象凑数。
std::optional<QJsonObject> ParseHttpJsonRpcBody(const QByteArray& raw, qint64 expectedId) {
    const QByteArray trimmed = raw.trimmed();
    if (trimmed.startsWith('{')) {
        QJsonParseError err;
        const auto doc = QJsonDocument::fromJson(trimmed, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            if (IsResponseFor(doc.object(), expectedId)) return doc.object();
            return std::nullopt;
        }
    }
    for (const QByteArray& line : raw.split('\n')) {
        QByteArray clean = line.trimmed();
        if (clean.startsWith("data:")) clean = clean.mid(5).trimmed();
        if (!clean.startsWith('{')) continue;
        QJsonParseError err;
        const auto doc = QJsonDocument::fromJson(clean, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;
        if (IsResponseFor(doc.object(), expectedId)) return doc.object();
    }
    return std::nullopt;
}

// 取 JSON-RPC result 对象；远端 error 或缺少 result 都按失败返回，避免把空对象当成功交给模型。
std::optional<QJsonObject> RpcResult(const QJsonObject& response, const QString& secret, const QString& url,
    bool* remoteError, QString* error) {
    if (remoteError != nullptr) *remoteError = false;
    if (response.contains(QStringLiteral("error"))) {
        if (remoteError != nullptr) *remoteError = true;
        QString message = response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().trimmed();
        if (message.isEmpty()) message = QStringLiteral("远端返回错误。");
        *error = RedactText(message.left(kMaxRemoteMessageChars), secret, url);
        return std::nullopt;
    }
    const QJsonValue result = response.value(QStringLiteral("result"));
    if (!result.isObject()) {
        *error = QStringLiteral("MCP 响应缺少 result 对象。");
        return std::nullopt;
    }
    return result.toObject();
}

QString HttpFailureMessage(const QString& host, QNetworkReply::NetworkError error, int status) {
    if (status >= 400) return QStringLiteral("HTTP MCP（%1）返回 HTTP %2。").arg(host, QString::number(status));
    QString detail;
    switch (error) {
    case QNetworkReply::ConnectionRefusedError: detail = QStringLiteral("连接被拒绝"); break;
    case QNetworkReply::HostNotFoundError: detail = QStringLiteral("找不到主机"); break;
    case QNetworkReply::TimeoutError: detail = QStringLiteral("连接超时"); break;
    case QNetworkReply::RemoteHostClosedError: detail = QStringLiteral("远端关闭了连接"); break;
    case QNetworkReply::SslHandshakeFailedError: detail = QStringLiteral("TLS 握手失败"); break;
    case QNetworkReply::OperationCanceledError: detail = QStringLiteral("请求已取消"); break;
    default: detail = QStringLiteral("网络错误 %1").arg(static_cast<int>(error)); break;
    }
    return QStringLiteral("HTTP MCP（%1）请求失败：%2。").arg(host, detail);
}

// stdio 子进程只继承系统运行所需的变量，编辑器进程里的其他密钥（各家 API Key 等）不传下去。
const QStringList& InheritedEnvironmentNames() {
    static const QStringList kNames{
        QStringLiteral("PATH"), QStringLiteral("PATHEXT"), QStringLiteral("SYSTEMROOT"), QStringLiteral("SYSTEMDRIVE"),
        QStringLiteral("WINDIR"), QStringLiteral("COMSPEC"), QStringLiteral("TEMP"), QStringLiteral("TMP"),
        QStringLiteral("USERPROFILE"), QStringLiteral("HOMEDRIVE"), QStringLiteral("HOMEPATH"),
        QStringLiteral("APPDATA"), QStringLiteral("LOCALAPPDATA"), QStringLiteral("PROGRAMDATA"),
        QStringLiteral("ALLUSERSPROFILE"), QStringLiteral("PUBLIC"),
        QStringLiteral("PROGRAMFILES"), QStringLiteral("PROGRAMFILES(X86)"), QStringLiteral("PROGRAMW6432"),
        QStringLiteral("COMMONPROGRAMFILES"), QStringLiteral("COMMONPROGRAMFILES(X86)"), QStringLiteral("COMMONPROGRAMW6432"),
        QStringLiteral("NUMBER_OF_PROCESSORS"), QStringLiteral("PROCESSOR_ARCHITECTURE"), QStringLiteral("PROCESSOR_IDENTIFIER"),
        QStringLiteral("OS"), QStringLiteral("USERNAME"), QStringLiteral("USERDOMAIN"), QStringLiteral("COMPUTERNAME"),
        // npx/uvx 等下载依赖时需要沿用本机代理设置。
        QStringLiteral("HTTP_PROXY"), QStringLiteral("HTTPS_PROXY"), QStringLiteral("NO_PROXY"), QStringLiteral("ALL_PROXY"),
    };
    return kNames;
}

QString ClientVersion() {
    return QString::fromUtf8(War3Map::BuildInfo::Version.data(), static_cast<qsizetype>(War3Map::BuildInfo::Version.size()));
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
        quint64 generation = 0; // 每次停止/重连递增；旧连接的回调按代号丢弃
        qint64 nextRpcId = 1;
        QElapsedTimer connectedTimer;
        QProcess* process = nullptr;
        QTimer* reconnectTimer = nullptr;
        QByteArray stdoutBuffer;
        QHash<qint64, PendingRequest> pending;
        QList<QPointer<QNetworkReply>> inflight;
        QList<DiscoveredExternalMcpTool> tools;
#ifdef Q_OS_WIN
        HANDLE job = nullptr;
#endif
    };

    explicit Impl(ExternalMcpManager* owner)
        : q(owner), fallbackCredentials(std::make_unique<CredentialStore>()), credentialStore(fallbackCredentials.get()) {
        network = new QNetworkAccessManager(q);
    }

    ~Impl() {
        StopAll(false);
    }

    [[nodiscard]] std::shared_ptr<ServerRuntime> Current(const QString& serverId, quint64 generation) const {
        auto target = servers.value(serverId);
        return target && target->generation == generation ? target : nullptr;
    }

    [[nodiscard]] bool Allowed(const ServerRuntime& rt) const {
        return ExternalMcpConnectionAllowed(rt.config, settings);
    }

    void TerminateProcessTree(ServerRuntime& rt) {
#ifdef Q_OS_WIN
        if (rt.job != nullptr) {
            // 结束整个 Job，包括 MCP 进程派生的子孙进程（如 npx → node）。
            static_cast<void>(TerminateJobObject(rt.job, 1));
            CloseHandle(rt.job);
            rt.job = nullptr;
        }
#endif
        if (rt.process) {
            rt.process->disconnect(q);
            if (rt.process->state() != QProcess::NotRunning) {
                rt.process->kill();
                rt.process->waitForFinished(500);
            }
            rt.process->deleteLater();
            rt.process = nullptr;
        }
    }

    void StopServer(ServerRuntime& rt, const QString& reason = {}) {
        ++rt.generation;
        if (rt.reconnectTimer) rt.reconnectTimer->stop();
        const auto replies = std::exchange(rt.inflight, {});
        const auto pendingCopy = std::exchange(rt.pending, {});
        for (const auto& reply : replies) {
            if (reply) reply->abort();
        }
        TerminateProcessTree(rt);
        rt.stdoutBuffer.clear();
        rt.connecting = false;
        rt.connected = false;
        rt.connectedTimer.invalidate();
        rt.sessionId.clear();
        rt.secret.fill(QChar(0));
        rt.secret.clear();
        if (!reason.isEmpty()) rt.error = reason;
        // 等待中的调用（含工具调用的局部事件循环）必须得到结果，不能悬挂。
        const QString message = reason.isEmpty() ? QStringLiteral("MCP 连接已关闭。") : reason;
        for (auto it = pendingCopy.constBegin(); it != pendingCopy.constEnd(); ++it) {
            if (it->onError) it->onError(message);
        }
    }

    void StopAll(bool notify = true) {
        const auto all = std::exchange(servers, {});
        serverOrder.clear();
        for (const auto& rt : all) {
            StopServer(*rt);
            if (rt->reconnectTimer) rt->reconnectTimer->deleteLater();
            rt->reconnectTimer = nullptr;
        }
        if (notify) emit q->StatusChanged();
    }

    void RebuildToolNames(ServerRuntime& rt) {
        QSet<QString> used;
        for (auto& tool : rt.tools) {
            tool.serverName = rt.config.name;
            tool.minimumPermission = rt.config.minimumPermission;
            tool.qualifiedName = BuildQualifiedName(rt.slug, tool.remoteName, used);
            used.insert(tool.qualifiedName);
        }
    }

    void ApplySettings(const AgentSettings& nextSettings) {
        settings = nextSettings;
        QSet<QString> nextIds;
        for (const auto& s : settings.externalMcpServers) nextIds.insert(s.id);

        // 移除已删除的服务器。
        for (const QString& existingId : std::as_const(serverOrder)) {
            if (nextIds.contains(existingId)) continue;
            if (auto rt = servers.take(existingId)) {
                StopServer(*rt);
                if (rt->reconnectTimer) rt->reconnectTimer->deleteLater();
                rt->reconnectTimer = nullptr;
            }
        }
        serverOrder.clear();
        for (const auto& cfg : settings.externalMcpServers) {
            if (cfg.id.isEmpty() || serverOrder.contains(cfg.id)) continue;
            serverOrder.push_back(cfg.id);
            auto rt = servers.value(cfg.id);
            const bool fresh = !rt;
            if (fresh) {
                rt = std::make_shared<ServerRuntime>();
                rt->reconnectTimer = new QTimer(q);
                rt->reconnectTimer->setSingleShot(true);
                const QString id = cfg.id;
                QObject::connect(rt->reconnectTimer, &QTimer::timeout, q, [this, id] {
                    if (auto target = servers.value(id)) ConnectServer(*target, false);
                });
                servers.insert(cfg.id, rt);
            }
            const bool endpointChanged = !fresh && !ConnectionConfigEquals(rt->config, cfg);
            rt->config = cfg;
            rt->slug = ServerSlugFor(cfg);
            RebuildToolNames(*rt);
            if (!Allowed(*rt)) {
                // AI 关闭、服务器停用或权限不足：不启动进程、不建立连接，已运行的立即停止。
                StopServer(*rt);
                rt->tools.clear();
                rt->instructions.clear();
                rt->error.clear();
                rt->autoReconnectAttempts = 0;
                continue;
            }
            if (fresh || endpointChanged || (!rt->connected && !rt->connecting && rt->error.isEmpty())) {
                ConnectServer(*rt, true);
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
        if (!Allowed(rt)) {
            emit q->StatusChanged();
            return;
        }
        if (!rt.config.credentialRef.isEmpty() && credentialStore != nullptr) {
            QString credError;
            rt.secret = credentialStore->Read(rt.config.credentialRef, &credError);
            if (!credError.isEmpty()) {
                rt.secret.clear();
                rt.error = credError;
                emit q->StatusChanged();
                return;
            }
        }
        if (rt.secret.contains(QLatin1Char('\r')) || rt.secret.contains(QLatin1Char('\n')) || rt.secret.contains(QChar(0))) {
            rt.secret.fill(QChar(0));
            rt.secret.clear();
            rt.error = QStringLiteral("MCP 密钥包含换行或空字符，未连接。");
            emit q->StatusChanged();
            return;
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

    QProcessEnvironment ChildEnvironment(const ServerRuntime& rt) const {
        QProcessEnvironment env;
        const auto system = QProcessEnvironment::systemEnvironment();
        for (const QString& name : InheritedEnvironmentNames()) {
            if (system.contains(name)) env.insert(name, system.value(name));
        }
        for (const auto& kv : rt.config.env) env.insert(kv.first, kv.second);
        if (!rt.secret.isEmpty()) {
            const QString secretName = rt.config.credentialEnv.trimmed().isEmpty()
                ? QString::fromLatin1(kDefaultExternalMcpCredentialEnv)
                : rt.config.credentialEnv.trimmed();
            env.insert(secretName, rt.secret);
        }
        return env;
    }

    void StartStdioProcess(ServerRuntime& rt) {
#ifdef Q_OS_WIN
        rt.job = CreateJobObjectW(nullptr, nullptr);
        if (rt.job != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(rt.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                CloseHandle(rt.job);
                rt.job = nullptr;
            }
        }
        if (rt.job == nullptr) {
            rt.connecting = false;
            rt.error = QStringLiteral("无法创建进程作业对象，未启动 MCP 进程。");
            emit q->StatusChanged();
            return;
        }
#endif
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
        proc->setProcessEnvironment(ChildEnvironment(rt));
        if (!rt.config.cwd.trimmed().isEmpty()) proc->setWorkingDirectory(rt.config.cwd.trimmed());
        proc->setProgram(rt.config.command.trimmed());
        proc->setArguments(rt.config.args);

        const QString serverId = rt.config.id;
        const quint64 generation = rt.generation;
        QObject::connect(proc, &QProcess::started, q, [this, serverId, generation] {
            auto target = Current(serverId, generation);
            if (!target || !target->process) return;
#ifdef Q_OS_WIN
            bool assigned = false;
            if (target->job != nullptr && target->process->processId() > 0) {
                HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                    static_cast<DWORD>(target->process->processId()));
                if (child != nullptr) {
                    assigned = AssignProcessToJobObject(target->job, child) != FALSE;
                    CloseHandle(child);
                }
            }
            if (!assigned) {
                StopServer(*target, QStringLiteral("无法将 MCP 进程纳入作业对象，已停止该进程。"));
                emit q->StatusChanged();
                return;
            }
#endif
            BeginHandshake(*target);
        });
        QObject::connect(proc, &QProcess::readyReadStandardOutput, q, [this, serverId, generation] {
            if (auto target = Current(serverId, generation)) ReadStdioOutput(*target);
        });
        QObject::connect(proc, &QProcess::readyReadStandardError, q, [this, serverId, generation] {
            auto target = Current(serverId, generation);
            if (!target || !target->process) return;
            target->process->setReadChannel(QProcess::StandardError);
            while (target->process->bytesAvailable() > 0) target->process->read(32768);
            target->process->setReadChannel(QProcess::StandardOutput);
        });
        QObject::connect(proc, &QProcess::errorOccurred, q, [this, serverId, generation](QProcess::ProcessError err) {
            auto target = Current(serverId, generation);
            if (!target) return;
            if (err == QProcess::FailedToStart) {
                StopServer(*target, QStringLiteral("无法启动 MCP 进程：%1").arg(target->config.command));
                emit q->StatusChanged();
            }
        });
        QObject::connect(proc, &QProcess::finished, q, [this, serverId, generation](int exitCode, QProcess::ExitStatus) {
            auto target = Current(serverId, generation);
            if (!target) return;
            const bool wasConnected = target->connected;
            // 只有稳定运行足够久才重置重连计数；握手后立即退出的进程最多重启配置的次数。
            const bool stable = wasConnected && target->connectedTimer.isValid()
                && target->connectedTimer.elapsed() >= kStableConnectionMs;
            StopServer(*target, QStringLiteral("MCP 进程已退出（代码 %1）").arg(exitCode));
            if (stable) target->autoReconnectAttempts = 0;
            if (wasConnected && Allowed(*target) && target->autoReconnectAttempts < kMaxAutoReconnects) {
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
        const quint64 generation = rt.generation;
        for (;;) {
            // 分发回调可能停止或重连本服务器；代号变化后不再处理旧缓冲。
            if (rt.generation != generation) return;
            // 兼容 Content-Length 头帧与标准 NDJSON 行帧。
            if (rt.stdoutBuffer.startsWith("Content-Length:")) {
                const auto headerEnd = rt.stdoutBuffer.indexOf("\r\n\r\n");
                if (headerEnd < 0) break;
                const auto firstLine = rt.stdoutBuffer.left(rt.stdoutBuffer.indexOf("\r\n"));
                bool ok = false;
                const qint64 bodyLen = firstLine.mid(15).trimmed().toLongLong(&ok);
                if (!ok || bodyLen < 0 || bodyLen > kMaxStdioBufferBytes) {
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
            const QByteArray line = rt.stdoutBuffer.left(nl).trimmed();
            rt.stdoutBuffer.remove(0, nl + 1);
            if (line.isEmpty()) continue;
            DispatchIncomingJson(rt, line);
        }
    }

    void WriteStdioFrame(ServerRuntime& rt, const QJsonObject& frame) {
        if (!rt.process || rt.process->state() != QProcess::Running) return;
        const QByteArray line = QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
        static_cast<void>(rt.process->write(line));
    }

    void DispatchIncomingJson(ServerRuntime& rt, const QByteArray& bytes) {
        QJsonParseError err;
        const auto doc = QJsonDocument::fromJson(bytes, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
        const auto obj = doc.object();
        if (obj.contains(QStringLiteral("method"))) {
            // 服务器发起的请求不是对本端请求的响应；只回应 ping，其余明确答复不支持。
            if (!obj.contains(QStringLiteral("id"))) return;
            QJsonObject reply{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), obj.value(QStringLiteral("id"))}};
            if (obj.value(QStringLiteral("method")).toString() == QStringLiteral("ping")) {
                reply.insert(QStringLiteral("result"), QJsonObject{});
            } else {
                reply.insert(QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32601},
                    {QStringLiteral("message"), QStringLiteral("Method not found")}});
            }
            WriteStdioFrame(rt, reply);
            return;
        }
        const QJsonValue idValue = obj.value(QStringLiteral("id"));
        if (!idValue.isDouble()) return;
        const auto found = rt.pending.find(idValue.toInteger(-1));
        if (found == rt.pending.end()) return;
        const PendingRequest pending = std::move(found.value());
        rt.pending.erase(found);
        if (pending.onResponse) pending.onResponse(obj);
    }

    void FailPending(ServerRuntime& rt, qint64 id, const QString& message) {
        const auto found = rt.pending.find(id);
        if (found == rt.pending.end()) return;
        const PendingRequest pending = std::move(found.value());
        rt.pending.erase(found);
        if (pending.onError) pending.onError(message);
    }

    void SendRpc(ServerRuntime& rt, const QString& method, const QJsonObject& params,
        bool isNotification, int timeoutMs,
        std::function<void(const QJsonObject&)> onResponse = {},
        std::function<void(const QString&)> onError = {}) {
        QJsonObject frame{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), method}};
        if (!params.isEmpty()) frame.insert(QStringLiteral("params"), params);
        const qint64 id = isNotification ? -1 : rt.nextRpcId++;
        if (!isNotification) {
            frame.insert(QStringLiteral("id"), id);
            rt.pending.insert(id, {std::move(onResponse), std::move(onError)});
        }

        const QString serverId = rt.config.id;
        const quint64 generation = rt.generation;
        if (rt.config.transport == QStringLiteral("stdio")) {
            if (!rt.process || rt.process->state() != QProcess::Running) {
                if (!isNotification) FailPending(rt, id, QStringLiteral("stdio MCP 进程未运行。"));
                return;
            }
            if (!isNotification) {
                QTimer::singleShot(timeoutMs, q, [this, serverId, generation, id] {
                    if (auto target = Current(serverId, generation)) FailPending(*target, id, QStringLiteral("MCP 请求超时。"));
                });
            }
            const QByteArray line = QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
            if (rt.process->write(line) != line.size() && !isNotification) {
                FailPending(rt, id, QStringLiteral("无法写入 stdio MCP 管道。"));
            }
            return;
        }

        // Streamable HTTP JSON-RPC 2.0
        QNetworkRequest request{QUrl(rt.config.url)};
        // 携带鉴权头时不自动跟随重定向；地址变化需作者在设置中明确修改。
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Accept", "application/json, text/event-stream");
        if (!rt.secret.isEmpty()) {
            const bool sendAsIs = rt.config.authorizationRaw
                || rt.secret.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive);
            request.setRawHeader("Authorization", sendAsIs
                ? rt.secret.toUtf8() : (QByteArrayLiteral("Bearer ") + rt.secret.toUtf8()));
        }
        if (!rt.sessionId.isEmpty()) {
            request.setRawHeader("Mcp-Session-Id", rt.sessionId.toUtf8());
        }
        QNetworkReply* reply = network->post(request, QJsonDocument(frame).toJson(QJsonDocument::Compact));
        rt.inflight.push_back(reply);
        auto received = std::make_shared<QByteArray>();
        auto overflow = std::make_shared<bool>(false);
        auto timedOut = std::make_shared<bool>(false);
        QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, received, overflow] {
            if (*overflow) return;
            received->append(reply->readAll());
            if (received->size() > kMaxHttpBodyBytes) {
                *overflow = true;
                reply->abort();
            }
        });
        QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [reply, overflow](qint64, qint64 total) {
            if (!*overflow && total > kMaxHttpBodyBytes) {
                *overflow = true;
                reply->abort();
            }
        });
        auto* timer = new QTimer(reply);
        timer->setSingleShot(true);
        QObject::connect(timer, &QTimer::timeout, reply, [reply, timedOut] {
            *timedOut = true;
            reply->abort();
        });
        timer->start(timeoutMs);

        QObject::connect(reply, &QNetworkReply::finished, q,
            [this, serverId, generation, reply, id, isNotification, received, overflow, timedOut] {
                reply->deleteLater();
                auto target = Current(serverId, generation);
                if (!target) return;
                target->inflight.removeIf([reply](const QPointer<QNetworkReply>& item) { return item.isNull() || item == reply; });
                if (!*overflow) {
                    received->append(reply->readAll());
                    if (received->size() > kMaxHttpBodyBytes) *overflow = true;
                }
                const QString host = HostOf(target->config.url);
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                QString failure;
                if (*overflow) {
                    failure = QStringLiteral("HTTP MCP（%1）响应超过 8 MiB，已中止。").arg(host);
                } else if (*timedOut) {
                    failure = QStringLiteral("HTTP MCP（%1）请求超时。").arg(host);
                } else if (status >= 300 && status < 400) {
                    failure = QStringLiteral("HTTP MCP（%1）返回重定向（HTTP %2），已拒绝跟随。").arg(host, QString::number(status));
                } else if (reply->error() != QNetworkReply::NoError || status >= 400) {
                    // 不使用 errorString()：其中含完整地址，可能把路径中的密钥带给模型。
                    failure = HttpFailureMessage(host, reply->error(), status);
                }
                if (!failure.isEmpty()) {
                    if (!isNotification) FailPending(*target, id, failure);
                    return;
                }
                if (isNotification) return;
                const QByteArray sid = reply->rawHeader("Mcp-Session-Id");
                if (!sid.isEmpty()) target->sessionId = QString::fromUtf8(sid);
                const auto parsed = ParseHttpJsonRpcBody(*received, id);
                if (!parsed) {
                    FailPending(*target, id, QStringLiteral("HTTP MCP（%1）返回的内容中没有与请求编号对应的 JSON-RPC 响应。").arg(host));
                    return;
                }
                const auto found = target->pending.find(id);
                if (found == target->pending.end()) return;
                const PendingRequest pending = std::move(found.value());
                target->pending.erase(found);
                if (pending.onResponse) pending.onResponse(*parsed);
            });
    }

    void BeginHandshake(ServerRuntime& rt) {
        const QString serverId = rt.config.id;
        const quint64 generation = rt.generation;
        const QJsonObject initParams{
            {QStringLiteral("protocolVersion"), QStringLiteral("2025-03-26")},
            {QStringLiteral("capabilities"), QJsonObject{}},
            {QStringLiteral("clientInfo"), QJsonObject{
                {QStringLiteral("name"), QStringLiteral("Audastra")},
                {QStringLiteral("version"), ClientVersion()},
            }},
        };
        SendRpc(rt, QStringLiteral("initialize"), initParams, false, 12000,
            [this, serverId, generation](const QJsonObject& response) {
                auto target = Current(serverId, generation);
                if (!target) return;
                QString err;
                const auto result = RpcResult(response, target->secret, target->config.url, nullptr, &err);
                if (!result) {
                    StopServer(*target, QStringLiteral("MCP 握手失败：%1").arg(err));
                    emit q->StatusChanged();
                    return;
                }
                target->instructions = result->value(QStringLiteral("instructions")).toString().trimmed()
                    .left(kMaxStoredInstructionChars);
                SendRpc(*target, QStringLiteral("notifications/initialized"), {}, true, 5000);
                FetchToolsPage(*target, QString{}, 1);
            },
            [this, serverId, generation](const QString& err) {
                auto target = Current(serverId, generation);
                if (!target) return;
                StopServer(*target, err);
                emit q->StatusChanged();
            });
    }

    void FetchToolsPage(ServerRuntime& rt, const QString& cursor, int page) {
        if (page > kMaxToolListPages) {
            StopServer(rt, QStringLiteral("MCP 工具列表分页超过 %1 页，已停止读取。").arg(kMaxToolListPages));
            emit q->StatusChanged();
            return;
        }
        const QString serverId = rt.config.id;
        const quint64 generation = rt.generation;
        QJsonObject params;
        if (!cursor.isEmpty()) params.insert(QStringLiteral("cursor"), cursor);
        SendRpc(rt, QStringLiteral("tools/list"), params, false, 12000,
            [this, serverId, generation, cursor, page](const QJsonObject& response) {
                auto target = Current(serverId, generation);
                if (!target) return;
                QString err;
                const auto result = RpcResult(response, target->secret, target->config.url, nullptr, &err);
                if (!result) {
                    StopServer(*target, QStringLiteral("读取 MCP 工具列表失败：%1").arg(err));
                    emit q->StatusChanged();
                    return;
                }
                const QJsonValue toolsValue = result->value(QStringLiteral("tools"));
                if (!toolsValue.isArray()) {
                    StopServer(*target, QStringLiteral("MCP 工具列表缺少 tools 数组。"));
                    emit q->StatusChanged();
                    return;
                }
                QSet<QString> remoteNames;
                QSet<QString> qualifiedNames;
                for (const auto& existing : std::as_const(target->tools)) {
                    remoteNames.insert(existing.remoteName);
                    qualifiedNames.insert(existing.qualifiedName);
                }
                for (const auto& item : toolsValue.toArray()) {
                    if (target->tools.size() >= kMaxToolsPerServer) break;
                    const auto obj = item.toObject();
                    const QString remoteName = obj.value(QStringLiteral("name")).toString().trimmed();
                    // 同一服务器重复的工具名只保留第一个，避免整个工具目录因重名被拒。
                    if (remoteName.isEmpty() || remoteNames.contains(remoteName)) continue;
                    DiscoveredExternalMcpTool tool;
                    tool.serverId = target->config.id;
                    tool.serverName = target->config.name;
                    tool.remoteName = remoteName;
                    tool.qualifiedName = BuildQualifiedName(target->slug, remoteName, qualifiedNames);
                    tool.description = obj.value(QStringLiteral("description")).toString(remoteName);
                    tool.inputSchema = obj.value(QStringLiteral("inputSchema")).toObject();
                    if (tool.inputSchema.isEmpty()) {
                        tool.inputSchema = {{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), QJsonObject{}}};
                    }
                    tool.minimumPermission = target->config.minimumPermission;
                    remoteNames.insert(remoteName);
                    qualifiedNames.insert(tool.qualifiedName);
                    target->tools.push_back(std::move(tool));
                }
                const QString nextCursor = result->value(QStringLiteral("nextCursor")).toString();
                if (!nextCursor.isEmpty() && target->tools.size() < kMaxToolsPerServer) {
                    if (nextCursor == cursor) {
                        StopServer(*target, QStringLiteral("MCP 工具列表分页游标未前进，已停止读取。"));
                        emit q->StatusChanged();
                        return;
                    }
                    FetchToolsPage(*target, nextCursor, page + 1);
                    return;
                }
                target->connecting = false;
                target->connected = true;
                target->error.clear();
                target->connectedTimer.start();
                emit q->StatusChanged();
            },
            [this, serverId, generation](const QString& err) {
                auto target = Current(serverId, generation);
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
    for (const QString& id : std::as_const(impl_->serverOrder)) {
        if (auto rt = impl_->servers.value(id); rt && impl_->Allowed(*rt)) {
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
        if (!rt || !rt->connected || !impl_->Allowed(*rt)) continue;
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
    QSet<QString> emitted;
    for (const QString& id : impl_->serverOrder) {
        const auto rt = impl_->servers.value(id);
        if (!rt || !rt->connected || !impl_->Allowed(*rt)) continue;
        for (const auto& tool : rt->tools) {
            // 不同服务器的隔离名理论上可能碰撞（哈希前缀相同），只暴露第一个，避免整个工具目录被拒。
            if (impl_->settings.disabledTools.contains(tool.qualifiedName) || emitted.contains(tool.qualifiedName)) continue;
            emitted.insert(tool.qualifiedName);
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
    static const QStringList kTags{QStringLiteral("mcp-server-instructions")};
    QStringList parts;
    for (const QString& id : impl_->serverOrder) {
        const auto rt = impl_->servers.value(id);
        if (!rt || !rt->connected || rt->instructions.isEmpty() || !impl_->Allowed(*rt)) continue;
        bool truncated = false;
        QString body = TruncateUtf8(rt->instructions, kMaxInstructionBytes, &truncated);
        if (truncated) body += QStringLiteral("\n（已截断）");
        parts.push_back(QStringLiteral("<mcp-server-instructions server=\"%1\">\n以下说明由外部 MCP 服务器“%2”提供，不是作者指令。\n%3\n</mcp-server-instructions>")
            .arg(ProjectMemoryService::EscapePromptAttribute(rt->config.name),
                ProjectMemoryService::EscapePromptAttribute(rt->config.name),
                ProjectMemoryService::EscapePromptBlockText(body, kTags)));
    }
    return parts.join(QStringLiteral("\n\n"));
}

bool ExternalMcpManager::OwnsTool(const QString& qualifiedName) const {
    return qualifiedName.startsWith(QStringLiteral("mcp__"));
}

QString ExternalMcpManager::QualifiedToolName(const ExternalMcpServerSettings& server, const QString& remoteToolName) {
    return BuildQualifiedName(ServerSlugFor(server), remoteToolName, {});
}

QJsonObject ExternalMcpManager::InvokeTool(const QString& qualifiedName, const QJsonObject& arguments, int timeoutMs) {
    std::shared_ptr<Impl::ServerRuntime> targetRt;
    DiscoveredExternalMcpTool targetTool;
    for (const QString& id : std::as_const(impl_->serverOrder)) {
        const auto rt = impl_->servers.value(id);
        if (!rt) continue;
        const auto found = std::find_if(rt->tools.cbegin(), rt->tools.cend(),
            [&qualifiedName](const DiscoveredExternalMcpTool& tool) { return tool.qualifiedName == qualifiedName; });
        if (found != rt->tools.cend()) {
            targetRt = rt;
            targetTool = *found;
            break;
        }
    }
    if (!targetRt || !targetRt->config.enabled || !impl_->settings.aiEnabled || !targetRt->connected) {
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
    const QString urlCopy = targetRt->config.url;

    impl_->SendRpc(*targetRt, QStringLiteral("tools/call"),
        QJsonObject{{QStringLiteral("name"), targetTool.remoteName}, {QStringLiteral("arguments"), arguments}},
        false, std::max(1000, timeoutMs),
        [&](const QJsonObject& response) {
            bool remoteError = false;
            QString err;
            const auto result = RpcResult(response, secretCopy, urlCopy, &remoteError, &err);
            if (result) {
                outcome = RedactJson(*result, secretCopy).toObject();
            } else {
                outcome = McpToolError(remoteError ? QStringLiteral("MCP_REMOTE_ERROR") : QStringLiteral("MCP_INVALID_RESPONSE"), err);
            }
            finished = true;
            loop.quit();
        },
        [&](const QString& errText) {
            outcome = McpToolError(QStringLiteral("MCP_TRANSPORT_ERROR"), RedactText(errText, secretCopy, urlCopy));
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
    const auto presentButNot = [](const QJsonValue& value, bool expected) {
        return !value.isUndefined() && !value.isNull() && !expected;
    };
    QList<ExternalMcpServerSettings> parsed;
    QStringList refused;
    for (auto it = serversMap.constBegin(); it != serversMap.constEnd(); ++it) {
        const QString displayName = it.key().trimmed().left(256);
        const QString label = displayName.isEmpty() ? QStringLiteral("未命名") : displayName;
        if (!it.value().isObject()) {
            refused.push_back(QStringLiteral("%1（条目不是对象）").arg(label));
            continue;
        }
        const QJsonObject item = it.value().toObject();
        ExternalMcpServerSettings server;
        server.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        server.name = displayName.isEmpty() ? QStringLiteral("MCP") : displayName;
        // 粘贴的配置可能来自他处，导入后保持停用，由作者核对后再启用。
        server.enabled = false;
        server.minimumPermission = QStringLiteral("full");

        const QString url = item.value(QStringLiteral("url")).toString(item.value(QStringLiteral("serverUrl")).toString()).trimmed();
        const QString command = item.value(QStringLiteral("command")).toString().trimmed();
        QString refusal;
        QString secret;

        if (!url.isEmpty()) {
            server.transport = QStringLiteral("http");
            server.url = url;
            const QUrl parsedUrl(url);
            if (!parsedUrl.isValid() || parsedUrl.host().isEmpty()) {
                refusal = QStringLiteral("地址无效");
            } else if (!parsedUrl.userInfo().isEmpty() || parsedUrl.hasQuery() || parsedUrl.hasFragment()) {
                refusal = QStringLiteral("地址含用户信息、查询参数或片段，可能携带密钥；请改用 Authorization 请求头");
            }
            const QJsonValue headersValue = item.value(QStringLiteral("headers"));
            if (refusal.isEmpty() && presentButNot(headersValue, headersValue.isObject())) refusal = QStringLiteral("headers 不是对象");
            const QJsonObject headers = headersValue.toObject();
            int authorizationHeaders = 0;
            for (auto hit = headers.constBegin(); refusal.isEmpty() && hit != headers.constEnd(); ++hit) {
                if (hit.key().compare(QStringLiteral("Authorization"), Qt::CaseInsensitive) != 0) {
                    refusal = QStringLiteral("不支持自定义请求头 %1，只支持 Authorization").arg(hit.key().left(64));
                    break;
                }
                if (!hit.value().isString()) { refusal = QStringLiteral("Authorization 不是文本"); break; }
                if (++authorizationHeaders > 1) { refusal = QStringLiteral("含多个 Authorization 请求头"); break; }
                const QString value = hit.value().toString().trimmed();
                if (value.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive)) {
                    secret = value.mid(7).trimmed();
                    server.authorizationRaw = false;
                } else {
                    // Basic 等其他方案保留完整值原样发送。
                    secret = value;
                    server.authorizationRaw = !value.isEmpty();
                }
            }
        } else if (!command.isEmpty()) {
            server.transport = QStringLiteral("stdio");
            server.command = command;
            const QJsonValue argsValue = item.value(QStringLiteral("args"));
            if (presentButNot(argsValue, argsValue.isArray())) refusal = QStringLiteral("args 不是数组");
            for (const auto& arg : argsValue.toArray()) {
                if (!refusal.isEmpty()) break;
                if (!arg.isString()) { refusal = QStringLiteral("args 含非文本项"); break; }
                server.args.push_back(arg.toString());
            }
            if (refusal.isEmpty() && ExternalMcpArgsCarrySecret(server.args)) {
                refusal = QStringLiteral("启动参数疑似包含明文密钥；请改为通过环境变量传入");
            }
            server.cwd = item.value(QStringLiteral("cwd")).toString().trimmed();
            const QJsonValue envValue = item.value(QStringLiteral("env"));
            if (refusal.isEmpty() && presentButNot(envValue, envValue.isObject())) refusal = QStringLiteral("env 不是对象");
            const QJsonObject envObj = envValue.toObject();
            QStringList secretNames;
            for (auto eit = envObj.constBegin(); refusal.isEmpty() && eit != envObj.constEnd(); ++eit) {
                if (eit.key().isEmpty() || eit.key().contains(QLatin1Char('='))) {
                    refusal = QStringLiteral("环境变量名无效");
                    break;
                }
                if (!eit.value().isString()) {
                    refusal = QStringLiteral("环境变量 %1 不是文本").arg(eit.key().left(64));
                    break;
                }
                const QString value = eit.value().toString();
                if (ExternalMcpSecretName(eit.key()) || ExternalMcpValueLooksSecret(value)) {
                    // 密钥按原变量名保留（进程仍收到 GITHUB_TOKEN 等），值转入凭据库。
                    secretNames.push_back(eit.key());
                    secret = value;
                } else {
                    server.env.push_back({eit.key(), value});
                }
            }
            if (refusal.isEmpty() && secretNames.size() > 1) {
                refusal = QStringLiteral("含多个密钥环境变量（%1），每个服务器只支持一个").arg(secretNames.join(QStringLiteral("、")));
            }
            if (refusal.isEmpty() && secretNames.size() == 1) server.credentialEnv = secretNames.front();
        } else {
            refusal = QStringLiteral("缺少 command 或 url 字段");
        }

        if (!refusal.isEmpty()) {
            secret.fill(QChar(0));
            refused.push_back(QStringLiteral("%1（%2）").arg(label, refusal));
            continue;
        }
        if (!secret.isEmpty() && extractedSecrets != nullptr) extractedSecrets->insert(server.id, secret);
        secret.fill(QChar(0));
        parsed.push_back(std::move(server));
    }
    if (error != nullptr) {
        if (!refused.isEmpty()) *error = QStringLiteral("未导入：%1").arg(refused.join(QStringLiteral("；")));
        else if (parsed.isEmpty()) *error = QStringLiteral("未能解析出有效的 MCP 服务器配置。");
    }
    return parsed;
}

} // namespace War3Map::Agent
