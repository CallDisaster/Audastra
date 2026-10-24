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

// 同一冻结无水快照的原版角分类，row-major输出A/B/H/L/R/S/!。
// maximumCliffLevel必须来自实际Terrain.MaxHeight；maximumBytes约束完整分类数组。
// 这是纯算法查询，不授予资源来源、旧WPM解释或保存资格。
[[nodiscard]] Result<std::vector<char>> ClassifyNativeDryRampCorners(
    const TerrainGridSnapshot& terrain, std::uint8_t maximumCliffLevel,
    std::size_t maximumBytes = 64 * 1024 * 1024);

struct TerrainNativeRampState {
    char corner = '!';
    float heightOffset = 0;
    // 5A82D0(mode=0)：至少一个相邻有效地格含R/S角；用于只清除失效的旧坡道位。
    bool belongsToRampCell = false;
    // 5A82D0(mode=1)：相邻含R/S地格至少一个角权总和>1，即5A8570返回0。
    bool hasUnblockedRampCell = false;
};

// 读取当前快照最多第二圈邻域，干/湿地共用完整分类；不分配全图标签。
// maximumCliffLevel必须来自实际Terrain规则，不猜默认最大值。
[[nodiscard]] Result<TerrainNativeRampState> QueryNativeTerrainRamp(
    const TerrainGridSnapshot& terrain, std::uint8_t maximumCliffLevel, std::uint32_t x, std::uint32_t y);

// slot15 是未选悬崖材质的哨兵，不是第16个材质。只允许其所在侧的四个
// 相邻地格全平、双侧完整3×3无坡道/边界/未知字段时，与实际有效槽互转。
// 对称检查保留保存后撤销的能力；它不授予资源来源或WPM保存资格，调用方
// 仍须核实有效槽的CliffTypes行，并完整生成和核对旧/新通行网格。
[[nodiscard]] bool IsNativeTerrainCliffSentinelTransition(
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after, std::size_t index) noexcept;

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
    // 完整special段及其实际贴图生成的逐格层。bit0为覆盖，0xCE为禁止位，
    // 覆盖时替换普通地面/悬崖查询，仍受可玩范围、边界和水规则约束。
    std::span<const std::uint8_t> specialAttributes;
    // 坡道角分类和有效水地高的实际 Terrain/MaxHeight 派生值。无坡道时不需要；
    // 不能由默认14或“已验证”标记代替。资源owner绑定完整配置来源。
    std::optional<std::uint8_t> maximumCliffLevel;
};

// 只返回 row-major 的原版六位 0xEE，不带 WPM 文件头、不混入源 WPM 或
// 工程限制。绑定输入、旧基线比较、0x11 保留及事务回读归统一保存计划。
// maximumBytes 限制输出及坡道角分类工作数组的合计占用。
// 支持静态地图快照的普通悬崖、坡道与水：R加64，S依完整邻域加32/64/96或0；
// 不包含运行时活动地形形变。地形偏移须128对齐，所有顶点世界XY须落在
// 原版九位坐标打包的 [-32768,32640] 内。未提供完整特殊地形层、
// 未知字段、不完整规则或未闭合的坐标/范围精度明确失败，绝不返回部分网格。
[[nodiscard]] Result<std::vector<std::byte>> GenerateTerrainNativePathing(
    const TerrainNativePathingInput& input, std::size_t maximumBytes = 64 * 1024 * 1024);

} // namespace War3Map::MapData
