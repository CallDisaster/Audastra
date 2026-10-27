#pragma once
#include "TerrainWaterEditPreparation.h"
#include "War3MapCore/Services/TerrainTextureSlotEditService.h"

namespace War3Map::QtProject {
struct TerrainTextureSlotEditCapture {
    TerrainWaterEditCapture resources;
    Services::PreparedTerrainEdit prepared;
};
// 在后台使用冻结会话，资源请求仍绑定原会话，候选只用于实际资源资格捕获。
[[nodiscard]] Result<TerrainTextureSlotEditCapture> PrepareTerrainTextureSlotEditCapture(
    std::shared_ptr<const TerrainWaterEditRequest> request,
    std::shared_ptr<const MapData::MapDataEditSession> frozen,
    const Services::TerrainEditBinding& binding, const Services::TerrainTextureSlotCommand& command);
} // namespace War3Map::QtProject
