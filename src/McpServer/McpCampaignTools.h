#pragma once
#include "McpServer.h"
#include <vector>

namespace War3Map::Mcp {
// 独立 MCP 与活动 GUI 复用相同 schema、权限和会话命令。
[[nodiscard]] std::vector<ToolDefinition> CampaignToolDefinitions(McpServer& server);
void RegisterCampaignTools(McpServer& server);
}
