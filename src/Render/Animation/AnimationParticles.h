#pragma once
#include "AnimationInternal.h"

namespace War3Map::Render {
struct AnimationParticleBinding {
    std::uint32_t requestIndex = 0, assetIndex = 0;
    std::vector<std::uint32_t> quotas, textures;
    std::vector<float> layerAlpha;
    std::shared_ptr<const std::vector<std::uint32_t>> materialFlags;
    bool quotaLimited = false;
};
// 计算真实拥有的输出容量；旧帧的size变小也不会免除仍保留的flags容量。
[[nodiscard]] inline std::uint64_t AnimationParticleOutputBytes(std::size_t vertices, std::size_t draws,
    std::size_t flags) noexcept {
    return std::uint64_t(vertices)*sizeof(War3RenderParticleVertexV2)
        + std::uint64_t(draws)*sizeof(War3RenderParticleDrawV2) + std::uint64_t(flags)*sizeof(std::uint32_t);
}
[[nodiscard]] inline std::uint64_t AnimationParticleOutputBytes(const AnimationFrame& frame) noexcept {
    return AnimationParticleOutputBytes(frame.particleVertices.capacity(), frame.particleDraws.capacity(),
        frame.particleMaterialFlags.capacity());
}
struct AnimationParticleState {
    Particles::State state;
    Particles::Timeline timeline;
    std::optional<War3RenderCore::LocalCamera> camera;
    std::uint64_t storageBytes = 0, stateHash = 0;
    Particles::Counts counts;
    // 交互预览：上次模拟结束时刻的发射器节点。下一帧的固定步在它与当前节点之间插值，不逐步重求骨架。
    std::vector<Particles::Anchor> anchors;
    std::uint64_t anchorElapsedMs = 0;
};
struct AnimationParticleCheckpoint {
    std::uint64_t sceneVersion = 0, contentVersion = 0;
    // 画外/预算省略复用不可变状态；只复制本帧获准推进的实例。
    std::vector<std::shared_ptr<const AnimationParticleState>> instances;
    std::vector<std::uint8_t> frozen;
    std::uint64_t stateBytes = 0;
};
struct AnimationParticleAdmission {
    bool admitted = false;
    bool eligible = false; // 在视野内、主姿势未隐藏也未按预算保留静态：参与本帧准入。
    std::string code;
    std::vector<std::uint32_t> quotas;
    std::uint64_t anchors = 0, nodes = 0, stateBytes = 0, vertices = 0, draws = 0;
    std::uint64_t particles = 0, segments = 0; // 本实例的粒子与条带配额合计
};
// 交互预览粒子顶点缓冲的容量滞后（准备线程独占）：不够时扩到实际量约 1.25 倍；连续 120 帧用量都低于目标一半，
// 才按这段时间的峰值约 1.25 倍缩回。各帧数组够用、且不比目标大一半以上就沿用，避免反复分配后首次写入缺页。
struct AnimationParticleVertexCapacity {
    std::size_t target = 0, lowPeak = 0;
    std::uint32_t lowFrames = 0;
};
// 按上面的滞后为本帧数组定容量（不改长度）；重新分配时旧内容不搬，调用方随后整段覆盖。
// 返回 0 沿用、1 扩、2 缩、3 空数组首次分配（新建帧）。
std::uint32_t ReserveAnimationParticleVertices(AnimationParticleVertexCapacity& capacity,
    std::vector<War3RenderParticleVertexV2>& vertices, std::size_t used);
struct AnimationParticleHost {
    std::vector<AnimationParticleBinding> bindings;
    std::unique_ptr<War3RenderCore::PoseBatchEvaluator> evaluator;
    AnimationParticleLimits limits;
    Particles::Limits simulation;
    std::uint64_t maxFrameBytes = 0, fixedFrameBytes = 0, stateBytes = 0;
    std::uint32_t maxDynamicVertices = 0, maxDynamicDraws = 0;
    bool retainUnsupportedStatic = false;
    AnimationPlaybackPolicy playbackPolicy = AnimationPlaybackPolicy::StrictReplay;
    std::uint32_t workers = 0; // 1：交互预览粒子也逐实例串行，用于并行一致性对照；其余用共用动画线程池。
    AnimationParticleVertexCapacity vertexCapacity; // 交互预览帧粒子顶点缓冲的容量目标
    // 交互预览逐绑定的准入计划，跨帧复用存储（准备线程独占）；每帧只重写视野内绑定，视野外绑定不读。
    std::vector<AnimationParticleAdmission> admissions;
    bool omitRejectedLinks = false; // 逐实例原因仍在不可变 capability/query；不为无模拟实例超额分配帧回执。
    // 交互预览：本帧主姿势求值顺带写下发射器节点，粒子直接复用，不再为当前时刻单独求骨架。
    std::vector<std::uint32_t> requestBindings; // 请求→绑定；无绑定为 UINT32_MAX。每帧按当前绑定表重建。
    std::vector<std::uint32_t> anchorOffsets; // 绑定→frameAnchors 起点，按该绑定资产的发射器节点数排列。
    std::vector<Particles::Anchor> frameAnchors;
    std::vector<std::uint64_t> anchorSerials; // 绑定→写入时的帧序号；等于 anchorSerial 才属于本帧。
    std::uint64_t anchorSerial = 0;
    // 绑定→回收帧里只剩本服务持有的旧状态。下一次复制该绑定的状态时借用其数组容量，内容照常整份复制。
    std::vector<std::shared_ptr<const AnimationParticleState>> spareStates;
    // 场景里声明了 PRE2/RIBB/PREM 的请求（升序）。其余请求在前置两轮检查里都直接跳过，不必每帧逐个读能力表。
    // 场景不可变；按场景版本与能力表身份缓存。
    std::uint64_t capabilitySceneVersion = 0;
    const void* capabilityTable = nullptr;
    std::size_t capabilityCount = 0;
    std::vector<std::uint32_t> capabilityRequests;
};
// 回收帧的粒子检查点：只有本服务还持有的状态记为对应绑定的备用缓冲（准备线程调用）。
void RetireAnimationParticleCheckpoint(AnimationParticleHost& host,
    std::shared_ptr<const AnimationParticleCheckpoint> checkpoint) noexcept;
// 每帧主姿势求值前调用（仅交互预览）：按当前绑定表重建索引并开启新一帧的锚点序号。
void BeginAnimationParticleAnchors(AnimationParticleHost& host, std::size_t requestCount,
    std::span<const AnimationAsset> assets);
// 主姿势成功求值后写入该请求的发射器节点。没有绑定或绑定资产不是本次求值的资产时不写。
void CaptureAnimationParticleAnchors(AnimationParticleHost& host, std::uint32_t request, std::uint32_t assetIndex,
    const AnimationAsset& asset, std::span<const War3RenderCore::NodePose> nodes);
[[nodiscard]] Status ReadAnimationParticleAsset(AnimationAsset& asset, std::span<const std::byte> bytes,
    const AnimationLimits& limits);
[[nodiscard]] Result<std::shared_ptr<AnimationParticleHost>> CreateAnimationParticles(AnimatedModelScene& scene,
    std::span<const AnimationAsset> assets, std::span<const std::uint32_t> requestAssets,
    std::span<const War3RenderCore::CompiledModel> compiled, const AnimationResourceReader& read,
    const AnimationLimits& limits, std::uint64_t epoch, std::uint64_t fixedFrameBytes);
[[nodiscard]] Status PrepareAnimationParticles(AnimationParticleHost& host, const AnimatedModelScene& scene,
    std::span<const AnimationAsset> assets, std::span<const std::uint32_t> requestAssets,
    AnimationFrame& frame, const AnimationFrame* previous);
} // namespace War3Map::Render
