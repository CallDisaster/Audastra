#pragma once

#include "TerrainWaterEditPreparation.h"
#include "War3MapCore/Services/TerrainResizeEditService.h"

namespace War3Map::QtProject {

// World控件和Live MCP共享一次后台捕获；所有输入均由GUI先冻结，后台不借活动工程。
struct TerrainResizeEditCapture {
    std::shared_ptr<const TerrainWaterEditRequest> request;
    std::shared_ptr<const MapData::MapDataEditSession> frozen;
    MapData::TerrainResizeResources resources;
    std::optional<Services::PreparedTerrainResizeEdit> prepared;
    MapData::TerrainResizeLimits limits;
};

[[nodiscard]] Result<TerrainResizeEditCapture> PrepareTerrainResizeEditCapture(
    std::shared_ptr<const TerrainWaterEditRequest> request,
    std::shared_ptr<const MapData::MapDataEditSession> frozen,
    const Services::MapDataQueryBinding& document,
    const std::optional<MapData::TerrainResizeIntent>& intent = {},
    const std::string& expectedResources = {}, const MapData::TerrainResizeLimits& limits = {});
// 后台真实来源/资源复核；调用前后宿主还须核活动工程和会话CAS。
[[nodiscard]] Status VerifyTerrainResizeEditCapture(const TerrainResizeEditCapture& capture);

} // namespace War3Map::QtProject
