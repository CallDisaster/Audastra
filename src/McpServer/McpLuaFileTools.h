#pragma once
#include "McpServer.h"
#include <functional>
#include <vector>

namespace War3Map::Mcp {
// 地图 Lua 文件与单人测试：只有打开该地图的编辑器能提供，独立 stdio 进程返回明确拒绝。
// lua_files_list / lua_file_read 只读；lua_file_write 进入同一撤销栈；map_test_start 启动本机游戏。
struct LuaFileToolHost { std::function<QJsonObject(const QString& name, const QJsonObject& arguments)> invoke; };
[[nodiscard]] std::vector<ToolDefinition> LuaFileToolDefinitions();
// 校验参数后交给宿主；不接受额外字段、路径穿越或超限正文。
[[nodiscard]] QJsonObject InvokeLuaFileTool(const QString& name, const QJsonObject& arguments, const LuaFileToolHost& host);
void RegisterLuaFileTools(McpServer& server);
} // namespace War3Map::Mcp
