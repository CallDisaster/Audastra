#include "D3D11Scene.h"
#include <new>

namespace War3Map::Render {
void D3D11Scene::SetModelLayers(const ModelSceneLayers& layers) {
    // 入口已经验证预算；此处仍拒绝越界，不以截断方式丢掉编辑层。
    if (layers.size() > additionalModels_.size()) return;
    auto previousLayers = std::move(additionalModels_);
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const auto& input = layers[i];
        if (!input) continue; // 插件的拥有数据不含空项。
        for (auto& previous : previousLayers) {
            if (previous.requested && previous.requested->id == input->id
                && previous.requested->parentModelEpoch == input->parentModelEpoch) {
                additionalModels_[i] = std::move(previous);
                break;
            }
        }
        auto& slot = additionalModels_[i];
        if (slot.requested == input) continue;
        slot.requested = input;
        try {
            HRESULT hr = E_INVALIDARG;
            if (input->role == WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW && !groundDecalRasterizer_) {
                D3D11_RASTERIZER_DESC raster{};
                raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
                // 编辑器预览近似：只解除真正共面的深度竞争，不抬高来源几何。
                // 小幅负偏移并限幅；原版偏移和非共面投影仍需独立证明。
                raster.DepthBias = -1; raster.SlopeScaledDepthBias = -.25f; raster.DepthBiasClamp = -1e-7f;
                hr = device_->CreateRasterizerState(&raster, &groundDecalRasterizer_);
                if (FAILED(hr)) { slot.result = hr; continue; } // 保留旧accepted GPU和完成身份。
            }
            if (input->models && input->pose) {
                const bool sameAssets = slot.gpu && slot.accepted && slot.accepted->models == input->models
                    && slot.accepted->particles == input->particles && slot.accepted->role == input->role;
                if (sameAssets) {
                    hr = input->particles && input->particleFrame
                        ? slot.gpu->SetParticleFrame(*input->pose, *input->particleFrame)
                        : slot.gpu->SetSkinPose(*input->pose);
                } else {
                    auto gpu = std::make_unique<D3D11Scene>();
                    gpu->borrowedContext_ = true;
                    gpu->device_ = device_; gpu->context_ = context_;
                    gpu->modelVertexShader_ = modelVertexShader_; gpu->modelPixelShader_ = modelPixelShader_;
                    gpu->modelLayout_ = modelLayout_; gpu->modelSampler_ = modelSampler_;
                    gpu->particleSampler_ = particleSampler_;
                    gpu->modelConstants_ = modelConstants_; gpu->modelBlend_ = modelBlend_;
                    gpu->terrainDepth_ = terrainDepth_; gpu->modelDepthRead_ = modelDepthRead_;
#ifdef WAR3_RENDER_TEST_FAULTS
                    gpu->testFault_ = testFault_;
#endif
                    hr = input->particles && input->particleFrame
                        ? gpu->SetParticleModels(*input->models, *input->pose, *input->particles, *input->particleFrame)
                        : gpu->SetModels(*input->models, input->pose.get());
                    if (SUCCEEDED(hr)) slot.gpu = std::move(gpu);
                }
            }
            slot.result = hr;
            if (SUCCEEDED(hr)) slot.accepted = input;
        } catch (const std::bad_alloc&) { slot.result = E_OUTOFMEMORY; }
    }
    additionalModelCount_ = layers.size();
}

std::vector<War3RenderModelLayerStatusV2> D3D11Scene::ModelLayerStatuses() const {
    std::vector<War3RenderModelLayerStatusV2> result;
    result.reserve(additionalModelCount_);
    for (std::size_t i = 0; i < additionalModelCount_; ++i) {
        const auto& slot = additionalModels_[i];
        War3RenderModelLayerStatusV2 status{sizeof(War3RenderModelLayerStatusV2)};
        status.result = slot.result;
        if (slot.requested) { status.layerId = slot.requested->id; status.parentModelEpoch = slot.requested->parentModelEpoch; }
        status.revision = slot.accepted ? slot.accepted->revision : 0;
        if (slot.gpu) { status.skin = slot.gpu->SkinStatus(); status.particles = slot.gpu->ParticleStatus(); }
        result.push_back(status);
    }
    return result;
}

void D3D11Scene::DrawModelLayers(int pass) {
    for (std::size_t i = 0; i < additionalModelCount_; ++i) {
        const auto& slot = additionalModels_[i];
        if (const auto& gpu = slot.gpu) {
            const bool decal = slot.accepted && slot.accepted->role == WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW;
            if (decal && pass != 1) continue; // typed入口保证只有blend/no-write模型，无粒子。
            if (decal) context_->RSSetState(groundDecalRasterizer_.Get());
            gpu->DrawModels(pass); gpu->DrawParticles(pass);
            if (decal) context_->RSSetState(rasterizer_.Get()); // 后续普通层和编辑overlay恢复原状态。
        }
    }
}
}
