#pragma once

#include "TerrainStraightRampFixtures.h"

namespace War3Map::TerrainConnectedRampFixtures {
using namespace TerrainStraightRampFixtures;

inline Bytes ConnectedPathingLiteral(SurfaceCase surface, bool created) {
    auto expected = StraightPathingLiteral(surface, false, created);
    // 北侧已有三点R，创建南侧条带后中间地格含两个R，打开直墙与低侧通道。
    // 两端各留一个R的肩格仍阻断。已有水顶点的严格阈值沿用独立水深字面表。
    for (unsigned y = 60; y < 72; ++y) for (unsigned x = 64; x < 72; ++x) {
        const unsigned cx = x / 4, cy = y / 4;
        const bool blocked = created
            ? (cx == 16 && cy != 16) || (cx == 17 && (cy == 15 || cy == 17))
            : cx == 16 || (cx == 17 && (cy == 16 || cy == 17));
        auto pixel = blocked ? std::byte{0xdb} : std::byte{0x51};
        if (x >= 66 && x < 70 && y >= 62 && y < 66) {
            unsigned depth = 2;
            if (surface == SurfaceCase::BelowSurface) depth = created ? 0 : 1;
            else if (created && surface != SurfaceCase::DeepEquality) depth = 1;
            if (depth == 2) pixel = std::byte{0x1b};
            else if (depth == 1 && !blocked) pixel = std::byte{0x59};
        }
        expected[16 + y * 128 + x] = pixel;
    }
    return expected;
}

struct ConnectedFixture : StraightFixture {
    explicit ConnectedFixture(SurfaceCase surface = SurfaceCase::SurfaceEquality, bool created = false) : StraightFixture(surface) {
        for (unsigned x = 16; x <= 18; ++x) grid.vertices[17 * 33 + x].textureAndFlagsRaw |= 0x10;
        if (created) for (unsigned x = 16; x <= 18; ++x) grid.vertices[16 * 33 + x].textureAndFlagsRaw |= 0x10;
        RefreshTerrainDerived(grid, 0, 0, 33, 33);
        members[0].data = Take(SerializeMapTerrain(grid));
        members[1].data = ConnectedPathingLiteral(surface, created);
        members[3].data = Take(UnitsLiteral(created).Serialize());
        members[4].data = Take(DoodadsLiteral(created).Serialize());
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / "connected-source.w3x", members)).path)));
        edits = FreshSession();
    }
    Bytes TerrainLiteral(bool created) const {
        auto expected = grid;
        for (unsigned x = 16; x <= 18; ++x) {
            auto& flags = expected.vertices[16 * 33 + x].textureAndFlagsRaw;
            flags = std::uint8_t(created ? flags | 0x10 : flags & ~0x10);
        }
        return Take(SerializeMapTerrain(expected));
    }
    void CheckState(const MapDataEditSession& session, bool created, bool followed = true) const {
        const auto snapshot = session.Snapshot();
        Require(snapshot.Units() && snapshot.Doodads()
            && Take(SerializeMapTerrain(*session.Terrain())) == TerrainLiteral(created)
            && Take(snapshot.Units()->Serialize()) == Take(UnitsLiteral(created && followed).Serialize())
            && Take(snapshot.Doodads()->Serialize()) == Take(DoodadsLiteral(created && followed).Serialize()),
            "connected terrain keeps the original complete strip and follows only the new R64/R32 change");
    }
};
} // namespace War3Map::TerrainConnectedRampFixtures
