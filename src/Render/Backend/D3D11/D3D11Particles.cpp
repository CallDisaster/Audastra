#include "D3D11Scene.h"
#include "ParticleVS.h"
#include "ParticlePS.h"
#include <cstring>
#include <new>
#include <stdexcept>

namespace War3Map::Render {
HRESULT D3D11Scene::InitializeParticles() {
    if (particleVertexShader_) return S_OK;
    Ptr<ID3D11VertexShader> vertex;
    Ptr<ID3D11PixelShader> pixel;
    Ptr<ID3D11InputLayout> layout;
    auto hr = device_->CreateVertexShader(kParticleVS, sizeof(kParticleVS), nullptr, &vertex);
    if (FAILED(hr)) return hr;
    hr = device_->CreatePixelShader(kParticlePS, sizeof(kParticlePS), nullptr, &pixel);
    if (FAILED(hr)) return hr;
    constexpr D3D11_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    hr = device_->CreateInputLayout(elements, 3, kParticleVS, sizeof(kParticleVS), &layout);
    if (FAILED(hr)) return hr;
    particleVertexShader_ = std::move(vertex); particlePixelShader_ = std::move(pixel);
    particleLayout_ = std::move(layout);
    return S_OK;
}
HRESULT D3D11Scene::PrepareParticleFrame(const ParticleFrameData& frame, ParticleGpuFrame& candidate) {
    if (!frame.ValidMaterialFlags() || frame.PayloadBytes() > 64ull * 1024 * 1024) return E_INVALIDARG;
    // 只写非活动槽。即使后续模型提交失败，上一对模型/粒子 GPU 资源仍完整可画。
    candidate.draws = frame.draws;
    candidate.drawMaterialFlags = frame.drawMaterialFlags;
    const auto bytes = static_cast<UINT>(frame.vertices.size() * sizeof(War3RenderParticleVertexV2));
    if (!bytes) return S_OK;
    auto hr = InitializeParticles();
    if (FAILED(hr)) return hr;
    const auto& spare = particleFrames_[1u - activeParticleFrame_];
    if (spare.capacity >= bytes) {
        candidate.vertices = spare.vertices; candidate.capacity = spare.capacity;
    } else {
        D3D11_BUFFER_DESC descriptor{};
        descriptor.ByteWidth = bytes; descriptor.Usage = D3D11_USAGE_DYNAMIC;
        descriptor.BindFlags = D3D11_BIND_VERTEX_BUFFER; descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device_->CreateBuffer(&descriptor, nullptr, &candidate.vertices);
        if (FAILED(hr)) return hr;
        candidate.capacity = bytes;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(candidate.vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;
    std::memcpy(mapped.pData, frame.vertices.data(), bytes);
    context_->Unmap(candidate.vertices.Get(), 0);
    return device_->GetDeviceRemovedReason();
}
void D3D11Scene::PublishParticleFrame(const ParticleFrameData& frame, ParticleGpuFrame& candidate) noexcept {
    const auto next = 1u - activeParticleFrame_;
    particleFrames_[next].vertices.Swap(candidate.vertices);
    std::swap(particleFrames_[next].capacity, candidate.capacity);
    particleFrames_[next].draws.swap(candidate.draws);
    particleFrames_[next].drawMaterialFlags.swap(candidate.drawMaterialFlags);
    activeParticleFrame_ = next;
    particleStatus_.sceneVersion = frame.sceneVersion; particleStatus_.contentVersion = frame.contentVersion;
    particleStatus_.vertexCount = static_cast<std::uint32_t>(frame.vertices.size());
    particleStatus_.drawCount = static_cast<std::uint32_t>(frame.draws.size());
    ++particleStatus_.dynamicUploadCount; // 初始帧和有效空帧也代表一次成功发布。
}
HRESULT D3D11Scene::SetParticleModels(const ModelScene& models, const SkinPose& pose,
    const ParticleSceneData& particles, const ParticleFrameData& frame) try {
    if (pose.sceneVersion != frame.sceneVersion || pose.contentVersion != frame.contentVersion) return E_INVALIDARG;
    std::vector<Ptr<ID3D11ShaderResourceView>> textures;
    textures.reserve(particles.textures.size());
    for (const auto& source : particles.textures) {
        Ptr<ID3D11ShaderResourceView> view;
        const auto hr = CreateTexture(source, view);
        if (FAILED(hr)) return hr;
        textures.push_back(std::move(view));
    }
    ParticleGpuFrame candidate;
    auto hr = PrepareParticleFrame(frame, candidate);
    if (FAILED(hr)) return hr;
#ifdef WAR3_RENDER_TEST_FAULTS
    if (ConsumeTestFault(1)) return E_FAIL;
#endif
    hr = SetModels(models, &pose);
    if (FAILED(hr)) return hr;
    // 模型已提交，后面只能 noexcept swap 与标量赋值，不能再次分配或调用 D3D。
    particleTextures_.swap(textures);
    if (!particles.textures.empty()) ++particleStatus_.textureUploadCount;
    PublishParticleFrame(frame, candidate);
    return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (const std::length_error&) { return E_INVALIDARG; }

HRESULT D3D11Scene::SetParticleFrame(const SkinPose& pose, const ParticleFrameData& frame) try {
    if (!particleStatus_.sceneVersion || frame.sceneVersion != particleStatus_.sceneVersion
        || frame.contentVersion <= particleStatus_.contentVersion || pose.sceneVersion != frame.sceneVersion
        || pose.contentVersion != frame.contentVersion) return E_INVALIDARG;
    ParticleGpuFrame candidate;
    auto hr = PrepareParticleFrame(frame, candidate);
    if (FAILED(hr)) return hr;
#ifdef WAR3_RENDER_TEST_FAULTS
    if (ConsumeTestFault(1)) return E_FAIL;
#endif
    hr = SetSkinPose(pose);
    if (FAILED(hr)) return hr;
    PublishParticleFrame(frame, candidate);
    return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (const std::length_error&) { return E_INVALIDARG; }

void D3D11Scene::ClearParticles() noexcept {
    for (auto& frame : particleFrames_) {
        frame.vertices.Reset(); frame.capacity = 0; frame.draws.clear();
        std::vector<std::uint32_t>().swap(frame.drawMaterialFlags);
    }
    particleTextures_.clear();
    particleStatus_.sceneVersion = particleStatus_.contentVersion = 0;
    particleStatus_.vertexCount = particleStatus_.drawCount = 0;
    // 上传计数在 renderer 生存期内单调，退出模式不会伪造上传。
}
War3RenderParticleStatusV2 D3D11Scene::ParticleStatus() const noexcept { return particleStatus_; }
void D3D11Scene::DrawParticles(int pass) {
    const auto& frame = particleFrames_[activeParticleFrame_];
    if (!particleStatus_.sceneVersion || frame.draws.empty()) return;
    context_->IASetInputLayout(particleLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto* vertices = frame.vertices.Get();
    const UINT stride = sizeof(War3RenderParticleVertexV2), offset = 0;
    context_->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
    context_->VSSetShader(particleVertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(particlePixelShader_.Get(), nullptr, 0);
    auto* constants = modelConstants_.Get(); context_->PSSetConstantBuffers(2, 1, &constants);
    auto* sampler = modelSampler_.Get(); context_->PSSetSamplers(1, 1, &sampler);
    context_->OMSetDepthStencilState(pass == 0 ? terrainDepth_.Get() : modelDepthRead_.Get(), 0);
    // ABI 粒子枚举与 MDX 材质枚举次序不同，不能直接作索引。
    constexpr UINT filters[]{2, 3, 5, 6, 1, 4, 0};
    for (std::size_t drawIndex = 0; drawIndex < frame.draws.size(); ++drawIndex) {
        if (editorVisibility_ && !editorVisibility_->particleDrawVisible[drawIndex]) continue;
        const auto& draw = frame.draws[drawIndex];
        const auto filter = filters[draw.filter];
        if ((filter >= 2) != (pass == 1)) continue;
        const auto materialFlags = frame.drawMaterialFlags.empty() ? 0u : frame.drawMaterialFlags[drawIndex];
        const auto layerFlags = WAR3_RENDER_MODEL_UNSHADED
            | ((materialFlags & WAR3_PARTICLE_UNFOGGED) ? WAR3_RENDER_MODEL_UNFOGGED : 0u);
        const ModelLayerConstants values{1, filter, layerFlags, 1};
        context_->UpdateSubresource(modelConstants_.Get(), 0, nullptr, &values, 0, 0);
        auto* texture = particleTextures_[draw.texture].Get(); context_->PSSetShaderResources(0, 1, &texture);
        // PRE2 AlphaKey / RIBB Transparent 均提交原版 mode 1：裁剪后仍使用源 alpha 混合。
        // 只复用 mode 2 的混合状态；不改变 pass、深度或其他模型材质路径。
        const auto blendFilter = filter == 1 ? 2 : filter;
        context_->OMSetBlendState(blendFilter >= 2 ? modelBlend_[blendFilter].Get() : nullptr, nullptr, 0xffffffff);
        context_->Draw(draw.vertexCount, draw.firstVertex);
    }
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff); context_->OMSetDepthStencilState(nullptr, 0);
    ID3D11ShaderResourceView* empty = nullptr; context_->PSSetShaderResources(0, 1, &empty);
}
}
