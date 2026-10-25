#include "TerrainResizeEditPreparation.h"

#include <exception>

namespace War3Map::QtProject {
namespace {
Status Failure(const char* code, const char* message) {
    return Status::Failure("Terrain.Resize.Qt", code, message);
}
}

Status VerifyTerrainResizeEditCapture(const TerrainResizeEditCapture& capture) {
    if (!capture.request || !capture.frozen)
        return Failure("TERRAIN_RESIZE_CAPTURE_INVALID", "地图尺寸缺少完整冻结来源。");
    const auto& request = *capture.request;
    const auto path = VerifyTerrainWaterEditRequestSourcePath(request);
    if (!path.Succeeded()) return path;
    if (request.archive) return capture.resources.VerifySources(*request.archive, *capture.frozen, capture.limits);
    if (request.directory) return capture.resources.VerifySources(*request.directory, *capture.frozen, capture.limits);
    return Failure("TERRAIN_RESIZE_SOURCE_REQUIRED", "地图尺寸缺少真实地图来源。");
}

Result<TerrainResizeEditCapture> PrepareTerrainResizeEditCapture(
    std::shared_ptr<const TerrainWaterEditRequest> request,
    std::shared_ptr<const MapData::MapDataEditSession> frozen,
    const Services::MapDataQueryBinding& document,
    const std::optional<MapData::TerrainResizeIntent>& intent,
    const std::string& expectedResources, const MapData::TerrainResizeLimits& limits) {
    using Output = Result<TerrainResizeEditCapture>;
    if (!request || !frozen || !request->terrain || request->sessionIdentity != frozen->SessionIdentity()
        || request->revision != frozen->Revision() || request->terrainSourceRevision != frozen->TerrainSourceRevision())
        return Output::Failure("Terrain.Resize.Qt", "TERRAIN_RESIZE_CAPTURE_INVALID", "地图尺寸的来源与冻结会话不一致。");
    try {
        const auto path = VerifyTerrainWaterEditRequestSourcePath(*request);
        if (!path.Succeeded()) return {{}, path.diagnostics};
        if (!request->archive && !request->directory)
            return Output::Failure("Terrain.Resize.Qt", "TERRAIN_RESIZE_SOURCE_REQUIRED", "地图尺寸缺少真实地图来源。");
        auto resources = request->archive
            ? Services::TerrainResizeEditService::Capture(*request->archive, *frozen, request->gameRoot, limits)
            : Services::TerrainResizeEditService::Capture(*request->directory, *frozen, request->gameRoot, limits);
        if (!resources.Succeeded() || !resources.value) return {{}, std::move(resources.diagnostics)};
        if (!expectedResources.empty() && resources.value->DependencyFingerprint() != expectedResources)
            return Output::Failure("Terrain.Resize.Qt", "TERRAIN_RESIZE_RESOURCES_CHANGED", "地图尺寸规则或空间来源已变化，请重新读取当前尺寸。");
        std::optional<Services::PreparedTerrainResizeEdit> prepared;
        if (intent) {
            const Services::TerrainEditBinding binding{document, request->sessionIdentity, request->revision, request->terrainSourceRevision};
            auto candidate = Services::PrepareTerrainResizeEdit(*frozen, document, binding, *resources.value, *intent, limits);
            if (!candidate.Succeeded() || !candidate.value) return {{}, std::move(candidate.diagnostics)};
            prepared.emplace(std::move(*candidate.value));
        }
        TerrainResizeEditCapture capture{std::move(request), std::move(frozen), std::move(*resources.value), std::move(prepared), limits};
        const auto checked = VerifyTerrainResizeEditCapture(capture);
        if (!checked.Succeeded()) return {{}, checked.diagnostics};
        return Output::Success(std::move(capture));
    } catch (const std::exception&) {
        return Output::Failure("Terrain.Resize.Qt", "TERRAIN_RESIZE_CAPTURE_FAILED", "地图尺寸来源读取或候选准备失败，本次没有修改地图。");
    }
}

} // namespace War3Map::QtProject
