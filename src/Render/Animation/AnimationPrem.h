#pragma once
#include "AnimationInternal.h"
#include "War3MapCore/Render/AnimationPremFrame.h"
#include <unordered_map>

namespace War3Map::Render {
struct AnimationPremChildAsset {
    std::string path;
    War3RenderCore::AssetSource source;
    std::uint32_t sequenceIndex = UINT32_MAX, firstSlot = UINT32_MAX;
};
struct AnimationPremHost {
    std::shared_ptr<const std::vector<AnimationPremCapability>> capabilities;
    std::vector<std::vector<std::uint32_t>> assetCapabilities;
    std::vector<std::uint32_t> birthAssetIndices; // 正式父资产编号 -> 独立出生求值器紧凑编号；未准入为UINT32_MAX。
    std::vector<AnimationPremChildAsset> children;
    std::shared_ptr<ModelAnimationService> childService;
    std::shared_ptr<const AnimationFrame> childStand;
    std::unique_ptr<War3RenderCore::PoseBatchEvaluator> evaluator;
    std::uint64_t epoch = 0, storageBytes = 0, workingBytes = 0, extraAssetBytes = 0;
    std::uint64_t fixedFrameBytes = 0;
    AnimationPremBudgetBreakdown budget;
    // 只由场景与资产决定的逐帧筛选结果，按场景版本与请求表身份缓存（准备线程独占）：
    // 发射器实例计数、父资产带已准备 PREM 的请求（升序），以及这些请求对应的渲染实例（升序）。
    struct RequestCache {
        std::uint64_t sceneVersion = 0;
        const void* requestTable = nullptr;
        std::size_t requestCount = 0, assetCount = 0;
        std::uint64_t supported = 0, unsupported = 0;
        std::vector<std::uint32_t> requests, rendered;
    } requestCache;
};
[[nodiscard]] Status CreateAnimationPrem(ModelAnimationService::State& state, const AnimationResourceReader& read);
[[nodiscard]] Status PrepareAnimationPrem(ModelAnimationService::State& state, AnimationFrame& frame,
    const AnimationFrame* previous, const AnimationAdvance& advance);
} // namespace War3Map::Render
