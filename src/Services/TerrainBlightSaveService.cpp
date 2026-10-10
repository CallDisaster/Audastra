#include "War3MapCore/Services/TerrainBlightSaveService.h"

#include "War3MapCore/Services/MapEditSaveService.h"

#include <utility>

namespace War3Map::Services {

Result<TerrainBlightSaveReceipt> SaveTerrainBlightCandidate(
    const TerrainBlightSaveRequest& request, const MapSaveService::Progress& progress) {
    MapEditSaveRequest edit;
    edit.sourceMap = request.sourceMap;
    edit.targetCandidate = request.targetCandidate;
    edit.toolRoot = request.toolRoot;
    edit.gameRoot = request.gameRoot;
    edit.expectedSourceSha256 = request.expectedSourceSha256;
    edit.expectedTerrainSha256 = request.expectedTerrainSha256;
    edit.effectiveLayers = request.effectiveLayers;

    MapEditTerrainStroke stroke;
    stroke.brush.kind = MapData::TerrainBrushKind::Blight;
    stroke.brush.shape = MapData::TerrainBrushShape::Circle;
    stroke.brush.radius = request.radius;
    stroke.brush.value = request.setBlight;
    stroke.pointSpace = MapEditPointSpace::TerrainVertex;
    stroke.points.push_back({static_cast<double>(request.vertexX), static_cast<double>(request.vertexY)});
    edit.terrainStrokes.push_back(std::move(stroke));

    auto saved = SaveMapEditCandidate(edit, progress);
    const bool succeeded = saved.Succeeded();
    Result<TerrainBlightSaveReceipt> result;
    result.diagnostics = std::move(saved.diagnostics);
    if (!succeeded || !saved.value) return result;
    auto& receipt = *saved.value;
    result.value.emplace(TerrainBlightSaveReceipt{
        std::move(receipt.candidate), std::move(receipt.sourceSha256),
        std::move(receipt.candidateSha256), std::move(receipt.originalTerrainSha256),
        std::move(receipt.savedTerrainSha256), std::move(receipt.savedPathingSha256),
        receipt.memberCount, receipt.scriptSeconds, std::move(receipt.effectiveLayers)});
    return result;
}

} // namespace War3Map::Services
