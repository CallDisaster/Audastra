#pragma once

#include "McpServer.h"
#include "War3MapCore/Services/TerrainEditService.h"

#include <functional>

namespace War3Map::Mcp {

struct TerrainEditToolContext {
    // 宿主保证整个同步调用期间原会话有效，不允许把临时复制会话冒充当前 UI/事务。
    const MapData::MapDataEditSession* session = nullptr;
    Services::MapDataQueryBinding document;
    bool includesActiveTransactionOverlay = false;
    bool includesUnsavedUiRevision = false;
};

struct TerrainEditToolHost {
    std::function<QJsonObject()> identity;
    std::function<Result<TerrainEditToolContext>()> context;
    std::function<Result<MapData::TerrainStrokeResult>(const Services::PreparedTerrainEdit&,
        const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>&)> apply;
};

[[nodiscard]] QJsonObject TerrainEditBindingJson(const Services::TerrainEditBinding& binding);
[[nodiscard]] std::vector<ToolDefinition> TerrainEditToolDefinitions();
[[nodiscard]] QJsonObject InvokeTerrainEditTool(const QString& name,
    const QJsonObject& arguments, const TerrainEditToolHost& host);
void RegisterTerrainEditTools(McpServer& server);

} // namespace War3Map::Mcp
