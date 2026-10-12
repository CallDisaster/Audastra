#pragma once

#include "McpMapDataTools.h"
#include "War3MapCore/MapData/MapDataPlacementEditing.h"
#include "War3MapCore/Services/PlacementCreation.h"

namespace War3Map::Mcp {

using PlacementPrepublishValidator = MapDataPrepublishValidator;
// GUI 使用原会话、原 Undo；独立宿主负责原来源的一批 create/delete/copy 或同原行重复 rotation 策略。
// facade 仅应用一个共用候选，不嵌套 Write/保存；完整回执在发布前验证预算。
struct PlacementToolHost {
    std::function<QJsonObject()> identity;
    std::function<Result<MapDataToolContext>()> context;
    std::function<Result<MapData::MapDataEditSession::EditReceipt>(
        const MapData::PreparedMapDataPlacement&, const PlacementPrepublishValidator&)> apply;
    std::function<Result<Services::ObjectCommandContext>(ObjectData::ObjectType)> objectContext;
    std::function<Result<MapData::TerrainPointSample>(const MapData::TerrainStrokePoint&)> terrainPoint;
};

[[nodiscard]] std::vector<ToolDefinition> PlacementToolDefinitions();
[[nodiscard]] QJsonObject InvokePlacementTool(
    const QString& name, const QJsonObject& arguments, const PlacementToolHost& host);
void RegisterPlacementTools(McpServer& server);

} // namespace War3Map::Mcp
