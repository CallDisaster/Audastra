#pragma once

#include "McpServer.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Services/NameResolution.h"

#include <functional>
#include <memory>
#include <vector>

namespace War3Map::Mcp {

struct VariableToolContext {
    const Trigger::TriggerDocument* document = nullptr;
    const Trigger::UiCatalog* catalog = nullptr;
    std::shared_ptr<const Trigger::MapObjectReferenceSnapshot> mapObjects;
    std::function<Services::NameResolution(std::string_view, std::string_view)> nameResolver;
};

// 宿主必须在候选验证之后、SealDocument/原 Undo 发布之前调用；失败不得发布。
// GUI 的 changes 使用原改动面板 payload；独立事务可传空对象。
using VariablePrepublishValidator = std::function<Status(
    const Trigger::TriggerDocument& before, const Trigger::TriggerDocument& after,
    const Trigger::TriggerGraphEditReceipt& receipt, const QJsonObject& changes)>;

// facade 只转换 Graph DTO 和回执，不负责创建事务、撤销或保存。
struct VariableToolHost {
    std::function<QJsonObject()> identity;
    std::function<Result<VariableToolContext>()> context;
    std::function<Result<Trigger::TriggerGraphEditReceipt>(
        const Trigger::TriggerGraphEditBatch&, const VariablePrepublishValidator&)> apply;
};

[[nodiscard]] std::vector<ToolDefinition> VariableToolDefinitions();
[[nodiscard]] QJsonObject InvokeVariableTool(
    const QString& name, const QJsonObject& arguments, const VariableToolHost& host);
void RegisterVariableTools(McpServer& server);

} // namespace War3Map::Mcp
