#pragma once

#include "War3MapCore/MapData/TerrainRampEditing.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace War3Map::TerrainJoinedRampCases {
using namespace MapData;
inline void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class T> T Take(Result<T> result, std::string context = {}) {
    if (!result.Succeeded() || !result.value) {
        std::string text = "原版连接坡道夹具失败 " + context;
        for (const auto& diagnostic : result.diagnostics) text += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(text);
    }
    return std::move(*result.value);
}
inline TerrainWaterLoadedRules Rules() { return {90, -256, 1536, 0, 64, 14, true, {{"Tg01", true}}, {"Cl01"}}; }
inline TerrainGridSnapshot Grid(bool inner, unsigned rotation) {
    TerrainGridSnapshot grid; grid.header.version = 11; grid.header.mainTileset = 'L';
    grid.header.width = grid.header.height = 33; grid.header.offset = {-2048, -2048};
    grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"Cl01"}; grid.vertices.resize(33 * 33);
    for (unsigned y = 0; y < 33; ++y) for (unsigned x = 0; x < 33; ++x) {
        auto rx = x, ry = y;
        for (unsigned turn = 0; turn < rotation; ++turn) { const auto next = 32 - ry; ry = rx; rx = next; }
        auto& vertex = grid.vertices[ry * 33 + rx];
        vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936;
        vertex.cliffAndLayerRaw = std::uint8_t(inner ? (x >= 16 && y >= 16 ? 2 : 3) : (x <= 16 && y <= 16 ? 3 : 2));
    }
    RefreshTerrainDerived(grid, 0, 0, 33, 33); return grid;
}
inline TerrainStrokePoint Rotate(TerrainStrokePoint point, unsigned rotation) {
    for (unsigned turn = 0; turn < rotation; ++turn) point = {-point.y, point.x};
    return point;
}
inline TerrainBrush Brush(bool enabled = true) {
    TerrainBrush brush; brush.kind = TerrainBrushKind::CornerRamp; brush.value = enabled; return brush;
}
inline TerrainWaterStrokeCandidate Prepare(const TerrainGridSnapshot& source, TerrainStrokePoint point, bool enabled = true) {
    return Take(PrepareTerrainCornerRampStroke(source, Rules(), Brush(enabled), std::span(&point, 1), {}, {}, {},
        TerrainCornerRampAlgorithm::NativeJoinedRamps), std::to_string(point.x) + "," + std::to_string(point.y)
        + (enabled ? " create" : " remove"));
}
inline void ReferenceQuarterSelections() {
    const auto outer = Grid(false, 0), inner = Grid(true, 0);
    const std::array<TerrainStrokePoint, 3> outerPoints{{{32, 32}, {96, 32}, {32, 96}}};
    const std::array<TerrainStrokePoint, 3> innerPoints{{{-32, -32}, {-96, -32}, {-32, -96}}};
    constexpr std::array<std::size_t, 3> counts{9, 3, 3};
    for (std::size_t i = 0; i < counts.size(); ++i) {
        Require(Take(PlanTerrainCornerRamp(outer, outerPoints[i], TerrainCornerRampAlgorithm::NativeJoinedRamps)).selectedVertexCount == counts[i]
            && Take(PlanTerrainCornerRamp(inner, innerPoints[i], TerrainCornerRampAlgorithm::NativeJoinedRamps)).selectedVertexCount == counts[i],
            "原版按参考角的轴差选点，三高一低地格也有合法三点落点");
    }
    Require(!PlanTerrainCornerRamp(outer, {96, 96}, TerrainCornerRampAlgorithm::NativeJoinedRamps).Succeeded()
        && !PlanTerrainCornerRamp(inner, {-96, -96}, TerrainCornerRampAlgorithm::NativeJoinedRamps).Succeeded(),
        "两轴均无层差的对角落点不能猜测连接方向");
}
inline void CrossingsAndRotations() {
    for (const bool inner : {false, true}) for (unsigned rotation = 0; rotation < 4; ++rotation) {
        const auto source = Grid(inner, rotation); const auto raw = Take(SerializeMapTerrain(source));
        const auto first = Rotate(inner ? TerrainStrokePoint{-32, 160} : TerrainStrokePoint{32, -224}, rotation);
        const auto second = Rotate(inner ? TerrainStrokePoint{160, -32} : TerrainStrokePoint{-224, 32}, rotation);
        const auto junction = Rotate(inner ? TerrainStrokePoint{-32, -32} : TerrainStrokePoint{32, 32}, rotation);
        const auto firstPlan = Take(PlanTerrainCornerRamp(source, first, TerrainCornerRampAlgorithm::NativeJoinedRamps));
        Require(firstPlan.selectedVertexCount == 3, "原版直台阶明确选择三点，不能填九点");
        Require(!PlanTerrainCornerRamp(source, first, TerrainCornerRampAlgorithm::NativeWetCorners).Succeeded(),
            "旧四版不因新连接能力接受直坡落点");
        const auto a = Prepare(source, first);
        auto junctionSource = a.terrain;
        const auto beforeSecond = Take(SerializeMapTerrain(a.terrain));
        auto secondAttempt = PrepareTerrainCornerRampStroke(a.terrain, Rules(), Brush(), std::span(&second, 1), {}, {}, {},
            TerrainCornerRampAlgorithm::NativeJoinedRamps);
        if (inner) {
            Require(!secondAttempt.Succeeded() && !secondAttempt.value
                && std::any_of(secondAttempt.diagnostics.begin(), secondAttempt.diagnostics.end(),
                    [](const auto& d) { return d.code == "TERRAIN_CORNER_RAMP_INVALID"; }),
                "内角两条未连接细坡的LRHB形态保持原版拒绝，不能靠放宽live检查通过");
            Require(Take(SerializeMapTerrain(a.terrain)) == beforeSecond, "非法中间形态不能污染第一条已提交坡道");
        } else junctionSource = Take(std::move(secondAttempt)).terrain;
        Require(!PrepareTerrainCornerRampStroke(junctionSource, Rules(), Brush(), std::span(&junction, 1), {}, {}, {},
            TerrainCornerRampAlgorithm::NativeWetCorners).Succeeded(), "旧算法4仍拒绝相邻坡道交汇，不因新模式扩大历史接受域");
        const auto plan = Take(PlanTerrainCornerRamp(junctionSource, junction, TerrainCornerRampAlgorithm::NativeJoinedRamps));
        Require(plan.selectedVertexCount == 9, "原版交汇转角明确选择九点");
        const auto joined = Prepare(junctionSource, junction);
        const auto branch = Prepare(joined.terrain, second);
        Require(Take(SerializeMapTerrain(branch.terrain)) == Take(SerializeMapTerrain(joined.terrain)),
            "先完成转角后可接另一方向的完整条带，不重复扩张已经包含的R位");
        for (const auto index : plan.vertexIndices) Require(joined.terrain.vertices[index].IsRamp(), "连接保留请求九点R");
        for (std::size_t i = 0; i < source.vertices.size(); ++i) {
            const auto& before = source.vertices[i]; const auto& after = joined.terrain.vertices[i];
            Require(before.groundHeightRaw == after.groundHeightRaw && before.cliffAndLayerRaw == after.cliffAndLayerRaw
                && before.waterHeightAndFlagsRaw == after.waterHeightAndFlagsRaw && before.variationRaw == after.variationRaw,
                "连接不靠改变地高、层级、水或变体伪造通路");
        }
        const auto repeated = Prepare(joined.terrain, junction);
        Require(Take(SerializeMapTerrain(repeated.terrain)) == Take(SerializeMapTerrain(joined.terrain)),
            "已连接请求重复执行不继续扩大R域");
        Require(Take(SerializeMapTerrain(source)) == raw, "所有预备候选保持源快照不变");
        const auto isolated = Prepare(source, first);
        const auto removed = Prepare(isolated.terrain, first, false);
        Require(Take(SerializeMapTerrain(removed.terrain)) == raw, "孤立三点坡创建后移除恢复全部原字节");
    }
}
inline void Refusals() {
    const auto point = TerrainStrokePoint{32, 32}; const auto source = Grid(false, 0);
    auto unsupported = source; unsupported.vertices[16 * 33 + 16].textureFlagsExtensionRaw = 1;
    Require(!PrepareTerrainCornerRampStroke(unsupported, Rules(), Brush(), std::span(&point, 1), {}, {}, {},
        TerrainCornerRampAlgorithm::NativeJoinedRamps).Succeeded(), "未知位保持拒绝");
    auto flat = source; for (auto& vertex : flat.vertices) vertex.cliffAndLayerRaw = 2;
    Require(!PlanTerrainCornerRamp(flat, point, TerrainCornerRampAlgorithm::NativeJoinedRamps).Succeeded(),
        "平地不能被新模式填成坡道");
    TerrainWaterBrushLimits limits; limits.maximumWork = 1;
    Require(!PrepareTerrainCornerRampStroke(source, Rules(), Brush(), std::span(&point, 1), {}, {}, limits,
        TerrainCornerRampAlgorithm::NativeJoinedRamps).Succeeded(), "连接仍受原工作量预算约束");
    Require(!PlanTerrainCornerRamp(source, point, static_cast<TerrainCornerRampAlgorithm>(255)).Succeeded(),
        "未知算法版本仍拒绝");
}
inline void KnownPartialRampCanBeCompleted() {
    auto source = Grid(true, 0); source.vertices[17 * 33 + 16].textureAndFlagsRaw |= 0x10;
    const auto before = Take(SerializeMapTerrain(source)); const TerrainStrokePoint point{-32, -32};
    Require(!PrepareTerrainCornerRampStroke(source, Rules(), Brush(), std::span(&point, 1), {}, {}, {},
        TerrainCornerRampAlgorithm::NativeWetCorners).Succeeded(), "旧版本继续拒绝部分九点");
    const auto completed = Prepare(source, point);
    const auto plan = Take(PlanTerrainCornerRamp(source, point, TerrainCornerRampAlgorithm::NativeJoinedRamps));
    for (const auto index : plan.vertexIndices) Require(completed.terrain.vertices[index].IsRamp(), "已知原始R位可由显式连接操作补完整");
    Require(Take(SerializeMapTerrain(source)) == before, "修复候选不改写其原始部分坡道");
}
inline void StraightRampBelowAnotherDryTier() {
    auto source = Grid(false, 0);
    for (unsigned y = 0; y < 33; ++y) for (unsigned x = 0; x < 33; ++x)
        source.vertices[y * 33 + x].cliffAndLayerRaw = y >= 17 ? 4 : y == 16 ? 3 : 2;
    RefreshTerrainDerived(source, 0, 0, 33, 33);
    const TerrainStrokePoint point{-96, -96};
    auto expected = source;
    for (unsigned y = 14; y <= 16; ++y) expected.vertices[y * 33 + 15].textureAndFlagsRaw |= 0x10;
    const auto prepared = Prepare(source, point);
    Require(Take(SerializeMapTerrain(prepared.terrain)) == Take(SerializeMapTerrain(expected)),
        "下缘直坡只写原版三个R位，背后的下一层不加入本次笔划");
    source.vertices[14 * 33 + 14].cliffAndLayerRaw = 4;
    const auto wrongSide = PrepareTerrainCornerRampStroke(source, Rules(), Brush(), std::span(&point, 1), {}, {}, {},
        TerrainCornerRampAlgorithm::NativeJoinedRamps);
    Require(!wrongSide.Succeeded(), "额外高层必须在直坡高点背面");
}
inline void Run() {
    ReferenceQuarterSelections(); CrossingsAndRotations(); Refusals(); KnownPartialRampCanBeCompleted();
    StraightRampBelowAnotherDryTier();
}
} // namespace War3Map::TerrainJoinedRampCases
