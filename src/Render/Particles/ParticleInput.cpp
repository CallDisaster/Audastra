#include "ParticleInput.h"
#include "War3MapCore/RenderPluginMaterialState.h"
#include <algorithm>
#include <cmath>

namespace War3Map::Render {
namespace {
bool SupportedPoseHeader(const War3RenderSkinUpdateV2& pose) {
    if (pose.structSize == sizeof(pose)) return true;
    const War3RenderDynamicColoredSkinUpdateV2* colored = nullptr;
    const War3RenderMaterialSkinUpdateV2* material = nullptr;
    const War3RenderMaterialStateSkinUpdateV2* state = nullptr;
    if (pose.structSize == sizeof(War3RenderMaterialStateSkinUpdateV2)) {
        state = reinterpret_cast<const War3RenderMaterialStateSkinUpdateV2*>(&pose);
        if (state->materialStateFormat != WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1
            || state->reserved != 0
            || state->materialStateCount > 1048576
            || state->materialInstanceSliceCount != pose.instanceCount
            || (state->materialStateCount != 0 && !state->materialStates)
            || (state->materialInstanceSliceCount != 0 && !state->materialInstanceSlices)) {
            return false;
        }
        material = &state->base;
    } else if (pose.structSize == sizeof(War3RenderMaterialSkinUpdateV2)) {
        material = reinterpret_cast<const War3RenderMaterialSkinUpdateV2*>(&pose);
    } else if (pose.structSize == sizeof(War3RenderDynamicColoredSkinUpdateV2)) {
        colored = reinterpret_cast<const War3RenderDynamicColoredSkinUpdateV2*>(&pose);
    } else {
        return false;
    }
    if (material) {
        // 只读取完整已知尾部；材质模式可以没有动态 RGB，但不能吞掉坏 RGB 声明。
        if (material->layerAlphaFormat != WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1
            || material->reserved != 0
            || material->layerAlphaCount > 1048576
            || material->instanceSliceCount != pose.instanceCount
            || (material->layerAlphaCount != 0 && !material->layerAlphas)
            || (material->instanceSliceCount != 0 && !material->instanceSlices)) {
            return false;
        }
        if (material->base.colorFormat == 0) {
            if (material->base.colorCount != 0 || material->base.colors) return false;
        } else {
            colored = &material->base;
        }
    }
    if (colored && (colored->colorFormat != WAR3_RENDER_DYNAMIC_MESH_RGB_V1
        || colored->colorCount != pose.alphaCount
        || colored->colorCount > 1048576
        || (colored->colorCount != 0 && !colored->colors))) {
        return false;
    }
    // 在扫描尾部数组前使用与 CopySkinUpdate 相同的64 MiB姿势工作预算。
    // instanceCount本身不是地图实例总量上限；超预算帧仍由共用模型提交合同拒绝。
    const std::uint64_t bytes = std::uint64_t(pose.instanceCount) * sizeof(War3RenderSkinnedModelInstanceV2)
        + std::uint64_t(pose.groupCount) * sizeof(War3RenderGroupMatrixV2)
        + std::uint64_t(pose.alphaCount) * sizeof(float)
        + (colored ? std::uint64_t(colored->colorCount) * sizeof(War3RenderMeshColorV2) : 0)
        + (material ? std::uint64_t(material->layerAlphaCount) * sizeof(float)
            + std::uint64_t(material->instanceSliceCount) * sizeof(War3RenderMaterialLayerSliceV2) : 0)
        + (state ? std::uint64_t(state->materialStateCount) * sizeof(War3RenderMaterialStateV2)
            + std::uint64_t(state->materialInstanceSliceCount) * sizeof(War3RenderMaterialLayerSliceV2) : 0);
    if (bytes > 64ull * 1024 * 1024) return false;
    if (material) {
        for (std::uint32_t i = 0; i < material->layerAlphaCount; ++i) {
            // Hermite 的有限越界值合法，不能把 KMTA 错当静态 0..1 属性。
            if (!std::isfinite(material->layerAlphas[i])) return false;
        }
        for (std::uint32_t i = 0; i < material->instanceSliceCount; ++i) {
            const auto& slice = material->instanceSlices[i];
            if (slice.first > material->layerAlphaCount
                || slice.count > material->layerAlphaCount - slice.first) {
                return false;
            }
        }
    }
    if (state) {
        for (std::uint32_t i = 0; i < state->materialStateCount; ++i) {
            const auto& value = state->materialStates[i];
            if (value.samplerFlags & ~3u) return false;
            for (const auto uv : value.uvTransform) if (!std::isfinite(uv)) return false;
        }
        for (std::uint32_t i = 0; i < state->materialInstanceSliceCount; ++i) {
            const auto& slice = state->materialInstanceSlices[i];
            if (slice.first > state->materialStateCount
                || slice.count > state->materialStateCount - slice.first) return false;
        }
        // textureIndex属于模型驻留贴图表，不能用粒子textures参数检验。
        // 完整模型层数、共享片段、贴图范围和模式/版本仍交给CopySkinUpdate。
    }
    // 与 CopySkinUpdate 的已知扩展合同相同。此处不替代后续完整模型布局、
    // 矩阵/材质片段、64 MiB 工作量和初始化版本校验，也不截断任何尾部。
    if (colored) {
        for (std::uint32_t i = 0; i < colored->colorCount; ++i) {
            for (const auto value : colored->colors[i].rgb) {
                if (!std::isfinite(value) || value < 0 || value > 1) return false;
            }
        }
    }
    return true;
}
}
bool ParticleApiSupported(const War3RenderPluginApiV2* a) noexcept {
    return a && a->structSize>=offsetof(War3RenderPluginApiV2,captureParticleFrameRgba)+sizeof(a->captureParticleFrameRgba)
        && a->apiVersion==WAR3_RENDER_PLUGIN_API_V2 && a->submitParticleModelScene
        && a->updateParticleModelFrame && a->captureParticleFrameRgba;
}
std::optional<std::uint64_t> ParticleInputPayloadBytes(const War3RenderParticleUpdateV2& in) noexcept {
    const War3RenderParticleMaterialUpdateV2* material = nullptr;
    if (in.structSize == sizeof(War3RenderParticleMaterialUpdateV2)) {
        material = reinterpret_cast<const War3RenderParticleMaterialUpdateV2*>(&in);
        if (material->materialFlagsFormat != WAR3_RENDER_PARTICLE_MATERIAL_FLAGS_V1
            || material->materialFlagsCount != in.drawCount
            || (material->materialFlagsCount == 0) != (material->materialFlags == nullptr)) return std::nullopt;
    } else if (in.structSize != sizeof(in)) return std::nullopt;
    if(in.reserved||!in.sceneVersion||!in.contentVersion||in.vertexCount>1048576
        ||in.drawCount>65536||(in.vertexCount&&!in.vertices)||(in.drawCount&&!in.draws)) return std::nullopt;
    const auto bytes = std::uint64_t(in.vertexCount) * sizeof(War3RenderParticleVertexV2)
        + std::uint64_t(in.drawCount) * sizeof(War3RenderParticleDrawV2)
        + (material ? std::uint64_t(material->materialFlagsCount) * sizeof(std::uint32_t) : 0);
    return bytes > 64ull * 1024 * 1024 ? std::nullopt : std::optional<std::uint64_t>(bytes);
}
bool CopyParticleInput(const War3RenderParticleUpdateV2& in,std::uint32_t textures,ParticleFrameData& out) {
    if (!ParticleInputPayloadBytes(in) || textures > 256 || !in.modelPose || !SupportedPoseHeader(*in.modelPose)
        || in.modelPose->sceneVersion != in.sceneVersion || in.modelPose->contentVersion != in.contentVersion) return false;
    const auto* material = in.structSize == sizeof(War3RenderParticleMaterialUpdateV2)
        ? reinterpret_cast<const War3RenderParticleMaterialUpdateV2*>(&in) : nullptr;
    ParticleFrameData copy;copy.sceneVersion=in.sceneVersion;copy.contentVersion=in.contentVersion;
    if(in.vertexCount)copy.vertices.assign(in.vertices,in.vertices+in.vertexCount);
    if(in.drawCount)copy.draws.assign(in.draws,in.draws+in.drawCount);
    if (material && material->materialFlagsCount)
        copy.drawMaterialFlags.assign(material->materialFlags, material->materialFlags + material->materialFlagsCount);
    if (!copy.ValidMaterialFlags()) return false;
    for(const auto& v:copy.vertices){
        for(const auto x:v.position)if(!std::isfinite(x)||std::abs(x)>1e7f)return false;
        for(const auto x:v.uv)if(!std::isfinite(x)||std::abs(x)>1e7f)return false;
        for(const auto x:v.color)if(!std::isfinite(x)||x<0||x>1)return false;
    }
    std::uint32_t end=0;
    for(const auto& d:copy.draws){
        if(!d.vertexCount||d.vertexCount%3||d.firstVertex!=end||d.vertexCount>in.vertexCount-end
            ||d.texture>=textures||d.filter>WAR3_PARTICLE_OPAQUE)return false;
        end+=d.vertexCount;
    }
    if(end!=in.vertexCount)return false;
    out=std::move(copy);return true;
}
} // namespace War3Map::Render
