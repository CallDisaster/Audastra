#include "War3MapCore/Render/AnimationRenderer.h"
#include "War3MapCore/RenderPluginMaterialState.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace War3Map::Render {
namespace {
Status Failure(const char* code, const char* text) { return Status::Failure("Render.Animation",code,text); }
Status CheckMaterialFrame(const War3RenderPluginApiV2* api,War3RendererHandle handle,
    const AnimatedModelScene& scene,const AnimationFrame& frame) {
    if (scene.dynamicMaterialAlphas!=frame.dynamicMaterialAlphas)
        return Failure("ANIMATION_LAYER_ALPHA_MAPPING","场景与 owned 帧的显式材质模式不同。");
    if (scene.dynamicMaterialStates!=frame.dynamicMaterialStates || (scene.dynamicMaterialStates && !scene.dynamicMaterialAlphas))
        return Failure("ANIMATION_MATERIAL_STATE_MAPPING","场景与owned帧的材质状态模式或绝对alpha模式不一致。");
    if (!scene.dynamicMaterialAlphas) return Status::Ok();
    if (frame.layerSlices.size()!=frame.instances.size() || frame.dynamicColors!=scene.dynamicColors || frame.layerAlphas.size()>1048576)
        return Failure("ANIMATION_LAYER_ALPHA_MAPPING","当前材质片段、实例或 RGB 模式不属于同一 owned 帧。");
    War3RenderSkinMaterialCapabilitiesV2 capabilities{}; capabilities.base.base.base.base.structSize=sizeof(capabilities);
    if (!api->getSkinStatus || api->getSkinStatus(handle,&capabilities.base.base.base.base)!=0 ||
        capabilities.layerAlphaFormat!=WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1 || capabilities.reserved || capabilities.base.reserved)
        return Failure("ANIMATION_MATERIAL_ALPHA_BACKEND_UNSUPPORTED","插件未声明 KMTA 动态层透明度能力，未以静态层替代。");
    if (scene.dynamicMaterialStates) {
        if (frame.materialStateSlices.size()!=frame.instances.size() || frame.materialStates.size()>1048576)
            return Failure("ANIMATION_MATERIAL_STATE_MAPPING","材质状态片段数量不属于当前实例帧。");
        War3RenderSkinMaterialStateCapabilitiesV2 stateCaps{}; stateCaps.base.base.base.base.base.structSize=sizeof(stateCaps);
        if (api->getSkinStatus(handle,&stateCaps.base.base.base.base.base)!=0
            || stateCaps.materialStateFormat!=WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1 || stateCaps.reserved)
            return Failure("ANIMATION_MATERIAL_STATE_BACKEND_UNSUPPORTED","插件未声明动态材质状态能力，未静默使用旧贴图。");
    }
    return Status::Ok();
}
bool ReadPresentationTarget(const War3RenderPluginApiV2* api, War3RendererHandle handle,
    War3RenderEditorPresentationTargetV2& target) {
    target = {sizeof(target)};
    return api->getEditorPresentationTarget(handle, &target) == 0 && target.structSize >= sizeof(target)
        && target.modelEpoch && target.mode <= WAR3_EDITOR_PRESENTATION_PARTICLES
        && std::uint64_t(target.instanceCount)*sizeof(War3RenderSkinnedModelInstanceV2)<=64ull*1024*1024 && target.particleDrawCount <= 65536;
}
bool VisibilityMatchesFrame(const EditorVisibilityMasks& masks, const AnimationFrame& frame) {
    return masks.presentationRevision && masks.instances.size() == frame.instances.size()
        && masks.particleDraws.size() == frame.particleDraws.size()
        && masks.instances.size()<=UINT32_MAX && masks.particleDraws.size() <= 65536;
}
bool TargetMatchesFrame(const War3RenderEditorPresentationTargetV2& target, const AnimationFrame& frame) {
    return target.sceneVersion == frame.sceneVersion && target.contentVersion == frame.contentVersion
        && target.instanceCount == frame.instances.size() && target.particleDrawCount == frame.particleDraws.size();
}
bool SameTarget(const War3RenderEditorPresentationTargetV2& a, const War3RenderEditorPresentationTargetV2& b) {
    return a.mode == b.mode && a.modelEpoch == b.modelEpoch && a.sceneVersion == b.sceneVersion
        && a.contentVersion == b.contentVersion && a.instanceCount == b.instanceCount && a.particleDrawCount == b.particleDrawCount;
}
bool SameResidentAssets(const AnimatedModelScene& a, const AnimatedModelScene& b) {
    return a.generation == b.generation && a.sourceIdentity == b.sourceIdentity && a.dynamicColors == b.dynamicColors
        && a.dynamicMaterialAlphas==b.dynamicMaterialAlphas
        && a.dynamicMaterialStates==b.dynamicMaterialStates
        && a.vertices.SharesStorageWith(b.vertices) && a.meshes.SharesStorageWith(b.meshes)
        && a.models.SharesStorageWith(b.models) && a.meshColors.SharesStorageWith(b.meshColors)
        && a.drawLayerBindings.SharesStorageWith(b.drawLayerBindings) && a.modelLayerRanges.SharesStorageWith(b.modelLayerRanges)
        && a.geometry.vertices.SharesStorageWith(b.geometry.vertices)
        && a.geometry.indices.SharesStorageWith(b.geometry.indices)
        && a.geometry.layers.SharesStorageWith(b.geometry.layers)
        && a.geometry.meshes.SharesStorageWith(b.geometry.meshes)
        && a.geometry.models.SharesStorageWith(b.geometry.models)
        && a.geometry.textures.SharesStorageWith(b.geometry.textures)
        && a.geometry.vertexGroups.SharesStorageWith(b.geometry.vertexGroups)
        && a.particleTextures.SharesStorageWith(b.particleTextures);
}
template<class T>
bool SamePrefix(const SharedSceneVector<T>& before, const SharedSceneVector<T>& after) {
    if (before.size() > after.size()) return false;
    if (before.SharesStorageWith(after) || before.empty()) return true;
    if constexpr (std::is_trivially_copyable_v<T>) {
        // COW复制的驻留描述保留原对象表示；插件另行逐字段检查新suffix。
        return std::memcmp(before.data(), after.data(), before.size() * sizeof(T)) == 0;
    } else {
        for (std::size_t i = 0; i < before.size(); ++i) {
            if (before[i].path != after[i].path || before[i].width != after[i].width
                || before[i].height != after[i].height || before[i].rgba != after[i].rgba) return false;
        }
        return true;
    }
}
bool SameDrawLayerPrefix(const AnimatedModelScene& before, const AnimatedModelScene& after) {
    if (SamePrefix(before.drawLayerBindings,after.drawLayerBindings)) return true;
    if (before.dynamicMaterialStates || !after.dynamicMaterialStates || before.drawLayerBindings.size()>after.drawLayerBindings.size())
        return false;
    // 仅首次显式KMTF模式可将旧固定WRAP元数据升级为已验证的真实TEXS flags。
    // 编译层身份/预览alpha逐位不变；GPU静态几何、层及资源前缀仍由原检查完整保护。
    for (std::size_t i=0;i<before.drawLayerBindings.size();++i) {
        const auto& old=before.drawLayerBindings[i]; const auto& next=after.drawLayerBindings[i];
        if (old.materialLayer!=next.materialLayer || std::memcmp(&old.previewAlphaScale,&next.previewAlphaScale,sizeof(float)) ||
            (old.staticSamplerFlags!=next.staticSamplerFlags && (old.staticSamplerFlags!=3 || next.staticSamplerFlags>3)))
            return false;
    }
    return true;
}
bool SameAssetPrefix(const AnimatedModelScene& before, const AnimatedModelScene& after) {
    if (!after.meshColors.empty() && after.meshColors.size() != after.meshes.size()) return false;
    if (before.meshColors.empty() && !after.meshColors.empty()) {
        for (std::size_t i = 0; i < before.meshes.size(); ++i)
            for (const float channel : after.meshColors[i].rgb) if (channel != 1.0f) return false;
    }
    return before.generation == after.generation && before.sourceIdentity == after.sourceIdentity
        && (!before.dynamicColors || after.dynamicColors)
        && (!before.dynamicMaterialAlphas || after.dynamicMaterialAlphas)
        && (!before.dynamicMaterialStates || after.dynamicMaterialStates)
        && SamePrefix(before.vertices, after.vertices) && SamePrefix(before.meshes, after.meshes)
        && SamePrefix(before.models, after.models) && SamePrefix(before.meshColors, after.meshColors)
        && SameDrawLayerPrefix(before,after) && SamePrefix(before.modelLayerRanges,after.modelLayerRanges)
        && SamePrefix(before.geometry.vertices, after.geometry.vertices)
        && SamePrefix(before.geometry.indices, after.geometry.indices)
        && SamePrefix(before.geometry.layers, after.geometry.layers)
        && SamePrefix(before.geometry.meshes, after.geometry.meshes)
        && SamePrefix(before.geometry.models, after.geometry.models)
        && SamePrefix(before.geometry.vertexGroups, after.geometry.vertexGroups)
        && SamePrefix(before.geometry.textures, after.geometry.textures)
        && SamePrefix(before.particleTextures, after.particleTextures);
}
template<class T> const auto* SuffixData(const T& values, std::size_t prefix) {
    return prefix < values.size() ? values.data() + prefix : nullptr;
}
std::uint32_t Count32(std::size_t size) {
    if (size > UINT32_MAX) throw std::length_error("asset suffix count exceeds ABI range");
    return static_cast<std::uint32_t>(size);
}
War3RenderEditorVisibilityUpdateV2 DescribeVisibility(const War3RenderEditorPresentationTargetV2& target,
    const EditorVisibilityMasks& masks) {
    return {sizeof(War3RenderEditorVisibilityUpdateV2), static_cast<std::uint32_t>(masks.instances.size()),
        static_cast<std::uint32_t>(masks.particleDraws.size()), 0, target, masks.presentationRevision,
        masks.instances.data(), masks.particleDraws.data(), nullptr, nullptr, nullptr};
}
}
bool AnimationRenderer::Supported(const War3RenderPluginApiV2* api) noexcept {
    return api && api->structSize >= offsetof(War3RenderPluginApiV2,captureSkinnedFrameRgba)+sizeof(api->captureSkinnedFrameRgba) &&
        api->apiVersion == WAR3_RENDER_PLUGIN_API_V2 && api->submitSkinnedModelScene && api->updateSkinMatrices && api->getSkinStatus && api->captureSkinnedFrameRgba;
}
bool AnimationRenderer::ParticleSupported(const War3RenderPluginApiV2* api) noexcept {
    return Supported(api) && api->structSize >= offsetof(War3RenderPluginApiV2,captureParticleFrameRgba)+sizeof(api->captureParticleFrameRgba) &&
        api->submitParticleModelScene && api->updateParticleModelFrame && api->captureParticleFrameRgba;
}
bool AnimationRenderer::PresentationSupported(const War3RenderPluginApiV2* api) noexcept {
    return Supported(api) && api->structSize >= offsetof(War3RenderPluginApiV2, submitEditorModelScene) + sizeof(api->submitEditorModelScene)
        && api->getEditorPresentationTarget && api->submitEditorVisibility && api->captureEditorPresentationFrameRgba && api->submitEditorModelScene;
}
bool AnimationRenderer::RosterSupported(const War3RenderPluginApiV2* api) noexcept {
    return PresentationSupported(api)
        && api->structSize >= offsetof(War3RenderPluginApiV2, getEditorInstanceRosterResult) + sizeof(api->getEditorInstanceRosterResult)
        && api->submitEditorInstanceRoster && api->getEditorInstanceRosterResult;
}
bool AnimationRenderer::AssetAppendSupported(const War3RenderPluginApiV2* api, War3RendererHandle handle) noexcept {
    if (!handle || !RosterSupported(api)
        || api->structSize < offsetof(War3RenderPluginApiV2, getEditorRosterCapabilities) + sizeof(api->getEditorRosterCapabilities)
        || !api->getEditorRosterCapabilities) return false;
    std::uint32_t capabilities = 0;
    return api->getEditorRosterCapabilities(handle, &capabilities) == 0
        && (capabilities & WAR3_RENDER_ROSTER_APPEND_ASSETS) != 0;
}
Result<std::uint64_t> AnimationRenderer::QueueRoster(std::shared_ptr<const AnimatedModelScene> scene,
    std::shared_ptr<const AnimationFrame> frame, const EditorVisibilityMasks& masks, bool assetsChanged) try {
    using Output = Result<std::uint64_t>;
    if (!handle_ || !RosterSupported(api_)) return Output::Failure("Render.Animation", "ANIMATION_ROSTER_UNSUPPORTED", "插件没有完整驻留实例事务尾部。");
    if (pendingRosterTicket_) return Output::Failure("Render.Animation", "ANIMATION_ROSTER_PENDING", "前一实例事务尚未完成，旧Target保持。");
    if (!scene_ || !frame_ || !scene || !frame || frame->sceneVersion != scene->sceneVersion
        || frame->documentRevision != scene->documentRevision || scene->sceneVersion <= scene_->sceneVersion
        || !VisibilityMatchesFrame(masks, *frame)) return Output::Failure("Render.Animation", "ANIMATION_ROSTER_STALE", "候选场景、帧或掩码身份无效。");
    if (assetsChanged ? (!AssetAppendSupported(api_, handle_) || !SameAssetPrefix(*scene_, *scene))
        : !SameResidentAssets(*scene_, *scene))
        return Output::Failure("Render.Animation", "ANIMATION_ROSTER_ASSET_CHANGE_UNSUPPORTED", "插件不支持资产追加，或候选改变了驻留资产前缀；旧帧保留。");
    War3RenderInstanceRosterResultV2 base{}; base.structSize = sizeof(base);
    if (api_->getEditorInstanceRosterResult(handle_, 0, &base) != 0 || base.structSize < sizeof(base)
        || base.state != WAR3_RENDER_ROSTER_TARGET || base.result || base.reserved || !base.assetIdentity
        || base.target.structSize < sizeof(base.target) || !base.target.modelEpoch
        || base.target.mode != (particleMode_ ? WAR3_EDITOR_PRESENTATION_PARTICLES : WAR3_EDITOR_PRESENTATION_SKINNED)
        || (presentationTarget_ && !SameTarget(base.target, *presentationTarget_))
        || !TargetMatchesFrame(base.target, *frame_) || base.presentationRevision != presentationRevision_)
        return Output::Failure("Render.Animation", "ANIMATION_ROSTER_STALE", "当前CPU驻留基准已被替换，未提交实例事务。");
    if (auto status=CheckMaterialFrame(api_,handle_,*scene,*frame); !status.Succeeded()) return {std::nullopt,std::move(status.diagnostics)};
    auto update = frame->Describe(); auto colored = frame->DescribeColored(); auto material=frame->DescribeMaterial(); auto state=frame->DescribeMaterialState();
    const auto* pose = scene->dynamicMaterialStates ? &state.base.base.base : scene->dynamicMaterialAlphas ? &material.base.base : scene->dynamicColors ? &colored.base : &update;
    const bool wantParticles = particleMode_ || (assetsChanged && scene->particlesEnabled && scene->particleInstanceCount);
    const auto particles = frame->DescribeParticles(pose);
    auto mask = DescribeVisibility(base.target, masks);
    War3RenderEditorInstanceRosterV2 request{}; request.structSize = sizeof(request); request.expected = base.target;
    request.expectedPresentationRevision = base.presentationRevision; request.expectedAssetIdentity = base.assetIdentity;
    request.expectedLayersRevision = base.layersRevision; request.candidatePose = pose;
    request.candidateParticles = wantParticles ? &particles : nullptr;
    request.visibility = &mask;
    War3RenderEditorAssetRosterV2 extended{};
    War3RenderAssetAppendV2 append{};
    ModelSceneDescriptor modelTextures, particleTextures;
    if (assetsChanged) {
        if (base.assetIdentity == UINT64_MAX)
            return Output::Failure("Render.Animation", "ANIMATION_ASSET_IDENTITY_EXHAUSTED", "资产身份编号耗尽，未提交事务。");
        append.structSize = sizeof(append);
        append.prefix = {Count32(scene_->vertices.size()), Count32(scene_->geometry.indices.size()),
            Count32(scene_->geometry.layers.size()), Count32(scene_->meshes.size()), Count32(scene_->models.size()),
            Count32(scene_->geometry.textures.size()), Count32(scene_->particleTextures.size())};
        append.vertexCount = Count32(scene->vertices.size() - scene_->vertices.size());
        append.indexCount = Count32(scene->geometry.indices.size() - scene_->geometry.indices.size());
        append.layerCount = Count32(scene->geometry.layers.size() - scene_->geometry.layers.size());
        append.meshCount = Count32(scene->meshes.size() - scene_->meshes.size());
        append.modelCount = Count32(scene->models.size() - scene_->models.size());
        append.modelTextureCount = Count32(scene->geometry.textures.size() - scene_->geometry.textures.size());
        append.particleTextureCount = Count32(scene->particleTextures.size() - scene_->particleTextures.size());
        append.colorCount = scene->meshColors.empty() ? 0 : append.meshCount;
        append.vertices = SuffixData(scene->vertices, scene_->vertices.size());
        append.indices = SuffixData(scene->geometry.indices, scene_->geometry.indices.size());
        append.layers = SuffixData(scene->geometry.layers, scene_->geometry.layers.size());
        append.meshes = SuffixData(scene->meshes, scene_->meshes.size());
        append.models = SuffixData(scene->models, scene_->models.size());
        append.colors = append.colorCount ? SuffixData(scene->meshColors, scene_->meshes.size()) : nullptr;
        modelTextures = DescribeModelTextures(std::span<const ModelTextureRgba>(scene->geometry.textures.data(), scene->geometry.textures.size()).subspan(scene_->geometry.textures.size()));
        particleTextures = DescribeModelTextures(std::span<const ModelTextureRgba>(scene->particleTextures.data(), scene->particleTextures.size()).subspan(scene_->particleTextures.size()));
        append.modelTextures = append.modelTextureCount ? modelTextures.chains.data() : nullptr;
        append.particleTextures = append.particleTextureCount ? particleTextures.chains.data() : nullptr;
        extended.base = request; extended.base.structSize = sizeof(extended);
        extended.base.flags = WAR3_RENDER_ROSTER_APPEND_ASSETS; extended.assets = &append;
    }
    std::uint64_t ticket = 0;
    if (api_->submitEditorInstanceRoster(handle_, assetsChanged ? &extended.base : &request, &ticket) != 0 || !ticket)
        return Output::Failure("Render.Animation", "ANIMATION_ROSTER_REJECTED", "插件拒绝驻留实例事务，旧Target未改变。");
    pendingRosterTicket_ = ticket; pendingAssetIdentity_ = base.assetIdentity + (assetsChanged ? 1 : 0);
    pendingPresentationRevision_ = masks.presentationRevision;
    pendingLayersRevision_ = base.layersRevision + 1;
    pendingTarget_ = base.target; ++pendingTarget_.modelEpoch;
    pendingTarget_.mode = wantParticles ? WAR3_EDITOR_PRESENTATION_PARTICLES : WAR3_EDITOR_PRESENTATION_SKINNED;
    pendingTarget_.sceneVersion = frame->sceneVersion; pendingTarget_.contentVersion = frame->contentVersion;
    pendingTarget_.instanceCount = static_cast<std::uint32_t>(frame->instances.size());
    pendingTarget_.particleDrawCount = static_cast<std::uint32_t>(frame->particleDraws.size());
    pendingScene_ = std::move(scene); pendingFrame_ = std::move(frame);
    return Output::Success(ticket);
} catch (const std::exception&) { return Result<std::uint64_t>::Failure("Render.Animation", "ANIMATION_ROSTER_FAILED", "实例事务准备失败，未替换Target。"); }
Result<War3RenderInstanceRosterResultV2> AnimationRenderer::CancelRoster(std::uint64_t ticket) {
    using Output = Result<War3RenderInstanceRosterResultV2>;
    if (!handle_ || !ticket || ticket != pendingRosterTicket_ || !RosterSupported(api_))
        return Output::Failure("Render.Animation", "ANIMATION_ROSTER_TICKET_STALE", "待取消实例票据已失效，未释放候选。");
    if (api_->structSize < offsetof(War3RenderPluginApiV2, cancelEditorInstanceRoster) + sizeof(api_->cancelEditorInstanceRoster)
        || !api_->cancelEditorInstanceRoster)
        return Output::Failure("Render.Animation", "ANIMATION_ROSTER_CANCEL_UNSUPPORTED", "旧插件不支持确切票据取消，须保留候选并等待终态。");
    War3RenderInstanceRosterResultV2 result{}; result.structSize = sizeof(result);
    if (api_->cancelEditorInstanceRoster(handle_, ticket, &result) != 0 || result.structSize < sizeof(result)
        || result.reserved || result.ticket != ticket || result.state < WAR3_RENDER_ROSTER_QUEUED
        || result.state > WAR3_RENDER_ROSTER_SUPERSEDED)
        return Output::Failure("Render.Animation", "ANIMATION_ROSTER_CANCEL_FAILED", "实例取消未取得可信回执，须保留候选并等待终态。");
    // 沿原Poll的同完成帧身份门发布或释放；不凭cancel返回绕过严格完成回执检查。
    return result.state == WAR3_RENDER_ROSTER_QUEUED ? Output::Success(result) : PollRoster(ticket);
}
Result<War3RenderInstanceRosterResultV2> AnimationRenderer::PollRoster(std::uint64_t ticket) {
    using Output = Result<War3RenderInstanceRosterResultV2>;
    if (!ticket || !RosterSupported(api_)) return Output::Failure("Render.Animation", "ANIMATION_ROSTER_UNSUPPORTED", "实例票据或插件尾部不可用。");
    War3RenderInstanceRosterResultV2 result{}; result.structSize = sizeof(result);
    if (api_->getEditorInstanceRosterResult(handle_, ticket, &result) != 0 || result.structSize < sizeof(result)
        || result.reserved || result.ticket != ticket || result.state < WAR3_RENDER_ROSTER_QUEUED
        || result.state > WAR3_RENDER_ROSTER_SUPERSEDED)
        return Output::Failure("Render.Animation", "ANIMATION_ROSTER_RESULT_INVALID", "实例票据查询失败或回执格式无效。");
    if (ticket == pendingRosterTicket_ && result.state == WAR3_RENDER_ROSTER_COMPLETED) {
        const auto& completed = result.completed;
        const auto& presentation = completed.presentation;
        if (result.result || result.assetIdentity != pendingAssetIdentity_ || !pendingFrame_
            || result.layersRevision != pendingLayersRevision_ || result.target.structSize < sizeof(result.target)
            || !SameTarget(result.target, pendingTarget_)
            || result.presentationRevision != pendingPresentationRevision_ || !TargetMatchesFrame(result.target, *pendingFrame_)
            || completed.structSize < sizeof(completed) || completed.view.structSize < sizeof(completed.view)
            || !completed.view.frameCount || !completed.view.surfaceEpoch || !completed.view.sceneEpoch || !completed.view.viewEpoch
            || completed.camera.structSize < sizeof(completed.camera) || completed.skin.structSize < sizeof(completed.skin)
            || completed.skin.active != 1 || completed.skin.result || completed.skin.frameCount != completed.view.frameCount
            || completed.skin.sceneVersion != pendingFrame_->sceneVersion || completed.skin.contentVersion != pendingFrame_->contentVersion
            || presentation.structSize < sizeof(presentation) || presentation.reserved
            || presentation.modelEpoch != result.target.modelEpoch || presentation.mode != result.target.mode
            || presentation.sceneVersion != result.target.sceneVersion || presentation.contentVersion != result.target.contentVersion
            || presentation.presentationRevision != pendingPresentationRevision_
            || presentation.instanceCount != pendingFrame_->instances.size()
            || presentation.particleDrawCount != pendingFrame_->particleDraws.size()
            || presentation.visibleInstanceCount > presentation.instanceCount
            || presentation.visibleParticleDrawCount > presentation.particleDrawCount
            || (result.target.mode == WAR3_EDITOR_PRESENTATION_PARTICLES
                && (completed.particles.structSize < sizeof(completed.particles) || completed.particles.reserved
                    || completed.particles.sceneVersion != pendingFrame_->sceneVersion
                    || completed.particles.contentVersion != pendingFrame_->contentVersion
                    || completed.particles.vertexCount != pendingFrame_->particleVertices.size()
                    || completed.particles.drawCount != pendingFrame_->particleDraws.size())))
            return Output::Failure("Render.Animation", "ANIMATION_ROSTER_COMPLETION_STALE", "完成帧与实例票据身份不同，未发布新Target。");
        scene_ = std::move(pendingScene_); frame_ = std::move(pendingFrame_);
        presentationTarget_ = result.target; presentationRevision_ = result.presentationRevision;
        particleMode_ = result.target.mode == WAR3_EDITOR_PRESENTATION_PARTICLES;
        pendingRosterTicket_ = pendingAssetIdentity_ = pendingPresentationRevision_ = pendingLayersRevision_ = 0;
        pendingTarget_ = {};
    } else if (ticket == pendingRosterTicket_ && result.state != WAR3_RENDER_ROSTER_QUEUED) {
        pendingScene_.reset(); pendingFrame_.reset(); pendingRosterTicket_ = pendingAssetIdentity_ = pendingPresentationRevision_ = pendingLayersRevision_ = 0;
        pendingTarget_ = {};
    }
    return Output::Success(result);
}
Status AnimationRenderer::Submit(std::shared_ptr<const AnimatedModelScene> scene,
    std::shared_ptr<const AnimationFrame> frame, const EditorVisibilityMasks* visibility) try {
    if (pendingRosterTicket_) return Failure("ANIMATION_ROSTER_PENDING", "实例事务尚未完成，未提交另一场景。");
    if (!handle_ || !Supported(api_)) return Failure("ANIMATION_BACKEND_UNSUPPORTED","渲染插件没有矩阵组蒙皮尾部接口。");
    if (!scene || !frame || scene->sceneVersion!=frame->sceneVersion || scene->documentRevision!=frame->documentRevision)
        return Failure("ANIMATION_FRAME_STALE","静态场景与初始帧的身份不一致。");
    if (auto status=CheckMaterialFrame(api_,handle_,*scene,*frame); !status.Succeeded()) return status;
    if (frame->sharedPoseRanges) {
        War3RenderSkinSharedRangeCapabilitiesV2 capabilities{}; capabilities.base.base.base.structSize=sizeof(capabilities);
        if (api_->getSkinStatus(handle_,&capabilities.base.base.base)!=0 ||
            capabilities.sharedRangeMatrixFormat!=WAR3_RENDER_GROUP_LOCAL_COLUMN_3X4_SHARED_RANGES || capabilities.reserved)
            return Failure("ANIMATION_SHARED_RANGES_UNSUPPORTED","插件未声明共享 Stand 姿势片段能力；旧静态场景保留。");
    }
    War3RenderEditorPresentationTargetV2 target{};
    if (visibility && (!PresentationSupported(api_) || !ReadPresentationTarget(api_, handle_, target)))
        return Failure("ANIMATION_PRESENTATION_UNSUPPORTED", "渲染插件没有完整编辑器显隐接口，未提交掩码或替代透明度。");
    if (visibility && !VisibilityMatchesFrame(*visibility, *frame))
        return Failure("ANIMATION_PRESENTATION_INVALID", "显隐掩码数量或修订与实际动画帧不一致。");
    const bool declaresParticles=std::any_of(scene->particleCapabilities.begin(),scene->particleCapabilities.end(),
        [](const auto& c) {return c.particle2Count || c.ribbonCount || c.premCount;});
    if (frame->instances.empty() && declaresParticles && !(scene->particlesEnabled && scene->particleInstanceCount &&
        std::any_of(scene->particleCapabilities.begin(),scene->particleCapabilities.end(),[](const auto& c) {
            return c.available && (c.particle2Count || c.ribbonCount);
        })))
        return Failure("ANIMATION_SCENE_EMPTY","没有可绘制模型或受支持的粒子实例，未提交空场景。");
    AnimatedSceneDescriptor descriptor; descriptor.Refresh(*scene,*frame);
    if (!scene->meshColors.empty() && scene->meshColors.size()!=scene->meshes.size())
        return Failure("ANIMATION_COLOR_MAPPING","静态颜色数量与网格数量不一致。");
    bool nonWhite=false;
    for (const auto& color:scene->meshColors) for (const float component:color.rgb) {
        if (!std::isfinite(component) || component<0.0f || component>1.0f)
            return Failure("MDX_GEOA_COLOR_INVALID","网格活动颜色必须是有限的 0..1 RGB。");
        nonWhite=nonWhite || component!=1.0f;
    }
    War3RenderColoredSkinnedSceneV2 colored{};
    const War3RenderSkinnedModelSceneV2* submission=&descriptor.scene;
    if (nonWhite) {
        // 必须查询显式能力，不能以旧插件忽略尾部为由把彩色模型画成白色。
        War3RenderSkinColorCapabilitiesV2 capabilities{};
        capabilities.base.structSize=sizeof(capabilities);
        if (api_->getSkinStatus(handle_,&capabilities.base)!=0 ||
            capabilities.colorFormat!=WAR3_RENDER_STATIC_MESH_RGB_V1 || capabilities.reserved!=0)
            return Failure("ANIMATION_COLOR_BACKEND_UNSUPPORTED","渲染插件不支持 GEOA 静态 RGB；旧模型保留。");
        colored.base=descriptor.scene; colored.base.structSize=sizeof(colored);
        colored.colorFormat=WAR3_RENDER_STATIC_MESH_RGB_V1;
        colored.colorCount=static_cast<std::uint32_t>(scene->meshColors.size());
        colored.colors=scene->meshColors.data(); submission=&colored.base;
    }
    if (scene->dynamicColors) {
        War3RenderSkinDynamicColorCapabilitiesV2 capabilities{}; capabilities.base.base.structSize=sizeof(capabilities);
        if (frame->colors.size()!=frame->alphas.size()) return Failure("ANIMATION_COLOR_MAPPING","动态RGB和透明度数量不符。");
        if (api_->getSkinStatus(handle_,&capabilities.base.base)!=0 ||
            capabilities.dynamicColorFormat!=WAR3_RENDER_DYNAMIC_MESH_RGB_V1 || capabilities.reserved!=0)
            return Failure("ANIMATION_DYNAMIC_COLOR_BACKEND_UNSUPPORTED","渲染插件不支持KGAC动态RGB，旧目标保留。");
    }
    if (scene->particlesEnabled && scene->particleInstanceCount) {
        if (!ParticleSupported(api_))
            return Failure("ANIMATION_PARTICLE_BACKEND_UNSUPPORTED","渲染插件没有完整粒子尾部，组合场景未提交。");
        const auto textures=DescribeModelTextures(scene->particleTextures);
        const auto update=frame->DescribeParticles(submission->initialPose);
        const War3RenderParticleSceneV2 particles{sizeof(particles),static_cast<std::uint32_t>(textures.chains.size()),
            textures.chains.data(),submission,&update};
        auto mask = visibility ? DescribeVisibility(target, *visibility) : War3RenderEditorVisibilityUpdateV2{};
        const War3RenderEditorModelSceneV2 presented{sizeof(presented), 0, nullptr, nullptr, &particles, &mask};
        const auto submitted = visibility ? api_->submitEditorModelScene(handle_, &presented) : api_->submitParticleModelScene(handle_,&particles);
        if (submitted != 0)
            return Status::Failure("Render.Animation", "ANIMATION_SCENE_REJECTED",
                "渲染插件拒绝模型与粒子组合场景，旧目标保留。 result=" + std::to_string(submitted)
                + " pose_bytes=" + std::to_string(update.modelPose->structSize)
                + " material_alpha=" + std::to_string(scene->dynamicMaterialAlphas)
                + " dynamic_rgb=" + std::to_string(scene->dynamicColors));
    } else {
        auto mask = visibility ? DescribeVisibility(target, *visibility) : War3RenderEditorVisibilityUpdateV2{};
        const War3RenderEditorModelSceneV2 presented{sizeof(presented), 0, nullptr, submission, nullptr, &mask};
        if ((visibility ? api_->submitEditorModelScene(handle_, &presented) : api_->submitSkinnedModelScene(handle_,submission))!=0)
            return Failure("ANIMATION_SCENE_REJECTED","渲染插件拒绝模型静态场景，旧目标保留。");
    }
    presentationTarget_.reset(); presentationRevision_ = 0;
    if (visibility) {
        ++target.modelEpoch; target.sceneVersion = frame->sceneVersion; target.contentVersion = frame->contentVersion;
        target.instanceCount = static_cast<std::uint32_t>(frame->instances.size());
        target.particleDrawCount = static_cast<std::uint32_t>(frame->particleDraws.size());
        target.mode = scene->particlesEnabled && scene->particleInstanceCount ? WAR3_EDITOR_PRESENTATION_PARTICLES : WAR3_EDITOR_PRESENTATION_SKINNED;
        presentationTarget_ = target; presentationRevision_ = visibility->presentationRevision;
    }
    particleMode_ = scene->particlesEnabled && scene->particleInstanceCount;
    scene_=std::move(scene); frame_=std::move(frame); return {};
} catch (const std::exception&) { return Failure("ANIMATION_SUBMIT_FAILED","动画提交失败，未宣称上传成功。"); }
Status AnimationRenderer::Update(std::shared_ptr<const AnimationFrame> frame, const EditorVisibilityMasks* visibility) {
    if (pendingRosterTicket_) return Failure("ANIMATION_ROSTER_PENDING", "实例事务尚未完成，旧Target保持。");
    if (!frame_ || !frame || frame->sceneVersion!=frame_->sceneVersion || frame->contentVersion<=frame_->contentVersion ||
        frame->documentRevision!=frame_->documentRevision) return Failure("ANIMATION_FRAME_STALE","动态更新的场景、修订或版本无效。");
    if (auto status=CheckMaterialFrame(api_,handle_,*scene_,*frame); !status.Succeeded()) return status;
    auto update=frame->Describe();
    auto colored=frame->DescribeColored(); auto material=frame->DescribeMaterial(); auto state=frame->DescribeMaterialState();
    if (scene_->dynamicColors && frame->colors.size()!=frame->alphas.size())
        return Failure("ANIMATION_COLOR_MAPPING","动态RGB和透明度数量不符。");
    const auto* pose=scene_->dynamicMaterialStates ? &state.base.base.base : scene_->dynamicMaterialAlphas ? &material.base.base : scene_->dynamicColors ? &colored.base : &update;
    if (visibility) {
        War3RenderEditorPresentationTargetV2 target{};
        if (!PresentationSupported(api_) || !ReadPresentationTarget(api_, handle_, target))
            return Failure("ANIMATION_PRESENTATION_UNSUPPORTED", "渲染插件没有完整编辑器显隐接口。");
        if (!TargetMatchesFrame(target, *frame_) || !VisibilityMatchesFrame(*visibility, *frame))
            return Failure("ANIMATION_PRESENTATION_STALE", "当前渲染目标或显隐掩码已被新的动画帧取代。");
        auto mask = DescribeVisibility(target, *visibility);
        const auto particles = frame->DescribeParticles(pose);
        if (particleMode_) mask.particleFrame = &particles;
        else mask.modelPose = pose;
        if (api_->submitEditorVisibility(handle_, &mask) != 0)
            return Failure("ANIMATION_UPDATE_REJECTED", "渲染插件拒绝姿势与显隐组合更新，旧目标保留。");
        target.contentVersion = frame->contentVersion; target.particleDrawCount = static_cast<std::uint32_t>(frame->particleDraws.size());
        presentationTarget_ = target; presentationRevision_ = visibility->presentationRevision;
    } else if (particleMode_) {
        const auto particles=frame->DescribeParticles(pose);
        if (!ParticleSupported(api_) || api_->updateParticleModelFrame(handle_,&particles)!=0)
            return Failure("ANIMATION_UPDATE_REJECTED","渲染插件拒绝模型与粒子组合更新，旧目标保留。");
    } else if (api_->updateSkinMatrices(handle_,pose)!=0)
        return Failure("ANIMATION_UPDATE_REJECTED","渲染插件拒绝矩阵更新，旧目标保留。");
    if (!visibility) { presentationTarget_.reset(); presentationRevision_ = 0; }
    frame_=std::move(frame); return {};
}
Status AnimationRenderer::Present(const EditorVisibilityMasks& visibility) {
    if (pendingRosterTicket_) return Failure("ANIMATION_ROSTER_PENDING", "实例事务尚未完成，旧Target保持。");
    if (!handle_ || !frame_ || !PresentationSupported(api_))
        return Failure("ANIMATION_PRESENTATION_UNSUPPORTED", "没有动画目标或完整编辑器显隐接口。");
    War3RenderEditorPresentationTargetV2 target{};
    if (!ReadPresentationTarget(api_, handle_, target) || !TargetMatchesFrame(target, *frame_)
        || !VisibilityMatchesFrame(visibility, *frame_))
        return Failure("ANIMATION_PRESENTATION_STALE", "显隐目标的模型、姿势或批次数量已被取代。");
    const auto mask = DescribeVisibility(target, visibility);
    if (api_->submitEditorVisibility(handle_, &mask) != 0)
        return Failure("ANIMATION_PRESENTATION_REJECTED", "渲染插件拒绝显隐掩码，旧呈现保留。");
    presentationTarget_ = target; presentationRevision_ = visibility.presentationRevision;
    return {};
}
Result<War3RenderSkinStatusV2> AnimationRenderer::StatusNow() const {
    if (!handle_ || !Supported(api_)) return Result<War3RenderSkinStatusV2>::Failure("Render.Animation",
        "ANIMATION_BACKEND_UNSUPPORTED","渲染插件没有蒙皮状态接口。");
    War3RenderSkinStatusV2 status{}; status.structSize=sizeof(status);
    if (api_->getSkinStatus(handle_,&status)!=0 || status.structSize < sizeof(status) || status.result<0)
        return Result<War3RenderSkinStatusV2>::Failure("Render.Animation","ANIMATION_GPU_FAILED","动画 GPU 上传或绘制失败。");
    return Result<War3RenderSkinStatusV2>::Success(status);
}
Result<AnimationCaptureReceipt> AnimationRenderer::Capture(std::span<std::byte> rgba, std::uint64_t viewRevision) const {
    using Output=Result<AnimationCaptureReceipt>;
    if (!frame_ || !Supported(api_) || rgba.empty()) return Output::Failure("Render.Animation",
        "ANIMATION_CAPTURE_UNAVAILABLE","没有已提交动画目标或可用读回缓冲。");
    War3RenderSkinnedCapturedFrameV2 captured{}; captured.structSize=sizeof(captured);
    std::optional<War3RenderParticleStatusV2> particles;
    std::optional<War3RenderEditorPresentationStatusV2> presentation;
    if (presentationTarget_) {
        War3RenderEditorCapturedFrameV2 combined{}; combined.structSize = sizeof(combined);
        if (!PresentationSupported(api_) || api_->captureEditorPresentationFrameRgba(handle_, presentationTarget_->modelEpoch,
            presentationRevision_, rgba.data(), rgba.size(), &combined) != 0)
            return Output::Failure("Render.Animation", "ANIMATION_PRESENTATION_CAPTURE_FAILED", "指定显隐修订的组合完成帧未读回或已被取代。");
        const auto& status = combined.presentation;
        if (combined.structSize < sizeof(combined) || status.structSize < sizeof(status) || status.reserved
            || status.mode != presentationTarget_->mode || status.modelEpoch != presentationTarget_->modelEpoch
            || status.presentationRevision != presentationRevision_ || status.sceneVersion != frame_->sceneVersion
            || status.contentVersion != frame_->contentVersion || status.instanceCount != frame_->instances.size()
            || status.particleDrawCount != frame_->particleDraws.size() || status.visibleInstanceCount > status.instanceCount
            || status.visibleParticleDrawCount > status.particleDrawCount || status.drawCallCount > 262144)
            return Output::Failure("Render.Animation", "ANIMATION_PRESENTATION_CAPTURE_STALE", "显隐、实例或姿势身份与同次完成帧不一致。");
        captured = {sizeof(captured), combined.frame, combined.skin}; presentation = status;
        if (particleMode_) particles = combined.particles;
    } else if (particleMode_) {
        War3RenderParticleCapturedFrameV2 combined{}; combined.structSize=sizeof(combined);
        if (!ParticleSupported(api_) || api_->captureParticleFrameRgba(handle_,rgba.data(),rgba.size(),&combined)!=0)
            return Output::Failure("Render.Animation","ANIMATION_CAPTURE_FAILED","模型与粒子组合完成帧未读回或已被更新取代。");
        if (combined.structSize < sizeof(combined))
            return Output::Failure("Render.Animation", "ANIMATION_CAPTURE_STALE", "粒子组合回执结构过短。");
        captured=combined.models; particles=combined.particles;
    } else if (api_->captureSkinnedFrameRgba(handle_,rgba.data(),rgba.size(),&captured)!=0)
        return Output::Failure("Render.Animation","ANIMATION_CAPTURE_FAILED","完成帧未读回或已被更新取代。");
    const auto& image = captured.frame.capture;
    const auto& view = captured.frame.view;
    const auto& camera = captured.frame.camera;
    const float cameraFields[]{camera.targetX,camera.targetY,camera.targetZ,camera.distance,camera.yawRadians,camera.pitchRadians};
    if (captured.structSize < sizeof(captured) || captured.frame.structSize < sizeof(captured.frame) ||
        image.structSize < sizeof(image) || view.structSize < sizeof(view) || camera.structSize < sizeof(camera) ||
        captured.skin.structSize < sizeof(captured.skin) || image.width == 0 || image.height == 0 ||
        image.width > 4096 || image.height > 4096 || static_cast<std::uint64_t>(image.width) * image.height * 4 > rgba.size() ||
        image.width != view.width || image.height != view.height || !image.frameCount || !image.ticket ||
        view.documentRevision != viewRevision || !view.surfaceEpoch || !view.sceneEpoch || !view.viewEpoch ||
        !std::all_of(std::begin(view.viewProjection),std::end(view.viewProjection),[](float v){return std::isfinite(v);}) ||
        !std::all_of(std::begin(cameraFields),std::end(cameraFields),[](float v){return std::isfinite(v);}) ||
        captured.skin.active != 1 || captured.skin.result!=0 || captured.skin.sceneVersion!=frame_->sceneVersion ||
        captured.skin.contentVersion!=frame_->contentVersion || captured.frame.capture.documentRevision!=viewRevision ||
        captured.skin.frameCount!=captured.frame.capture.frameCount || captured.frame.view.frameCount!=captured.skin.frameCount)
        return Output::Failure("Render.Animation","ANIMATION_CAPTURE_STALE","像素、姿势内容或视图修订未属于同一完成帧。");
    if (particles) {
        const auto& p = *particles;
        if (p.structSize < sizeof(p) || p.reserved || p.sceneVersion != frame_->sceneVersion || p.contentVersion != frame_->contentVersion
            || p.vertexCount != frame_->particleVertices.size() || p.drawCount != frame_->particleDraws.size() || !p.dynamicUploadCount)
            return Output::Failure("Render.Animation", "ANIMATION_CAPTURE_STALE", "粒子数量、内容版本与组合完成帧不一致。");
        const auto actualCamera=AnimationWorldCamera(camera);
        if (!frame_->worldCamera || frame_->worldCamera->eye!=actualCamera.eye || frame_->worldCamera->up!=actualCamera.up)
            return Output::Failure("Render.Animation","ANIMATION_CAPTURE_STALE","粒子帧的求值相机与实际绘制相机不同，等待同相机重放后再捕获。");
    }
    AnimationCaptureReceipt receipt(captured); receipt.particles=particles; receipt.presentation=presentation;
    return Output::Success(std::move(receipt));
}
} // namespace War3Map::Render
