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
    QStringList providerThinkingLevels;
    QStringList customThinkingLevels;
    bool thinkingLevelsCustomized = false;
    QString selectedThinkingLevel;
};

struct ResolvedThinkingCapability {
    QStringList levels;      // 按标准升序排列的可选档位：off / minimal / low / medium / high / xhigh / max
    QString defaultLevel;    // 模型预设档位（如 DeepSeek V4 为 high；为空代表跟随接口默认）
    QString source;          // "手动指定" | "接口获取" | "内置目录" | ""
    bool discovered = false; // false 表示未自动识别到该模型思考档位，可提示用户自行勾选
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
    QString credentialRef;              // 存于 CredentialStore 的密钥引用
    // stdio：密钥注入的环境变量名（保留导入时的原名，如 GITHUB_TOKEN）；为空时使用 MCP_AUTH_TOKEN。
    QString credentialEnv;
    // http：true 时密钥是完整 Authorization 值（如 "Basic xxx"）原样发送；false 时按 Bearer 令牌发送。
    bool authorizationRaw = false;
    QString minimumPermission = QStringLiteral("full"); // "read" | "edit" | "save" | "full"
};

inline constexpr char kDefaultExternalMcpCredentialEnv[] = "MCP_AUTH_TOKEN";

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
    QString scope;            // 记忆条目：project / global；其他条目为空
    bool unconfirmed = false; // 记忆条目：作者未确认，未注入提示词
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
[[nodiscard]] const QStringList& AllThinkingLevels();
[[nodiscard]] bool IsValidThinkingLevel(const QString& level);
[[nodiscard]] QString NormalizeThinkingLevelToken(const QString& raw);
[[nodiscard]] QStringList NormalizeThinkingLevels(const QStringList& rawLevels);
[[nodiscard]] QString ThinkingLevelShortLabel(const QString& level);
[[nodiscard]] QString ThinkingLevelMenuLabel(const QString& level);
[[nodiscard]] ResolvedThinkingCapability ResolveThinkingCapability(
    const ModelSettings& model, const QString& protocol = {}, const QString& baseUrl = {});
[[nodiscard]] QString EffectiveThinkingLevel(
    const ModelSettings& model, const QString& protocol = {}, const QString& baseUrl = {});
[[nodiscard]] bool ExternalMcpPermissionSatisfied(const QString& minimumPermission, const AgentSettings& settings);
// 外部 MCP 只有在内置 AI 开启、该服务器启用且当前权限满足其最低档位时才允许启动进程或建立连接。
[[nodiscard]] bool ExternalMcpConnectionAllowed(const ExternalMcpServerSettings& server, const AgentSettings& settings);
// 名称（环境变量、参数、请求头、查询键）是否像密钥：末词为 password/secret/token/key/pat/credential/auth 等，大小写不敏感。
[[nodiscard]] bool ExternalMcpSecretName(const QString& name);
// 取值是否具有常见令牌格式（sk-、ghp_、github_pat_、AKIA…、Bearer/Basic 头值等）。
[[nodiscard]] bool ExternalMcpValueLooksSecret(const QString& value);
// 启动参数是否疑似携带明文密钥：--token=…、--api-key 值、Authorization: …、URL 查询或用户信息中的密钥、常见令牌格式。
[[nodiscard]] bool ExternalMcpArgsCarrySecret(const QStringList& args);

} // namespace War3Map::Agent
