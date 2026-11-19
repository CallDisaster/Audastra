#pragma once

#include "AgentTypes.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>

namespace War3Map::Agent {

class CredentialStore;

struct DiscoveredExternalMcpTool {
    QString serverId;
    QString serverName;
    QString remoteName;     // 远端 MCP 原始工具名
    QString qualifiedName;  // 隔离名：mcp__<服务器名片段_服务器ID哈希>__<工具名>，不随列表顺序变化
    QString description;
    QJsonObject inputSchema;
    QString minimumPermission;
};

// 外部自定义 MCP 客户端管理器（支持 stdio 子进程与 Streamable HTTP JSON-RPC 2.0）：
// - 仅当内置 AI 开启、服务器启用且权限满足最低档位时才启动进程或建立连接，每次 ApplySettings 重新判断；
// - 异步完成 initialize -> notifications/initialized -> tools/list 握手；
// - Windows 下每个 stdio 服务器使用独立的 Kill-On-Close Job，停止时连同子孙进程一起结束；
// - 子进程只继承系统必需的环境变量白名单，加上该服务器配置的变量与密钥；
// - 支持单工具禁用、有限次数的崩溃重连与手动一键重连；
// - 支持解析标准 mcpServers JSON 配置，密钥转入凭据库，无法安全转换的条目拒绝导入。
class ExternalMcpManager final : public QObject {
    Q_OBJECT
public:
    explicit ExternalMcpManager(QObject* parent = nullptr);
    ~ExternalMcpManager() override;

    void SetCredentialStore(CredentialStore* store);
    void ApplySettings(const AgentSettings& settings);
    void Reconnect(const QString& serverId);
    void ReconnectAll();
    void StopAll();

    [[nodiscard]] QList<ExternalMcpRuntimeStatus> Statuses() const;
    [[nodiscard]] QList<DiscoveredExternalMcpTool> DiscoveredTools() const;
    [[nodiscard]] QStringList AllDiscoveredToolNames() const;

    // 返回在当前权限档位下实际可暴露给内置助手的 MCP 工具定义数组。
    [[nodiscard]] QJsonArray EnabledToolDefinitions() const;
    // 各服务器自带的 instructions：每份限长，并标明来自哪个服务器、不是作者指令。
    [[nodiscard]] QString CombinedServerInstructions() const;

    [[nodiscard]] bool OwnsTool(const QString& qualifiedName) const;
    [[nodiscard]] QJsonObject InvokeTool(const QString& qualifiedName, const QJsonObject& arguments, int timeoutMs = 30000);

    // 某服务器某远端工具的隔离名；只由服务器 ID 与名称决定，与其他服务器及列表顺序无关（同服务器内重名时运行时另加序号）。
    [[nodiscard]] static QString QualifiedToolName(const ExternalMcpServerSettings& server, const QString& remoteToolName);

    // 解析标准 { "mcpServers": { ... } } 或直接服务器映射对象。导入的服务器默认停用。
    // 密钥（Authorization 头、密钥类环境变量）提取到 extractedSecrets[serverId]，不写入配置结构；
    // 含多个密钥、自定义请求头、参数或地址查询中带密钥的服务器拒绝导入，原因写入 error（其余服务器照常返回）。
    [[nodiscard]] static QList<ExternalMcpServerSettings> ParseMcpServersJson(
        const QString& jsonText, QHash<QString, QString>* extractedSecrets = nullptr, QString* error = nullptr);

signals:
    void StatusChanged();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace War3Map::Agent
