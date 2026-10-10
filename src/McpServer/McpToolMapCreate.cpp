#include "McpToolInternal.h"
#include "War3MapCore/Services/MapCreateService.h"

namespace War3Map::Mcp {
void RegisterMapCreateTool(McpServer& server) {
    ToolDefinition tool;
    tool.name = "create_map_candidate";
    tool.permission = ToolPermission::Save;
    tool.description = "生成原创经典空地图，当前仅 classic-lordaeron-64-flat-v1：64×64 洛丹伦平地、"
        "1 名人类玩家、中心开始点、无对战初始化。输出与工作目录均须不存在。复用正式脚本构建和保存服务；"
        "不切换活动工程，不宣称原版或游戏验证。工具/游戏根由宿主配置。";
    tool.maxResponseBytes = 64 * 1024;
    const QJsonObject text{{"type", "string"}};
    tool.inputSchema = BasicSchema({{"profile", QJsonObject{{"type", "string"},
            {"enum", QJsonArray{QString::fromLatin1(Services::kClassicFlatMapProfile.data())}}}},
        {"name", text}, {"author", text}, {"output_path", text}, {"workspace_path", text}},
        {"profile", "name", "output_path", "workspace_path"});
    tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
            if (it.key() != "profile" && it.key() != "name" && it.key() != "author"
                && it.key() != "output_path" && it.key() != "workspace_path")
                return Failure("MCP_CREATE_FIELD_UNSUPPORTED", "新建请求含未支持字段。");
        if (server.Workspace() && server.Workspace()->HasActiveTransaction())
            return Failure("MCP_CREATE_TRANSACTION_ACTIVE", "请先结束活动编辑事务，再生成独立新地图。");
        for (const auto* field : {"profile", "name", "author"})
            if ((parameters.contains(field) || std::string_view(field) != "author")
                && !parameters.value(field).isString())
                return Failure("MCP_CREATE_TEXT_INVALID", "profile/name/author 必须是字符串。");
        const auto target = ParseAbsolutePath(parameters, "output_path");
        const auto workspace = ParseAbsolutePath(parameters, "workspace_path");
        const auto toolRoot = HostConfiguredSaveRoot("FORGE_MCP_YDWE_ROOT");
        const auto gameRoot = HostConfiguredSaveRoot("FORGE_MCP_WAR3_ROOT");
        if (!target.Succeeded()) return Failure(target);
        if (!workspace.Succeeded()) return Failure(workspace);
        if (!toolRoot.Succeeded()) return Failure(toolRoot);
        if (!gameRoot.Succeeded()) return Failure(gameRoot);
        Services::MapCreateRequest request;
        request.target = *target.value; request.workspace = *workspace.value;
        request.toolRoot = *toolRoot.value; request.gameRoot = *gameRoot.value;
        request.options.profile = parameters.value("profile").toString().toUtf8().toStdString();
        request.options.name = parameters.value("name").toString().toUtf8().toStdString();
        request.options.author = parameters.value("author").toString().toUtf8().toStdString();
        const auto result = Services::CreateMapCandidate(request);
        if (!result.Succeeded() || !result.value) return Failure(result);
        auto response = DiagnosticsJson(result.diagnostics);
        response["ok"] = true;
        response["created"] = true;
        response["path"] = QString::fromStdWString(result.value->path.wstring());
        response["sha256"] = QString::fromStdString(result.value->sha256);
        response["profile"] = QString::fromStdString(result.value->profile);
        response["member_count"] = static_cast<double>(result.value->memberCount);
        response["script_rebuilt"] = result.value->scriptRebuilt;
        response["original_editor_verified"] = false;
        response["runtime_verified"] = false;
        return response;
    };
    server.RegisterTool(std::move(tool));
}
} // namespace War3Map::Mcp
