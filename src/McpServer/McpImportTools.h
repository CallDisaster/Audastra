#pragma once

#include "McpServer.h"
#include "War3MapCore/Services/ImportInventoryQuery.h"

#include <functional>
#include <memory>
#include <vector>

namespace War3Map::Mcp {

// 只读host，没有apply/save接口。context必须绑定与identity相同的documentId/revision。
// root持有原来源lease，构造Core快照；facade结束前复核宿主身份和完整来源。
struct ImportToolHost {
    std::function<QJsonObject()> identity;
    std::function<Result<std::shared_ptr<const Services::ImportInventorySnapshot>>()> context;
};

[[nodiscard]] std::vector<ToolDefinition> ImportToolDefinitions();
[[nodiscard]] QJsonObject InvokeImportTool(
    const QString& name, const QJsonObject& arguments, const ImportToolHost& host);

} // namespace War3Map::Mcp
