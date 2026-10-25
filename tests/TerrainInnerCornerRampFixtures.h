#pragma once

#include "TerrainCornerRampFixtures.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"

#include <limits>
#include <string_view>

namespace War3Map::TerrainInnerCornerRampFixtures {
using namespace MapData;
using Bytes = std::vector<std::byte>;
inline constexpr std::size_t kBudget = TerrainCornerRampFixtures::kBudget;

inline void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "内角坡道原创样例";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
inline TerrainGridSnapshot Grid(unsigned side = 17, int dx = 1, int dy = 1) {
    TerrainGridSnapshot grid; grid.header.version = 11; grid.header.mainTileset = 'L';
    grid.header.width = grid.header.height = side;
    grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"Cl01"}; grid.vertices.resize(std::size_t(side) * side);
    const int center = int(side / 2);
    for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x) {
        auto& vertex = grid.vertices[std::size_t(y) * side + x];
        vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.variationRaw = 3;
        vertex.cliffAndLayerRaw = (int(x) - center) * dx <= 0 && (int(y) - center) * dy <= 0 ? 2 : 3;
    }
    RefreshTerrainDerived(grid, 0, 0, side, side); return grid;
}
inline TerrainStrokePoint Point(unsigned side = 17, int dx = 1, int dy = 1) {
    return {double(side / 2) * 128 + 32 * dx, double(side / 2) * 128 + 32 * dy};
}
inline TerrainWaterLoadedRules Rules() { return {90, -256, 1536, 0, 128, 14, true, {{"Tg01", true}}, {"Cl01"}}; }
inline TerrainBrush Brush(bool create = true) { TerrainBrush brush; brush.kind = TerrainBrushKind::CornerRamp; brush.value = create; return brush; }

// 独立写出完整45字节头与七字节记录；expected不经过产品W3E writer或坡道生成器。
inline Bytes TerrainLiteral(bool created, unsigned side = 17, int dx = 1, int dy = 1) {
    Bytes bytes;
    const auto text = [&](std::string_view value) { for (const char c : value) bytes.push_back(std::byte(c)); };
    const auto number = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(std::byte((value >> shift) & 255));
    };
    text("W3E!"); number(11); text("L"); number(0); number(1); text("Tg01"); number(1); text("Cl01");
    number(side); number(side); number(0); number(0);
    const int center = int(side / 2);
    for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x) {
        const bool low = (int(x) - center) * dx <= 0 && (int(y) - center) * dy <= 0;
        const bool ramp = created && int(x) >= center - 1 && int(x) <= center + 1
            && int(y) >= center - 1 && int(y) <= center + 1;
        bytes.insert(bytes.end(), {std::byte{0}, std::byte{0x20}, std::byte{0}, std::byte{0x1f},
            ramp ? std::byte{0x10} : std::byte{0}, std::byte{3}, low ? std::byte{2} : std::byte{3}});
    }
    return bytes;
}

// A=40、B=CA。内角S权2，(8,8)为96高；R权1。创建后打开三个原悬崖格，
// (7,6)/(6,7)各只接触一个R，成为两肩阻挡。每个地格展开4x4，镜像用cell索引。
inline Bytes PathingLiteral(bool created, unsigned side = 17, int dx = 1, int dy = 1, bool protectedBits = false) {
    const unsigned cells = side - 1, center = side / 2, width = cells * 4;
    Bytes bytes(std::size_t(width) * width);
    for (unsigned y = 0; y < width; ++y) for (unsigned x = 0; x < width; ++x) {
        const auto cx = dx > 0 ? x / 4 : cells - 1 - x / 4;
        const auto cy = dy > 0 ? y / 4 : cells - 1 - y / 4;
        bool blocked = (cx == center && cy <= center) || (cy == center && cx <= center);
        if (created && ((cx == center && cy == center - 1) || (cx == center - 1 && cy == center)
            || (cx == center && cy == center))) blocked = false;
        if (created && ((cx == center - 1 && cy == center - 2) || (cx == center - 2 && cy == center - 1))) blocked = true;
        bytes[std::size_t(y) * width + x] = std::byte((blocked ? 0xca : 0x40) | (protectedBits ? 0x11 : 0));
    }
    return bytes;
}

// 原创完整归档与真实规则owner。此fixture也供Qt工程冷恢复测试复用。
struct Fixture : TerrainCornerRampFixtures::Fixture {
    Fixture() {
        grid = TerrainInnerCornerRampFixtures::Grid(33); point = TerrainInnerCornerRampFixtures::Point(33);
        members[0].data = TerrainLiteral(false); members[1].data = PathingLiteral(false);
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / "inner-source.w3x", members)).path)));
        edits = FreshSession();
    }
    Bytes TerrainLiteral(bool created) const { return TerrainInnerCornerRampFixtures::TerrainLiteral(created, 33); }
    static Bytes PathingLiteral(bool created) {
        Bytes bytes{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
        for (const auto value : {0u, 128u, 128u}) for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(std::byte((value >> shift) & 255));
        const auto pixels = TerrainInnerCornerRampFixtures::PathingLiteral(created, 33, 1, 1, true);
        bytes.insert(bytes.end(), pixels.begin(), pixels.end()); return bytes;
    }
    void CheckState(const MapDataEditSession& session, bool created) const {
        const auto snapshot = session.Snapshot();
        Require(snapshot.Units() && snapshot.Doodads()
            && Take(SerializeMapTerrain(*session.Terrain())) == TerrainLiteral(created)
            && Take(snapshot.Units()->Serialize()) == Take(units.Serialize())
            && Take(snapshot.Doodads()->Serialize()) == Take(doodads.Serialize()),
            "inner complete W3E and both authored placement streams equal independent literals");
    }
};

} // namespace War3Map::TerrainInnerCornerRampFixtures
