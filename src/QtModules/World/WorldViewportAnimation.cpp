#include "WorldViewport.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <QScopedValueRollback>

namespace War3Map::Modules {
Render::AnimationView WorldViewport::AnimationViewSnapshot() const {
    const auto size=PhysicalSize();
    Render::AnimationView view; view.camera=camera_;
    view.physicalWidth=static_cast<std::uint32_t>(size.width());
    view.physicalHeight=static_cast<std::uint32_t>(size.height());
    return view;
}
bool WorldViewport::HasAnimationSupport() const noexcept { return Render::AnimationRenderer::Supported(api_); }
bool WorldViewport::HasParticleSupport() const noexcept { return Render::AnimationRenderer::ParticleSupported(api_); }
bool WorldViewport::HasModelLayerSupport() const noexcept { return Render::ModelSceneLayerRenderer::Supported(api_); }
bool WorldViewport::HasInstanceRosterSupport() const noexcept { return Render::AnimationRenderer::RosterSupported(api_); }
void WorldViewport::NotifyAcceptedAnimationFrame() {
    // 仅父主层接受后调用；附加层 Set/Update 不回调，避免每帧提交形成递归。
    if (acceptedAnimationObserver_) acceptedAnimationObserver_(animationScene_, animationFrame_);
}
Result<std::uint64_t> WorldViewport::QueueAnimationRoster(std::shared_ptr<const Render::AnimatedModelScene> scene,
    std::shared_ptr<const Render::AnimationFrame> frame, std::shared_ptr<Render::WorldModelScene> picking,
    bool assetsChanged, std::function<bool(std::uint32_t)> visible) {
    using Output = Result<std::uint64_t>;
    if (!renderer_ || failed_ || !animationRenderer_ || pendingInstanceRoster_ || !scene || !frame || !picking
        || !HasInstanceRosterSupport() || scene->sceneVersion != frame->sceneVersion
        || scene->documentRevision != frame->documentRevision || picking->instances.size() != picking->instanceRequests.size())
        return Output::Failure("World.Instance", "WORLD_INSTANCE_ROSTER_UNAVAILABLE", "实例事务与当前渲染器或候选身份不匹配。");
    EditorVisibilityBuffers buffers;
    animationVisibilityCache_ = {};
    {
        QScopedValueRollback<std::function<bool(std::uint32_t)>> predicate(instanceVisible_, std::move(visible));
        if (!BuildEditorVisibility(nullptr, frame.get(), buffers))
            return Output::Failure("World.Instance", "WORLD_INSTANCE_VISIBILITY_INVALID", "新增名单的显隐与请求身份不匹配。");
    }
    if (editorPresentationRevision_ == std::numeric_limits<std::uint64_t>::max())
        return Output::Failure("World.Instance", "WORLD_INSTANCE_REVISION_EXHAUSTED", "实例呈现修订已耗尽。");
    const auto masks = buffers.Describe(editorPresentationRevision_ + 1);
    auto ticket = animationRenderer_->QueueRoster(scene, frame, masks, assetsChanged);
    if (!ticket.Succeeded()) { animationVisibilityCache_ = {}; return ticket; }
    pendingInstanceRoster_ = PendingInstanceRoster{*ticket.value, std::move(scene), std::move(frame), std::move(picking)};
    return ticket;
}
Result<War3RenderInstanceRosterResultV2> WorldViewport::CancelPendingAnimationRoster(std::uint64_t ticket) {
    using Output = Result<War3RenderInstanceRosterResultV2>;
    if (!animationRenderer_ || !pendingInstanceRoster_ || pendingInstanceRoster_->ticket != ticket)
        return Output::Failure("World.Instance", "WORLD_INSTANCE_TICKET_STALE", "待取消实例票据已经失效，未清除候选。");
    auto cancelled = animationRenderer_->CancelRoster(ticket);
    if (!cancelled.Succeeded() || cancelled.value->state == WAR3_RENDER_ROSTER_QUEUED) return cancelled;
    return PollAnimationRoster(ticket); // 已完成仍按原帧发布CPU/点选；拒绝/撤销才释放候选。
}
Result<War3RenderInstanceRosterResultV2> WorldViewport::PollAnimationRoster(std::uint64_t ticket) {
    using Output = Result<War3RenderInstanceRosterResultV2>;
    if (!animationRenderer_ || !pendingInstanceRoster_ || pendingInstanceRoster_->ticket != ticket)
        return Output::Failure("World.Instance", "WORLD_INSTANCE_TICKET_STALE", "待完成实例票据已经失效。");
    auto result = animationRenderer_->PollRoster(ticket);
    if (!result.Succeeded() || result.value->state == WAR3_RENDER_ROSTER_QUEUED) return result;
    if (result.value->state == WAR3_RENDER_ROSTER_COMPLETED) {
        CancelMove(); selectedInstance_.reset();
        models_ = std::move(pendingInstanceRoster_->picking);
        animationScene_ = std::move(pendingInstanceRoster_->scene);
        animationFrame_ = std::move(pendingInstanceRoster_->frame);
        editorPresentationTarget_ = animationRenderer_->PresentationTarget();
        editorPresentationRevision_ = animationRenderer_->PresentationRevision();
        animationPickingCurrent_ = true; editorVisibilityDirty_ = false;
        // 插件的主层epoch已变化；旧附加层桥不得向新父epoch提交旧票据。
        modelLayerBridges_.clear(); modelLayerBridgeHandle_ = renderer_;
        RetainAnimationPickFrame();
        NotifyAcceptedAnimationFrame();
        if (measurementEnabled_) emit MeasurementFrameAccepted();
    }
    pendingInstanceRoster_.reset(); animationVisibilityCache_ = {};
    return result;
}
Status WorldViewport::SetModelLayer(std::uint64_t id, std::shared_ptr<const Render::AnimatedModelScene> scene,
    std::shared_ptr<const Render::AnimationFrame> frame, std::uint32_t role) {
    if (!renderer_ || failed_ || !HasModelLayerSupport())
        return Status::Failure("World.ModelLayer", "WORLD_MODEL_LAYER_UNAVAILABLE", "附加模型层尚未就绪。");
    if (modelLayerBridgeHandle_ != renderer_) { modelLayerBridges_.clear(); modelLayerBridgeHandle_ = renderer_; }
    auto& binding = modelLayerBridges_[id];
    if (!binding.renderer) binding.renderer = std::make_unique<Render::ModelSceneLayerRenderer>(api_, renderer_, id);
    // 先准备来源快照；失败提交不能替换仍在 GPU 使用的旧绑定。
    auto source = scene ? Render::AnimationSession{scene->generation, scene->documentRevision, scene->sourceIdentity}
                        : Render::AnimationSession{};
    auto result = binding.renderer->Submit(scene, std::move(frame), role);
    if (result.Succeeded()) { binding.scene = std::move(scene); binding.source = std::move(source); }
    return result;
}
Status WorldViewport::UpdateModelLayer(std::uint64_t id, std::shared_ptr<const Render::AnimationFrame> frame) {
    const auto found = modelLayerBridges_.find(id);
    if (!renderer_ || failed_ || modelLayerBridgeHandle_ != renderer_ || found == modelLayerBridges_.end())
        return Status::Failure("World.ModelLayer", "WORLD_MODEL_LAYER_STALE", "附加模型层已失效。");
    return found->second.renderer->Update(std::move(frame));
}
Status WorldViewport::RemoveModelLayer(std::uint64_t id) {
    const auto found = modelLayerBridges_.find(id);
    if (found == modelLayerBridges_.end()) return {};
    if (renderer_ && !failed_ && modelLayerBridgeHandle_ == renderer_) {
        auto removed = found->second.renderer->Remove();
        if (!removed.Succeeded()) return removed;
    }
    modelLayerBridges_.erase(found); return {};
}
Status WorldViewport::RemovePremModelLayer(std::uint64_t id) {
    auto status = RemoveModelLayer(id);
    if (!status.Succeeded()) {
        animationFailure_ = Status::Failure("World.PREM", "WORLD_PREM_REMOVE_RENDERER_STOPPED",
            "PREM 旧子层无法清理，视口已停用；CPU 场景和地图保留，请重新加载视口。");
        ReportPluginFailure("removeModelSceneLayer.PREM", -1);
        failed_ = true;
        DestroyRenderer(); // 复用原有生命周期，不另建设备销毁或自动重启流程。
        SetStatus(tr("PREM 子层清理失败，视口已停用（WORLD_PREM_REMOVE_RENDERER_STOPPED）"));
        return animationFailure_;
    }
    return status;
}
void WorldViewport::SetBugPlaceholderPicking(std::uint64_t layer,
    std::shared_ptr<const Services::WorldModelBugPlaceholderPlan> plan,
    std::shared_ptr<const Render::AnimatedModelScene> scene, std::shared_ptr<const Render::AnimationFrame> frame,
    std::vector<MapData::PlacementId> owners, std::function<bool()> live, Services::WorldModelBugFlagsReader flags) {
    bugPlaceholderPicking_ = std::make_shared<const BugPlaceholderBinding>(BugPlaceholderBinding{layer,
        measurementRendererEpoch_, std::move(plan), std::move(scene), std::move(frame), std::move(owners), std::move(live), std::move(flags)});
}
void WorldViewport::ClearBugPlaceholderPicking() { bugPlaceholderPicking_.reset(); }
bool WorldViewport::HasBugPlaceholderPicking(const std::shared_ptr<const Services::WorldModelBugPlaceholderPlan>& plan) const {
    if (!bugPlaceholderPicking_ || bugPlaceholderPicking_->plan != plan
        || bugPlaceholderPicking_->rendererEpoch != measurementRendererEpoch_ || modelLayerBridgeHandle_ != renderer_) return false;
    const auto layer = modelLayerBridges_.find(bugPlaceholderPicking_->layer);
    return layer != modelLayerBridges_.end() && layer->second.scene == bugPlaceholderPicking_->scene
        && layer->second.renderer && layer->second.renderer->Target() == bugPlaceholderPicking_->frame;
}
void WorldViewport::InvalidateAnimationPicking() {
    ClearBugPlaceholderPicking();
    animationPickHistory_.clear();
    if (!animationScene_) return;
    animationPickingCurrent_ = false;
    CancelMove(); pressView_.reset(); pressModelPolicy_.reset();
    ClearCoordinates(tr("动画画面已冻结，只读；等待同来源的新模型场景"));
}
bool WorldViewport::SubmitAnimation() {
    if (!renderer_) return true;
    std::unique_ptr<Render::AnimationRenderer> bridge;
    if (!SubmitAnimation(animationScene_, animationFrame_, bridge)) return false;
    animationRenderer_ = std::move(bridge);
    RetainAnimationPickFrame();
    NotifyAcceptedAnimationFrame();
    return true;
}
bool WorldViewport::SubmitAnimation(std::shared_ptr<const Render::AnimatedModelScene> scene,
    std::shared_ptr<const Render::AnimationFrame> frame, std::unique_ptr<Render::AnimationRenderer>& output) {
    const auto reject = [this](const char* code, const char* message) {
        animationFailure_ = Status::Failure("World.Animation.Submit", code, message); return false;
    };
    auto bridge=std::make_unique<Render::AnimationRenderer>(api_,renderer_);
    EditorVisibilityBuffers buffers;
    const bool presented = HasEditorVisibilitySupport();
    if ((presented || instanceVisible_) && !BuildEditorVisibility(nullptr, frame.get(), buffers))
        return reject("WORLD_ANIMATION_VISIBILITY_INVALID", "场景显隐掩码未能与实例身份对应。");
    if (!presented && instanceVisible_ && (std::find(buffers.instances.begin(), buffers.instances.end(), 0) != buffers.instances.end()
        || std::find(buffers.particleDraws.begin(), buffers.particleDraws.end(), 0) != buffers.particleDraws.end())) {
        return reject("WORLD_ANIMATION_VISIBILITY_UNSUPPORTED", "当前渲染插件不支持编辑器显隐，未提交被隐藏的动画实例。");
    }
    if (presented && editorPresentationRevision_ == std::numeric_limits<std::uint64_t>::max())
        return reject("WORLD_ANIMATION_PRESENTATION_EXHAUSTED", "编辑器显隐修订已耗尽。");
    const auto masks = buffers.Describe(editorPresentationRevision_ + 1);
    auto status=bridge->Submit(scene,frame,presented ? &masks : nullptr);
    if (!status.Succeeded()) { animationFailure_ = std::move(status); return false; }
    editorPresentationTarget_.reset(); editorVisibilityDirty_ = false;
    if (presented) {
        editorPresentationTarget_ = bridge->PresentationTarget(); editorPresentationRevision_ = bridge->PresentationRevision();
    }
    output=std::move(bridge); return true;
}
bool WorldViewport::SetAnimationScene(std::shared_ptr<const Render::AnimatedModelScene> scene,
    std::shared_ptr<const Render::AnimationFrame> frame,std::shared_ptr<Render::WorldModelScene> picking) {
    animationFailure_ = {};
    const auto reject = [this](const char* code, const char* message) {
        animationFailure_ = Status::Failure("World.Animation.Install", code, message); return false;
    };
    if (pendingInstanceRoster_)
        return reject("WORLD_ANIMATION_ROSTER_PENDING", "实例事务尚未取得终态，保留拥有者并暂缓替换场景。");
    if (!scene || !frame || !picking || scene->sceneVersion!=frame->sceneVersion ||
        scene->documentRevision!=frame->documentRevision || frame->poses.size()!=frame->instances.size() ||
        picking->instances.size()!=picking->instanceRequests.size() || (api_ && !HasAnimationSupport()) ||
        (scene->particlesEnabled && scene->particleInstanceCount && !HasParticleSupport()))
        return reject("WORLD_ANIMATION_SCENE_INVALID", "场景、初始帧、点选身份或插件能力不匹配。");
    Render::AnimationPickingInstances(*scene,*frame,animationPickingScratch_);
    if (animationPickingScratch_.instanceRequests!=picking->instanceRequests
        || animationPickingScratch_.instances.size()!=picking->instances.size())
        return reject("WORLD_ANIMATION_PICK_MAPPING", "可见姿势与点选实例数量不一致。");
    std::unique_ptr<Render::AnimationRenderer> bridge;
    if (renderer_) {
        if (!SubmitAnimation(scene, frame, bridge)) return false;
    }
    CancelMove(); selectedInstance_.reset(); models_=std::move(picking);
    animationScene_=std::move(scene); animationFrame_=std::move(frame); animationRenderer_=std::move(bridge);
    animationPickingCurrent_ = true;
    RetainAnimationPickFrame();
    NotifyAcceptedAnimationFrame();
    if (measurementEnabled_) emit MeasurementFrameAccepted();
    return true;
}
bool WorldViewport::UpdateAnimationFrame(std::shared_ptr<const Render::AnimationFrame> frame,
    const std::shared_ptr<Render::WorldModelScene>& picking) try {
    using Clock = std::chrono::steady_clock;
    const auto nanoseconds = [](auto begin) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    };
    AnimationReceiveTiming timing;
    animationFailure_ = {};
    const auto reject = [this](const char* code, const char* message) {
        animationFailure_ = Status::Failure("World.Animation.Update", code, message); return false;
    };
    if (!animationPickingCurrent_ || !animationScene_ || !animationFrame_ || !frame || !picking || frame->sceneVersion!=animationFrame_->sceneVersion ||
        frame->documentRevision!=animationFrame_->documentRevision || frame->contentVersion<=animationFrame_->contentVersion ||
        frame->instances.size()!=animationFrame_->instances.size() || frame->poses.size()!=frame->instances.size())
        return reject("WORLD_ANIMATION_FRAME_STALE", "点选已失效，或待提交帧的场景、地图修订、内容版本、实例数量与已接收帧不匹配。");
    // 先准备过滤后的轻量点选场景；上传被拒绝时不改旧选择、请求映射或实例变换。
    timing.sceneVersion = frame->sceneVersion; timing.contentVersion = frame->contentVersion;
    const auto pickingStarted = Clock::now();
    Render::AnimationPickingInstances(*animationScene_,*frame,animationPickingScratch_);
    auto& next=animationPickingScratch_;
    std::optional<std::uint32_t> selected;
    if (selectedInstance_ && models_ && *selectedInstance_<models_->instanceRequests.size()) {
        const auto request=models_->instanceRequests[*selectedInstance_];
        if (request!=Render::kBakedInstance) {
            const auto found=std::find(next.instanceRequests.begin(),next.instanceRequests.end(),request);
            if (found!=next.instanceRequests.end()) selected=static_cast<std::uint32_t>(found-next.instanceRequests.begin());
        }
    }
    timing.pickingNs = nanoseconds(pickingStarted);
    if (renderer_) {
        if (!animationRenderer_) return reject("WORLD_ANIMATION_BRIDGE_MISSING", "当前渲染器没有已接收的动画提交桥。");
        const auto visibilityStarted = Clock::now();
        EditorVisibilityBuffers buffers;
        const bool presented = HasEditorVisibilitySupport();
        if ((presented || instanceVisible_) && !BuildEditorVisibility(nullptr, frame.get(), buffers))
            return reject("WORLD_ANIMATION_VISIBILITY_INVALID", "待更新帧显隐掩码未能与实例身份对应。");
        if (!presented && instanceVisible_ && (std::find(buffers.instances.begin(), buffers.instances.end(), 0) != buffers.instances.end()
            || std::find(buffers.particleDraws.begin(), buffers.particleDraws.end(), 0) != buffers.particleDraws.end()))
            return reject("WORLD_ANIMATION_VISIBILITY_UNSUPPORTED", "插件不支持待更新帧的编辑器显隐。");
        if (presented && editorPresentationRevision_ == std::numeric_limits<std::uint64_t>::max())
            return reject("WORLD_ANIMATION_PRESENTATION_EXHAUSTED", "编辑器显隐修订已耗尽。");
        const auto masks = buffers.Describe(editorPresentationRevision_ + 1);
        timing.visibilityNs = nanoseconds(visibilityStarted);
        const auto submitStarted = Clock::now();
        auto updated = animationRenderer_->Update(frame, presented ? &masks : nullptr);
        timing.submitNs = nanoseconds(submitStarted);
        if (!updated.Succeeded()) { animationFailure_ = std::move(updated); return false; }
        editorPresentationTarget_.reset(); editorVisibilityDirty_ = false;
        if (presented) {
            editorPresentationTarget_ = animationRenderer_->PresentationTarget(); editorPresentationRevision_ = animationRenderer_->PresentationRevision();
        }
    }
    // 保留 WorldModule/Player 共用的场景指针，只有可见实例与对应请求一起交换。
    const auto installStarted = Clock::now();
    picking->instances.swap(next.instances);
    picking->instanceRequests.swap(next.instanceRequests);
    picking->skippedInstances=next.skippedInstances;
    models_=picking; animationFrame_=std::move(frame);
    RetainAnimationPickFrame();
    NotifyAcceptedAnimationFrame();
    const bool hiddenSelection=selectedInstance_.has_value() && !selected.has_value();
    selectedInstance_=selected;
    if (hiddenSelection) CancelMove();
    timing.installNs = nanoseconds(installStarted);
    const auto observerStarted = Clock::now();
    if (measurementEnabled_) emit MeasurementFrameAccepted();
    timing.observerNs = nanoseconds(observerStarted);
    animationReceiveTimings_.push_back(timing);
    if (animationReceiveTimings_.size() > 512) animationReceiveTimings_.pop_front();
    return true;
} catch (const std::exception&) {
    animationFailure_ = Status::Failure("World.Animation.Update", "WORLD_ANIMATION_UPDATE_EXCEPTION",
        "准备或提交动画帧时发生异常；未接收该帧。");
    return false;
}
} // namespace War3Map::Modules
