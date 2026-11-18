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
    QString qualifiedName;  // 隔离名：mcp__<serverSlug>__<remoteName>
    QString description;
    QJsonObject inputSchema;
    QString minimumPermission;
};

// 外部自定义 MCP 客户端管理器（支持 stdio 子进程与 Streamable HTTP JSON-RPC 2.0）：
// - 异步完成 initialize -> notifications/initialized -> tools/list 握手；
// - Windows 下将 stdio 子进程纳入 Kill-On-Close Job，防止残留孤儿进程；
// - 支持按最低权限档位门禁、单工具禁用、崩溃退避重连与手动一键重连；
// - 支持解析 Claude / Cursor 标准 mcpServers JSON 配置并自动剥离凭据。
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
    [[nodiscard]] QString CombinedServerInstructions() const;

    [[nodiscard]] bool OwnsTool(const QString& qualifiedName) const;
    [[nodiscard]] QJsonObject InvokeTool(const QString& qualifiedName, const QJsonObject& arguments, int timeoutMs = 30000);

    // 解析标准 { "mcpServers": { ... } } 或直接服务器映射对象；发现的明文鉴权值提取到 extractedSecrets[serverId]，不写入配置结构。
    [[nodiscard]] static QList<ExternalMcpServerSettings> ParseMcpServersJson(
        const QString& jsonText, QHash<QString, QString>* extractedSecrets = nullptr, QString* error = nullptr);

signals:
    void StatusChanged();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace War3Map::Agent
