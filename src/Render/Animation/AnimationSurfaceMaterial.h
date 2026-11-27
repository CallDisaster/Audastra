#pragma once
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/RenderPluginSurfaceMaterial.h"
#include <algorithm>
#include <cstddef>

namespace War3Map::Render {
// 插件是否声明了模型表面材质尾部。旧插件只写已知前缀，surfaceFormat 保持 0。
[[nodiscard]] inline bool AnimationSurfaceMaterialSupported(const War3RenderPluginApiV2* api,
    War3RendererHandle handle) noexcept {
    if (!api || !handle || api->apiVersion != WAR3_RENDER_PLUGIN_API_V2
        || api->structSize < offsetof(War3RenderPluginApiV2, getSkinStatus) + sizeof(api->getSkinStatus)
        || !api->getSkinStatus) return false;
    War3RenderSkinSurfaceCapabilitiesV2 capabilities{};
    auto* status = &capabilities.base.base.base.base.base.base.base;
    status->structSize = sizeof(capabilities);
    return api->getSkinStatus(handle, status) == 0 && capabilities.surfaceFormat == WAR3_RENDER_MODEL_SURFACE_V1
        && capabilities.reserved == 0;
}
// 场景里是否真有带材质的模型；全是无材质占位时不需要尾部。
[[nodiscard]] inline bool AnimationSceneHasSurfaces(const AnimatedModelScene& scene) {
    const auto& surfaces = scene.geometry.modelSurfaces;
    return std::any_of(surfaces.begin(), surfaces.end(), [](const War3RenderModelSurfaceV2& s) {
        return s.normalTexture != WAR3_RENDER_MODEL_NO_TEXTURE || s.ormTexture != WAR3_RENDER_MODEL_NO_TEXTURE
            || s.detailNormalTexture != WAR3_RENDER_MODEL_NO_TEXTURE || s.detailOrmTexture != WAR3_RENDER_MODEL_NO_TEXTURE
            || s.detailAlbedoTexture != WAR3_RENDER_MODEL_NO_TEXTURE;
    });
}
// 插件不支持时照旧着色，但要说明地图表面材质没有显示，不能静默。
[[nodiscard]] inline Status AnimationSurfaceMaterialWarning(const AnimatedModelScene& scene, bool supported) {
    if (supported || !AnimationSceneHasSurfaces(scene)) return Status::Ok();
    return {{{DiagnosticSeverity::Warning, "Render.Animation", "ANIMATION_SURFACE_MATERIAL_UNSUPPORTED",
        "当前渲染插件没有表面材质能力，地图的法线/ORM 材质未显示，模型按原贴图着色。", std::nullopt}}};
}
}
