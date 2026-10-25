#pragma once

#include "TerrainInnerCornerRampFixtures.h"

namespace War3Map::TerrainWetInnerCornerRampFixtures {
namespace C = TerrainInnerCornerRampFixtures;
namespace F = TerrainCornerRampFixtures;
using namespace MapData;
using F::Bytes;
using F::Require;
using F::Take;
inline constexpr auto kBudget = F::kBudget;

enum class Threshold { BelowSurface, SurfaceEquality, AboveSurface, BelowDeep, DeepEquality, AboveDeep };
struct Options { Threshold threshold = Threshold::SurfaceEquality; bool mixedWater = false; };

// 基底raw8193是0.25；Water.height=0.25贡献32，DeepLevelPathing=64。
// S96处的水面依次是96/96.25/96.5及160/160.25/160.5，全部用整数四分之一单位核对。
inline std::uint16_t WaterRaw(const Options& options) {
    switch (options.threshold) {
    case Threshold::BelowSurface: return 8448;
    case Threshold::SurfaceEquality: return 8449;
    case Threshold::AboveSurface: return 8450;
    case Threshold::BelowDeep: return 8704;
    case Threshold::DeepEquality: return 8705;
    case Threshold::AboveDeep: return 8706;
    }
    throw std::runtime_error("unknown wet inner fixture threshold");
}
inline bool HasWater(const Options& options, int x, int y) {
    return !options.mixedWater || (x == 0 && y == 0) || (x == 0 && y == -1) || (x == -4 && y == -4);
}
inline TerrainWaterLoadedRules Rules() { return {90, -256, 1536, 32, 64, 14, true, {{"Tg01", true}}, {"Cl01"}}; }
inline TerrainGridSnapshot Grid(const Options& options = {}, unsigned side = 17, int dx = 1, int dy = 1) {
    auto grid = C::Grid(side, dx, dy); const int center = int(side / 2);
    for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x) {
        auto& vertex = grid.vertices[std::size_t(y) * side + x];
        vertex.groundHeightRaw = 8193; vertex.waterHeightAndFlagsRaw = WaterRaw(options);
        if (HasWater(options, (int(x) - center) * dx, (int(y) - center) * dy)) vertex.textureAndFlagsRaw |= 0x40;
    }
    RefreshTerrainDerived(grid, 0, 0, side, side); return grid;
}
inline Bytes TerrainLiteral(bool created, const Options& options = {}, unsigned side = 17, int dx = 1, int dy = 1) {
    auto bytes = C::TerrainLiteral(created, side, dx, dy); const auto water = WaterRaw(options);
    const int center = int(side / 2);
    for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x) {
        const auto at = 45 + (std::size_t(y) * side + x) * 7;
        bytes[at] = std::byte{1}; bytes[at + 1] = std::byte{0x20};
        bytes[at + 2] = std::byte(water & 255); bytes[at + 3] = std::byte(water >> 8);
        if (HasWater(options, (int(x) - center) * dx, (int(y) - center) * dy)) bytes[at + 4] |= std::byte{0x40};
    }
    return bytes;
}

