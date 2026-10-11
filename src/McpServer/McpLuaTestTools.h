#pragma once
#include "McpServer.h"
#include <vector>

namespace War3Map::Mcp {
// 只有活动编辑器能提供本局自有游戏会话。独立stdio不复用别的进程/邮箱。
struct LuaTestToolHost { std::function<QJsonObject(bool submit)> invoke; };
[[nodiscard]] std::vector<ToolDefinition> LuaTestToolDefinitions();
[[nodiscard]] QJsonObject InvokeLuaTestTool(const QString& name, const QJsonObject& arguments, const LuaTestToolHost& host);
void RegisterLuaTestTools(McpServer& server);
} // namespace War3Map::Mcp
