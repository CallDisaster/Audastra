#include "TerrainTextureSlotEditPreparation.h"

namespace War3Map::QtProject {
Result<TerrainTextureSlotEditCapture> PrepareTerrainTextureSlotEditCapture(
    std::shared_ptr<const TerrainWaterEditRequest> request,
    std::shared_ptr<const MapData::MapDataEditSession> frozen,
    const Services::TerrainEditBinding& binding, const Services::TerrainTextureSlotCommand& command) {
    using Output = Result<TerrainTextureSlotEditCapture>;
    if (!request || !frozen || request->terrain != frozen->Terrain())
        return Output::Failure("Terrain.TextureSlot", "TERRAIN_EDIT_STALE", "冻结会话与实际资源请求不一致。");
    const auto checked = Services::ValidateTerrainEditBinding(*frozen, binding.document, binding);
    if (!checked.Succeeded()) return {{}, checked.diagnostics};
    const auto preview = Services::PreviewTerrainTextureSlotCommand(*frozen, command);
    if (!preview.Succeeded()) return {{}, preview.diagnostics};
    auto resources = LoadTerrainWaterEditResources(std::move(request), *preview.value);
    if (!resources.Succeeded()) return {{}, std::move(resources.diagnostics)};
    auto plan = Services::PrepareTerrainTextureSlotEdit(*frozen, binding.document, binding, resources.value->resources, command);
    if (!plan.Succeeded()) return {{}, std::move(plan.diagnostics)};
    return Output::Success({std::move(*resources.value), std::move(*plan.value)});
}
} // namespace War3Map::QtProject
