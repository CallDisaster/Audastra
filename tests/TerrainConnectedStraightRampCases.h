#pragma once

#include "War3MapCore/MapData/TerrainRampEditing.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <utility>
#include <vector>

namespace War3Map::TerrainConnectedStraightRampCases {
using namespace MapData;
using Bytes = std::vector<std::byte>;

inline void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "Connected直坡原创样例失败";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ":" + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
inline std::array<unsigned, 2> Vertex(int normal, int tangent, int dx, int dy) {
    return {unsigned(16 + normal * dx - tangent * dy), unsigned(16 + normal * dy + tangent * dx)};
}
inline TerrainVertex& At(TerrainGridSnapshot& grid, int normal, int tangent, int dx, int dy) {
    const auto point = Vertex(normal, tangent, dx, dy); return grid.vertices[point[1] * 33 + point[0]];
}
inline TerrainWaterLoadedRules Rules() { return {90, -256, 1536, 0, 64, 14, true, {{"Tg01", true}}, {"Cl01"}}; }
struct Strips { bool below = false, target = false, above = false; };
inline bool HasStrip(Strips strips, unsigned y) {
    return (y == 15 && strips.below) || (y == 16 && strips.target) || (y == 17 && strips.above);
}
inline TerrainGridSnapshot Grid(int dx, int dy, Strips strips, int waterQuarters = -1, int blightQuarters = -1) {
    TerrainGridSnapshot grid; grid.header.version = 11; grid.header.mainTileset = 'L';
    grid.header.width = grid.header.height = 33;
    grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"Cl01"}; grid.vertices.resize(33 * 33);
    for (int y = 0; y < 33; ++y) for (int x = 0; x < 33; ++x) {
        auto& vertex = grid.vertices[std::size_t(y) * 33 + x];
        vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.variationRaw = 3;
        vertex.cliffAndLayerRaw = (x - 16) * dx + (y - 16) * dy <= 0 ? 3 : 2;
    }
    for (int tangent = -1; tangent <= 1; ++tangent) if (HasStrip(strips, unsigned(16 + tangent)))
        for (int normal = 0; normal < 3; ++normal) At(grid, normal, tangent, dx, dy).textureAndFlagsRaw |= 0x10;
    if (waterQuarters >= 0) {
        auto& vertex = At(grid, 1, 0, dx, dy); vertex.textureAndFlagsRaw |= 0x40;
        vertex.waterHeightAndFlagsRaw = std::uint16_t(8192 + waterQuarters);
    }
    if (blightQuarters >= 0) {
        auto& vertex = At(grid, -4, -4, dx, dy); vertex.textureAndFlagsRaw |= 0x60;
        vertex.waterHeightAndFlagsRaw = std::uint16_t(8192 + blightQuarters);
    }
    RefreshTerrainDerived(grid, 0, 0, 33, 33); return grid;
}
inline TerrainStrokePoint Point(int dx, int dy) {
    // 每轴32/96的显式canonical输入，参考高端顶点固定(16,16)。
    return {2048.0 + (dx < 0 ? -32 : 32), 2048.0 + (dy < 0 ? -32 : 32)};
}
inline std::array<unsigned, 2> CanonicalCell(unsigned x, unsigned y, int dx, int dy) {
    if (dx == 1) return {x, y};
    if (dy == 1) return {y, 31 - x};
    if (dx == -1) return {31 - x, 31 - y};
    return {31 - y, x};
}
inline Bytes PathingLiteral(int dx, int dy, Strips strips, int waterQuarters) {
    Bytes bytes(128 * 128);
    const auto wet = Vertex(1, 0, dx, dy);
    for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 128; ++x) {
        const auto cell = CanonicalCell(x / 4, y / 4, dx, dy);
        const bool first = HasStrip(strips, cell[1]), second = HasStrip(strips, cell[1] + 1);
        // 独立字面规则：墙面只有相邻两完整R条之间打开；低侧肩部恰一条R时阻断。
        // 不调用分类器或产品通行生成器来构造expected。
        const bool blocked = (cell[0] == 16 && !(first && second)) || (cell[0] == 17 && first != second);
        unsigned mask = blocked ? 0xca : 0x40;
        if (waterQuarters >= 0 && x >= wet[0] * 4 - 2 && x < wet[0] * 4 + 2
            && y >= wet[1] * 4 - 2 && y < wet[1] * 4 + 2) {
            // 目标中点无R时G=0，有R时G=64；只覆盖最近顶点的四分之一地格。
            const int groundQuarters = strips.target ? 256 : 0;
            if (groundQuarters <= waterQuarters - 256) mask = 0x0a;
            else if (groundQuarters <= waterQuarters) mask = blocked ? 0xca : 0x48;
        }
        bytes[y * 128 + x] = std::byte(mask);
    }
    return bytes;
}
inline Bytes Generate(const TerrainGridSnapshot& grid) {
    const std::array ground{TerrainNativeGroundRule{"Tg01", true, true, true}};
    TerrainNativePathingInput input; input.terrain = &grid; input.groundRules = ground;
    input.playableBounds = {0, 0, 4096, 4096}; input.maximumCliffLevel = 14;
    input.water = TerrainNativeWaterRules{0, 64, false}; return Take(GenerateTerrainNativePathing(input));
}
inline auto RawRecords(const TerrainGridSnapshot& grid) {
    // 非经典布局的拒绝样例本身不能序列化，仍逐字段核对全部原字而不降格丢位。
    std::vector<std::array<std::uint32_t, 7>> records; records.reserve(grid.vertices.size());
    for (const auto& vertex : grid.vertices) records.push_back({vertex.groundHeightRaw,
        vertex.waterHeightAndFlagsRaw, vertex.textureAndFlagsRaw, vertex.variationRaw,
        vertex.cliffAndLayerRaw, vertex.textureFlagsExtensionRaw, vertex.extendedTextureLayout ? 1u : 0u});
    return records;
}
inline void FourDirectionsAndWetLiterals() {
    const DoodadPlacements doodads;
    for (const auto direction : {std::array{1, 0}, std::array{0, 1}, std::array{-1, 0}, std::array{0, -1}})
        for (const auto neighbors : {Strips{}, Strips{true, false, false}, Strips{false, false, true}, Strips{true, false, true}})
            for (const int water : {-1, 255, 256, 511, 512}) {
                const auto dx = direction[0], dy = direction[1]; const auto source = Grid(dx, dy, neighbors, water);
                const auto beforeBytes = Take(SerializeMapTerrain(source)); const auto point = Point(dx, dy);
                TerrainBrush brush; brush.kind = TerrainBrushKind::StraightRamp;
                const auto plan = Take(PlanTerrainStraightRamp(source, point, TerrainStraightRampAlgorithm::Connected));
                Require(plan.highX == 16 && plan.highY == 16 && plan.directionX == dx && plan.directionY == dy,
                    "Connected四方向canonical点仍选择同一高端和法向");
                auto candidate = Take(PrepareTerrainStraightRampStroke(source, Rules(), brush, std::span(&point, 1), doodads,
                    {}, {}, TerrainStraightRampAlgorithm::Connected));
                auto afterStrips = neighbors; afterStrips.target = true;
                const auto expected = Grid(dx, dy, afterStrips, water);
                Require(candidate.result.changedVertices == 3
                    && Take(SerializeMapTerrain(candidate.terrain)) == Take(SerializeMapTerrain(expected))
                    && Take(SerializeMapTerrain(source)) == beforeBytes, "连接只增加目标三R，完整相邻条带和所有其它原字段保持");
                Require(Generate(source) == PathingLiteral(dx, dy, neighbors, water)
                    && Generate(candidate.terrain) == PathingLiteral(dx, dy, afterStrips, water),
                    "完整干湿WPM匹配独立墙面、肩部及最近顶点水深字面期望");
                const auto center = Vertex(1, 0, dx, dy);
                Require(Take(QueryNativeTerrainRamp(candidate.terrain, 14, center[0], center[1])).heightOffset == 64,
                    "连接中点仍为原版R64，不变成猜测高度");
                const auto repeat = Take(PrepareTerrainStraightRampStroke(candidate.terrain, Rules(), brush,
                    std::span(&point, 1), doodads, {}, {}, TerrainStraightRampAlgorithm::Connected));
                Require(repeat.result.changed.Empty(), "已连接完整条带重复创建不产生净变化");
                brush.value = false;
                const auto removed = Take(PrepareTerrainStraightRampStroke(candidate.terrain, Rules(), brush,
                    std::span(&point, 1), doodads, {}, {}, TerrainStraightRampAlgorithm::Connected));
                Require(removed.result.changedVertices == 3 && Take(SerializeMapTerrain(removed.terrain)) == beforeBytes
                    && Generate(removed.terrain) == PathingLiteral(dx, dy, neighbors, water),
                    "移除允许断开两侧连接，只清目标三R且原邻坡及完整WPM恢复");
                if (neighbors.below || neighbors.above) {
                    brush.value = true;
                    Require(!PrepareTerrainStraightRampStroke(source, Rules(), brush, std::span(&point, 1), doodads).Succeeded(),
                        "旧Independent接受域不因新Connected能力而扩大");
                }
            }
}
inline void BeforeAndFailureAtomicity() {
    const DoodadPlacements doodads; const auto point = Point(1, 0);
    TerrainBrush brush; brush.kind = TerrainBrushKind::StraightRamp;
    const auto rejects = [&](TerrainGridSnapshot source, std::string_view code = {}) {
        RefreshTerrainDerived(source, 0, 0, 33, 33); const auto before = RawRecords(source);
        const auto result = PrepareTerrainStraightRampStroke(source, Rules(), brush, std::span(&point, 1), doodads,
            {}, {}, TerrainStraightRampAlgorithm::Connected);
        Require(!result.Succeeded() && !result.value && RawRecords(source) == before,
            "Connected非法before或写后失败不返回前缀、不改来源原字");
        if (!code.empty()) Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; }), "Connected拒绝必须定位预期安全门");
    };
    auto source = Grid(1, 0, {false, false, true});
    At(source, 1, 4, 1, 0).textureAndFlagsRaw |= 0x10;
    Require(PlanTerrainStraightRamp(source, point, TerrainStraightRampAlgorithm::Connected).Succeeded(),
        "三点局部preview不能授予完整halo中损坏邻坡的编辑资格");
    rejects(source, "TERRAIN_STRAIGHT_RAMP_BEFORE_DOMAIN");
    brush.value = false; rejects(source, "TERRAIN_STRAIGHT_RAMP_BEFORE_DOMAIN"); brush.value = true;
    source = Grid(1, 0, {}); At(source, -3, 4, 1, 0).groundHeightRaw += 1; rejects(source);
    source = Grid(1, 0, {}); At(source, -3, 4, 1, 0).cliffAndLayerRaw = 2; rejects(source);
    source = Grid(1, 0, {}); At(source, -3, 4, 1, 0).textureAndFlagsRaw |= 0x80; rejects(source);
    source = Grid(1, 0, {}); At(source, -3, 4, 1, 0).waterHeightAndFlagsRaw |= 0x4000; rejects(source);
    source = Grid(1, 0, {}); At(source, -3, 4, 1, 0).waterHeightAndFlagsRaw |= 0x8000; rejects(source);
    source = Grid(1, 0, {}); At(source, -3, 4, 1, 0).extendedTextureLayout = true; rejects(source);
    source = Grid(1, 0, {});
    const auto before = Take(SerializeMapTerrain(source));
    for (const bool memory : {false, true}) {
        TerrainWaterBrushLimits limits; if (memory) limits.maximumCandidateBytes = 1; else limits.maximumWork = 1;
        const auto failed = PrepareTerrainStraightRampStroke(source, Rules(), brush, std::span(&point, 1), doodads,
            {}, limits, TerrainStraightRampAlgorithm::Connected);
        Require(!failed.Succeeded() && !failed.value && Take(SerializeMapTerrain(source)) == before,
            "Connected工作量与候选字节预算不可放宽或留下部分修改");
    }
    Require(!PlanTerrainStraightRamp(source, point, static_cast<TerrainStraightRampAlgorithm>(0)).Succeeded()
        && !PlanTerrainStraightRamp(source, point, static_cast<TerrainStraightRampAlgorithm>(3)).Succeeded(),
        "未定义算法不得落入Independent或Connected兼容分支");
    for (auto& vertex : source.vertices) vertex.cliffAndLayerRaw = 2;
    for (unsigned y = 0; y < 33; ++y) for (unsigned x = 0; x < 33; ++x)
        if (x <= 2) source.vertices[y * 33 + x].cliffAndLayerRaw = 3;
    RefreshTerrainDerived(source, 0, 0, 33, 33);
    const TerrainStrokePoint edge{288, 2080};
    Require(PlanTerrainStraightRamp(source, edge, TerrainStraightRampAlgorithm::Connected).Succeeded()
        && !PrepareTerrainStraightRampStroke(source, Rules(), brush, std::span(&edge, 1), doodads,
            {}, {}, TerrainStraightRampAlgorithm::Connected).Succeeded(), "三点存在不等于完整dirty加halo在地图内");
}
inline void WetBlightFinisherAndSpecial() {
    const DoodadPlacements empty; const auto point = Point(1, 0);
    TerrainBrush brush; brush.kind = TerrainBrushKind::StraightRamp;
    for (const int surface : {512, 513}) {
        const auto source = Grid(1, 0, {false, false, true}, 256, surface);
        auto expected = Grid(1, 0, {false, true, true}, 256, surface);
        if (surface == 513) At(expected, -4, -4, 1, 0).textureAndFlagsRaw &= std::uint8_t(~0x20);
        const auto prepared = Take(PrepareTerrainStraightRampStroke(source, Rules(), brush, std::span(&point, 1), empty,
            {}, {}, TerrainStraightRampAlgorithm::Connected));
        Require(Take(SerializeMapTerrain(prepared.terrain)) == Take(SerializeMapTerrain(expected)),
            "Connected仍共用原版荒芜结束器：水面等于G保留，再高四分之一才清除");
    }
    const auto source = Grid(1, 0, {false, false, true}); const auto before = Take(SerializeMapTerrain(source));
    DoodadPlacements special; special.specialDoodads.push_back({"Doo0", 0, {16, 16}});
    const std::array shapes{TerrainWaterSpecialDoodadShape{0, "Doo0", 0, {16, 16}, 16, 16, 17, 17, {1, 0, 1, 0}}};
    const auto failed = PrepareTerrainStraightRampStroke(source, Rules(), brush, std::span(&point, 1), special,
        shapes, {}, TerrainStraightRampAlgorithm::Connected);
    Require(!failed.Succeeded() && !failed.value && Take(SerializeMapTerrain(source)) == before,
        "Connected不得借相邻坡道删除受影响special或改写其原始字段");
}
inline void Run() { FourDirectionsAndWetLiterals(); BeforeAndFailureAtomicity(); WetBlightFinisherAndSpecial(); }
} // namespace War3Map::TerrainConnectedStraightRampCases
