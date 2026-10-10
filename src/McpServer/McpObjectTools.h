#pragma once

#include "McpServer.h"
#include "War3MapCore/Services/ObjectCommands.h"

#include <functional>
#include <vector>

namespace War3Map::Mcp {

// GUI/独立 MCP 仅提供可信上下文、身份和发布接缝；JSON schema 和领域操作共用。
struct ObjectToolHost {
    std::function<QJsonObject()> identity;
    std::function<Result<Services::ObjectCommandContext>(ObjectData::ObjectType)> context;
    std::function<Status(const Services::ObjectCommandPlan&)> commit;
    std::function<Services::ObjectDeletionAssessment(const ObjectData::ObjectKey&)> audit;
};

[[nodiscard]] std::vector<ToolDefinition> ObjectToolDefinitions();
[[nodiscard]] QJsonObject InvokeObjectTool(
    const QString& name, const QJsonObject& arguments, const ObjectToolHost& host);
[[nodiscard]] QJsonValue ObjectValueJson(const ObjectData::Value& value,
    const ObjectData::ObjectFieldDefinition* definition = nullptr);
void RegisterObjectTools(McpServer& server);

} // namespace War3Map::Mcp
