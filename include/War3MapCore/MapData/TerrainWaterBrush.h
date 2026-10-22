#pragma once

#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/MapPlacements.h"

#include <utility>

namespace War3Map::MapData {

enum class TerrainWaterBrushKind { Shallow, Deep };

struct TerrainWaterGroundRule {
    std::string rawcode;
    bool permitsBlight;
};

// 必须由当前地图最终加载的 Terrain 配置、Water 与地形资源提供；没有兼容默认值。
struct TerrainWaterLoadedRules {
    float maxSlopeDegrees, minHeight, maxHeight, waterLayerOffset, deepWaterThreshold;
    std::uint8_t maximumCliffLevel;
    bool enforceWaterLimits;
    std::vector<TerrainWaterGroundRule> groundRules;
    std::vector<std::string> availableCliffTilesets;

    TerrainWaterLoadedRules(float slope, float minimum, float maximum, float waterOffset,
        float deepThreshold, std::uint8_t maximumLevel, bool enforce,
        std::vector<TerrainWaterGroundRule> ground, std::vector<std::string> cliffs)
        : maxSlopeDegrees(slope), minHeight(minimum), maxHeight(maximum), waterLayerOffset(waterOffset),
          deepWaterThreshold(deepThreshold), maximumCliffLevel(maximumLevel), enforceWaterLimits(enforce),
          groundRules(std::move(ground)), availableCliffTilesets(std::move(cliffs)) {}
};

// 一笔开始时捕获；后续落点复用原始 B/R/L，不从已改动的地形重新取样。
struct TerrainWaterStrokeAnchor {
    std::uint16_t groundHeightRaw, waterHeightRaw;
    std::uint8_t cliffLevel;
    bool hasWater;
};

struct TerrainWaterBrush {
    TerrainWaterBrushKind kind;
    TerrainBrushShape shape;
    std::uint32_t size; // 经典尺寸 1..8，顶点半径 size-1；平台阶段使用 size+2。
    std::uint8_t cliffTexture;
    std::uint32_t randomSeed; // 明确的单笔 MSVCRT 序列起始状态，不使用进程全局 rand。
    TerrainWaterStrokeAnchor anchor;
};

// 由实际解析的 DOO special 记录及其已加载资源生成。范围是地格半开区间；
// relativeCliffLevels 是 (width+1)*(height+1) 个顶点的行优先相对层级。
// Core 按实际处理区域检查，范围外的特殊记录不会因为已有层级不匹配而拒绝本笔。
struct TerrainWaterSpecialDoodadShape {
    std::uint32_t sourceRecordIndex;
    std::string rawcode;
    std::int32_t variation;
    std::array<std::int32_t, 2> position;
    std::uint32_t x0, y0, x1, y1;
    std::vector<std::int8_t> relativeCliffLevels;
};

struct TerrainWaterBrushLimits {
    // 只能收紧；候选副本、marker/访问位、DFS 栈与脏块列表均在分配前计量。
    std::size_t maximumCandidateBytes = 256u * 1024u * 1024u;
    std::size_t maximumWork = 16u * 1024u * 1024u;
    std::size_t maximumPoints = 65536;
    std::uint32_t maximumRecursionDepth = 254; // 不跨过原临时 byte marker 的回绕点。
};

struct TerrainWaterBrushResult {
    TerrainStrokeResult changed;
    TerrainStrokeResult processed; // 实际读取/处理范围，可能大于 changed 和原始笔刷。
    std::size_t changedVertices = 0, work = 0;
    std::uint32_t finalRandomState = 0;
    // 结构性后续工作；本纯 Core 返回值不授予 WPM/DOO/对象或整图保存资格。
    bool requiresPathingRebuild = false;
    bool requiresObjectValidityRefresh = false;
};

[[nodiscard]] Result<TerrainWaterStrokeAnchor> CaptureTerrainWaterStrokeAnchor(
    const TerrainGridSnapshot& terrain, const TerrainStrokePoint& point);

// owned W3E 候选内依原顺序执行水面、扩大平台、层级/palette/variation、岸线、
// 荒芜、越界地高衰减和 palette 传播。旧坡道/未知位只在实际访问区域拒绝。
// specialShapes 必须与实际解析的 special 记录一一对应；需要删除的相交记录明确拒绝。
// 所有失败（含预算与分配失败）都不修改 terrain；成功后一次性替换候选。
[[nodiscard]] Result<TerrainWaterBrushResult> ApplyTerrainWaterStroke(
    TerrainGridSnapshot& terrain, const TerrainWaterLoadedRules& rules, const TerrainWaterBrush& brush,
    std::span<const TerrainStrokePoint> points, const DoodadPlacements& doodads,
    std::span<const TerrainWaterSpecialDoodadShape> specialShapes = {}, const TerrainWaterBrushLimits& limits = {});

} // namespace War3Map::MapData
