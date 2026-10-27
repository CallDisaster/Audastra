#pragma once
#include "War3MapCore/Services/TerrainEditService.h"
#include "War3MapCore/Services/TerrainTextureSlotCommand.h"

namespace War3Map::Services {
// 只准备资源捕获所需的候选网格；不会改变会话或授予保存资格。
[[nodiscard]] Result<std::shared_ptr<const MapData::TerrainGridSnapshot>> PreviewTerrainTextureSlotCommand(
    const MapData::MapDataEditSession& session, const TerrainTextureSlotCommand& command);
[[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainTextureSlotEdit(
    const MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const TerrainEditBinding& expected, const MapData::TerrainPathingResources& resources,
    const TerrainTextureSlotCommand& command);
} // namespace War3Map::Services
