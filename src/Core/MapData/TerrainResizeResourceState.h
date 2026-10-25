#pragma once

#include "War3MapCore/MapData/TerrainResizeResources.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapDocument.h"

namespace War3Map::MapData {
namespace TerrainResizeResourceDetail {
inline constexpr std::array<MapDataKind, 5> kSpatialKinds{MapDataKind::Settings, MapDataKind::Units,
    MapDataKind::Doodads, MapDataKind::Regions, MapDataKind::Cameras};
inline constexpr std::array<std::string_view, 8> kSourceNames{"war3map.w3e", "war3map.wpm", "war3map.w3i", "war3mapUnits.doo",
    "war3map.doo", "war3map.w3r", "war3map.w3c", "war3map.shd"};
} // namespace TerrainResizeResourceDetail

// Services从实际来源建立并发布const所有权；Core仅核验已冻结的领域材料。
// startup保留来源租约，但其捕获与重读只由Services执行。
struct TerrainResizeResources::State {
    TerrainResizeDimensionRules dimensions;
    std::array<float, 4> margins{};
    ArchiveExactMember shadow;
    std::array<ArchiveExactMemberIdentity, TerrainResizeResourceDetail::kSourceNames.size()> physical;
    std::array<ArchiveExactMemberIdentity, TerrainResizeResourceDetail::kSpatialKinds.size()> current;
    MapDocument spatial;
    std::shared_ptr<const TerrainGridSnapshot> terrain;
    std::optional<Game::NativeDoodadCategoryDirectory> startup;
    std::optional<TerrainPathingResources> native;
    std::vector<TerrainWaterSpecialDoodadShape> special;
    std::uint64_t session = 0, revision = 0, sourceRevision = 0;
    std::size_t captureBytes = 0, ownedBytes = 0, work = 0;
    std::string fingerprint;
    explicit State(MapDocument document) : spatial(std::move(document)) {}
};

} // namespace War3Map::MapData
