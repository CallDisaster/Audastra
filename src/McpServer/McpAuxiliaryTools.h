#pragma once

#include "McpMapDataTools.h"
#include "War3MapCore/Services/AuxiliaryEditing.h"

namespace War3Map::Mcp {

// 只做协议映射；所有结构、来源、引用与原生布局规则仍由 AuxiliaryEditing 判断。
struct AuxiliaryToolHost {
    std::function<QJsonObject()> identity;
    std::function<Result<MapDataToolContext>()> context;
    std::function<Services::AuxiliaryReferenceContext()> references;
    std::function<Result<MapData::MapDataEditSession::EditReceipt>(
        const Services::PreparedAuxiliaryEdit&, const MapDataPrepublishValidator&)> apply;
};

[[nodiscard]] std::vector<ToolDefinition> AuxiliaryToolDefinitions();
[[nodiscard]] QJsonObject InvokeAuxiliaryTool(const QString& name, const QJsonObject& arguments,
    const AuxiliaryToolHost& host);
void RegisterAuxiliaryTools(McpServer& server);

} // namespace War3Map::Mcp