// 固定纯内角的独立高度表：两R各64，中心S96，五H不加128（保存水查询flags不含H偏移）。
// 不调用产品分类/有效高度/水查询函数，也不按生成器输出填expected。
inline int GroundQuarter(bool created, int x, int y) {
    int result = 1 + (x <= 0 && y <= 0 ? 0 : 512);
    if (created && ((x == 0 && y == -1) || (x == -1 && y == 0))) result += 256;
    if (created && x == 0 && y == 0) result += 384;
    return result;
}
inline Bytes PathingPixels(bool created, const Options& options = {}, unsigned side = 17,
    int dx = 1, int dy = 1, bool protectedBits = false) {
    const unsigned cells = side - 1, width = cells * 4; const int center = int(side / 2);
    Bytes pixels(std::size_t(width) * width); const int surface = int(WaterRaw(options)) - 8192 + 128;
    // depth×cliff×strict四角；表中的常量逐项从原版六mode推导，nearest取2x2象限。
    constexpr unsigned masks[3][2][2] = {{{0x40,0x00},{0xca,0x0a}},{{0x48,0x08},{0xca,0x0a}},{{0x0a,0x0a},{0x0a,0x0a}}};
    for (unsigned py = 0; py < width; ++py) for (unsigned px = 0; px < width; ++px) {
        const unsigned cx = dx > 0 ? px / 4 : cells - 1 - px / 4;
        const unsigned cy = dy > 0 ? py / 4 : cells - 1 - py / 4;
        bool blocked = (cx == unsigned(center) && cy <= unsigned(center)) || (cy == unsigned(center) && cx <= unsigned(center));
        if (created && ((cx == unsigned(center) && cy == unsigned(center - 1))
            || (cx == unsigned(center - 1) && cy == unsigned(center)) || (cx == unsigned(center) && cy == unsigned(center)))) blocked = false;
        if (created && ((cx == unsigned(center - 1) && cy == unsigned(center - 2))
            || (cx == unsigned(center - 2) && cy == unsigned(center - 1)))) blocked = true;
        const int nearestX = (int((px + 2) / 4) - center) * dx, nearestY = (int((py + 2) / 4) - center) * dy;
        const int ground = GroundQuarter(created, nearestX, nearestY);
        const unsigned depth = !HasWater(options, nearestX, nearestY) || surface < ground ? 0 : surface - ground >= 256 ? 2 : 1;
        bool strict = true;
        for (int y = 0; y <= 1; ++y) for (int x = 0; x <= 1; ++x) {
            const int rx = (int(px / 4) + x - center) * dx, ry = (int(py / 4) + y - center) * dy;
            strict = strict && HasWater(options, rx, ry) && surface > GroundQuarter(created, rx, ry);
        }
        pixels[std::size_t(py) * width + px] = std::byte(masks[depth][blocked ? 1 : 0][strict ? 1 : 0] | (protectedBits ? 0x11 : 0));
    }
    return pixels;
}
inline Bytes WpmLiteral(bool created, const Options& options = {}, unsigned side = 33, int dx = 1, int dy = 1) {
    Bytes bytes{std::byte{'M'},std::byte{'P'},std::byte{'3'},std::byte{'W'}};
    for (const auto value : {0u, (side - 1) * 4, (side - 1) * 4}) for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(std::byte((value >> shift) & 255));
    auto pixels = PathingPixels(created, options, side, dx, dy, true);
    bytes.insert(bytes.end(), pixels.begin(), pixels.end()); return bytes;
}

struct Fixture : C::Fixture {
    Options options;
    explicit Fixture(Options settings = {}) : options(settings) {
        grid = TerrainWetInnerCornerRampFixtures::Grid(options, 33);
        for (auto* position : {&units.units[0].position, &doodads.doodads[0].position}) { (*position)[0] = 2048; (*position)[1] = 2048; }
        for (auto* position : {&units.units[1].position, &doodads.doodads[1].position}) { (*position)[0] = 2048; (*position)[1] = 1920; }
        members[0].data = TerrainLiteral(false); members[1].data = PathingLiteral(false);
        for (auto& member : members) {
            if (member.name == "war3mapUnits.doo") member.data = Take(units.Serialize());
            if (member.name == "war3map.doo") member.data = Take(doodads.Serialize());
            if (member.name == "TerrainArt/Water.slk") member.data = F::Slk({"waterID","height","impassable"}, {{"LSha","0.25","0"}});
            if (member.name == "UI/MiscData.txt") member.data = F::Text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n[Water]\nDeepLevelPathing=64\n"
                "[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n");
        }
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / "wet-inner-source.w3x", members)).path)));
        edits = FreshSession();
    }
    Bytes TerrainLiteral(bool created) const { return TerrainWetInnerCornerRampFixtures::TerrainLiteral(created, options, 33); }
    Bytes PathingLiteral(bool created) const { return WpmLiteral(created, options); }
    UnitPlacements UnitsLiteral(bool followed) const {
        auto value = units; if (followed) { value.units[0].position[2] += 96; value.units[1].position[2] += 64; } return value;
    }
    DoodadPlacements DoodadsLiteral(bool followed) const {
        auto value = doodads; if (followed) { value.doodads[0].position[2] += 96; value.doodads[1].position[2] += 64; } return value;
    }
    void CheckState(const MapDataEditSession& session, bool created, bool follow = false) const {
        const auto snapshot = session.Snapshot();
        Require(snapshot.Units() && snapshot.Doodads() && Take(SerializeMapTerrain(*session.Terrain())) == TerrainLiteral(created)
            && Take(snapshot.Units()->Serialize()) == Take(UnitsLiteral(created && follow).Serialize())
            && Take(snapshot.Doodads()->Serialize()) == Take(DoodadsLiteral(created && follow).Serialize()),
            "wet inner complete W3E and both authored placement streams equal independent quarter-unit/S96/R64 literals");
    }
};
} // namespace War3Map::TerrainWetInnerCornerRampFixtures
