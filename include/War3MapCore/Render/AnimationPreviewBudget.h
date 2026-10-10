#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace War3Map::Render {
enum class AnimationPreviewQuality { Complete, Balanced, PowerSaving };
[[nodiscard]] std::string_view AnimationPreviewQualityName(AnimationPreviewQuality quality) noexcept;
[[nodiscard]] std::optional<AnimationPreviewQuality> AnimationPreviewQualityFromName(std::string_view name) noexcept;

// 仅交互世界预览可显式申请；不更改严格回放、资产读取或64MiB帧内存合同。
// 锚点只按获准实例逐个求值，未预分配该数量的矩阵；节点访问仍受原硬预算限制。
inline constexpr std::uint32_t kMaximumPreviewAnchorSamples = 65536;
struct AnimationParticleFrameBudget {
    std::uint32_t maxInstances = UINT32_MAX;
    std::uint32_t maxAnchorSamples = kMaximumPreviewAnchorSamples;
};
struct AnimationPreviewBudgetSample {
    std::uint64_t frameNumber = 0, contentVersion = 0, completedSteadyNs = 0;
    std::uint64_t completedCpuNs = 0, animationCpuNs = 0;
    std::optional<std::uint64_t> gpuDrawNs;
    bool completed = false, visible = false;
};
struct AnimationPreviewBudgetStatus {
    AnimationPreviewQuality quality = AnimationPreviewQuality::Balanced;
    bool temporaryFull = false, hasCompletedFrameSample = false;
    std::uint32_t instanceLimit = 2048, anchorSampleLimit = 18432;
    std::size_t budgetOmittedInstances = 0, budgetOmittedEmitters = 0;
    std::uint64_t completedCpuNs = 0, animationCpuNs = 0, smoothedCostNs = 0;
    std::optional<std::uint64_t> gpuDrawNs;
    std::uint64_t targetCostNs = 33333333;
    std::uint32_t minimumIntervalMs = 33;
};
// 单所有者、Qt-free；只接收实际完成帧样本。计时不参与模拟/资产身份与数值指纹。
class AnimationPreviewBudgetController final {
public:
    void SetQuality(AnimationPreviewQuality quality) noexcept;
    void SetTemporaryFull(bool enabled) noexcept;
    // 换图时解除临时全开；持久化质量由宿主拥有，不随场景清理丢失。
    void ResetMeasurements() noexcept;
    void SetOmissions(std::size_t instances, std::size_t emitters) noexcept;
    [[nodiscard]] bool Observe(const AnimationPreviewBudgetSample& sample) noexcept;
    [[nodiscard]] AnimationParticleFrameBudget ParticleBudget(std::uint32_t frameSteps = 8) const noexcept;
    [[nodiscard]] AnimationPreviewBudgetStatus Status() const noexcept;
private:
    [[nodiscard]] bool Full() const noexcept;
    [[nodiscard]] std::uint32_t InitialLimit() const noexcept;
    [[nodiscard]] std::uint32_t MaximumLimit() const noexcept;
    AnimationPreviewQuality quality_ = AnimationPreviewQuality::Balanced;
    bool temporaryFull_ = false, measured_ = false;
    std::uint32_t limit_ = 2048, slow_ = 0, fast_ = 0;
    std::size_t omittedInstances_ = 0, omittedEmitters_ = 0;
    std::uint64_t lastFrame_ = 0, lastContent_ = 0, lastSampleNs_ = 0, lastDecisionNs_ = 0;
    std::uint64_t completedCpuNs_ = 0, animationCpuNs_ = 0, smoothedCostNs_ = 0;
    std::optional<std::uint64_t> gpuDrawNs_;
};
} // namespace War3Map::Render
