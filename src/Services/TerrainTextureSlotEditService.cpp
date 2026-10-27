#include "War3MapCore/Services/TerrainTextureSlotEditService.h"
#include <algorithm>
#include <new>

namespace War3Map::Services {
Result<std::shared_ptr<const MapData::TerrainGridSnapshot>> PreviewTerrainTextureSlotCommand(
    const MapData::MapDataEditSession& session, const TerrainTextureSlotCommand& command) {
    using Output = Result<std::shared_ptr<const MapData::TerrainGridSnapshot>>;
    const auto terrain = session.Terrain();
    if (!terrain) return Output::Failure("Terrain.TextureSlot", "TERRAIN_EDIT_UNAVAILABLE", "当前会话没有完整地形。");
    const auto normalized = NormalizeTerrainTextureSlotCommand(command, &terrain->header);
    if (!normalized.Succeeded()) return {{}, normalized.diagnostics};
    constexpr std::size_t maximumBytes = 512u * 1024u * 1024u;
    if (terrain->vertices.size() > std::min((maximumBytes - 16u * 1024u) / 256,
        std::size_t{64} * 1024u * 1024u / 40))
        return Output::Failure("Terrain.TextureSlot", "TERRAIN_PALETTE_BUDGET", "贴图候选超过512MiB预算。");
    try {
        auto after = std::make_shared<MapData::TerrainGridSnapshot>(*terrain);
        const auto replaced = MapData::ReplaceTerrainTextureSlot(*after, command.table, command.slot, command.rawcode);
        if (!replaced.Succeeded()) return {{}, replaced.diagnostics};
        return Output::Success(std::move(after));
    } catch (const std::bad_alloc&) {
        return Output::Failure("Terrain.TextureSlot", "TERRAIN_PALETTE_BUDGET", "无法分配贴图候选，当前会话未修改。");
    }
}

Result<PreparedTerrainEdit> PrepareTerrainTextureSlotEdit(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected,
    const MapData::TerrainPathingResources& resources, const TerrainTextureSlotCommand& command) {
    const auto binding = ValidateTerrainEditBinding(session, document, expected);
    if (!binding.Succeeded()) return {{}, binding.diagnostics};
    if (!session.Terrain()) return Result<PreparedTerrainEdit>::Failure("Terrain.TextureSlot", "TERRAIN_EDIT_UNAVAILABLE", "当前会话没有完整地形。");
    const auto valid = NormalizeTerrainTextureSlotCommand(command, &session.Terrain()->header);
    if (!valid.Succeeded()) return {{}, valid.diagnostics};
    auto plan = session.PrepareTerrainTextureSlot(command.table, command.slot, command.rawcode, &resources);
    if (!plan.Succeeded() || !plan.value) return {{}, plan.diagnostics};
    return Result<PreparedTerrainEdit>::Success(PreparedTerrainEdit(expected, std::move(*plan.value)));
}
} // namespace War3Map::Services
