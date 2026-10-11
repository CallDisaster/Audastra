#pragma once

#include "services/ObjectCommandsFixture.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

namespace War3Map::Tests {

inline std::vector<std::byte> PlacementTerrainBytes() {
    MapData::TerrainGridSnapshot terrain;
    terrain.header.mainTileset = 'L';
    terrain.header.width = terrain.header.height = 17;
    terrain.header.offset = {-1024, -1024};
    terrain.header.groundTilesets = {"Ldrt", "Lgrs"};
    terrain.header.cliffTilesets = {"CLdi"};
    terrain.vertices.resize(17 * 17);
    for (auto& vertex : terrain.vertices) {
        vertex.groundHeightRaw = 8192;
        vertex.waterHeightAndFlagsRaw = 7936;
        vertex.cliffAndLayerRaw = 2;
    }
    MapData::RefreshTerrainDerived(terrain, 0, 0, 17, 17);
    const auto bytes = MapData::SerializeMapTerrain(terrain);
    if (!bytes.Succeeded()) throw std::runtime_error("original placement terrain");
    return *bytes.value;
}

inline void AddPlacementCustomObjects(ObjectCommandsFixture& fixture) {
    using ObjectData::ObjectType;
    for (const auto& kind : ObjectCommandsFixture::kinds) {
        if (kind.type != ObjectType::Unit && kind.type != ObjectType::Item
            && kind.type != ObjectType::Doodad && kind.type != ObjectType::Destructable) continue;
        const auto created = Services::PlanObjectCreation(fixture.Context(kind.type), kind.base, std::string(kind.custom), {});
        if (!created.Succeeded()) throw std::runtime_error("original custom placement type");
        *fixture.documents[static_cast<std::size_t>(kind.type)] = created.value->after;
    }
}

} // namespace War3Map::Tests
