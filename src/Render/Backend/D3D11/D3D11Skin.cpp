#include "D3D11Scene.h"
#include "SkinVS.h"
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace War3Map::Render {
HRESULT D3D11Scene::InitializeMaterialStateSamplers() {
    if (materialStateSamplers_[0]) return S_OK;
    if (!modelSampler_) return E_FAIL;
    std::array<Ptr<ID3D11SamplerState>, 4> candidate;
    D3D11_SAMPLER_DESC description{}; modelSampler_->GetDesc(&description);
    candidate[3] = modelSampler_;
    for (UINT flags = 0; flags != 3; ++flags) {
        description.AddressU = (flags & 1) ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        description.AddressV = (flags & 2) ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        const auto result = device_->CreateSamplerState(&description, &candidate[flags]);
        if (FAILED(result)) return result;
    }
    materialStateSamplers_.swap(candidate); return S_OK;
}
bool D3D11Scene::CountModelDrawLayers(const ModelScene& models, std::vector<std::uint32_t>& counts) {
    counts.assign(models.models.size(), 0);
    for (std::size_t index = 0; index < models.models.size(); ++index) {
        const auto& model = models.models[index];
        if (model.firstMesh > models.meshes.size() || model.meshCount > models.meshes.size() - model.firstMesh) return false;
        for (std::uint32_t mesh = 0; mesh < model.meshCount; ++mesh) {
            const auto& source = models.meshes[model.firstMesh + mesh];
            if (source.firstLayer > models.layers.size() || source.layerCount > models.layers.size() - source.firstLayer
                || source.layerCount > (std::numeric_limits<std::uint32_t>::max)() - counts[index]) return false;
            counts[index] += source.layerCount;
        }
    }
    return models.dynamicMaterialAlphas ? models.modelLayerCounts == counts : models.modelLayerCounts.empty();
}
bool D3D11Scene::ValidMaterialPose(const SkinPose& pose, const std::vector<std::uint32_t>& counts, bool material) noexcept {
    if (pose.dynamicMaterialAlphas != material) return false;
    if (!material) return pose.layerAlphas.empty() && pose.layerSlices.empty();
    if (pose.layerSlices.size() != pose.instances.size()) return false;
    for (std::size_t index = 0; index < pose.instances.size(); ++index) {
        const auto model = pose.instances[index].instance.model;
        const auto& slice = pose.layerSlices[index];
        if (model >= counts.size() || slice.count != counts[model] || slice.first > pose.layerAlphas.size()
            || slice.count > pose.layerAlphas.size() - slice.first) return false;
    }
    for (const auto alpha : pose.layerAlphas) if (!std::isfinite(alpha)) return false;
    return true;
}
bool D3D11Scene::ValidMaterialStatePose(const SkinPose& pose, const std::vector<std::uint32_t>& counts,
    bool enabled, std::size_t textureCount) noexcept {
    if (pose.dynamicMaterialStates != enabled) return false;
    if (!enabled) return pose.materialStates.empty() && pose.materialStateSlices.empty();
    if (!pose.dynamicMaterialAlphas || pose.materialStateSlices.size() != pose.instances.size()) return false;
    for (std::size_t index = 0; index < pose.instances.size(); ++index) {
        const auto model = pose.instances[index].instance.model; const auto& slice = pose.materialStateSlices[index];
        if (model >= counts.size() || slice.count != counts[model] || slice.first > pose.materialStates.size()
            || slice.count > pose.materialStates.size() - slice.first) return false;
    }
    for (const auto& value : pose.materialStates) {
        if ((value.textureIndex != WAR3_RENDER_MODEL_NO_TEXTURE && value.textureIndex >= textureCount) || (value.samplerFlags & ~3u)) return false;
        for (const auto uv : value.uvTransform) if (!std::isfinite(uv)) return false;
    }
    return true;
}
HRESULT D3D11Scene::InitializeSkinning() {
    Ptr<ID3D11VertexShader> shader;
    Ptr<ID3D11InputLayout> layout;
    Ptr<ID3D11Buffer> constants;
    auto hr = device_->CreateVertexShader(kSkinVS, sizeof(kSkinVS), nullptr, &shader);
    if (FAILED(hr)) return hr;
    constexpr D3D11_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 4, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 20, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 36, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 52, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 4, DXGI_FORMAT_R32_UINT, 1, 64, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 5, DXGI_FORMAT_R32_UINT, 1, 68, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 6, DXGI_FORMAT_R32_UINT, 1, 72, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 7, DXGI_FORMAT_R32_UINT, 1, 76, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
    static_assert(sizeof(War3RenderSkinnedModelVertexV2) == 36 && sizeof(War3RenderSkinnedModelInstanceV2) == 72);
    static_assert(offsetof(War3RenderSkinnedModelInstanceV2, firstGroup) == 64);
    hr = device_->CreateInputLayout(elements, 12, kSkinVS, sizeof(kSkinVS), &layout);
    if (FAILED(hr)) return hr;
    hr = Buffer(nullptr, sizeof(SkinMeshConstants), D3D11_BIND_CONSTANT_BUFFER, &constants);
    if (FAILED(hr)) return hr;
    skinVertexShader_ = std::move(shader); skinLayout_ = std::move(layout); skinMeshConstants_ = std::move(constants);
    return S_OK;
}
HRESULT D3D11Scene::CreateSkinBuffers(const SkinPose& pose, SkinGpuBuffers& buffers) {
    if (pose.instances.empty()) return S_OK;
    const auto fits = [](std::size_t count, std::size_t stride) { return count <= (std::numeric_limits<UINT>::max)() / stride; };
    if (!fits(pose.instances.size(), sizeof(SkinGpuInstance)) || !fits(pose.groups.size(), sizeof(War3RenderGroupMatrixV2))
        || !fits(pose.alphas.size(), sizeof(float)) || !fits(pose.colors.size(), sizeof(War3RenderMeshColorV2))
        || !fits(pose.layerAlphas.size(), sizeof(float)) || !fits(pose.materialStates.size(), sizeof(War3RenderMaterialStateV2))
        || (pose.dynamicMaterialAlphas && pose.layerSlices.size() != pose.instances.size())
        || (pose.dynamicMaterialStates && pose.materialStateSlices.size() != pose.instances.size())) return E_INVALIDARG;
    buffers.instanceData.resize(pose.instances.size());
    for (std::size_t index = 0; index < pose.instances.size(); ++index)
        buffers.instanceData[index] = {pose.instances[index], pose.dynamicMaterialAlphas ? pose.layerSlices[index].first : 0,
            pose.dynamicMaterialStates ? pose.materialStateSlices[index].first : 0};
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(pose.instances.size() * sizeof(SkinGpuInstance));
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{buffers.instanceData.data(), 0, 0};
    auto hr = device_->CreateBuffer(&desc, &initial, &buffers.instances);
    if (FAILED(hr)) return hr;
    const auto structured = [&](const void* values, UINT count, UINT stride, Ptr<ID3D11Buffer>& buffer,
        Ptr<ID3D11ShaderResourceView>& view) {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = count * stride; description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        description.StructureByteStride = stride;
        const D3D11_SUBRESOURCE_DATA data{values, 0, 0};
        auto result = device_->CreateBuffer(&description, &data, &buffer);
        if (FAILED(result)) return result;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_UNKNOWN; srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = count;
        return device_->CreateShaderResourceView(buffer.Get(), &srv, &view);
    };
    hr = structured(pose.groups.data(), static_cast<UINT>(pose.groups.size()), sizeof(War3RenderGroupMatrixV2),
        buffers.groups, buffers.groupView);
    if (FAILED(hr)) return hr;
    hr = structured(pose.alphas.data(), static_cast<UINT>(pose.alphas.size()), sizeof(float), buffers.alphas, buffers.alphaView);
    if (FAILED(hr)) return hr;
    if (!pose.colors.empty()) {
        hr = structured(pose.colors.data(), static_cast<UINT>(pose.colors.size()), sizeof(War3RenderMeshColorV2), buffers.colors, buffers.colorView);
        if (FAILED(hr)) return hr;
    }
    if (pose.dynamicMaterialAlphas) {
        if (!pose.layerAlphas.empty()) {
            hr = structured(pose.layerAlphas.data(), static_cast<UINT>(pose.layerAlphas.size()), sizeof(float), buffers.layerAlphas, buffers.layerAlphaView);
            if (FAILED(hr)) return hr;
        }
#ifdef WAR3_RENDER_TEST_FAULTS
        if (ConsumeTestFault(7)) return E_FAIL;
#endif
    }
    if (pose.dynamicMaterialStates) {
        buffers.materialStateData = pose.materialStates;
        if (!pose.materialStates.empty()) {
            hr = structured(pose.materialStates.data(), static_cast<UINT>(pose.materialStates.size()), sizeof(War3RenderMaterialStateV2), buffers.materialStates, buffers.materialStateView);
            if (FAILED(hr)) return hr;
        }
#ifdef WAR3_RENDER_TEST_FAULTS
        if (ConsumeTestFault(8)) return E_FAIL;
#endif
    }
    return S_OK;
}
HRESULT D3D11Scene::PrepareSkinRoster(const SkinPose& pose, const ModelScene& verifiedStaticLayout,
    std::array<SkinGpuBuffers, 2>& candidate, std::vector<std::pair<UINT, UINT>>& ranges) {
    if (!skinned_ || !device_ || !context_
        || !pose.sceneVersion || !pose.contentVersion
        || (pose.sceneVersion==skinSceneVersion_ && pose.contentVersion<=skinContentVersion_)
        || verifiedStaticLayout.models.size()!=modelRanges_.size()
        || verifiedStaticLayout.meshGroups!=skinMeshGroups_
        || verifiedStaticLayout.modelGroupCounts.size()!=modelRanges_.size()
        || verifiedStaticLayout.dynamicMaterialAlphas != skinDynamicMaterialAlphas_
        || verifiedStaticLayout.dynamicMaterialStates != skinDynamicMaterialStates_
        || (skinDynamicMaterialAlphas_ && verifiedStaticLayout.modelLayerCounts != skinModelLayerCounts_)
        || !ValidMaterialPose(pose, skinModelLayerCounts_, skinDynamicMaterialAlphas_)
        || !ValidMaterialStatePose(pose, skinModelLayerCounts_, skinDynamicMaterialStates_, modelTextures_.size())
        || (skinDynamicColors_ ? pose.colors.size()!=pose.alphas.size() : !pose.colors.empty())) return E_INVALIDARG;
    for (std::size_t model=0;model<modelRanges_.size();++model) {
        const auto& old=modelRanges_[model];const auto& verified=verifiedStaticLayout.models[model];
        if (old.firstMesh!=verified.firstMesh || old.meshCount!=verified.meshCount) return E_INVALIDARG;
    }
    return CreateRosterBuffers(pose, modelRanges_.size(), candidate, ranges);
}
HRESULT D3D11Scene::CreateRosterBuffers(const SkinPose& pose, std::size_t modelCount,
    std::array<SkinGpuBuffers, 2>& candidate, std::vector<std::pair<UINT, UINT>>& ranges) {
    const auto fits=[](std::size_t count,std::size_t stride) {
        return count<=(std::numeric_limits<UINT>::max)()/stride;
    };
    if (!fits(pose.instances.size(),sizeof(SkinGpuInstance))
        || !fits(pose.groups.size(),sizeof(War3RenderGroupMatrixV2)) || !fits(pose.alphas.size(),sizeof(float))
        || !fits(pose.colors.size(),sizeof(War3RenderMeshColorV2)) || !fits(pose.layerAlphas.size(),sizeof(float))
        || !fits(pose.materialStates.size(),sizeof(War3RenderMaterialStateV2))) return E_INVALIDARG;
    ranges.assign(modelCount, {0, 0});
    std::uint32_t previous=0;
    for (std::size_t index=0;index<pose.instances.size();++index) {
        const auto model=pose.instances[index].instance.model;
        if (model>=ranges.size() || model<previous) return E_INVALIDARG;
        previous=model;auto& range=ranges[model];
        if (!range.second) range.first=static_cast<UINT>(index);
        ++range.second;
    }
    for (auto& slot:candidate) {
        const auto result=CreateSkinBuffers(pose,slot);
        if (FAILED(result)) return result;
    }
    const auto result=device_->GetDeviceRemovedReason();
    if (FAILED(result)) return result;
#ifdef WAR3_RENDER_TEST_FAULTS
    if (ConsumeTestFault(2)) return E_FAIL;
#endif
    return S_OK;
}
void D3D11Scene::PublishSkinRoster(const SkinPose& pose, std::array<SkinGpuBuffers, 2>& candidate,
    std::vector<std::pair<UINT, UINT>>& ranges) noexcept {
    skinBuffers_.swap(candidate);modelInstanceRanges_.swap(ranges);
    modelInstanceCount_=static_cast<UINT>(pose.instances.size());activeSkinBuffer_=0;
    skinGroupCount_=static_cast<UINT>(pose.groups.size());skinAlphaCount_=static_cast<UINT>(pose.alphas.size());
    skinLayerAlphaCount_=static_cast<UINT>(pose.layerAlphas.size());skinDynamicMaterialAlphas_=pose.dynamicMaterialAlphas;
    skinMaterialStateCount_=static_cast<UINT>(pose.materialStates.size());skinDynamicMaterialStates_=pose.dynamicMaterialStates;
    skinSceneVersion_=pose.sceneVersion;skinContentVersion_=pose.contentVersion;
    if (!pose.instances.empty()) ++matrixUploadCount_; // 空绑定没有动态GPU上传。
}
HRESULT D3D11Scene::RebindSkinRoster(const SkinPose& pose, const ModelScene& verifiedStaticLayout) try {
    // 旧基础入口继续拒绝活跃掩码/粒子，不把旧编号解释成新的实例编号。
    if (editorVisibility_ || particleStatus_.sceneVersion) return E_INVALIDARG;
    std::array<SkinGpuBuffers, 2> candidate;
    std::vector<std::pair<UINT, UINT>> ranges;
    const auto result = PrepareSkinRoster(pose, verifiedStaticLayout, candidate, ranges);
    if (FAILED(result)) return result;
    PublishSkinRoster(pose, candidate, ranges);
    return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (const std::length_error&) { return E_INVALIDARG; }
HRESULT D3D11Scene::PrepareInstanceRoster(std::shared_ptr<const SkinPose> ownedPose, const ModelScene& verifiedStaticLayout,
    std::shared_ptr<const ParticleFrameData> particles, std::shared_ptr<const EditorVisibility> visibility,
    std::unique_ptr<PreparedInstanceRoster>& output) try {
    if (!ownedPose || !visibility || (!!particles != (particleStatus_.sceneVersion != 0))) return E_INVALIDARG;
    const auto& pose = *ownedPose;
    const auto& status = visibility->status;
    if (status.instanceCount != pose.instances.size() || status.sceneVersion != pose.sceneVersion
        || status.contentVersion != pose.contentVersion || !status.modelEpoch || !status.presentationRevision
        || visibility->modelRuns.size() != modelRanges_.size()
        || status.mode != (particles ? WAR3_EDITOR_PRESENTATION_PARTICLES : WAR3_EDITOR_PRESENTATION_SKINNED)
        || status.particleDrawCount != (particles ? particles->draws.size() : 0)
        || visibility->particleDrawVisible.size() != status.particleDrawCount) return E_INVALIDARG;
    auto candidate = std::make_unique<PreparedInstanceRoster>();
    candidate->pose = std::move(ownedPose); candidate->particles = std::move(particles); candidate->visibility = std::move(visibility);
    if (candidate->particles) {
        const auto& frame = *candidate->particles;
        if (frame.sceneVersion != pose.sceneVersion || frame.contentVersion != pose.contentVersion
            || frame.vertices.size() > 1048576 || frame.draws.size() > 65536) return E_INVALIDARG;
        for (const auto& draw : frame.draws) if (draw.texture >= particleTextures_.size()) return E_INVALIDARG;
        const auto result = PrepareParticleFrame(frame, candidate->particleFrame);
        if (FAILED(result)) return result;
#ifdef WAR3_RENDER_TEST_FAULTS
        if (ConsumeTestFault(1)) return E_FAIL;
#endif
    }
    const auto result = PrepareSkinRoster(pose, verifiedStaticLayout, candidate->buffers, candidate->ranges);
    if (FAILED(result)) return result;
    output = std::move(candidate);
    return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (const std::length_error&) { return E_INVALIDARG; }
HRESULT D3D11Scene::PrepareAssetRoster(std::shared_ptr<const ModelScene> models, std::shared_ptr<const SkinPose> pose,
    std::shared_ptr<const ParticleSceneData> particleAssets, std::shared_ptr<const ParticleFrameData> particles,
    std::shared_ptr<const EditorVisibility> visibility, std::unique_ptr<PreparedInstanceRoster>& output) try {
    if (!skinned_ || !models || !pose || !visibility || !models->staticBase || models->initialSkin != pose
        || !pose->sceneVersion || !pose->contentVersion || pose->sceneVersion <= skinSceneVersion_
        || (skinDynamicColors_ && !models->dynamicColors)
        || (skinDynamicMaterialAlphas_ && !models->dynamicMaterialAlphas)
        || (skinDynamicMaterialStates_ && !models->dynamicMaterialStates)
        || !ValidMaterialPose(*pose, models->modelLayerCounts, models->dynamicMaterialAlphas)
        || !ValidMaterialStatePose(*pose, models->modelLayerCounts, models->dynamicMaterialStates, models->TextureCount())
        || (models->dynamicColors ? pose->colors.size() != pose->alphas.size() : !pose->colors.empty())
        || (particleStatus_.sceneVersion && !particles) || (!!particleAssets != !!particles)) return E_INVALIDARG;
    if (pose->dynamicMaterialStates) {
        const auto hr = InitializeMaterialStateSamplers(); if (FAILED(hr)) return hr;
    }
    // 初始空场景没有上传实例；首次append按既有可选路径准备shader/CB，失败不发布。
    if (!pose->instances.empty() && !skinVertexShader_) {
        const auto hr = InitializeSkinning(); if (FAILED(hr)) return hr;
    }
    const auto& status = visibility->status;
    if (status.instanceCount != pose->instances.size() || status.sceneVersion != pose->sceneVersion
        || status.contentVersion != pose->contentVersion || !status.modelEpoch || !status.presentationRevision
        || visibility->modelRuns.size() != models->models.size()
        || status.mode != (particles ? WAR3_EDITOR_PRESENTATION_PARTICLES : WAR3_EDITOR_PRESENTATION_SKINNED)
        || status.particleDrawCount != (particles ? particles->draws.size() : 0)
        || visibility->particleDrawVisible.size() != status.particleDrawCount) return E_INVALIDARG;
    auto candidate = std::make_unique<PreparedInstanceRoster>();
    candidate->assets = std::make_unique<PreparedStaticAssets>();
    auto hr = PrepareStaticAssets(*models, particleAssets.get(), *candidate->assets); if (FAILED(hr)) return hr;
    candidate->assets->dynamicColors = models->dynamicColors;
    candidate->assets->dynamicMaterialAlphas = models->dynamicMaterialAlphas;
    candidate->assets->dynamicMaterialStates = models->dynamicMaterialStates;
    candidate->pose = std::move(pose); candidate->particles = std::move(particles); candidate->visibility = std::move(visibility);
    if (candidate->particles) {
        const auto& frame = *candidate->particles;
        if (frame.sceneVersion != candidate->pose->sceneVersion || frame.contentVersion != candidate->pose->contentVersion
            || frame.vertices.size() > 1048576 || frame.draws.size() > 65536) return E_INVALIDARG;
        for (const auto& draw : frame.draws) if (draw.texture >= candidate->assets->particleTextures.size()) return E_INVALIDARG;
        hr = PrepareParticleFrame(frame, candidate->particleFrame); if (FAILED(hr)) return hr;
#ifdef WAR3_RENDER_TEST_FAULTS
        if (ConsumeTestFault(1)) return E_FAIL;
#endif
    }
    hr = CreateRosterBuffers(*candidate->pose, models->models.size(), candidate->buffers, candidate->ranges);
    if (FAILED(hr)) return hr;
    output = std::move(candidate); return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (const std::length_error&) { return E_INVALIDARG; }
void D3D11Scene::PublishInstanceRoster(PreparedInstanceRoster& candidate) noexcept {
    if (candidate.assets) {
        auto& assets = *candidate.assets;
        modelPages_.swap(assets.pages); modelMeshPages_.swap(assets.meshPages);
        modelTextures_.swap(assets.textures); particleTextures_.swap(assets.particleTextures);
        modelLayers_.swap(assets.layers); modelMeshes_.swap(assets.meshes); modelRanges_.swap(assets.models);
        skinMeshGroups_.swap(assets.meshGroups); skinMeshColors_.swap(assets.meshColors);
        skinModelLayerCounts_.swap(assets.modelLayerCounts);
        modelVertexCount_ = assets.vertexCount; modelIndexCount_ = assets.indexCount;
        skinDynamicColors_ = assets.dynamicColors;
        skinDynamicMaterialAlphas_ = assets.dynamicMaterialAlphas;
        skinDynamicMaterialStates_ = assets.dynamicMaterialStates;
        if (assets.modelUpload) ++staticUploadCount_;
        if (assets.particleTextureUpload) ++particleStatus_.textureUploadCount;
    }
    PublishSkinRoster(*candidate.pose, candidate.buffers, candidate.ranges);
    if (candidate.particles) PublishParticleFrame(*candidate.particles, candidate.particleFrame);
    editorVisibility_.swap(candidate.visibility);
}
HRESULT D3D11Scene::SetSkinPose(const SkinPose& pose) {
    if (!skinned_ || pose.sceneVersion != skinSceneVersion_ || pose.contentVersion <= skinContentVersion_
        || pose.instances.size() != modelInstanceCount_ || pose.groups.size() != skinGroupCount_
        || pose.alphas.size() != skinAlphaCount_ || (skinDynamicColors_ ? pose.colors.size() != skinAlphaCount_ : !pose.colors.empty())
        || pose.layerAlphas.size() != skinLayerAlphaCount_
        || pose.materialStates.size() != skinMaterialStateCount_
        || !ValidMaterialPose(pose, skinModelLayerCounts_, skinDynamicMaterialAlphas_)
        || !ValidMaterialStatePose(pose, skinModelLayerCounts_, skinDynamicMaterialStates_, modelTextures_.size())) return E_INVALIDARG;
    const auto next = 1u - activeSkinBuffer_;
    auto& buffer = skinBuffers_[next];
    if (!pose.instances.empty()) {
        if (!buffer.instances || !buffer.groups || !buffer.alphas || (skinDynamicColors_ && !buffer.colors)
            || buffer.instanceData.size() != pose.instances.size()
            || (skinLayerAlphaCount_ && (!buffer.layerAlphas || !buffer.layerAlphaView))
            || (skinMaterialStateCount_ && (!buffer.materialStates || !buffer.materialStateView || buffer.materialStateData.size() != pose.materialStates.size()))) return E_FAIL;
        for (std::size_t index = 0; index < pose.instances.size(); ++index)
            buffer.instanceData[index] = {pose.instances[index], skinDynamicMaterialAlphas_ ? pose.layerSlices[index].first : 0,
                skinDynamicMaterialStates_ ? pose.materialStateSlices[index].first : 0};
        // DEFAULT资源由D3D11跟踪正在使用的旧内容；不重新建静态资源，也不借用宿主指针跨调用。
        context_->UpdateSubresource(buffer.instances.Get(), 0, nullptr, buffer.instanceData.data(), 0, 0);
        context_->UpdateSubresource(buffer.groups.Get(), 0, nullptr, pose.groups.data(), 0, 0);
        context_->UpdateSubresource(buffer.alphas.Get(), 0, nullptr, pose.alphas.data(), 0, 0);
        if (skinDynamicColors_) context_->UpdateSubresource(buffer.colors.Get(), 0, nullptr, pose.colors.data(), 0, 0);
        if (skinDynamicMaterialAlphas_) {
            if (skinLayerAlphaCount_) context_->UpdateSubresource(buffer.layerAlphas.Get(), 0, nullptr, pose.layerAlphas.data(), 0, 0);
#ifdef WAR3_RENDER_TEST_FAULTS
            if (ConsumeTestFault(7)) return E_FAIL;
#endif
        }
        if (skinDynamicMaterialStates_) {
            if (skinMaterialStateCount_) {
                std::copy(pose.materialStates.begin(), pose.materialStates.end(), buffer.materialStateData.begin());
                context_->UpdateSubresource(buffer.materialStates.Get(), 0, nullptr, buffer.materialStateData.data(), 0, 0);
            }
#ifdef WAR3_RENDER_TEST_FAULTS
            if (ConsumeTestFault(8)) return E_FAIL;
#endif
        }
    }
    const auto hr = device_->GetDeviceRemovedReason();
    if (FAILED(hr)) return hr;
#ifdef WAR3_RENDER_TEST_FAULTS
    if (ConsumeTestFault(2)) return E_FAIL;
#endif
    activeSkinBuffer_ = next; skinContentVersion_ = pose.contentVersion;
    if (!pose.instances.empty()) ++matrixUploadCount_;
    return S_OK;
}
War3RenderSkinStatusV2 D3D11Scene::SkinStatus() const noexcept {
    War3RenderSkinStatusV2 status{};
    status.structSize = sizeof(status); status.active = skinned_ ? 1u : 0u;
    status.sceneVersion = skinSceneVersion_; status.contentVersion = skinContentVersion_;
    status.staticUploadCount = staticUploadCount_; status.matrixUploadCount = matrixUploadCount_;
    return status;
}
}
