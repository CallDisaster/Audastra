#pragma once

#include "JsonRpc.h"
#include "War3MapCore/Types.h"
#include "War3MapCore/Services/MapScriptTargetSelection.h"

#include <QJsonObject>
#include <QSet>
#include <QString>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace War3Map {
class EditorSession;
namespace ObjectData { class LniObjectDocument; }
namespace Trigger { class TriggerDocument; class UiCatalog; }
}

namespace War3Map::Mcp {

class McpWorkspace;
class McpTerrain;

/// MCP 工具处理函数签名
using ToolHandler = std::function<QJsonValue(const QJsonObject& params)>;

/// MCP 资源提供函数签名
using ResourceHandler = std::function<QJsonValue(const QString& uri)>;

enum class ToolPermission : std::uint8_t { Query = 1, Edit = 2, Save = 4, External = 8 };

struct ToolPolicy {
    // 旧 stdio 默认保持既有工具可用；宿主按作者设置传入明确权限。
    bool allowQuery = true;
    bool allowEdit = true;
    bool allowSave = true;
    bool allowExternal = true;
    QSet<QString> disabledTools;

    [[nodiscard]] bool Allows(ToolPermission permission) const noexcept;
};

struct ToolDefinition {
    QString name;
    QString description;
    QJsonObject inputSchema;
    ToolHandler handler;
    // 0 表示沿用旧工具行为；设置后按最终 JSON-RPC stdio 字节数限流。
    std::size_t maxResponseBytes = 0;
    ToolPermission permission = ToolPermission::Query;
    // 有界写工具先约束外层 request id，避免编辑成功后才因回执预算被拒绝。
    // 0 保留既有工具行为；额度按 JSON 编码后的 id 字节数计算。
    std::size_t maxRequestIdBytes = 0;
};

struct ResourceDefinition {
    QString uri;
    QString name;
    QString description;
    QString mimeType;
    ResourceHandler handler;
};

/// 与传输无关的 MCP dispatcher；旧 stdio 默认 2024-11-05。
/// 嵌入和 HTTP 均串行调用，不复制领域文档或 overlay。
class McpServer {
public:
    McpServer();
    ~McpServer();

    /// 设置工作区路径（打开地图项目）
    void SetWorkspace(const std::filesystem::path& projectRoot,
                      const std::filesystem::path& uiDataRoot);
    // 仅进程启动配置设置；协议请求只能选择已配置目标，不能传执行器路径。
    void SetLuaMapTarget(Services::LuaMapTargetRequest target) { luaMapTarget_ = std::move(target); }
    [[nodiscard]] const std::optional<Services::LuaMapTargetRequest>& LuaMapTarget() const noexcept { return luaMapTarget_; }

    /// 注册工具
    void RegisterTool(ToolDefinition tool);

    /// 可信 GUI 宿主替换既有处理器，保留 schema 与权限，不能由协议调用。
    [[nodiscard]] bool SetToolHandler(const QString& name, ToolHandler handler);
    void SetToolPolicy(ToolPolicy policy);
    [[nodiscard]] const ToolPolicy& Policy() const noexcept { return toolPolicy_; }
    [[nodiscard]] bool IsToolEnabled(const QString& name) const;
    // 仅真实提供通知通道时声明 listChanged；HTTP JSON-only 不安装 sink。
    void SetNotificationSink(std::function<void(const QByteArray&)> sink);
    [[nodiscard]] static bool SupportsProtocolVersion(const QString& version) noexcept;

    /// 注册资源
    void RegisterResource(ResourceDefinition resource);
    void ClearResources();
    void RemoveResourcesByPrefix(const QString& prefix);

    /// 处理单条 JSON-RPC 请求，返回响应（notification 返回空）
    std::optional<JsonRpcResponse> HandleMessage(const QByteArray& line);

    /// 运行 stdio 事件循环（阻塞）
    void RunStdio();

    /// 获取安全 MCP 工作区或其底层 EditorSession（初始化失败时为 null）。
    [[nodiscard]] McpWorkspace* Workspace() const noexcept;
    [[nodiscard]] EditorSession* Session() const noexcept;
    [[nodiscard]] McpTerrain* Terrain() const noexcept;
    [[nodiscard]] const std::string& DocumentId() const noexcept { return documentId_; }
    [[nodiscard]] std::uint64_t DocumentGeneration() const noexcept { return terrainGeneration_; }
    [[nodiscard]] const std::vector<Diagnostic>& InitializationDiagnostics() const noexcept {
        return initializationDiagnostics_;
    }

private:
    JsonRpcResponse HandleInitialize(const JsonRpcRequest& req);
    JsonRpcResponse HandleToolsList(const JsonRpcRequest& req);
    JsonRpcResponse HandleToolsCall(const JsonRpcRequest& req);
    JsonRpcResponse HandleResourcesList(const JsonRpcRequest& req);
    JsonRpcResponse HandleResourcesRead(const JsonRpcRequest& req);
    void NotifyToolsChanged();

    std::unique_ptr<McpWorkspace> workspace_;
    std::optional<Services::LuaMapTargetRequest> luaMapTarget_;
    // 后声明，保证先销毁借用 workspace 的只读地形适配器。
    std::unique_ptr<McpTerrain> terrain_;
    std::string documentId_;
    std::uint64_t terrainGeneration_ = 0;
    std::vector<Diagnostic> initializationDiagnostics_;
    std::filesystem::path projectRoot_;
    std::filesystem::path uiDataRoot_;
    std::unordered_map<QString, ToolDefinition> tools_;
    std::vector<ResourceDefinition> resources_;
    ToolPolicy toolPolicy_;
    std::function<void(const QByteArray&)> notificationSink_;
    bool initialized_ = false;
};

} // namespace War3Map::Mcp
