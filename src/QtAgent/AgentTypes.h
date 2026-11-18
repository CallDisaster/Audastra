#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace War3Map::Agent {

struct ModelSettings {
    QString id;
    QString name;
    bool enabled = true;
    qint64 contextOverride = 0;
    qint64 providerContext = 0;
    qint64 catalogContext = 0;
    qint64 maxOutputTokens = 0;
    bool toolsSupported = true;
    bool imagesSupported = false;
};

struct ProviderSettings {
    QString id;
    QString name;
    QString website;
    QString baseUrl;
    QString protocol = QStringLiteral("openai-chat");
    QString credentialRef;
    bool enabled = true;
    QList<ModelSettings> models;
};

// 外部自定义 MCP 服务器配置：支持 stdio 本地进程与 Streamable HTTP；鉴权密钥仅存 CredentialStore 引用。
struct ExternalMcpServerSettings {
    QString id;
    QString name;
    bool enabled = true;
    QString transport = QStringLiteral("stdio"); // "stdio" | "http"
    QString command;
    QStringList args;
    QString cwd;
    QList<QPair<QString, QString>> env; // 非敏感运行环境变量
    QString url;                        // HTTP / Streamable HTTP 地址
    QString credentialRef;              // 存于 CredentialStore 的 Bearer 密钥引用
    QString minimumPermission = QStringLiteral("full"); // "read" | "edit" | "save" | "full"
};

struct AgentSettings {
    bool aiEnabled = false;
    bool reducedMotion = false;
    // 助手或外部 MCP 改了触发器、变量时，编辑器跳到改动处（不抢焦点）。
    bool followChanges = false;
    QList<ProviderSettings> providers;
    bool allowEdits = false;
    bool allowSave = false;
    bool allowCommands = false;
    bool mcpEnabled = false;
    quint16 mcpPort = 8767;
    int maxAutoSteps = 128;
    QStringList disabledTools;
    QList<ExternalMcpServerSettings> externalMcpServers;
};

struct ContextWindow {
    qint64 tokens = 262144;
    QString source;
    bool estimated = true;
};

struct ContextNamedItem {
    QString name;
    QString detail;
    qint64 tokens = 0;
};

// 对齐 Claude Code / DSH TokenMeter 的上下文窗口真实构成快照。
struct ContextCompositionSnapshot {
    qint64 messagesTokens = 0;
    qint64 editorToolsTokens = 0;
    qint64 mcpToolsTokens = 0;
    qint64 systemPromptTokens = 0;
    qint64 skillsTokens = 0;
    qint64 mcpInstructionsTokens = 0;
    qint64 memoryFilesTokens = 0;
    double autocompactThresholdRatio = 0.80;
    QList<ContextNamedItem> editorToolItems;
    QList<ContextNamedItem> mcpToolItems;
    QList<ContextNamedItem> skillItems;
    QList<ContextNamedItem> memoryItems;

    [[nodiscard]] qint64 TotalEstimatedUsed() const noexcept {
        return messagesTokens + editorToolsTokens + mcpToolsTokens
            + systemPromptTokens + skillsTokens + mcpInstructionsTokens + memoryFilesTokens;
    }
};

struct ExternalMcpRuntimeStatus {
    QString id;
    QString name;
    QString transport;
    bool enabled = true;
    bool connected = false;
    bool connecting = false;
    bool blockedByPermission = false;
    int toolCount = 0;
    int disabledToolCount = 0;
    QString instructions;
    QString error;
};

[[nodiscard]] ContextWindow ResolveContextWindow(const ModelSettings& model);
[[nodiscard]] bool ExternalMcpPermissionSatisfied(const QString& minimumPermission, const AgentSettings& settings);

} // namespace War3Map::Agent
