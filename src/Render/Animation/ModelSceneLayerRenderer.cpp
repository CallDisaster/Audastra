#include "War3MapCore/Render/ModelSceneLayerRenderer.h"
#include "AnimationParticleMaterial.h"
#include <limits>
#include <exception>

namespace War3Map::Render {
namespace {
Status Failure(const char* code, const char* message) { return Status::Failure("Render.ModelLayer", code, message); }
}
bool ModelSceneLayerRenderer::Supported(const War3RenderPluginApiV2* api) noexcept {
    return api && api->apiVersion == WAR3_RENDER_PLUGIN_API_V2
        && api->structSize >= offsetof(War3RenderPluginApiV2, captureLayeredFrameRgba) + sizeof(api->captureLayeredFrameRgba)
        && api->submitModelSceneLayer && api->updateModelSceneLayer && api->removeModelSceneLayer
        && api->getModelLayersTarget && api->captureLayeredFrameRgba;
}
Status ModelSceneLayerRenderer::Submit(std::shared_ptr<const AnimatedModelScene> scene,
    std::shared_ptr<const AnimationFrame> frame, std::uint32_t role) try {
    if (!Supported(api_) || !handle_ || !id_) return Failure("MODEL_LAYER_UNSUPPORTED", "渲染插件没有附加模型层接口。");
    if (role != WAR3_RENDER_MODEL_LAYER_MODEL && role != WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW)
        return Failure("MODEL_LAYER_ROLE_INVALID", "附加模型层角色未知。");
    if (role == WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW) {
        std::uint32_t flags = 0;
        if (api_->structSize < offsetof(War3RenderPluginApiV2, getModelLayerCapabilities) + sizeof(api_->getModelLayerCapabilities)
            || !api_->getModelLayerCapabilities || api_->getModelLayerCapabilities(handle_, &flags) != 0
            || !(flags & WAR3_RENDER_MODEL_LAYER_CAP_GROUND_DECAL_PREVIEW))
            return Failure("MODEL_LAYER_GROUND_DECAL_UNSUPPORTED", "插件没有地面贴画预览角色；未以共面普通层替代。");
    }
    if (!scene || !frame || scene->sceneVersion != frame->sceneVersion || scene->documentRevision != frame->documentRevision)
        return Failure("MODEL_LAYER_FRAME_STALE", "附加层场景和姿势身份不符。");
    if (auto status=CheckAnimationParticleMaterialFrame(*frame); !status.Succeeded()) return status;
    if (scene->dynamicMaterialAlphas) {
        War3RenderSkinMaterialCapabilitiesV2 capabilities{}; capabilities.base.base.base.base.structSize=sizeof(capabilities);
        if (!api_->getSkinStatus || api_->getSkinStatus(handle_,&capabilities.base.base.base.base)!=0 ||
            capabilities.layerAlphaFormat!=WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1 || capabilities.reserved)
            return Failure("ANIMATION_MATERIAL_ALPHA_BACKEND_UNSUPPORTED","插件没有 KMTA 动态层能力，附加层未以静态层替代。");
    }
    War3RenderModelLayersTargetV2 target{sizeof(target)};
    if (api_->getModelLayersTarget(handle_, &target) != 0 || !target.parentModelEpoch)
        return Failure("MODEL_LAYER_TARGET_UNAVAILABLE", "附加层主场景身份尚未就绪。");
    // 主场景完整替换会使所有旧层失效；不能将旧 expectedRevision 用在新主场景上。
    const auto expected = parentEpoch_ == target.parentModelEpoch ? revision_ : 0;
    if (expected && role_ != role) return Failure("MODEL_LAYER_ROLE_CHANGED", "同一附加层不能改变展示角色；须先移除。");
    if (expected == std::numeric_limits<std::uint64_t>::max()) return Failure("MODEL_LAYER_REVISION_EXHAUSTED", "附加层修订已耗尽。");
    AnimatedSceneDescriptor descriptor; descriptor.Refresh(*scene, *frame);
    War3RenderColoredSkinnedSceneV2 colored{};
    const auto* model = &descriptor.scene;
    if (!scene->meshColors.empty()) {
        colored.base = descriptor.scene; colored.base.structSize = sizeof(colored);
        colored.colorFormat = WAR3_RENDER_STATIC_MESH_RGB_V1;
        colored.colorCount = static_cast<std::uint32_t>(scene->meshColors.size());
        colored.colors = scene->meshColors.data(); model = &colored.base;
    }
    const bool particles = scene->particlesEnabled && scene->particleInstanceCount;
    const bool particleMaterial=particles && AnimationParticleMaterialSupported(api_,handle_);
    auto particleWarning=particles ? AnimationParticleMaterialWarning(*scene,particleMaterial) : Status::Ok();
    const auto textures = DescribeModelTextures(scene->particleTextures);
    const AnimationParticleDescriptor particleFrame(*frame,model->initialPose,particleMaterial);
    const War3RenderParticleSceneV2 particleScene{sizeof(particleScene), static_cast<std::uint32_t>(textures.chains.size()),
        textures.chains.data(), model, particleFrame.Get()};
    // 普通层发送旧prefix，保证具有旧附加层接口的插件仍可使用；新角色先协商。
    const War3RenderModelSceneLayerV2 input{role == WAR3_RENDER_MODEL_LAYER_MODEL
        ? WAR3_RENDER_MODEL_SCENE_LAYER_PREFIX_SIZE : static_cast<std::uint32_t>(sizeof(input)), 0, id_, target.parentModelEpoch, expected + 1, expected,
        particles ? nullptr : model, particles ? &particleScene : nullptr, role, 0};
    if (api_->submitModelSceneLayer(handle_, &input) != 0)
        return Failure("MODEL_LAYER_SUBMIT_REJECTED", "附加模型层未接收；主世界保持原状。");
    scene_ = std::move(scene); frame_ = std::move(frame); parentEpoch_ = target.parentModelEpoch; revision_ = expected + 1; role_ = role;
    return particleWarning;
} catch (const std::exception&) { return Failure("MODEL_LAYER_SUBMIT_FAILED", "附加模型层准备失败；未宣称上传成功。"); }
Status ModelSceneLayerRenderer::Update(std::shared_ptr<const AnimationFrame> frame) {
    if (!frame_ || !scene_ || !frame || frame->sceneVersion != frame_->sceneVersion
        || frame->documentRevision != frame_->documentRevision || frame->contentVersion <= frame_->contentVersion
        || revision_ == std::numeric_limits<std::uint64_t>::max())
        return Failure("MODEL_LAYER_FRAME_STALE", "附加模型层更新的身份或修订已过期。");
    if (auto status=CheckAnimationParticleMaterialFrame(*frame); !status.Succeeded()) return status;
    const auto pose = frame->Describe(); const auto colored = frame->DescribeColored(); const auto material=frame->DescribeMaterial();
    const auto* skin = scene_->dynamicMaterialAlphas ? &material.base.base : scene_->dynamicColors ? &colored.base : &pose;
    const bool hasParticles = scene_->particlesEnabled && scene_->particleInstanceCount;
    const bool particleMaterial=hasParticles && AnimationParticleMaterialSupported(api_,handle_);
    auto particleWarning=hasParticles ? AnimationParticleMaterialWarning(*scene_,particleMaterial) : Status::Ok();
    const AnimationParticleDescriptor particles(*frame,skin,particleMaterial);
    const War3RenderModelLayerUpdateV2 input{sizeof(input), 0, id_, parentEpoch_, revision_ + 1, revision_,
        hasParticles ? nullptr : skin, hasParticles ? particles.Get() : nullptr};
    if (api_->updateModelSceneLayer(handle_, &input) != 0)
        return Failure("MODEL_LAYER_UPDATE_REJECTED", "附加模型层动态更新被拒绝；主世界保持原状。");
    frame_ = std::move(frame); ++revision_;
    return particleWarning;
}
Status ModelSceneLayerRenderer::Remove() {
    if (!frame_) return {};
    War3RenderModelLayersTargetV2 target{sizeof(target)};
    if (!Supported(api_) || !handle_ || api_->getModelLayersTarget(handle_, &target) != 0)
        return Failure("MODEL_LAYER_TARGET_UNAVAILABLE", "无法核验待移除附加层的主场景。");
    if (target.parentModelEpoch == parentEpoch_ && api_->removeModelSceneLayer(handle_, id_, parentEpoch_, revision_) != 0)
        return Failure("MODEL_LAYER_REMOVE_REJECTED", "附加模型层移除请求已过期或被拒绝。");
    scene_.reset(); frame_.reset(); revision_ = 0; parentEpoch_ = 0; role_ = WAR3_RENDER_MODEL_LAYER_MODEL; return {};
}
}
