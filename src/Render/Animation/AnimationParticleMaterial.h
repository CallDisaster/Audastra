#pragma once
#include "War3MapCore/Render/ModelAnimation.h"
#include <algorithm>

namespace War3Map::Render {
// 可选status尾部必须先清零；旧provider只写prefix时，能力字段仍为0。
[[nodiscard]] inline bool AnimationParticleMaterialSupported(const War3RenderPluginApiV2* api,
    War3RendererHandle handle) noexcept {
    if (!api || !handle || api->apiVersion != WAR3_RENDER_PLUGIN_API_V2
        || api->structSize < offsetof(War3RenderPluginApiV2, getSkinStatus) + sizeof(api->getSkinStatus)
        || !api->getSkinStatus) return false;
    War3RenderParticleMaterialCapabilitiesV2 capabilities{};
    auto* status = &capabilities.base.base.base.base.base.base;
    status->structSize = sizeof(capabilities);
    return api->getSkinStatus(handle, status) == 0
        && capabilities.particleMaterialFlagsFormat == WAR3_RENDER_PARTICLE_MATERIAL_FLAGS_V1
        && (capabilities.particleMaterialFlagsMask & WAR3_PARTICLE_UNFOGGED) != 0;
}
[[nodiscard]] inline Status CheckAnimationParticleMaterialFrame(const AnimationFrame& frame) {
    if (frame.particleVertices.size()>1048576 || frame.particleDraws.size()>65536
        || frame.particleMaterialFlags.size() != frame.particleDraws.size()
        || std::any_of(frame.particleMaterialFlags.begin(), frame.particleMaterialFlags.end(), [](std::uint32_t flags) {
            return (flags & ~std::uint32_t(WAR3_PARTICLE_UNFOGGED)) != 0;
        })) return Status::Failure("Render.Animation", "ANIMATION_PARTICLE_MATERIAL_MAPPING",
            "粒子材质位数量或已知位不属于当前owned draw帧。");
    return {};
}
[[nodiscard]] inline Status AnimationParticleMaterialWarning(const AnimatedModelScene& scene, bool supported) {
    if (scene.particleUnfogged && !supported)
        return {{{DiagnosticSeverity::Warning, "Render.Animation", "PARTICLE_UNFOGGED_PREVIEW",
            "插件未声明逐draw粒子材质能力；当前RIBB Unfogged仍应用环境雾，仅为预览近似。", std::nullopt}}};
    return {};
}
// 两份描述符只借用frame；所有同步提交入口使用同一个选择，不把尾部借到异步队列。
struct AnimationParticleDescriptor {
    War3RenderParticleUpdateV2 legacy;
    War3RenderParticleMaterialUpdateV2 material;
    bool supported;
    AnimationParticleDescriptor(const AnimationFrame& frame, const War3RenderSkinUpdateV2* pose, bool enabled) noexcept
        : legacy(frame.DescribeParticles(pose)), material(frame.DescribeParticleMaterial(pose)), supported(enabled) {}
    [[nodiscard]] const War3RenderParticleUpdateV2* Get() const noexcept { return supported ? &material.base : &legacy; }
};
} // namespace War3Map::Render
