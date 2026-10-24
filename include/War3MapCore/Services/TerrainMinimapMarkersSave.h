#pragma once

#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"

namespace War3Map::Services {

// 普通保存最终阶段共用的图标编排。能力不足保留来源MMP并警告；
// 实际I/O、来源变化或最终输出冲突仍失败。必须先于MapBLP准备调用。
[[nodiscard]] Status PrepareTerrainMinimapMarkers(MapSaveTask& task,
    const std::filesystem::path& gameRoot, const MapData::MapDataEditSession* edits,
    std::shared_ptr<const MapData::TerrainSavePlan>& plan);

} // namespace War3Map::Services
