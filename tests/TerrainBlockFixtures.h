#pragma once

#include "TerrainCornerRampFixtures.h"
#include "War3MapCore/MapData/TerrainBlockEditing.h"

namespace War3Map::TerrainBlockFixtures {
using namespace TerrainCornerRampFixtures;

inline Bytes TerrainLiteral(bool pasted) {
    Bytes wire;
    const auto put = [&](std::uint32_t value, unsigned count = 4) {
        for (unsigned byte = 0; byte < count; ++byte) wire.push_back(std::byte((value >> (8 * byte)) & 255));
    };
    const auto text = [&](std::string_view value) { for (const auto c : value) wire.push_back(std::byte(c)); };
    text("W3E!"); put(11); put('L', 1); put(0); put(2); text("Tg01Tg02"); put(1); text("Cl01");
    put(33); put(33); put(0); put(0);
    for (unsigned y = 0; y < 33; ++y) for (unsigned x = 0; x < 33; ++x) {
        unsigned ground = 8192, water = 8192, texture = 0, variation = 3, cliff = 0xf2;
        for (const unsigned shift : {0u, 16u}) {
            if (shift && !pasted) continue;
            if (x == 5 + shift && y == 5 + shift) { ground = 8320; cliff = 3; variation = 0x45; }
            if (x == 6 + shift && y == 5 + shift) { water = 8448; texture = 0x40; }
            if (x == 5 + shift && y == 6 + shift) texture = 0x20;
            if (x == 6 + shift && y == 6 + shift) texture = 1;
        }
        put(ground, 2); put(water, 2); put(texture, 1); put(variation, 1); put(cliff, 1);
    }
    return wire;
}

inline Bytes PathingLiteral(bool pasted) {
    Bytes wire{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
    for (const auto value : {0u, 128u, 128u}) for (unsigned shift = 0; shift < 32; shift += 8)
        wire.push_back(std::byte((value >> shift) & 255));
    wire.resize(16 + 128 * 128, std::byte{0x51});
    // 独立完整字面布局：四个悬崖地格、最近顶点4x4水/荒芜/禁走地面。
    // 源和目标相距16地格；不调用产品通行生成器来制造expected。
    for (const unsigned shift : {0u, 64u}) {
        if (shift && !pasted) continue;
        for (unsigned y = 16; y < 24; ++y) for (unsigned x = 16; x < 24; ++x)
            wire[16 + (y + shift) * 128 + x + shift] = std::byte{0xdb};
        for (unsigned y = 18; y < 22; ++y) for (unsigned x = 22; x < 26; ++x)
            wire[16 + (y + shift) * 128 + x + shift] = std::byte{0x1b};
        for (unsigned y = 22; y < 26; ++y) for (unsigned x = 18; x < 22; ++x)
            wire[16 + (y + shift) * 128 + x + shift] = y < 24 ? std::byte{0xfb} : std::byte{0x71};
        for (unsigned y = 22; y < 26; ++y) for (unsigned x = 22; x < 26; ++x)
            wire[16 + (y + shift) * 128 + x + shift] = x < 24 && y < 24 ? std::byte{0xdb} : std::byte{0x5b};
    }
    return wire;
}

struct BlockFixture : Fixture {
    TerrainCellRect sourceCells{4, 4, 8, 8};
    std::array<std::uint32_t, 2> destination{20, 20};
    BlockFixture() {
        grid = *Take(ParseMapTerrain(TerrainBlockFixtures::TerrainLiteral(false))).Snapshot();
        for (std::size_t row = 0; row < units.units.size(); ++row) {
            units.units[row].position[0] = doodads.doodads[row].position[0] = row == 0 ? 2688.f : row == 1 ? 2816.f : 0.f;
            units.units[row].position[1] = doodads.doodads[row].position[1] = row < 2 ? 2688.f : 0.f;
            units.units[row].hitPoints = int(71 + row);
            units.units[row].abilities = {{"A001", true, int(row + 1)}};
            doodads.doodads[row].itemSets.push_back({{{"I001", 27}, {"I002", 73}}});
        }
        members[0].data = TerrainBlockFixtures::TerrainLiteral(false); members[1].data = TerrainBlockFixtures::PathingLiteral(false);
        members[3].data = Take(units.Serialize()); members[4].data = Take(doodads.Serialize());
        for (auto& member : members) {
            if (member.name == "TerrainArt/Terrain.slk") member.data = Slk({"tileID", "walkable", "flyable", "buildable"},
                {{"Tg01", "1", "1", "1"}, {"Tg02", "0", "1", "0"}});
            if (member.name == "UI/MiscData.txt") member.data = Text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n"
                "[Water]\nDeepLevelPathing=64\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n");
        }
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / "block-source.w3x", members)).path)));
        edits = FreshSession();
    }
    TerrainBlockPasteIntent Intent() const {
        return {1, sourceCells, destination, Take(CaptureTerrainBlockFingerprint(*edits->Terrain(), sourceCells))};
    }
    UnitPlacements UnitsLiteral(bool pasted, bool follow = true) const {
        auto expected = units; if (pasted && follow) expected.units[0].position[2] += 160; return expected;
    }
    DoodadPlacements DoodadsLiteral(bool pasted, bool follow = true) const {
        auto expected = doodads; if (pasted && follow) expected.doodads[0].position[2] += 160; return expected;
    }
    void CheckState(const MapDataEditSession& session, bool pasted, bool follow = true) const {
        const auto snapshot = session.Snapshot();
        Require(Take(SerializeMapTerrain(*session.Terrain())) == TerrainBlockFixtures::TerrainLiteral(pasted)
            && snapshot.Units() && snapshot.Doodads()
            && Take(snapshot.Units()->Serialize()) == Take(UnitsLiteral(pasted, follow).Serialize())
            && Take(snapshot.Doodads()->Serialize()) == Take(DoodadsLiteral(pasted, follow).Serialize()),
            "whole block W3E and both full placement streams match independent literals");
    }
};

} // namespace War3Map::TerrainBlockFixtures
