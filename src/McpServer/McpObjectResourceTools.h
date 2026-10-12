#pragma once

#include "McpServer.h"
#include "War3MapCore/Services/ObjectResourceQuery.h"

namespace War3Map::Mcp {
// 只读入口；字段写入仍使用 objects_set_fields 的同一命令与事务。
struct ObjectResourceToolHost {
    std::function<QJsonObject()> identity;
    std::function<Result<std::shared_ptr<const Services::ObjectResourceCatalog>>(Services::ObjectResourceOrigin)> load;
    std::function<Result<Services::ObjectResourceField>(ObjectData::ObjectType, std::string_view)> field;
    std::function<Result<Services::ObjectResourceInfo>(std::string_view, Services::ObjectResourceKind)> inspect;
};
[[nodiscard]] std::vector<ToolDefinition> ObjectResourceToolDefinitions();
[[nodiscard]] QJsonObject InvokeObjectResourceTool(const QString& name, const QJsonObject& arguments,
    const ObjectResourceToolHost& host);
void RegisterObjectResourceTools(McpServer& server);
}
