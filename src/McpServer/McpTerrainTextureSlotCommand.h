#pragma once
#include "War3MapCore/Services/TerrainTextureSlotCommand.h"
#include <QJsonArray>
#include <QJsonObject>
#include <optional>

namespace War3Map::Mcp {
std::optional<Services::TerrainTextureSlotCommand> ParseTerrainTextureSlotCommand(
    const QJsonObject& object, QJsonObject& error);
QJsonArray TerrainTextureSlotCommandSchemas();
} // namespace War3Map::Mcp
