#pragma once
#include "War3MapCore/RenderPluginParticleMaterial.h"
#include "../Scene/ModelSceneData.h"
#include <optional>

namespace War3Map::Render {
struct ParticleFrameData {
    std::uint64_t sceneVersion=0, contentVersion=0;
    std::vector<War3RenderParticleVertexV2> vertices;
    std::vector<War3RenderParticleDrawV2> draws;
    // 空数组保留旧输入的受雾行为；扩展输入必须完整覆盖逐 draw 标志。
    std::vector<std::uint32_t> drawMaterialFlags;
    [[nodiscard]] std::uint64_t PayloadBytes() const noexcept {
        return std::uint64_t(vertices.size()) * sizeof(War3RenderParticleVertexV2)
            + std::uint64_t(draws.size()) * sizeof(War3RenderParticleDrawV2)
            + std::uint64_t(drawMaterialFlags.size()) * sizeof(std::uint32_t);
    }
    [[nodiscard]] bool ValidMaterialFlags() const noexcept {
        if (drawMaterialFlags.empty()) return true;
        if (drawMaterialFlags.size() != draws.size()) return false;
        for (const auto flags : drawMaterialFlags) if (flags & ~WAR3_PARTICLE_UNFOGGED) return false;
        return true;
    }
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
// 仅描述已知输入布局及其有界载荷；复制/完整姿势校验仍由CopyParticleInput负责。
[[nodiscard]] std::optional<std::uint64_t> ParticleInputPayloadBytes(
    const War3RenderParticleUpdateV2& input) noexcept;
// 粒子数组CPU验证/深拷贝；失败不修改output。已知基础/RGB/KMTA/KMTF头不替代完整模型布局校验。
// 仅接受已知完整姿势结构，绝不截断扩展；调用者还须用CopySkinUpdate验证/复制
// 完整modelPose，再与本输出一起发布。此函数单独成功不表示整对场景或GPU已发布。
[[nodiscard]] bool CopyParticleInput(const War3RenderParticleUpdateV2& input,
    std::uint32_t textures, ParticleFrameData& output);
} // namespace War3Map::Render
