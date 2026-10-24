#pragma once

#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

namespace War3Map::TerrainSaveTests {
// 在实际启动目录决定的网格上，验证非等价C类摆放及旧WPM来源保护。
void CheckRealCategoryDirectorySave(const MapArchiveSession& source,
    const Services::MapEditSaveRequest& request, const MapData::DoodadPlacements& placements,
    const MapData::TerrainGridSnapshot& before, const MapData::TerrainGridSnapshot& after,
    std::span<const std::byte> footprintBytes,
    const ObjectData::ObjectFieldSchema& schema, const ObjectData::StandardObjectCatalog& catalog);
}
