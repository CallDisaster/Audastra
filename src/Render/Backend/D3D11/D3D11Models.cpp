#include "D3D11Scene.h"
#include <new>
#include <stdexcept>
#include <limits>

namespace War3Map::Render {
HRESULT D3D11Scene::SetModels(const ModelScene& models, const SkinPose* latestPose) try {
    // 追加页必须由整组事务安装，不能误当成完整场景再次上传suffix。
    if (models.staticBase) return E_INVALIDARG;
    const auto* pose = models.initialSkin ? (latestPose ? latestPose : models.initialSkin.get()) : nullptr;
    if (pose && (pose->sceneVersion != models.initialSkin->sceneVersion || pose->instances.size() != models.instances.size()))
        return E_INVALIDARG;
    std::vector<std::uint32_t> modelLayerCounts;
    if (!CountModelDrawLayers(models, modelLayerCounts)
        || (pose && !ValidMaterialPose(*pose, modelLayerCounts, models.dynamicMaterialAlphas))
        || (pose && !ValidMaterialStatePose(*pose, modelLayerCounts, models.dynamicMaterialStates, models.TextureCount()))
        || (models.dynamicMaterialStates && models.materialStateTextureCount != models.TextureCount())
        || (!pose && (models.dynamicMaterialAlphas || models.dynamicMaterialStates))) return E_INVALIDARG;
    if (models.explicitStaticSamplers && (pose || models.staticSamplerFlags.size() != models.layers.size())) return E_INVALIDARG;
    auto staticSamplerFlags = models.staticSamplerFlags;
    if (models.explicitStaticSamplers
        && std::uint64_t(staticSamplerFlags.capacity()) * sizeof(std::uint32_t) > models.staticSamplerBackendBudget) return E_INVALIDARG;
    if ((pose && pose->dynamicMaterialStates) || models.explicitStaticSamplers) {
        const auto hr = InitializeMaterialStateSamplers(); if (FAILED(hr)) return hr;
    }
    if (pose && !models.instances.empty() && !skinVertexShader_) {
        // 可选路径按需初始化；蒙皮资源失败不能让仍可用的旧静态路径一起失效。
        const auto hr = InitializeSkinning();
        if (FAILED(hr)) return hr;
    }
    Ptr<ID3D11Buffer> vertices, indices, instances;
    std::array<SkinGpuBuffers, 2> skin;
    std::vector<Ptr<ID3D11ShaderResourceView>> textures;
    std::vector<GeometryPage> pages(models.indices.empty() ? 0u : 1u);
    std::vector<UINT> meshPages(models.meshes.size(), 0);
    // 所有可分配CPU元数据也先复制，不能在GPU句柄已交换后再抛出bad_alloc。
    auto layers = models.layers; auto meshes = models.meshes; auto modelRanges = models.models;
    auto meshGroups = models.meshGroups;
    auto meshColors = models.meshColors;
    if (!meshColors.empty() && meshColors.size() != models.meshes.size()) return E_INVALIDARG;
    std::vector<std::pair<UINT, UINT>> ranges(models.models.size(), {0, 0});
    for (std::size_t i = 0; i < models.instances.size(); ++i) {
        auto& range = ranges[models.instances[i].model];
        if (range.second == 0) range.first = static_cast<UINT>(i);
        ++range.second;
    }
    textures.reserve(models.textures.size());
    for (const auto& source : models.textures) {
        Ptr<ID3D11ShaderResourceView> view;
        const auto hr = CreateTexture(source, view);
        if (FAILED(hr)) return hr;
        textures.push_back(std::move(view));
    }
    if (!models.instances.empty()) {
        HRESULT hr;
        if (pose) {
            std::vector<War3RenderSkinnedModelVertexV2> interleaved;
            interleaved.reserve(models.vertices.size());
            for (std::size_t i = 0; i < models.vertices.size(); ++i)
                interleaved.push_back({models.vertices[i], models.vertexGroups[i]});
            hr = Buffer(interleaved.data(), static_cast<UINT>(interleaved.size() * sizeof(War3RenderSkinnedModelVertexV2)),
                D3D11_BIND_VERTEX_BUFFER, &vertices);
        } else {
            hr = Buffer(models.vertices.data(), static_cast<UINT>(models.vertices.size() * sizeof(War3RenderModelVertexV2)),
                D3D11_BIND_VERTEX_BUFFER, &vertices);
        }
        if (FAILED(hr)) return hr;
        hr = Buffer(models.indices.data(), static_cast<UINT>(models.indices.size() * sizeof(std::uint16_t)),
            D3D11_BIND_INDEX_BUFFER, &indices);
        if (FAILED(hr)) return hr;
        if (pose) {
            for (auto& slot : skin) { hr = CreateSkinBuffers(*pose, slot); if (FAILED(hr)) return hr; }
        } else {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(models.instances.size() * sizeof(War3RenderModelInstanceV2));
            desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            const D3D11_SUBRESOURCE_DATA initial{models.instances.data(), 0, 0};
            hr = device_->CreateBuffer(&desc, &initial, &instances);
            if (FAILED(hr)) return hr;
        }
    }
    const auto deviceResult = device_->GetDeviceRemovedReason();
    if (FAILED(deviceResult)) return deviceResult;
#ifdef WAR3_RENDER_TEST_FAULTS
    if (ConsumeTestFault(2)) return E_FAIL;
#endif
    if (!pages.empty()) { pages[0].vertices = vertices; pages[0].indices = indices; }
    // 从这里开始只move/swap标量及拥有者，无分配，整组发布。
    modelVertices_ = std::move(vertices); modelIndices_ = std::move(indices); modelInstances_ = std::move(instances);
    modelTextures_.swap(textures); modelLayers_.swap(layers); modelMeshes_.swap(meshes); modelRanges_.swap(modelRanges);
    modelStaticSamplerFlags_.swap(staticSamplerFlags);
    modelInstanceRanges_.swap(ranges); skinMeshGroups_.swap(meshGroups); skinBuffers_.swap(skin);
    skinModelLayerCounts_.swap(modelLayerCounts);
    modelPages_.swap(pages); modelMeshPages_.swap(meshPages);
    modelVertexCount_ = models.VertexCount(); modelIndexCount_ = models.IndexCount();
    skinMeshColors_.swap(meshColors);
    modelInstanceCount_ = static_cast<UINT>(models.instances.size());
    activeSkinBuffer_ = 0; skinned_ = pose != nullptr; skinDynamicColors_ = pose && (models.dynamicColors || !pose->colors.empty());
    skinDynamicMaterialAlphas_ = pose && models.dynamicMaterialAlphas;
    skinDynamicMaterialStates_ = pose && models.dynamicMaterialStates;
    skinSceneVersion_ = pose ? pose->sceneVersion : 0; skinContentVersion_ = pose ? pose->contentVersion : 0;
    skinGroupCount_ = pose ? static_cast<UINT>(pose->groups.size()) : 0;
    skinAlphaCount_ = pose ? static_cast<UINT>(pose->alphas.size()) : 0;
    skinLayerAlphaCount_ = pose ? static_cast<UINT>(pose->layerAlphas.size()) : 0;
    skinMaterialStateCount_ = pose ? static_cast<UINT>(pose->materialStates.size()) : 0;
    ClearEditorVisibility();
    if (!models.instances.empty()) { ++staticUploadCount_; if (pose) ++matrixUploadCount_; }
    return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (const std::length_error&) { return E_INVALIDARG; }

HRESULT D3D11Scene::PrepareStaticAssets(const ModelScene& models, const ParticleSceneData* particles,
    PreparedStaticAssets& output) {
    if (!models.staticBase || models.prefixVertices != modelVertexCount_ || models.prefixIndices != modelIndexCount_
        || models.prefixTextures != modelTextures_.size() || models.prefixLayers != modelLayers_.size()
        || models.prefixMeshes != modelMeshes_.size() || models.prefixModels != modelRanges_.size()
        || models.meshGroups.size() != models.meshes.size() || models.modelGroupCounts.size() != models.models.size()
        || (!models.meshColors.empty() && models.meshColors.size() != models.meshes.size())
        || models.StaticVertexGroups().size() != models.StaticVertices().size()
        || (particles && particles->prefixTextures != particleTextures_.size())
        || (!particles && !particleTextures_.empty())) return E_INVALIDARG;
    if (!CountModelDrawLayers(models, output.modelLayerCounts)) return E_INVALIDARG;
    // prefix来自已验证的owned基准，元数据也逐项核对驻留GPU布局，不能借append修改旧页。
    for (std::size_t i = 0; i < modelRanges_.size(); ++i)
        if (models.models[i].firstMesh != modelRanges_[i].firstMesh || models.models[i].meshCount != modelRanges_[i].meshCount
            || output.modelLayerCounts[i] != skinModelLayerCounts_[i]) return E_INVALIDARG;
    for (std::size_t i = 0; i < skinMeshGroups_.size(); ++i)
        if (models.meshGroups[i] != skinMeshGroups_[i]) return E_INVALIDARG;
    output.pages = modelPages_; output.meshPages = modelMeshPages_;
    output.textures = modelTextures_; output.particleTextures = particleTextures_;
    output.layers = models.layers; output.meshes = models.meshes; output.models = models.models;
    output.meshGroups = models.meshGroups; output.meshColors = models.meshColors;
    output.vertexCount = models.VertexCount(); output.indexCount = models.IndexCount();
    output.dynamicMaterialAlphas = models.dynamicMaterialAlphas;
    output.dynamicMaterialStates = models.dynamicMaterialStates;
    if (!models.StaticIndices().empty()) {
        GeometryPage page; page.firstVertex = models.prefixVertices; page.firstIndex = models.prefixIndices;
        std::vector<War3RenderSkinnedModelVertexV2> vertices;
        const auto& sourceVertices = models.StaticVertices(); const auto& sourceGroups = models.StaticVertexGroups();
        vertices.reserve(sourceVertices.size());
        for (std::size_t i = 0; i < sourceVertices.size(); ++i) vertices.push_back({sourceVertices[i], sourceGroups[i]});
        auto hr = Buffer(vertices.data(), static_cast<UINT>(vertices.size() * sizeof(War3RenderSkinnedModelVertexV2)), D3D11_BIND_VERTEX_BUFFER, &page.vertices);
        if (FAILED(hr)) return hr;
        const auto& indices = models.StaticIndices();
        hr = Buffer(indices.data(), static_cast<UINT>(indices.size() * sizeof(std::uint16_t)), D3D11_BIND_INDEX_BUFFER, &page.indices);
        if (FAILED(hr)) return hr;
        const auto pageIndex = static_cast<UINT>(output.pages.size());
        output.pages.push_back(std::move(page));
        output.meshPages.resize(models.meshes.size(), pageIndex);
        output.modelUpload = true;
    } else if (models.meshes.size() != modelMeshes_.size()) return E_INVALIDARG;
#ifdef WAR3_RENDER_TEST_FAULTS
    if (ConsumeTestFault(4)) return E_FAIL;
#endif
    for (const auto& texture : models.StaticTextures()) {
        Ptr<ID3D11ShaderResourceView> view;
        const auto hr = CreateTexture(texture, view); if (FAILED(hr)) return hr;
        output.textures.push_back(std::move(view)); output.modelUpload = true;
    }
    if (particles) for (const auto& texture : particles->textures) {
        Ptr<ID3D11ShaderResourceView> view;
        const auto hr = CreateTexture(texture, view); if (FAILED(hr)) return hr;
        output.particleTextures.push_back(std::move(view)); output.particleTextureUpload = true;
    }
#ifdef WAR3_RENDER_TEST_FAULTS
    if (ConsumeTestFault(5)) return E_FAIL;
#endif
    return device_->GetDeviceRemovedReason();
}

HRESULT D3D11Scene::SetModelInstances(const std::vector<War3RenderModelInstanceV2>& instances) {
    if (skinned_ || instances.size() != modelInstanceCount_) return E_INVALIDARG;
    if (instances.empty()) return S_OK;
    context_->UpdateSubresource(modelInstances_.Get(), 0, nullptr, instances.data(), 0, 0);
    return S_OK;
}
void D3D11Scene::DrawModels(int pass) {
    auto* instanceBuffer = skinned_ ? skinBuffers_[activeSkinBuffer_].instances.Get() : modelInstances_.Get();
    if (!instanceBuffer) return;
    context_->IASetInputLayout(skinned_ ? skinLayout_.Get() : modelLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT strides[]{static_cast<UINT>(skinned_ ? sizeof(War3RenderSkinnedModelVertexV2) : sizeof(War3RenderModelVertexV2)),
        static_cast<UINT>(skinned_ ? sizeof(SkinGpuInstance) : sizeof(War3RenderModelInstanceV2))};
    const UINT offsets[]{0, 0};
    context_->VSSetShader(skinned_ ? skinVertexShader_.Get() : modelVertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(modelPixelShader_.Get(), nullptr, 0);
    auto* constants = modelConstants_.Get();
    context_->VSSetConstantBuffers(2, 1, &constants); context_->PSSetConstantBuffers(2, 1, &constants);
    auto* sampler = modelSampler_.Get(); context_->PSSetSamplers(1, 1, &sampler);
    if (skinned_) {
        auto* meshConstants = skinMeshConstants_.Get(); context_->VSSetConstantBuffers(4, 1, &meshConstants);
        ID3D11ShaderResourceView* views[]{skinBuffers_[activeSkinBuffer_].groupView.Get(), skinBuffers_[activeSkinBuffer_].alphaView.Get(), skinBuffers_[activeSkinBuffer_].colorView.Get(), skinBuffers_[activeSkinBuffer_].layerAlphaView.Get(), skinBuffers_[activeSkinBuffer_].materialStateView.Get()};
        context_->VSSetShaderResources(2, 5, views);
    }
    context_->OMSetDepthStencilState(pass == 0 ? terrainDepth_.Get() : modelDepthRead_.Get(), 0);
    UINT activePage = (std::numeric_limits<UINT>::max)();
    for (std::size_t model = 0; model < modelRanges_.size(); ++model) {
        const auto [firstInstance, instanceCount] = modelInstanceRanges_[model];
        if (instanceCount == 0 || (editorVisibility_ && editorVisibility_->modelRuns[model].empty())) continue;
        const auto& range = modelRanges_[model];
        std::uint32_t modelLocalLayer = 0; // 实际draw层序，不是material序号或全局layerIndex。
        for (auto meshIndex = range.firstMesh; meshIndex < range.firstMesh + range.meshCount; ++meshIndex) {
            const auto& mesh = modelMeshes_[meshIndex];
            const auto pageIndex = modelMeshPages_[meshIndex];
            const auto& page = modelPages_[pageIndex];
            if (activePage != pageIndex) {
                ID3D11Buffer* buffers[]{page.vertices.Get(), instanceBuffer};
                context_->IASetVertexBuffers(0, 2, buffers, strides, offsets);
                context_->IASetIndexBuffer(page.indices.Get(), DXGI_FORMAT_R16_UINT, 0); activePage = pageIndex;
            }
            SkinMeshConstants skinValues{};
            if (skinned_) {
                const auto color = skinMeshColors_.empty() ? War3RenderMeshColorV2{{1, 1, 1}} : skinMeshColors_[meshIndex];
                skinValues = {skinMeshGroups_[meshIndex].first, meshIndex - range.firstMesh, skinDynamicColors_ ? 1u : 0u, skinDynamicMaterialAlphas_ ? 1u : 0u,
                    {color.rgb[0], color.rgb[1], color.rgb[2]}, 0, skinDynamicMaterialStates_ ? 1u : 0u, {0,0,0}};
                if (!skinDynamicMaterialAlphas_) context_->UpdateSubresource(skinMeshConstants_.Get(), 0, nullptr, &skinValues, 0, 0);
            }
            for (auto layerIndex = mesh.firstLayer; layerIndex < mesh.firstLayer + mesh.layerCount; ++layerIndex) {
                const auto& layer = modelLayers_[layerIndex];
                // 每层都绑定，包含legacy3；前一层/前一帧的动态 sampler 不能泄漏到当前层。
                auto* staticSampler = modelStaticSamplerFlags_.empty() ? modelSampler_.Get()
                    : materialStateSamplers_[modelStaticSamplerFlags_[layerIndex]].Get();
                context_->PSSetSamplers(1, 1, &staticSampler);
                const bool blended = layer.filterMode >= 2;
                if (blended != (pass == 1)) continue;
                if (skinned_ && skinDynamicMaterialAlphas_) {
                    skinValues.localLayerIndex = modelLocalLayer + layerIndex - mesh.firstLayer;
                    context_->UpdateSubresource(skinMeshConstants_.Get(), 0, nullptr, &skinValues, 0, 0);
                }
                const bool textured = layer.textureIndex != WAR3_RENDER_MODEL_NO_TEXTURE;
                const ModelLayerConstants values{layer.alpha, layer.filterMode, layer.flags, textured ? 1u : 0u};
                context_->UpdateSubresource(modelConstants_.Get(), 0, nullptr, &values, 0, 0);
                auto* resource = textured ? modelTextures_[layer.textureIndex].Get() : nullptr;
                context_->PSSetShaderResources(0, 1, &resource);
                context_->OMSetBlendState(blended ? modelBlend_[layer.filterMode].Get() : nullptr, nullptr, 0xffffffff);
                const auto drawRun = [&](UINT first, UINT count) {
                    if (!skinDynamicMaterialStates_) {
                        context_->DrawIndexedInstanced(mesh.indexCount, count, mesh.firstIndex - page.firstIndex, static_cast<INT>(mesh.baseVertex - page.firstVertex), first); return;
                    }
                    const auto& buffer = skinBuffers_[activeSkinBuffer_];
                    const auto stateAt = [&](UINT instance) -> const War3RenderMaterialStateV2& {
                        return buffer.materialStateData[buffer.instanceData[instance].firstMaterialState + skinValues.localLayerIndex];
                    };
                    const auto end = first + count;
                    while (first < end) {
                        const auto& state = stateAt(first); auto next = first + 1;
                        // 只合并已可见的连续实例；透明层绝不按纹理全局重排。
                        while (next < end && stateAt(next).textureIndex == state.textureIndex && stateAt(next).samplerFlags == state.samplerFlags) ++next;
                        const bool hasTexture = state.textureIndex != WAR3_RENDER_MODEL_NO_TEXTURE;
                        const ModelLayerConstants dynamicValues{layer.alpha, layer.filterMode, layer.flags, hasTexture ? 1u : 0u};
                        context_->UpdateSubresource(modelConstants_.Get(), 0, nullptr, &dynamicValues, 0, 0);
                        auto* textureView = hasTexture ? modelTextures_[state.textureIndex].Get() : nullptr;
                        auto* stateSampler = materialStateSamplers_[state.samplerFlags].Get();
                        context_->PSSetShaderResources(0, 1, &textureView); context_->PSSetSamplers(1, 1, &stateSampler);
                        context_->DrawIndexedInstanced(mesh.indexCount, next - first, mesh.firstIndex - page.firstIndex, static_cast<INT>(mesh.baseVertex - page.firstVertex), first);
                        first = next;
                    }
                };
                if (editorVisibility_) { for (const auto& [first, count] : editorVisibility_->modelRuns[model]) drawRun(first, count); }
                else drawRun(firstInstance, instanceCount);
            }
            modelLocalLayer += mesh.layerCount;
        }
    }
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff); context_->OMSetDepthStencilState(nullptr, 0);
    ID3D11ShaderResourceView* empty = nullptr; context_->PSSetShaderResources(0, 1, &empty);
    ID3D11ShaderResourceView* emptySkin[]{nullptr, nullptr, nullptr, nullptr, nullptr}; context_->VSSetShaderResources(2, 5, emptySkin);
}
}
