#pragma once
#include "McpServer.h"
#include "War3MapCore/Services/MapRuntimePreparation.h"
#include <functional>
#include <vector>

namespace War3Map::Mcp {
// 活动地图宿主复用工程设置事务及来源/revision核对；独立stdio不能代为批准。
struct MapRuntimeToolHost {
    std::function<QJsonObject(const QString&, const QJsonObject&)> invoke;
};
[[nodiscard]] std::vector<ToolDefinition> MapRuntimeToolDefinitions();
[[nodiscard]] QJsonObject InvokeMapRuntimeTool(const QString& name, const QJsonObject& arguments,
    const MapRuntimeToolHost& host);
void RegisterMapRuntimeTools(McpServer& server);
[[nodiscard]] QJsonObject MapRuntimeSaveProperties();
[[nodiscard]] Result<Services::MapRuntimeSaveOptions> DecodeMapRuntimeSaveOptions(const QJsonObject& arguments);
} // namespace War3Map::Mcp
