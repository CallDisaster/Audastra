#pragma once

#include "War3MapCore/Services/MapEditSaveService.h"

#include <span>
#include <string_view>

namespace War3Map::Services::MapEditValidation {

bool ValidSha(std::string_view value);
Status ValidateFieldOperations(std::span<const MapEditFieldOperation> operations);
Status ValidatePlacementOperation(const MapEditPlacementOperation& operation);
Status ValidateStrokes(std::span<const MapEditTerrainStroke> strokes);
Status AdvanceTerrainPointFrame(MapData::TerrainHeader& terrain, const MapData::TerrainResizeIntent& resize);
Result<std::vector<MapData::TerrainStrokePoint>> WorldPoints(
    const MapEditTerrainStroke& stroke, const MapData::TerrainHeader& terrain);

} // namespace War3Map::Services::MapEditValidation
