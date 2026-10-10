#include "War3MapCore/Render/AnimationPreviewBudget.h"
#include <algorithm>
#include <limits>

namespace War3Map::Render {
std::string_view AnimationPreviewQualityName(AnimationPreviewQuality quality) noexcept {
    switch (quality) {
    case AnimationPreviewQuality::Complete: return "complete";
    case AnimationPreviewQuality::Balanced: return "balanced";
    case AnimationPreviewQuality::PowerSaving: return "power-saving";
    }
    return {};
}
std::optional<AnimationPreviewQuality> AnimationPreviewQualityFromName(std::string_view name) noexcept {
    if (name == "complete") return AnimationPreviewQuality::Complete;
    if (name == "balanced") return AnimationPreviewQuality::Balanced;
    if (name == "power-saving") return AnimationPreviewQuality::PowerSaving;
    return std::nullopt;
}
bool AnimationPreviewBudgetController::Full() const noexcept {
    return temporaryFull_ || quality_ == AnimationPreviewQuality::Complete;
}
std::uint32_t AnimationPreviewBudgetController::InitialLimit() const noexcept {
    return quality_ == AnimationPreviewQuality::PowerSaving ? 256u : 2048u;
}
std::uint32_t AnimationPreviewBudgetController::MaximumLimit() const noexcept {
    return quality_ == AnimationPreviewQuality::PowerSaving ? 2048u : 8192u;
}
void AnimationPreviewBudgetController::SetQuality(AnimationPreviewQuality quality) noexcept {
    if (quality != AnimationPreviewQuality::Complete && quality != AnimationPreviewQuality::Balanced &&
        quality != AnimationPreviewQuality::PowerSaving) return;
    if (quality_ == quality) return;
    quality_ = quality;
    const bool temporary = temporaryFull_;
    ResetMeasurements(); temporaryFull_ = temporary;
}
void AnimationPreviewBudgetController::SetTemporaryFull(bool enabled) noexcept {
    temporaryFull_ = enabled; slow_ = fast_ = 0;
}
void AnimationPreviewBudgetController::ResetMeasurements() noexcept {
    temporaryFull_ = measured_ = false; limit_ = InitialLimit(); slow_ = fast_ = 0;
    lastFrame_ = lastContent_ = lastSampleNs_ = lastDecisionNs_ = 0;
    completedCpuNs_ = animationCpuNs_ = smoothedCostNs_ = 0; gpuDrawNs_.reset();
    omittedInstances_ = omittedEmitters_ = 0;
}
void AnimationPreviewBudgetController::SetOmissions(std::size_t instances, std::size_t emitters) noexcept {
    omittedInstances_ = instances; omittedEmitters_ = emitters;
}
AnimationParticleFrameBudget AnimationPreviewBudgetController::ParticleBudget(std::uint32_t frameSteps) const noexcept {
    if (Full()) return {};
    const auto anchors = std::uint64_t(limit_) * (1ull + frameSteps);
    return {limit_, static_cast<std::uint32_t>(std::min<std::uint64_t>(anchors, kMaximumPreviewAnchorSamples))};
}
AnimationPreviewBudgetStatus AnimationPreviewBudgetController::Status() const noexcept {
    AnimationPreviewBudgetStatus result;
    result.quality = quality_; result.temporaryFull = temporaryFull_; result.hasCompletedFrameSample = measured_;
    const auto budget = ParticleBudget(); result.instanceLimit = budget.maxInstances; result.anchorSampleLimit = budget.maxAnchorSamples;
    result.budgetOmittedInstances = omittedInstances_; result.budgetOmittedEmitters = omittedEmitters_;
    result.completedCpuNs = completedCpuNs_; result.animationCpuNs = animationCpuNs_; result.gpuDrawNs = gpuDrawNs_;
    result.smoothedCostNs = smoothedCostNs_;
    result.targetCostNs = quality_ == AnimationPreviewQuality::PowerSaving ? 16666667ull : 33333333ull;
    result.minimumIntervalMs = quality_ == AnimationPreviewQuality::PowerSaving && !temporaryFull_ ? 66u : 33u;
    return result;
}
bool AnimationPreviewBudgetController::Observe(const AnimationPreviewBudgetSample& sample) noexcept {
    if (!sample.completed || !sample.visible || !sample.frameNumber || !sample.contentVersion || !sample.completedSteadyNs ||
        sample.frameNumber <= lastFrame_ || sample.contentVersion <= lastContent_ || sample.completedSteadyNs <= lastSampleNs_ ||
        (!sample.completedCpuNs && !sample.gpuDrawNs.value_or(0))) return false;
    lastFrame_ = sample.frameNumber; lastContent_ = sample.contentVersion; lastSampleNs_ = sample.completedSteadyNs;
    completedCpuNs_ = sample.completedCpuNs; animationCpuNs_ = sample.animationCpuNs; gpuDrawNs_ = sample.gpuDrawNs;
    // CPU求值与绘制可能重叠，取实际成本的最大值；不把upload CPU计时冒称GPU计时。
    const auto cost = std::max({completedCpuNs_, animationCpuNs_, gpuDrawNs_.value_or(0)});
    if (!measured_) smoothedCostNs_ = cost;
    else if (cost >= smoothedCostNs_) smoothedCostNs_ += (cost - smoothedCostNs_) / 8;
    else smoothedCostNs_ -= (smoothedCostNs_ - cost) / 8;
    measured_ = true;
    if (Full()) return false;
    const auto target = Status().targetCostNs;
    if (smoothedCostNs_ > target + target / 5) { slow_ = std::min(slow_ + 1, 3u); fast_ = 0; }
    else if (smoothedCostNs_ < target * 3 / 4 && omittedInstances_) { fast_ = std::min(fast_ + 1, 12u); slow_ = 0; }
    else slow_ = fast_ = 0;
    if (lastDecisionNs_ && sample.completedSteadyNs - lastDecisionNs_ < 250000000ull) return false;
    const auto previous = limit_;
    if (slow_ >= 3) limit_ = std::max(32u, limit_ - std::max(1u, limit_ / 4));
    else if (fast_ >= 12) limit_ = std::min(MaximumLimit(), limit_ + std::max(1u, limit_ / 4));
    if (limit_ == previous) return false;
    lastDecisionNs_ = sample.completedSteadyNs; slow_ = fast_ = 0;
    return true;
}
} // namespace War3Map::Render
