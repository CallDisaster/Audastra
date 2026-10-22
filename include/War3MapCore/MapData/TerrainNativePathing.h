#pragma once

#include "War3MapCore/MapData/MapTerrain.h"

#include <optional>
#include <span>

namespace War3Map::MapData {

// 原版通行使用未取整的轴对齐世界范围。四数须为实际 binary32 值；
// 包含判断保留原版正/反投影及 float 落点，不是普通四边比较，
// 也不使用小地图的整数地格裁剪范围。X 跨度目前须可精确表示为
// 不超过26个有效二进制位；超出已证明等价的精度域会明确拒绝。
struct TerrainNativePlayableBounds {
    double left = 0;
    double bottom = 0;
    double right = 0;
    double top = 0;
};

struct TerrainNativeGroundRule {
    std::string rawcode;
    bool walkable = false;
    bool flyable = false;
    bool buildable = false;
};

struct TerrainNativeWaterRules {
    // 对应实际 Water.slk / 配置的 binary32 值。height 先按 float 乘 128，
    // 再加到原始水面；deepLevelPathing 已是世界单位，不再乘 128。
    float height = 0;
    float deepLevelPathing = 0;
    bool impassable = false;
};

struct TerrainNativeWaterState {
    std::uint8_t depth = 0; // 0: 无覆水；1: 浅水；2: 深水。
    bool strictCoverage = false; // 水面严格高于地面，与 depth!=0 不等价。
    [[nodiscard]] bool operator==(const TerrainNativeWaterState&) const noexcept = default;
};

// 保存资格比较和完整通行生成共用。只支持无坡道、已知经典顶点字段；
// 不读取 TerrainVertex::position 或显示网格近似高度。
[[nodiscard]] Result<TerrainNativeWaterState> ClassifyNativeTerrainWater(
    const TerrainVertex& vertex, const TerrainNativeWaterRules& rules);

struct TerrainNativePathingInput {
    const TerrainGridSnapshot* terrain = nullptr;
    TerrainNativePlayableBounds playableBounds;
    std::span<const TerrainNativeGroundRule> groundRules;
    std::optional<TerrainNativeWaterRules> water;
    // 已从最终对象集合合成的 selector 2 属性，索引与 WPM 相同。
    // 低六位为属性；0x40 抑制整个地形层。空 span 表示处处为零，
    // 非空则必须完整覆盖 (width-1)*4 × (height-1)*4 个通行格。
    // 来源捕获者负责证明该集合完整；这里不解释 DOO 或忽略未知对象。
    std::span<const std::uint8_t> staticAttributes;
    std::size_t specialDoodadCount = 0;
};

// 只返回 row-major 的原版六位 0xEE，不带 WPM 文件头、不混入源 WPM 或
// 工程限制。绑定输入、旧基线比较、0x11 保留及事务回读归统一保存计划。
// maximumBytes 限制输出占用；当前算法不另建与地图等大的临时网格。
// 支持普通无坡道悬崖；地形偏移须128对齐，所有顶点世界XY须落在
// 原版九位坐标打包的 [-32768,32640] 内。特殊地形覆盖、坡道、
// 未知字段、不完整规则或未闭合的坐标/范围精度明确失败，绝不返回部分网格。
[[nodiscard]] Result<std::vector<std::byte>> GenerateTerrainNativePathing(
    const TerrainNativePathingInput& input, std::size_t maximumBytes = 64 * 1024 * 1024);

} // namespace War3Map::MapData
