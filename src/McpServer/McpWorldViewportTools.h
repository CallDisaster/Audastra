#pragma once
#include "McpServer.h"
#include <vector>

namespace War3Map::Mcp {
// 视口只存在于活动编辑器。独立宿主保留同一schema，明确报告没有视口。
struct WorldViewportToolHost {
    std::function<QJsonObject(const QString&, const QJsonObject&)> invoke;
};
[[nodiscard]] std::vector<ToolDefinition> WorldViewportToolDefinitions();
[[nodiscard]] QJsonObject InvokeWorldViewportTool(const QString& name, const QJsonObject& arguments,
    const WorldViewportToolHost& host);
void RegisterWorldViewportTools(McpServer& server);
} // namespace War3Map::Mcp
