#pragma once
#include "War3MapCore/Render/ModelAnimation.h"

namespace War3Map::Render {
// 主模型和附加层共用同一能力/owned帧门，不能只协商alpha后丢弃material-state尾部。
[[nodiscard]] Status CheckAnimationMaterialFrame(const War3RenderPluginApiV2* api, War3RendererHandle handle,
    const AnimatedModelScene& scene, const AnimationFrame& frame);
}
