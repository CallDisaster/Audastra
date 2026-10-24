#pragma once

#include "TerrainCornerRampFixtures.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"

namespace War3Map::TerrainWetCornerRampFixtures {
using namespace TerrainCornerRampFixtures;

enum class ThresholdCase { BelowSurface, SurfaceEquality, BelowDeep, DeepEquality };

struct WetCase {
    ThresholdCase threshold = ThresholdCase::SurfaceEquality;
    bool blightAboveSurface = false;
};

// 先手推R64/S32的四组绝对水面（四分之一单位）；资源另加32，不能漏掉实际Water.height。
inline std::array<int, 2> SurfaceQuarters(ThresholdCase value) {
    switch (value) {
    case ThresholdCase::BelowSurface: return {383, 255};
    case ThresholdCase::SurfaceEquality: return {384, 256};
    case ThresholdCase::BelowDeep: return {639, 511};
    case ThresholdCase::DeepEquality: return {640, 512};
    }
    throw std::runtime_error("unknown literal threshold case");
}

inline Bytes WetPathingLiteral(WetCase value, bool created) {
    auto bytes = PathingLiteral(created);
    // literal depth pairs为before/after；不调用水分类器、坡道查询或产品生成器。
    std::array<unsigned, 2> r{}, s{};
    switch (value.threshold) {
    case ThresholdCase::BelowSurface: r = {1, 0}; s = {1, 0}; break;
    case ThresholdCase::SurfaceEquality: r = {2, 1}; s = {1, 1}; break;
    case ThresholdCase::BelowDeep: r = {2, 1}; s = {2, 1}; break;
    case ThresholdCase::DeepEquality: r = {2, 2}; s = {2, 2}; break;
    }
    const auto patch = [&](unsigned x0, unsigned y0, unsigned depth) {
        for (unsigned y = y0; y < y0 + 4; ++y) for (unsigned x = x0; x < x0 + 4; ++x) {
            auto& pixel = bytes[16 + y * 128 + x];
            const bool cliffBlocked = (std::to_integer<unsigned>(pixel) & 0x02) != 0;
            // 只有单个湿顶点，不会获得四角strict覆盖；0x11原样保留。
            if (depth == 2) pixel = std::byte{0x1b};
            else if (depth == 1) pixel = cliffBlocked ? std::byte{0xdb} : std::byte{0x59};
        }
    };
    // 最近顶点N的4x4覆盖与地格Q不同：17*4-2=66，16*4-2=62。
    patch(66, 62, r[created]); patch(66, 66, s[created]);
    // (12,12)无坡道邻格：水面==G仍浅水但不清Blight；增加一个raw才清。
    for (unsigned y = 46; y < 50; ++y) for (unsigned x = 46; x < 50; ++x)
        bytes[16 + y * 128 + x] = created && value.blightAboveSurface ? std::byte{0x59} : std::byte{0x79};
    return bytes;
}

struct WetFixture : Fixture {
    WetCase wet;
    explicit WetFixture(WetCase value = {}) : wet(value) {
        const auto surfaces = SurfaceQuarters(wet.threshold);
        const auto setWet = [&](unsigned x, unsigned y, int surfaceQuarters, bool blight = false) {
            auto& vertex = grid.vertices[y * 33 + x];
            vertex.textureAndFlagsRaw = std::uint8_t(0x40 | (blight ? 0x20 : 0));
            vertex.waterHeightAndFlagsRaw = std::uint16_t(8192 + surfaceQuarters - 128);
        };
        setWet(17, 16, surfaces[0]); setWet(17, 17, surfaces[1]);
        setWet(12, 12, 640 + int(wet.blightAboveSurface), true);
        // 高端(15,15)在S中心的第二圈内；有水标志但水面低于地面，不得全域拒水。
        setWet(15, 15, 0);
        RefreshTerrainDerived(grid, 0, 0, 33, 33);
        for (std::size_t i = 0; i < units.units.size(); ++i) {
            auto& unit = units.units[i]; unit.owner = int(i + 1); unit.rotation = float(i + 1) / 4;
            unit.hitPoints = int(87 + i); unit.mana = int(23 + i); unit.gold = int(103 + i);
            unit.heroLevel = int(i + 2); unit.heroStrength = int(i + 8);
            unit.heroAgility = int(i + 9); unit.heroIntelligence = int(i + 10);
            unit.itemSets.push_back({{{"I001", 25}, {"I002", 75}}});
            auto& doodad = doodads.doodads[i]; doodad.rotation = float(i + 1) / 8;
            doodad.lifePercent = std::uint8_t(61 + i); doodad.itemSets.push_back({{{"I003", 100}}});
        }
        members[0].data = Take(SerializeMapTerrain(grid)); members[1].data = WetPathingLiteral(wet, false);
        members[3].data = Take(units.Serialize()); members[4].data = Take(doodads.Serialize());
        for (auto& member : members) {
            if (member.name == "TerrainArt/Water.slk")
                member.data = Slk({"waterID", "height", "impassable"}, {{"LSha", "0.25", "0"}});
            if (member.name == "UI/MiscData.txt")
                member.data = Text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n[Water]\nDeepLevelPathing=64\n"
                    "[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n");
        }
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / "wet-source.w3x", members)).path)));
        edits = FreshSession();
    }
    Bytes TerrainLiteral(bool created) const {
        auto expected = grid;
        if (created) {
            for (unsigned y = 16; y <= 18; ++y) for (unsigned x = 16; x <= 18; ++x)
                expected.vertices[y * 33 + x].textureAndFlagsRaw |= 0x10;
            if (wet.blightAboveSurface) expected.vertices[12 * 33 + 12].textureAndFlagsRaw &= std::uint8_t(~0x20);
        }
        return Take(SerializeMapTerrain(expected));
    }
    UnitPlacements UnitsLiteral(bool created) const {
        auto expected = units;
        if (created) { expected.units[0].position[2] += 32; expected.units[1].position[2] += 64; }
        return expected;
    }
    DoodadPlacements DoodadsLiteral(bool created) const {
        auto expected = doodads;
        if (created) { expected.doodads[0].position[2] += 32; expected.doodads[1].position[2] += 64; }
        return expected;
    }
    void CheckState(const MapDataEditSession& session, bool created) const {
        const auto snapshot = session.Snapshot();
        Require(snapshot.Units() && snapshot.Doodads() && snapshot.Units()->units.size() == 3
            && snapshot.Doodads()->doodads.size() == 3, "wet fixture always retains both nonempty complete placement groups");
        Require(Take(SerializeMapTerrain(*session.Terrain())) == TerrainLiteral(created)
            && Take(snapshot.Units()->Serialize()) == Take(UnitsLiteral(created).Serialize())
            && Take(snapshot.Doodads()->Serialize()) == Take(DoodadsLiteral(created).Serialize()),
            "complete wet W3E and both placement streams match independent literals including nested payloads");
    }
};

inline TerrainBrush CornerBrush(bool create = true) {
    TerrainBrush brush; brush.kind = TerrainBrushKind::CornerRamp; brush.value = create; return brush;
}

} // namespace War3Map::TerrainWetCornerRampFixtures
