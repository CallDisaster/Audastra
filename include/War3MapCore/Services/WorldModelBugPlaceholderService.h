#pragma once
#include "War3MapCore/Services/WorldInstanceRegistry.h"
#include "War3MapCore/Services/WorldSelection.h"
#include "War3MapCore/RenderPluginFrameMetrics.h"
#include "War3MapCore/RenderPluginModelLayers.h"
#include "War3MapCore/Render/ViewRay.h"
#include "War3MapCore/Render/FrameCapture.h"

namespace War3Map::Services {

struct WorldModelBugPlaceholder {
    MapData::PlacementId owner;
    War3RenderCore::InstanceIdentity identity;
    std::uint32_t request = 0;
    std::array<float, 3> position{};
    std::string modelPath, reasonCode, reason;
    bool selectable = false;
    bool operator==(const WorldModelBugPlaceholder&) const = default;
};
struct WorldModelBugPlaceholderPlan {
    WorldInstanceBinding binding;
    std::shared_ptr<const WorldInstanceRegistry> registry;
    std::shared_ptr<const Render::AnimatedModelScene> sourceScene;
    std::uint64_t completedContentVersion = 0;
    std::vector<WorldModelBugPlaceholder> markers;
};
using WorldModelBugFlagsReader = std::function<std::optional<WorldEditorFlags>(const MapData::PlacementId&)>;
inline constexpr std::uint64_t WorldModelBugPlaceholderLayerId = 0x4255474d4f44454cull;
inline constexpr std::size_t kMaximumWorldModelBugSnapshotBytes = 16ull * 1024 * 1024;
struct WorldModelBugObservation {
    War3RenderCore::InstanceIdentity identity, stateIdentity;
    std::uint32_t request = 0;
    std::array<float, 3> position{};
    std::string reasonCode, reason;
    bool inView = true;
};
// 完整扫描父帧后的稀疏失败观察，不是AnimationFrame，也不拥有几何、骨骼或警告数组。
// 创建只证明CPU来源；宿主仍须用实际完成帧匹配scene/content/presentation后才能用于准备。
struct WorldModelBugPlaceholderFrameSnapshot {
    std::weak_ptr<const Render::AnimatedModelScene> sourceScene;
    std::uint64_t generation = 0, sceneVersion = 0, contentVersion = 0, documentRevision = 0;
    std::string sourceIdentity;
    std::size_t stateCount = 0, instanceCount = 0, ownedBytes = 0;
    std::vector<WorldModelBugObservation> observations;
};
[[nodiscard]] Result<WorldModelBugPlaceholderFrameSnapshot> CaptureWorldModelBugPlaceholderFrame(
    const std::shared_ptr<const Render::AnimatedModelScene>& scene, const Render::AnimationFrame& frame);
struct WorldModelBugPlaceholderCapturedFacts {
    std::uint64_t frameCount = 0, parentModelEpoch = 0, layersRevision = 0, layerRevision = 0;
    std::uint64_t layerSceneVersion = 0, layerContentVersion = 0;
    std::uint64_t preparedContentVersion = 0, sourceContentVersion = 0;
    std::uint64_t staticUploadCount = 0, matrixUploadCount = 0;
    std::uint32_t markerCount = 0;
    bool layerCaptured = false;
};
// capture须先由成功的CaptureCompletedFrame提供；这里只核同次读回与计划的身份/位置绑定，
// 不进行GPU调用，不以CPU计划或合成回执证明实际绘制。调用者另核live来源及当前重新分类结果。
[[nodiscard]] Result<WorldModelBugPlaceholderCapturedFacts> QualifyWorldModelBugPlaceholderCapture(
    const WorldModelBugPlaceholderPlan& plan, const Render::CapturedFrameRgba& capture);

// 纯CPU分类：frame必须由宿主的完成帧回执提供，函数自身不证明GPU完成。
// 只接受真实谱系；空字段、预算、视野外、正常全透明/纯粒子或局部GEOA不生成方块。
[[nodiscard]] Result<WorldModelBugPlaceholderPlan> PlanWorldModelBugPlaceholders(
    std::shared_ptr<const Render::AnimatedModelScene> scene, const Render::AnimationFrame& frame,
    std::shared_ptr<const WorldInstanceRegistry> registry, std::span<const MapData::PlacementId> owners,
    const WorldModelBugFlagsReader& flags);
[[nodiscard]] Result<WorldModelBugPlaceholderPlan> PlanWorldModelBugPlaceholders(
    std::shared_ptr<const Render::AnimatedModelScene> scene, const WorldModelBugPlaceholderFrameSnapshot& frame,
    std::shared_ptr<const WorldInstanceRegistry> registry, std::span<const MapData::PlacementId> owners,
    const WorldModelBugFlagsReader& flags);
// 只读场景没有可编辑谱系时，核对实际资产几何需求、完整同序姿势与普通请求覆盖。
// 正常纯粒子和宿主烘焙网格不制造摆放身份；局部组件问题不等于整模型占位需求。
// 不能产生占位、选择或编辑授权；捕获资格仍须核对同帧主层与附层清单。
[[nodiscard]] Result<WorldModelBugPlaceholderPlan> PlanWorldModelBugPlaceholderAbsence(
    std::shared_ptr<const Render::AnimatedModelScene> scene, const Render::AnimationFrame& frame);
// 宿主必须先成功getViewInfo取得当前sceneEpoch，再取同次完成metric；只核回执，不制造回执。
[[nodiscard]] bool WorldModelBugLayerFrameQualified(const War3RenderModelLayersTargetV2& before,
    const War3RenderModelLayersTargetV2& after, const Render::ViewProjectionFrame& currentView,
    const War3RenderFrameMetricsV2& metric) noexcept;

} // namespace War3Map::Services
