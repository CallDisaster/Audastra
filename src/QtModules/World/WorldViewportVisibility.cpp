#include "WorldViewport.h"
#include <QThread>
#include <algorithm>
#include <limits>

namespace War3Map::Modules {
bool WorldViewport::HasEditorVisibilitySupport() const noexcept {
    return api_ && api_->apiVersion == WAR3_RENDER_PLUGIN_API_V2
        && api_->structSize >= offsetof(War3RenderPluginApiV2, submitEditorModelScene) + sizeof(api_->submitEditorModelScene)
        && api_->getEditorPresentationTarget && api_->submitEditorVisibility && api_->captureEditorPresentationFrameRgba
        && api_->submitEditorModelScene;
}
void WorldViewport::SetInstanceVisible(std::function<bool(std::uint32_t)> predicate) {
    ++pickPolicyRevision_; animationPickHistory_.clear();
    bugPreparationHistory_.clear(); bugPreparationFailure_ = {};
    instanceVisible_ = std::move(predicate);
    animationVisibilityCache_ = {};
    editorVisibilityDirty_ = true;
}
bool WorldViewport::ReadEditorPresentationTarget(War3RenderEditorPresentationTargetV2& output) const {
    output = {sizeof(output)};
    if (!renderer_ || !HasEditorVisibilitySupport()) return false;
    const auto result = api_->getEditorPresentationTarget(renderer_, &output);
    if (result != 0) ReportPluginFailure("getEditorPresentationTarget", result);
    return result == 0 && output.structSize >= sizeof(output) && output.modelEpoch && output.mode <= WAR3_EDITOR_PRESENTATION_PARTICLES
        && output.particleDrawCount <= 65536;
}
bool WorldViewport::BuildEditorVisibility(const Render::WorldModelScene* models, const Render::AnimationFrame* frame,
    EditorVisibilityBuffers& output) const try {
    EditorVisibilityBuffers candidate;
    const auto visible = [this](std::uint32_t request) {
        return request == Render::kBakedInstance || !instanceVisible_ || instanceVisible_(request);
    };
    if (frame) {
        // 场景服务已核算 owned 帧字节；显隐掩码不能再按地图实例总数拒绝。
        if (frame->instances.size() > std::numeric_limits<std::uint32_t>::max() || frame->particleDraws.size() > 65536
            || frame->poses.size() != frame->instances.size()) return false;
        if (!frame->requestVisibility.empty() && (frame->requestVisibility.size()!=frame->states.size()
            || std::any_of(frame->requestVisibility.begin(),frame->requestVisibility.end(),[](auto value) { return value>1; }))) return false;
        auto& cache=animationVisibilityCache_;
        const bool rebuild=!cache.valid || cache.sceneVersion!=frame->sceneVersion || cache.documentRevision!=frame->documentRevision
            || cache.renderedRequests.size()!=frame->instances.size() || cache.editorRequests.size()!=frame->states.size();
        if (rebuild) {
            AnimationVisibilityCache next; next.sceneVersion=frame->sceneVersion; next.documentRevision=frame->documentRevision;
            next.editorRequests.reserve(frame->states.size());
            for (std::size_t request=0; request<frame->states.size(); ++request)
                next.editorRequests.push_back(static_cast<std::uint8_t>(visible(static_cast<std::uint32_t>(request))));
            next.renderedRequests.assign(frame->instances.size(),Render::kBakedInstance);
            std::vector<std::uint8_t> assigned(frame->instances.size(),0);
            for (const auto& pose:frame->poses) {
                if (pose.renderedInstance>=frame->instances.size() || assigned[pose.renderedInstance]
                    || (pose.requestIndex!=Render::kBakedInstance && pose.requestIndex>=frame->states.size())) return false;
                assigned[pose.renderedInstance]=1;
                next.renderedRequests[pose.renderedInstance]=pose.requestIndex;
                next.poseRendered.push_back(pose.renderedInstance); next.poseRequests.push_back(pose.requestIndex);
            }
            cache=std::move(next);
        } else {
            // 同场景帧的映射固定，仍逐项核验；不用每帧分配去重表或调用宿主选择服务。
            for (std::size_t i=0; i<frame->poses.size(); ++i)
                if (frame->poses[i].renderedInstance!=cache.poseRendered[i] || frame->poses[i].requestIndex!=cache.poseRequests[i]) return false;
        }
        if (!cache.valid || cache.requestVisibility!=frame->requestVisibility) {
            cache.instances.resize(frame->instances.size());
            for (std::size_t i=0; i<cache.instances.size(); ++i) {
                const auto request=cache.renderedRequests[i];
                cache.instances[i]=static_cast<std::uint8_t>(request==Render::kBakedInstance
                    || (cache.editorRequests[request] && (frame->requestVisibility.empty() || frame->requestVisibility[request])));
            }
            cache.requestVisibility=frame->requestVisibility; cache.valid=true;
        }
        candidate.instances=cache.instances;
        candidate.particleDraws.assign(frame->particleDraws.size(), 1);
        std::vector<std::uint8_t> assignedDraw(frame->particleDraws.size(), 0);
        for (const auto& link : frame->particleLinks) {
            if (link.firstDraw > candidate.particleDraws.size() || link.drawCount > candidate.particleDraws.size() - link.firstDraw) return false;
            if (link.requestIndex!=Render::kBakedInstance && link.requestIndex>=cache.editorRequests.size()) return false;
            const auto show = !link.outsideView && (link.requestIndex==Render::kBakedInstance || cache.editorRequests[link.requestIndex]);
            for (auto draw = link.firstDraw; draw < link.firstDraw + link.drawCount; ++draw) {
                if (assignedDraw[draw]) return false;
                assignedDraw[draw] = 1; candidate.particleDraws[draw] = static_cast<std::uint8_t>(show);
            }
        }
    } else if (models) {
        if (models->instances.size() > std::numeric_limits<std::uint32_t>::max()
            || models->instanceRequests.size() != models->instances.size()) return false;
        candidate.instances.reserve(models->instances.size());
        for (const auto request : models->instanceRequests) candidate.instances.push_back(static_cast<std::uint8_t>(visible(request)));
    }
    output = std::move(candidate);
    return true;
} catch (...) { return false; }
bool WorldViewport::SubmitStaticVisibility(const Render::WorldModelScene* models, const War3RenderModelSceneV2& scene) {
    EditorVisibilityBuffers buffers;
    War3RenderEditorPresentationTargetV2 target{};
    if (!BuildEditorVisibility(models, nullptr, buffers) || !ReadEditorPresentationTarget(target)
        || editorPresentationRevision_ == std::numeric_limits<std::uint64_t>::max()) return false;
    const auto revision = editorPresentationRevision_ + 1;
    const War3RenderEditorVisibilityUpdateV2 mask{sizeof(mask), static_cast<std::uint32_t>(buffers.instances.size()), 0, 0,
        target, revision, buffers.instances.data(), nullptr, nullptr, nullptr, nullptr};
    const War3RenderEditorModelSceneV2 presented{sizeof(presented), 0, &scene, nullptr, nullptr, &mask};
    const auto result = api_->submitEditorModelScene(renderer_, &presented);
    if (result != 0) { ReportPluginFailure("submitEditorModelScene", result); return false; }
    ++target.modelEpoch; target.mode = WAR3_EDITOR_PRESENTATION_STATIC;
    target.sceneVersion = target.contentVersion = 0; target.instanceCount = mask.instanceCount; target.particleDrawCount = 0;
    editorPresentationTarget_ = target; editorPresentationRevision_ = revision; editorVisibilityDirty_ = false;
    return true;
}
bool WorldViewport::UpdateStaticVisibility(bool transforms) {
    EditorVisibilityBuffers buffers;
    War3RenderEditorPresentationTargetV2 target{};
    if (!BuildEditorVisibility(models_.get(), nullptr, buffers) || !ReadEditorPresentationTarget(target)
        || target.mode != WAR3_EDITOR_PRESENTATION_STATIC || target.instanceCount != buffers.instances.size()
        || editorPresentationRevision_ == std::numeric_limits<std::uint64_t>::max()) return false;
    const auto revision = editorPresentationRevision_ + 1;
    const auto* instances = transforms && models_ && !models_->instances.empty() ? models_->instances.data() : nullptr;
    const War3RenderEditorVisibilityUpdateV2 mask{sizeof(mask), static_cast<std::uint32_t>(buffers.instances.size()), 0, 0,
        target, revision, buffers.instances.data(), nullptr, nullptr, nullptr, instances};
    const auto result = api_->submitEditorVisibility(renderer_, &mask);
    if (result != 0) { ReportPluginFailure("submitEditorVisibility", result); return false; }
    if (instances) ++target.modelEpoch;
    editorPresentationTarget_ = target; editorPresentationRevision_ = revision; editorVisibilityDirty_ = false;
    return true;
}
bool WorldViewport::RefreshEditorVisibility() try {
    if (QThread::currentThread() != thread()) return false;
    ++pickPolicyRevision_; animationPickHistory_.clear();
    bugPreparationHistory_.clear(); bugPreparationFailure_ = {};
    animationVisibilityCache_ = {}; // 宿主显隐修订已变化，下一次构建重新读取 predicate。
    editorVisibilityDirty_ = true;
    if (!renderer_) { SetStatus(tr("显隐已记录，渲染器尚未创建")); return false; }
    if (!HasEditorVisibilitySupport()) { SetStatus(tr("当前渲染插件不支持编辑器显隐")); return false; }
    bool accepted = false;
    if (animationFrame_) {
        EditorVisibilityBuffers buffers;
        if (animationRenderer_ && BuildEditorVisibility(nullptr, animationFrame_.get(), buffers)
            && editorPresentationRevision_ != std::numeric_limits<std::uint64_t>::max()) {
            const auto masks = buffers.Describe(editorPresentationRevision_ + 1);
            accepted = animationRenderer_->Present(masks).Succeeded();
            if (accepted) {
                editorPresentationTarget_ = animationRenderer_->PresentationTarget();
                editorPresentationRevision_ = animationRenderer_->PresentationRevision(); editorVisibilityDirty_ = false;
                RetainAnimationPickFrame(); // 暂停时显隐更新也要拥有对应完成票据，不能等下一热帧。
            }
        }
    } else accepted = UpdateStaticVisibility(false);
    if (!accepted) SetStatus(tr("显隐掩码与当前模型或姿势不一致，更新未接收"));
    else if (selectedInstance_ && models_ && *selectedInstance_ < models_->instanceRequests.size()
        && models_->instanceRequests[*selectedInstance_] != Render::kBakedInstance
        && instanceVisible_ && !instanceVisible_(models_->instanceRequests[*selectedInstance_])) {
        CancelMove(); selectedInstance_.reset();
    }
    return accepted;
} catch (...) { editorVisibilityDirty_ = true; SetStatus(tr("显隐回调失败，呈现状态未确认")); return false; }
} // namespace War3Map::Modules
