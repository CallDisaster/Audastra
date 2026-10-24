#pragma once

#include "TerrainCornerRampFixtures.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"

namespace War3Map::TerrainStraightRampFixtures {
using namespace TerrainCornerRampFixtures;

enum class SurfaceCase { BelowSurface, SurfaceEquality, BelowDeep, DeepEquality };

inline int SurfaceQuarters(SurfaceCase value) {
    switch (value) {
    case SurfaceCase::BelowSurface: return 383;
    case SurfaceCase::SurfaceEquality: return 384;
    case SurfaceCase::BelowDeep: return 639;
    case SurfaceCase::DeepEquality: return 640;
    }
    throw std::runtime_error("unknown straight ramp surface literal");
}

inline Bytes StraightPathingLiteral(SurfaceCase surface, bool blightAbove, bool created) {
    auto bytes = PathingLiteral(false);
    // 独立三点R条：一级垂直墙x16；两侧各一个单R角仍阻断，低端两肩新增阻断。
    for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 128; ++x) {
        const auto cx = x / 4, cy = y / 4;
        const bool blocked = cx == 16 || (created && cx == 17 && (cy == 15 || cy == 16));
        bytes[16 + y * 128 + x] = blocked ? std::byte{0xdb} : std::byte{0x51};
    }
    std::array<unsigned, 2> depth{};
    switch (surface) {
    case SurfaceCase::BelowSurface: depth = {1, 0}; break;
    case SurfaceCase::SurfaceEquality: case SurfaceCase::BelowDeep: depth = {2, 1}; break;
    case SurfaceCase::DeepEquality: depth = {2, 2}; break;
    }
    // 只有N=(17,16)含可覆水高度：最近顶点采样是66..69×62..65，不是整个地格。
    for (unsigned y = 62; y < 66; ++y) for (unsigned x = 66; x < 70; ++x) {
        auto& pixel = bytes[16 + y * 128 + x];
        if (depth[created] == 2) pixel = std::byte{0x1b};
        else if (depth[created] == 1)
            pixel = (std::to_integer<unsigned>(pixel) & 2u) ? std::byte{0xdb} : std::byte{0x59};
    }
    // 高侧(12,12)G=160，绝对水面160/160.25；严格>才清持久荒芜。
    for (unsigned y = 46; y < 50; ++y) for (unsigned x = 46; x < 50; ++x)
        bytes[16 + y * 128 + x] = created && blightAbove ? std::byte{0x59} : std::byte{0x79};
    return bytes;
}

struct StraightFixture : Fixture {
    SurfaceCase surface;
    bool blightAbove;
    explicit StraightFixture(SurfaceCase value = SurfaceCase::SurfaceEquality, bool clearBlight = false)
        : surface(value), blightAbove(clearBlight) {
        for (unsigned y = 0; y < 33; ++y) for (unsigned x = 0; x < 33; ++x) {
            auto& vertex = grid.vertices[y * 33 + x];
            vertex.cliffAndLayerRaw = x <= 16 ? 3 : 2;
        }
        const auto wet = [&](unsigned x, unsigned y, int quarters, bool blight = false) {
            auto& vertex = grid.vertices[y * 33 + x];
            vertex.textureAndFlagsRaw = std::uint8_t(0x40 | (blight ? 0x20 : 0));
            vertex.waterHeightAndFlagsRaw = std::uint16_t(8192 + quarters - 128);
        };
        wet(17, 16, SurfaceQuarters(surface));
        wet(12, 12, 640 + int(blightAbove), true);
        wet(15, 15, 0); // 同一实际分类halo中的水标志，水面低于高侧地面。
        RefreshTerrainDerived(grid, 0, 0, 33, 33);
        for (std::size_t i = 0; i < units.units.size(); ++i) {
            const float x = i < 2 ? 2176.f : 0.f, y = i == 0 ? 2048.f : i == 1 ? 2112.f : 0.f;
            units.units[i].position[0] = doodads.doodads[i].position[0] = x;
            units.units[i].position[1] = doodads.doodads[i].position[1] = y;
            units.units[i].owner = int(i + 1); units.units[i].hitPoints = int(87 + i);
            units.units[i].abilities = {{"A001", true, int(i + 1)}};
            doodads.doodads[i].itemSets.push_back({{{"I001", int(25 + i)}, {"I002", int(75 - i)}}});
        }
        members[0].data = Take(SerializeMapTerrain(grid));
        members[1].data = StraightPathingLiteral(surface, blightAbove, false);
        members[3].data = Take(units.Serialize()); members[4].data = Take(doodads.Serialize());
        for (auto& member : members) if (member.name == "TerrainArt/Water.slk")
            member.data = Slk({"waterID", "height", "impassable"}, {{"LSha", "0.25", "0"}});
        for (auto& member : members) if (member.name == "UI/MiscData.txt")
            member.data = TerrainCornerRampFixtures::Text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n[Water]\nDeepLevelPathing=64\n"
                "[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n");
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / "straight-source.w3x", members)).path)));
        edits = FreshSession();
    }
    Bytes TerrainLiteral(bool created) const {
        auto expected = grid;
        if (created) {
            for (unsigned x = 16; x <= 18; ++x) expected.vertices[16 * 33 + x].textureAndFlagsRaw |= 0x10;
            if (blightAbove) expected.vertices[12 * 33 + 12].textureAndFlagsRaw &= std::uint8_t(~0x20);
        }
        return Take(SerializeMapTerrain(expected));
    }
    UnitPlacements UnitsLiteral(bool created) const {
        auto expected = units;
        if (created) { expected.units[0].position[2] += 64; expected.units[1].position[2] += 32; }
        return expected;
    }
    DoodadPlacements DoodadsLiteral(bool created) const {
        auto expected = doodads;
        if (created) { expected.doodads[0].position[2] += 64; expected.doodads[1].position[2] += 32; }
        return expected;
    }
    void CheckState(const MapDataEditSession& session, bool created, bool followed = true) const {
        const auto snapshot = session.Snapshot();
        Require(snapshot.Units() && snapshot.Doodads()
            && Take(SerializeMapTerrain(*session.Terrain())) == TerrainLiteral(created)
            && Take(snapshot.Units()->Serialize()) == Take(UnitsLiteral(created && followed).Serialize())
            && Take(snapshot.Doodads()->Serialize()) == Take(DoodadsLiteral(created && followed).Serialize()),
            "complete straight W3E and both placement payloads match independent R64/R32 literals");
    }
};

inline TerrainBrush StraightBrush(bool create = true) {
    TerrainBrush brush; brush.kind = TerrainBrushKind::StraightRamp; brush.value = create; return brush;
}

} // namespace War3Map::TerrainStraightRampFixtures
