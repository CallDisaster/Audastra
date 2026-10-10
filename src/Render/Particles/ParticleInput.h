#pragma once
#include "War3MapCore/RenderPluginV2.h"
#include "../Scene/ModelSceneData.h"

namespace War3Map::Render {
struct ParticleFrameData {
    std::uint64_t sceneVersion=0, contentVersion=0;
    std::vector<War3RenderParticleVertexV2> vertices;
    std::vector<War3RenderParticleDrawV2> draws;
};
struct ParticleSceneData {
    std::vector<TextureRgba> textures;
    std::shared_ptr<const ParticleFrameData> initial;
    std::shared_ptr<const ParticleSceneData> staticBase;
    std::uint32_t prefixTextures = 0;
    std::uint32_t TextureCount() const noexcept { return prefixTextures + static_cast<std::uint32_t>(textures.size()); }
    const auto& StaticTextures() const noexcept { return textures; }
};
[[nodiscard]] bool ParticleApiSupported(const War3RenderPluginApiV2* api) noexcept;
// 粒子数组CPU验证/深拷贝；失败不修改output。已知基础/RGB/KMTA/KMTF头不替代完整模型布局校验。
// 仅接受已知完整姿势结构，绝不截断扩展；调用者还须用CopySkinUpdate验证/复制
// 完整modelPose，再与本输出一起发布。此函数单独成功不表示整对场景或GPU已发布。
[[nodiscard]] bool CopyParticleInput(const War3RenderParticleUpdateV2& input,
    std::uint32_t textures, ParticleFrameData& output);
} // namespace War3Map::Render
