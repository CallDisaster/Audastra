#pragma once
#include "McpMapDataTools.h"
#include "War3MapCore/Services/MapCustomSettingsEditing.h"

namespace War3Map::Mcp {
struct MapCustomSettingsToolHost {
    std::function<QJsonObject()> identity;
    std::function<Result<MapDataToolContext>()> context;
    std::function<Result<Services::MapCustomSettingsValidation>()> validation;
    std::function<Result<MapData::MapDataEditSession::EditReceipt>(
        const Services::PreparedMapCustomSettingsEdit&, const Services::MapCustomSettingsValidation&,
        const MapDataPrepublishValidator&)> apply;
};
[[nodiscard]] QJsonObject MapCustomSettingsOperationSchema();
[[nodiscard]] std::vector<ToolDefinition> MapCustomSettingsToolDefinitions();
[[nodiscard]] QJsonObject InvokeMapCustomSettingsTool(const QString& name,
    const QJsonObject& arguments, const MapCustomSettingsToolHost& host);
void RegisterMapCustomSettingsTools(McpServer& server);
} // namespace War3Map::Mcp
