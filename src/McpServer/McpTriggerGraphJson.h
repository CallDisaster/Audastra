#pragma once

#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Services/NameResolution.h"
#include <functional>
#include <QJsonObject>
#include <QJsonArray>

namespace War3Map::Mcp {
class McpWorkspace;

using GraphNameResolver = std::function<Services::NameResolution(std::string_view, std::string_view)>;
// 捕获当前地图 overlay 一次；对象/触发器为宿主同步查询期间的借用视图。
[[nodiscard]] GraphNameResolver CaptureWorkspaceNameResolver(McpWorkspace& workspace,
    const Trigger::TriggerDocument& document, const Trigger::MapObjectReferenceSnapshot* mapObjects = nullptr);

// 领域图 DTO 的唯一 JSON 边界。调用者另行验证自己的文档/事务身份，
// 不能把目录 revision、GUI 修订和图 binding 混为一体。
[[nodiscard]] Result<Trigger::TriggerGraphEditBatch> DecodeGraphBatch(const QJsonObject& request);
[[nodiscard]] QJsonObject GraphOperationSchema();
// ecas 表按函数名筛选的共用输入 schema（离线 MCP 与活动编辑器共用）。
[[nodiscard]] QJsonObject EcaFunctionFilterSchema();
// 图查询的输入字段（不含身份字段；离线 MCP 与活动编辑器共用）。
[[nodiscard]] QJsonObject GraphQueryProperties();
[[nodiscard]] QJsonObject GraphVariableRowJson(const Trigger::VariableRecord& record);
[[nodiscard]] QJsonObject GraphBindingJson(const Trigger::TriggerGraphBinding& binding);
[[nodiscard]] QJsonObject GraphRenameCodeAuditSchema();
[[nodiscard]] QJsonObject GraphRemovalCodeAuditSchema();
[[nodiscard]] Result<std::vector<Trigger::RemovalCodeConfirmation>> DecodeGraphRemovalCodeAudits(const QJsonArray&);
[[nodiscard]] Result<std::vector<Trigger::AtomicRenameCodeAudit>> DecodeGraphRenameCodeAudits(
    const QJsonArray& audits);
[[nodiscard]] QJsonObject QueryGraphDocument(
    const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog,
    const Trigger::MapObjectReferenceSnapshot* mapObjects, const QJsonObject& request,
    const GraphNameResolver& names = {}, const Trigger::TriggerGraphEditLimits& limits = {});

} // namespace War3Map::Mcp
