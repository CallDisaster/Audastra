#include "D3D11Scene.h"
#include "SkinVS.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace War3Map::Render {
namespace {
// 记录与本槽已上传副本不同的元素区段。相隔不足 4 KiB 的区段合并成一次提交；区段过多或变化
// 超过一半时整份提交副本。无论哪种方式，GPU 最终内容都与副本逐字节相同。
template <class T>
class ChangedRanges {
public:
    explicit ChangedRanges(std::size_t count) noexcept : count_(count) {}
    void Mark(std::size_t index) noexcept {
        if (open_ && index <= last_ + kGap) { last_ = index + 1; return; }
        Close(); first_ = index; last_ = index + 1; open_ = true;
    }
    void Upload(ID3D11DeviceContext& context, ID3D11Buffer& buffer, const T* shadow) noexcept {
        Close();
        if (!rangeCount_) return;
        if (whole_ || changed_ * 2 > count_) { context.UpdateSubresource(&buffer, 0, nullptr, shadow, 0, 0); return; }
        for (std::size_t index = 0; index < rangeCount_; ++index) {
            const auto& range = ranges_[index];
            const D3D11_BOX box{static_cast<UINT>(range.first * sizeof(T)), 0, 0, static_cast<UINT>(range.last * sizeof(T)), 1, 1};
            context.UpdateSubresource(&buffer, 0, &box, shadow + range.first, 0, 0);
        }
    }
private:
    static constexpr std::size_t kGap = (std::max)(std::size_t{1}, std::size_t{4096} / sizeof(T));
    static constexpr std::size_t kMaximumRanges = 64;
    struct Range { std::size_t first = 0, last = 0; };
    void Close() noexcept {
        if (!open_) return;
        open_ = false; changed_ += last_ - first_;
        if (rangeCount_ < kMaximumRanges) ranges_[rangeCount_++] = {first_, last_};
        else whole_ = true;
    }
    std::array<Range, kMaximumRanges> ranges_{};
    std::size_t count_ = 0, rangeCount_ = 0, changed_ = 0, first_ = 0, last_ = 0;
    bool open_ = false, whole_ = false;
};
// 按字节比较（与整份上传同一口径，-0/+0 与 NaN 位型都算变化），只把变化区段写回副本并提交。
template <class T>
void UploadChanged(ID3D11DeviceContext& context, ID3D11Buffer& buffer, std::vector<T>& shadow, const T* values) noexcept {
    ChangedRanges<T> changed(shadow.size());
    for (std::size_t index = 0; index < shadow.size(); ++index) {
        if (std::memcmp(&shadow[index], &values[index], sizeof(T)) == 0) continue;
        std::memcpy(&shadow[index], &values[index], sizeof(T)); changed.Mark(index);
    }
    changed.Upload(context, buffer, shadow.data());
}
// 只比较、提交列出实例的片段；slice(实例) 返回 {首元素, 个数}，各片段须按实例序递增且在副本范围内（调用前已核对）。
// 未列出实例的副本已与本次姿势相同，结果与整份比较逐字节相同。
template <class T, class Slice>
void UploadListed(ID3D11DeviceContext& context, ID3D11Buffer& buffer, std::vector<T>& shadow, const T* values,
    const std::vector<std::uint32_t>& listed, Slice slice) noexcept {
    ChangedRanges<T> changed(shadow.size());
    for (const auto instance : listed) {
        const auto [first, count] = slice(instance);
        for (std::size_t index = first; index < std::size_t(first) + count; ++index) {
            if (std::memcmp(&shadow[index], &values[index], sizeof(T)) == 0) continue;
            std::memcpy(&shadow[index], &values[index], sizeof(T)); changed.Mark(index);
        }
    }
    changed.Upload(context, buffer, shadow.data());
}
}
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
bool D3D11Scene::ValidMaterialPose(const SkinPose& pose, const std::vector<std::uint32_t>& counts, bool material,
    const std::vector<std::uint32_t>* only) noexcept {
    if (pose.dynamicMaterialAlphas != material) return false;
    if (!material) return pose.layerAlphas.empty() && pose.layerSlices.empty();
    if (pose.layerSlices.size() != pose.instances.size()) return false;
    const auto valid = [&](std::size_t index) {
        const auto model = pose.instances[index].instance.model;
        const auto& slice = pose.layerSlices[index];
        if (model >= counts.size() || slice.count != counts[model] || slice.first > pose.layerAlphas.size()
            || slice.count > pose.layerAlphas.size() - slice.first) return false;
        if (only) for (std::uint32_t k = 0; k < slice.count; ++k) if (!std::isfinite(pose.layerAlphas[slice.first + k])) return false;
        return true;
    };
    if (only) {
        for (const auto index : *only) if (index >= pose.instances.size() || !valid(index)) return false;
        return true;
    }
    for (std::size_t index = 0; index < pose.instances.size(); ++index) if (!valid(index)) return false;
    for (const auto alpha : pose.layerAlphas) if (!std::isfinite(alpha)) return false;
    return true;
}
bool D3D11Scene::ValidMaterialStatePose(const SkinPose& pose, const std::vector<std::uint32_t>& counts,
    bool enabled, std::size_t textureCount, const std::vector<std::uint32_t>* only) noexcept {
    if (pose.dynamicMaterialStates != enabled) return false;
    if (!enabled) return pose.materialStates.empty() && pose.materialStateSlices.empty();
    if (!pose.dynamicMaterialAlphas || pose.materialStateSlices.size() != pose.instances.size()) return false;
    const auto validState = [&](const War3RenderMaterialStateV2& value) {
        if ((value.textureIndex != WAR3_RENDER_MODEL_NO_TEXTURE && value.textureIndex >= textureCount) || (value.samplerFlags & ~3u)) return false;
        for (const auto uv : value.uvTransform) if (!std::isfinite(uv)) return false;
        return true;
    };
    const auto valid = [&](std::size_t index) {
        const auto model = pose.instances[index].instance.model; const auto& slice = pose.materialStateSlices[index];
        if (model >= counts.size() || slice.count != counts[model] || slice.first > pose.materialStates.size()
            || slice.count > pose.materialStates.size() - slice.first) return false;
        if (only) for (std::uint32_t k = 0; k < slice.count; ++k) if (!validState(pose.materialStates[slice.first + k])) return false;
        return true;
    };
    if (only) {
        for (const auto index : *only) if (index >= pose.instances.size() || !valid(index)) return false;
        return true;
    }
    for (std::size_t index = 0; index < pose.instances.size(); ++index) if (!valid(index)) return false;
    for (const auto& value : pose.materialStates) if (!validState(value)) return false;
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
    buffers.sceneVersion = pose.sceneVersion; buffers.contentVersion = pose.contentVersion;
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
    buffers.groupData = pose.groups;
    hr = structured(pose.alphas.data(), static_cast<UINT>(pose.alphas.size()), sizeof(float), buffers.alphas, buffers.alphaView);
    if (FAILED(hr)) return hr;
    buffers.alphaData = pose.alphas;
    if (!pose.colors.empty()) {
        hr = structured(pose.colors.data(), static_cast<UINT>(pose.colors.size()), sizeof(War3RenderMeshColorV2), buffers.colors, buffers.colorView);
        if (FAILED(hr)) return hr;
        buffers.colorData = pose.colors;
    }
    if (pose.dynamicMaterialAlphas) {
        if (!pose.layerAlphas.empty()) {
            hr = structured(pose.layerAlphas.data(), static_cast<UINT>(pose.layerAlphas.size()), sizeof(float), buffers.layerAlphas, buffers.layerAlphaView);
            if (FAILED(hr)) return hr;
            buffers.layerAlphaData = pose.layerAlphas;
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
            || frame.vertices.size() > kMaxParticleFrameVertices || frame.draws.size() > kMaxParticleFrameDraws) return E_INVALIDARG;
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
            || frame.vertices.size() > kMaxParticleFrameVertices || frame.draws.size() > kMaxParticleFrameDraws) return E_INVALIDARG;
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
        modelSurfaces_.swap(assets.modelSurfaces);
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
HRESULT D3D11Scene::SetSkinPose(const SkinPose& pose, const SkinPoseChangeHistory* changes) try {
    if (!skinned_ || pose.sceneVersion != skinSceneVersion_ || pose.contentVersion <= skinContentVersion_
        || pose.instances.size() != modelInstanceCount_ || pose.groups.size() != skinGroupCount_
        || pose.alphas.size() != skinAlphaCount_ || (skinDynamicColors_ ? pose.colors.size() != skinAlphaCount_ : !pose.colors.empty())
        || pose.layerAlphas.size() != skinLayerAlphaCount_
        || pose.materialStates.size() != skinMaterialStateCount_) return E_INVALIDARG;
    const auto next = 1u - activeSkinBuffer_;
    auto& buffer = skinBuffers_[next];
    // 下一槽的内容版本已知、且能沿变化记录补到本版本时，只比较/提交列出实例（逐实例固定布局，片段按实例序连续）。
    // 其余实例的副本已与本次姿势相同，且在写入本槽时校验过。
    bool partial = changes && pose.matrixFormat == WAR3_RENDER_GROUP_LOCAL_COLUMN_3X4 && buffer.contentVersion
        && buffer.sceneVersion == pose.sceneVersion
        && CollectSkinPoseChanges(changes, pose.sceneVersion, buffer.contentVersion, pose.contentVersion, &skinChanges_);
    using Slice = std::pair<std::uint32_t, std::uint32_t>;
    const auto groupSlice = [&](std::uint32_t index) {
        const auto first = pose.instances[index].firstGroup;
        const auto end = index + 1 < pose.instances.size() ? pose.instances[index + 1].firstGroup : static_cast<std::uint32_t>(pose.groups.size());
        return Slice{first, end - first};
    };
    const auto alphaSlice = [&](std::uint32_t index) {
        const auto first = pose.instances[index].firstAlpha;
        const auto end = index + 1 < pose.instances.size() ? pose.instances[index + 1].firstAlpha : static_cast<std::uint32_t>(pose.alphas.size());
        return Slice{first, end - first};
    };
    const auto layerSlice = [&](std::uint32_t index) { return Slice{pose.layerSlices[index].first, pose.layerSlices[index].count}; };
    const auto stateSlice = [&](std::uint32_t index) { return Slice{pose.materialStateSlices[index].first, pose.materialStateSlices[index].count}; };
    if (partial) {
        // 片段须在范围内且按实例序不回退；不满足时不冒险，整份比较。
        std::uint64_t groupEnd = 0, alphaEnd = 0, layerEnd = 0, stateEnd = 0;
        const auto ordered = [](Slice slice, std::uint64_t& end, std::size_t size) {
            if (slice.first < end || slice.first > size || slice.second > size - slice.first) return false;
            end = std::uint64_t(slice.first) + slice.second;
            return true;
        };
        const auto nextFirst = [&](std::uint32_t index, std::uint32_t own, std::uint32_t following, std::size_t size) {
            return own <= (index + 1 < pose.instances.size() ? std::size_t(following) : size);
        };
        for (const auto index : skinChanges_) {
            if (index >= pose.instances.size()) { partial = false; break; }
            const auto following = index + 1 < pose.instances.size() ? index + 1 : index;
            if (!nextFirst(index, pose.instances[index].firstGroup, pose.instances[following].firstGroup, pose.groups.size())
                || !nextFirst(index, pose.instances[index].firstAlpha, pose.instances[following].firstAlpha, pose.alphas.size())
                || !ordered(groupSlice(index), groupEnd, pose.groups.size()) || !ordered(alphaSlice(index), alphaEnd, pose.alphas.size())
                || (skinDynamicMaterialAlphas_ && !ordered(layerSlice(index), layerEnd, pose.layerAlphas.size()))
                || (skinDynamicMaterialStates_ && !ordered(stateSlice(index), stateEnd, pose.materialStates.size()))) { partial = false; break; }
        }
    }
    const auto* only = partial ? &skinChanges_ : nullptr;
    if (!ValidMaterialPose(pose, skinModelLayerCounts_, skinDynamicMaterialAlphas_, only)
        || !ValidMaterialStatePose(pose, skinModelLayerCounts_, skinDynamicMaterialStates_, modelTextures_.size(), only)) return E_INVALIDARG;
    if (!pose.instances.empty()) {
        if (!buffer.instances || !buffer.groups || !buffer.alphas || (skinDynamicColors_ && !buffer.colors)
            || buffer.instanceData.size() != pose.instances.size() || buffer.groupData.size() != pose.groups.size()
            || buffer.alphaData.size() != pose.alphas.size() || (skinDynamicColors_ && buffer.colorData.size() != pose.colors.size())
            || (skinLayerAlphaCount_ && (!buffer.layerAlphas || !buffer.layerAlphaView || buffer.layerAlphaData.size() != pose.layerAlphas.size()))
            || (skinMaterialStateCount_ && (!buffer.materialStates || !buffer.materialStateView || buffer.materialStateData.size() != pose.materialStates.size()))) return E_FAIL;
        // 写入前先作废本槽版本：中途失败时下次整份比较（副本与已提交的 GPU 内容仍逐字节一致）。
        buffer.contentVersion = 0;
        // DEFAULT资源由D3D11跟踪正在使用的旧内容；不重新建静态资源，也不借用宿主指针跨调用。
        // 每个数组先更新本槽副本再立即提交对应区段，中途失败时副本仍与已排队的 GPU 内容一致。
        ChangedRanges<SkinGpuInstance> instances(pose.instances.size());
        const auto markInstance = [&](std::size_t index) {
            const SkinGpuInstance value{pose.instances[index], skinDynamicMaterialAlphas_ ? pose.layerSlices[index].first : 0,
                skinDynamicMaterialStates_ ? pose.materialStateSlices[index].first : 0};
            if (std::memcmp(&buffer.instanceData[index], &value, sizeof(value)) == 0) return;
            std::memcpy(&buffer.instanceData[index], &value, sizeof(value)); instances.Mark(index);
        };
        if (partial) for (const auto index : skinChanges_) markInstance(index);
        else for (std::size_t index = 0; index < pose.instances.size(); ++index) markInstance(index);
        instances.Upload(*context_.Get(), *buffer.instances.Get(), buffer.instanceData.data());
        if (partial) {
            UploadListed(*context_.Get(), *buffer.groups.Get(), buffer.groupData, pose.groups.data(), skinChanges_, groupSlice);
            UploadListed(*context_.Get(), *buffer.alphas.Get(), buffer.alphaData, pose.alphas.data(), skinChanges_, alphaSlice);
            if (skinDynamicColors_) UploadListed(*context_.Get(), *buffer.colors.Get(), buffer.colorData, pose.colors.data(), skinChanges_, alphaSlice);
        } else {
            UploadChanged(*context_.Get(), *buffer.groups.Get(), buffer.groupData, pose.groups.data());
            UploadChanged(*context_.Get(), *buffer.alphas.Get(), buffer.alphaData, pose.alphas.data());
            if (skinDynamicColors_) UploadChanged(*context_.Get(), *buffer.colors.Get(), buffer.colorData, pose.colors.data());
        }
        if (skinDynamicMaterialAlphas_) {
            if (skinLayerAlphaCount_) {
                if (partial) UploadListed(*context_.Get(), *buffer.layerAlphas.Get(), buffer.layerAlphaData, pose.layerAlphas.data(), skinChanges_, layerSlice);
                else UploadChanged(*context_.Get(), *buffer.layerAlphas.Get(), buffer.layerAlphaData, pose.layerAlphas.data());
            }
#ifdef WAR3_RENDER_TEST_FAULTS
            if (ConsumeTestFault(7)) return E_FAIL;
#endif
        }
        if (skinDynamicMaterialStates_) {
            if (skinMaterialStateCount_) {
                if (partial) UploadListed(*context_.Get(), *buffer.materialStates.Get(), buffer.materialStateData, pose.materialStates.data(), skinChanges_, stateSlice);
                else UploadChanged(*context_.Get(), *buffer.materialStates.Get(), buffer.materialStateData, pose.materialStates.data());
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
    buffer.sceneVersion = pose.sceneVersion; buffer.contentVersion = pose.contentVersion;
    if (!pose.instances.empty()) ++matrixUploadCount_;
    return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
War3RenderSkinStatusV2 D3D11Scene::SkinStatus() const noexcept {
    War3RenderSkinStatusV2 status{};
    status.structSize = sizeof(status); status.active = skinned_ ? 1u : 0u;
    status.sceneVersion = skinSceneVersion_; status.contentVersion = skinContentVersion_;
    status.staticUploadCount = staticUploadCount_; status.matrixUploadCount = matrixUploadCount_;
    return status;
}
}
