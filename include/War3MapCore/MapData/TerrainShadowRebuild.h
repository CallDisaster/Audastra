#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace War3Map::MapData {

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

// 依据已取证的主轴步进、向零取整和高度相等即停止规则，生成独立候选。
// 不读写地图、不获取资源、不执行对象模型射线；输入 owner、完整性与修订
// 绑定由捕获者证明。失败或取消不返回部分候选，也不修改借用的高度场。
[[nodiscard]] Result<TerrainShadowProjection> ProjectTerrainShadow(
    const TerrainShadowHeightSamples& samples, const TerrainShadowRebuildLimits& limits = {},
    std::stop_token cancellation = {});

} // namespace War3Map::MapData
