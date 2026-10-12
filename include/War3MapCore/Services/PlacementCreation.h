#pragma once

#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/MapData/TerrainPointQuery.h"
#include "War3MapCore/Services/ObjectCommands.h"

namespace War3Map::Services {

using NewPlacementRecord = std::variant<MapData::UnitPlacement, MapData::DoodadPlacement>;

// 编辑器既有的经典格式默认字段；界面预览与正式新建共用，不从模型外观猜地图记录。
// 调用方须先确认 type 是 Unit/Item/Doodad/Destructable，坐标为地图记录中的绝对高度。
[[nodiscard]] NewPlacementRecord MakeDefaultPlacement(ObjectData::ObjectType type, const std::string& rawcode,
    std::int32_t owner, const std::array<float, 3>& position);

struct PlacementCreateRequest {
    ObjectData::ObjectType type = ObjectData::ObjectType::Unit;
    std::string rawcode;
    MapData::TerrainStrokePoint point;
    float heightOffset = 0; // 相对当前原始地面的地图记录偏移；不是物编 moveHeight。
    float facingRadians = 4.71238898f;
    std::array<float, 3> scale{1, 1, 1};
    std::int32_t variation = 0;
    std::optional<std::int32_t> owner, heroLevel, hitPointsPercent, mana;
    // 本步接受独立内联掉落组；不猜或创建 W3I 的随机掉落表。
    std::vector<MapData::PlacementItemSet> itemSets;
};

// source 来自可信宿主当前物编目录；sample 来自同修订 InspectTerrainPoint，必须和请求点相同。
// drops 只在确有掉落时提供，借用真实 Item 目录，不能用调用方声称“类型存在”的布尔值替代。
[[nodiscard]] Result<NewPlacementRecord> PreparePlacementCreation(const PlacementCreateRequest& request,
    const ObjectCommandContext& source, const MapData::TerrainPointSample& sample,
    const ObjectCommandContext* drops = nullptr);

} // namespace War3Map::Services
