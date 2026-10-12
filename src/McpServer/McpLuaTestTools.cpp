#include "McpLuaTestTools.h"
#include "McpToolInternal.h"

namespace War3Map::Mcp {
std::vector<ToolDefinition> LuaTestToolDefinitions() {
    std::vector<ToolDefinition> result;
    for (const auto* name : {"lua_test_hot_reload", "lua_test_reload_result"}) {
        ToolDefinition tool; tool.name = name;
        tool.description = tool.name == "lua_test_hot_reload" ? "把当前Lua触发器差异发送到本编辑器启动的单人测试；不保存地图"
            : "查询本次Lua热重载的真实游戏回执，含序号、耗时和来源位置";
        tool.permission = tool.name == "lua_test_hot_reload" ? ToolPermission::External : ToolPermission::Query;
        tool.inputSchema = BasicSchema({{"project_identity", QJsonObject{{"type", "string"}, {"maxLength", 128}}},
            {"expected_revision", QJsonObject{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}}}},
            {"project_identity", "expected_revision"});
        tool.maxRequestIdBytes = 128; tool.maxResponseBytes = 32 * 1024; result.push_back(std::move(tool));
    }
    return result;
}
QJsonObject InvokeLuaTestTool(const QString& name, const QJsonObject& args, const LuaTestToolHost& host) {
    if (name != "lua_test_hot_reload" && name != "lua_test_reload_result")
        return Failure("LUA_TEST_TOOL_UNKNOWN", "未知Lua测试工具。");
    if (args.size() != 2 || !args.value("project_identity").isString() || args.value("project_identity").toString().isEmpty()
        || args.value("project_identity").toString().size() > 128 || args.value("project_identity").toString().contains(QChar::Null)
        || !ParseUnsignedId(args, "expected_revision", true).Succeeded())
        return Failure("LUA_TEST_ARGUMENT_INVALID", "需要当前工程身份和编辑修订，不接受路径、邮箱句柄或任意代码。");
    if (!host.invoke) return Failure("LUA_RELOAD_NO_SESSION", "此宿主没有自有Lua单人测试会话，请连接运行该测试的编辑器MCP端口。");
    return host.invoke(name == "lua_test_hot_reload");
}
void RegisterLuaTestTools(McpServer& server) {
    for (auto tool : LuaTestToolDefinitions()) {
        const auto name = tool.name;
        tool.handler = [name](const QJsonObject& args) { return InvokeLuaTestTool(name, args, {}); };
        server.RegisterTool(std::move(tool));
    }
}
} // namespace War3Map::Mcp
