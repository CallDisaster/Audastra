#pragma once

#include "McpServer.h"
#include "War3MapCore/Services/TerrainEditService.h"
#include "War3MapCore/Services/TerrainBoundsEditService.h"

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
    std::function<Result<Services::PreparedTerrainEdit>(const TerrainEditToolContext&,
        const Services::TerrainEditBinding&, const MapData::TerrainWaterStrokeOptions&,
        std::span<const MapData::TerrainStrokePoint>, bool)> prepareWater;
    std::function<Result<Services::PreparedTerrainEdit>(const TerrainEditToolContext&,
        const Services::TerrainEditBinding&, const MapData::TerrainBrush&,
        std::span<const MapData::TerrainStrokePoint>, bool)> prepareCliff;
    std::function<Result<Services::PreparedTerrainEdit>(const TerrainEditToolContext&,
        const Services::TerrainEditBinding&, const MapData::TerrainBrush&,
        std::span<const MapData::TerrainStrokePoint>, bool)> prepareCornerRamp;
    std::function<Result<Services::PreparedTerrainEdit>(const TerrainEditToolContext&,
        const Services::TerrainEditBinding&, const MapData::TerrainBrush&,
        std::span<const MapData::TerrainStrokePoint>, bool, MapData::TerrainStraightRampAlgorithm)> prepareStraightRamp;
    std::function<Result<Services::PreparedTerrainBoundsEdit>(const TerrainEditToolContext&,
        const Services::TerrainEditBinding&, const MapData::TerrainBoundsEditIntent&)> prepareBounds;
    std::function<Status()> verifyBounds;
    std::function<Status(const Services::PreparedTerrainBoundsEdit&,
        const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>&)> applyBounds;
    // 准备水刷的实际owner由宿主保留到返回；提交前重新核来源和资源身份。
    std::function<Status()> verifyWater;
    std::function<Result<MapData::TerrainStrokeResult>(const Services::PreparedTerrainEdit&,
        const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>&)> apply;
};

[[nodiscard]] QJsonObject TerrainEditBindingJson(const Services::TerrainEditBinding& binding);
[[nodiscard]] std::vector<ToolDefinition> TerrainEditToolDefinitions();
[[nodiscard]] QJsonObject InvokeTerrainEditTool(const QString& name,
    const QJsonObject& arguments, const TerrainEditToolHost& host);
void RegisterTerrainEditTools(McpServer& server);

} // namespace War3Map::Mcp
