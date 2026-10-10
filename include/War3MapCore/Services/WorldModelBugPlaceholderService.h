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
// 宿主必须先成功getViewInfo取得当前sceneEpoch，再取同次完成metric；只核回执，不制造回执。
[[nodiscard]] bool WorldModelBugLayerFrameQualified(const War3RenderModelLayersTargetV2& before,
    const War3RenderModelLayersTargetV2& after, const Render::ViewProjectionFrame& currentView,
    const War3RenderFrameMetricsV2& metric) noexcept;

} // namespace War3Map::Services
