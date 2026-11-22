#pragma once
#include <cstdint>

// 单帧粒子输出的硬上限。编辑器侧的准入与几何生成、插件输入校验和 D3D11 后端共用这一组数；
// 只在这里改，避免某一处仍按旧值拒绝整帧。
namespace War3Map::Render {
// 每顶点 36 字节，满载约 108 MiB：单个 D3D11 缓冲在显存较小的设备上只保证 128 MiB，不再往上提。
inline constexpr std::uint32_t kMaxParticleFrameVertices = 3145728;
inline constexpr std::uint32_t kMaxParticleFrameDraws = 65536;
// 粒子帧自身（顶点、draw、逐 draw 标志）的载荷上限。
inline constexpr std::uint64_t kMaxParticleFramePayloadBytes = 128ull * 1024 * 1024;
// 同一次提交里模型姿势（至多 64 MiB）与粒子帧合计的载荷上限。
inline constexpr std::uint64_t kMaxPoseAndParticlePayloadBytes = 256ull * 1024 * 1024;
// 场景级粒子池与条带段的配置上限（全部获准实例的发射器配额之和）；默认值仍是 65536 / 32768。
inline constexpr std::uint32_t kMaxSceneParticles = 1048576, kMaxSceneRibbonSegments = 524288;
static_assert(std::uint64_t(kMaxParticleFrameVertices) * 36 + std::uint64_t(kMaxParticleFrameDraws) * 20
    <= kMaxParticleFramePayloadBytes);
}
