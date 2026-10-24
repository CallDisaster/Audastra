#pragma once

#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapTerrain.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace War3Map::MapData {

// 对实际经典profile的[Light]/Direction前三个value执行原版归一化和取负。
// 不读DNC、不选默认值；资源捕获者须按完整profile追加链取得这三个分量。
[[nodiscard]] Result<std::array<float, 3>> PrepareTerrainShadowLightDirection(
    const std::array<float, 3>& editorDirection);

// 原版显式计算阴影的地形投影阶段：每地格 4×4 个、间距 32 的像素中心。
// receiverHeights 是已完成特殊地形/普通地面采样及 +0.1 偏置后的 binary32
// 高度场，row-major；不能以显示顶点高度、旧 SHD 或缺失采样填零代替。
// lightDirection 对应地形实际光照方向，不能由本组件猜测默认值。
struct TerrainShadowHeightSamples {
    std::uint32_t width = 0, height = 0;
    std::span<const float> receiverHeights;
    std::array<float, 3> lightDirection{};
};

struct TerrainShadowRebuildLimits {
    std::size_t maximumDimension = 8192;
    std::size_t maximumPixels = 64u * 1024u * 1024u;
    std::size_t maximumSampleBytes = 256u * 1024u * 1024u;
    // 高度检查、投影起点和被投影像素分别计一次；只允许收紧硬预算。
    std::size_t maximumWork = 256u * 1024u * 1024u;
};

struct TerrainShadowProjection {
    std::uint32_t width = 0, height = 0;
    // 仅地形投影的持久 bit0（0/1）。未包含对象模型射线命中，不能直接
    // 作为完整 SHD 或赋予保存资格；原始 SHD codec 的无损语义不受影响。
    std::vector<std::uint8_t> terrainShadowBits;
    std::size_t work = 0;
};

struct TerrainShadowReceiverGrid {
    std::uint32_t width = 0, height = 0;
    std::vector<float> receiverHeights;
    std::size_t work = 0;
};

// 只验证完整普通表面与逻辑 receiver 尺寸，不分配高度场。返回实际顶点
// 检查工作量；采样器及最终资源捕获共用本门禁，不能把通过视为已采样。
[[nodiscard]] Result<std::size_t> ValidateOrdinaryTerrainShadowSurface(
    const TerrainGridSnapshot& terrain, std::span<const SpecialDoodadPlacement> specialDoodads,
    const TerrainShadowRebuildLimits& limits = {}, std::stop_token cancellation = {});

// 从实际经典 W3E 原始字段采样普通地形，每个 SHD 像素中心取高并加 0.1。
// 复用 Core 的同一 SW/SE/NW、SE/NE/NW 三角采样；不读取显示 position。
// 当前拒绝坡道、异层悬崖及实际 special DOO 集合。水面不参与接收高度，
// 这不代表忽略水资源即可获得完整阴影/保存资格。普通 DOO 模型遮挡另算。
[[nodiscard]] Result<TerrainShadowReceiverGrid> SampleOrdinaryTerrainShadowHeights(
    const TerrainGridSnapshot& terrain, std::span<const SpecialDoodadPlacement> specialDoodads,
    const TerrainShadowRebuildLimits& limits = {}, std::stop_token cancellation = {});

// 依据已取证的主轴步进、向零取整和高度相等即停止规则，生成独立候选。
// 不读写地图、不获取资源、不执行对象模型射线；输入 owner、完整性与修订
// 绑定由捕获者证明。失败或取消不返回部分候选，也不修改借用的高度场。
[[nodiscard]] Result<TerrainShadowProjection> ProjectTerrainShadow(
    const TerrainShadowHeightSamples& samples, const TerrainShadowRebuildLimits& limits = {},
    std::stop_token cancellation = {});

} // namespace War3Map::MapData
