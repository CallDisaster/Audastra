#pragma once
#include "McpServer.h"
#include <vector>

namespace War3Map::Mcp {
// 游戏界面预览只存在于活动编辑器（界面工作区）。独立宿主保留同一 schema，明确报告不支持。
struct GameUiToolHost {
    std::function<QJsonObject(const QString&, const QJsonObject&)> invoke;
};
[[nodiscard]] std::vector<ToolDefinition> GameUiToolDefinitions();
// 校验参数后交给宿主；宿主只收到已通过 schema 与取值范围检查的参数。
[[nodiscard]] QJsonObject InvokeGameUiTool(const QString& name, const QJsonObject& arguments, const GameUiToolHost& host);
void RegisterGameUiTools(McpServer& server);
} // namespace War3Map::Mcp
