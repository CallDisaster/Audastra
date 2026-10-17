#include "AnimationInternal.h"
#include "AnimationParticles.h"
#include <exception>
#include <utility>

namespace War3Map::Render {
Result<std::shared_ptr<const AnimationAssetSnapshot>> ModelAnimationService::CaptureAssetSnapshot() const try {
    auto result = std::shared_ptr<AnimationAssetSnapshot>(new AnimationAssetSnapshot);
    result->maxTextureDimension_ = state_->limits.geometry.maxTextureDimension;
    result->mipPolicyVersion_ = ModelTextureMipPolicyVersion;
    result->allowRigidStaticPreview_ = state_->limits.allowRigidStaticPreview;
    const auto& scene = *state_->scene;
    // 内容来自已经有界的服务；共享不免除使用方的本次原字节、解析和累计几何预算。
    for (std::size_t i = 0; i < scene.specs.size(); ++i) {
        const auto index = state_->requestAssets.at(i);
        if (index >= state_->assets.size() || !state_->assets[index].parsed) continue;
        const auto key = AnimationPathKey(scene.specs[i].model.modelPath);
        const auto resource = state_->resources.find(key);
        if (resource == state_->resources.end()) continue;
        result->models_.try_emplace(key, AnimationAssetSnapshot::Model{
            resource->second->sourceLayer, resource->second->digest, state_->assets[index].parsed});
    }
    result->decoded_ = scene.geometry.textures;
    for (std::uint32_t i = 0; i < result->decoded_.size(); ++i) {
        const auto& texture = std::as_const(result->decoded_)[i];
        if (!texture.ownedIdentity.empty() || texture.mipPolicyVersion != ModelTextureMipPolicyVersion
            || !ModelTextureValid(texture,state_->limits.geometry.maxTextureDimension)) continue;
        const auto key = AnimationPathKey(texture.path);
        const auto resource = state_->resources.find(key);
        if (resource == state_->resources.end()) continue;
        result->textures_.try_emplace(key, AnimationAssetSnapshot::Texture{
            resource->second->sourceLayer, resource->second->digest, i});
    }
    return Result<std::shared_ptr<const AnimationAssetSnapshot>>::Success(std::move(result));
} catch (const std::exception&) {
    return Result<std::shared_ptr<const AnimationAssetSnapshot>>::Failure("Render.Animation",
        "ANIMATION_ASSET_CAPTURE_FAILED", "静态解析/解码来源缓存捕获失败，未借用可变动画或预览材质。");
}
} // namespace War3Map::Render
