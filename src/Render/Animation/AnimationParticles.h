#pragma once
#include "AnimationInternal.h"

namespace War3Map::Render {
struct AnimationParticleBinding {
    std::uint32_t requestIndex = 0, assetIndex = 0;
    std::vector<std::uint32_t> quotas, textures;
    std::vector<float> layerAlpha;
    bool quotaLimited = false;
};
struct AnimationParticleState {
    Particles::State state;
    Particles::Timeline timeline;
    std::optional<War3RenderCore::LocalCamera> camera;
    std::uint64_t storageBytes = 0, stateHash = 0;
    Particles::Counts counts;
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
    std::string code;
    std::vector<std::uint32_t> quotas;
    std::uint64_t anchors = 0, nodes = 0, stateBytes = 0, vertices = 0, draws = 0;
};
struct AnimationParticleHost {
    std::vector<AnimationParticleBinding> bindings;
    std::unique_ptr<War3RenderCore::PoseBatchEvaluator> evaluator;
    AnimationParticleLimits limits;
    Particles::Limits simulation;
    std::uint64_t maxFrameBytes = 0, fixedFrameBytes = 0, stateBytes = 0;
    std::uint32_t maxDynamicVertices = 0, maxDynamicDraws = 0;
    bool retainUnsupportedStatic = false;
    AnimationPlaybackPolicy playbackPolicy = AnimationPlaybackPolicy::StrictReplay;
    bool omitRejectedLinks = false; // 逐实例原因仍在不可变 capability/query；不为无模拟实例超额分配帧回执。
};
[[nodiscard]] Status ReadAnimationParticleAsset(AnimationAsset& asset, std::span<const std::byte> bytes,
    bool enabled, bool retainUnsupportedStatic);
[[nodiscard]] Result<std::shared_ptr<AnimationParticleHost>> CreateAnimationParticles(AnimatedModelScene& scene,
    std::span<const AnimationAsset> assets, std::span<const std::uint32_t> requestAssets,
    std::span<const War3RenderCore::CompiledModel> compiled, const AnimationResourceReader& read,
    const AnimationLimits& limits, std::uint64_t epoch, std::uint64_t fixedFrameBytes);
[[nodiscard]] Status PrepareAnimationParticles(AnimationParticleHost& host, const AnimatedModelScene& scene,
    std::span<const AnimationAsset> assets, std::span<const std::uint32_t> requestAssets,
    AnimationFrame& frame, const AnimationFrame* previous);
} // namespace War3Map::Render
