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
};
[[nodiscard]] Status CreateAnimationPrem(ModelAnimationService::State& state, const AnimationResourceReader& read);
[[nodiscard]] Status PrepareAnimationPrem(ModelAnimationService::State& state, AnimationFrame& frame,
    const AnimationFrame* previous, const AnimationAdvance& advance);
} // namespace War3Map::Render
