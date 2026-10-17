#include "D3D11Scene.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <new>
#include <stdexcept>
#include "SceneVS.h"
#include "ScenePS.h"
#include "TerrainVS.h"
#include "TerrainPS.h"
#include "ModelVS.h"
#include "ModelPS.h"

namespace War3Map::Render {
using namespace DirectX;
Environment DefaultEnvironment() {
    Environment environment;
    auto& v = environment.values;
    v.structSize = sizeof(War3RenderEnvironmentV2);
    const float toward[]{-0.45f, 0.35f, 0.82f};
    std::copy(std::begin(toward), std::end(toward), v.towardLight);
    std::fill(std::begin(v.lightColor), std::end(v.lightColor), 0.6698592f);
    std::fill(std::begin(v.ambientColor), std::end(v.ambientColor), 0.35f);
    v.fogMode = WAR3_RENDER_FOG_NONE;
    return environment;
}
HRESULT D3D11Scene::Buffer(const void* data, UINT bytes, UINT bind, ID3D11Buffer** output) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = data;
    return device_->CreateBuffer(&desc, data ? &initial : nullptr, output);
}
HRESULT D3D11Scene::Initialize(bool warp) {
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL actual{};
    HRESULT hr = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
        &device_, &actual, &context_);
    if (FAILED(hr)) return hr;
    featureLevel_ = static_cast<UINT>(actual);
    softwareDevice_ = warp; // 创建的实际驱动类型；此处没有硬件失败后的隐式回退。
    Ptr<IDXGIDevice> dxgiDevice;
    Ptr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(device_.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter))
        && SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
        char name[512]{};
        if (WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, name, sizeof(name), nullptr, nullptr) > 0) {
            auto count = std::min(std::strlen(name), adapterName_.size() - 1);
            // 截断只落在 UTF-8 字符边界，不能产出半个多字节字符。
            while (count && (static_cast<unsigned char>(name[count]) & 0xc0u) == 0x80u) --count;
            std::copy_n(name, count, adapterName_.data());
        }
    }
    hr = device_->CreateVertexShader(kSceneVS, sizeof(kSceneVS), nullptr, &vertexShader_);
    if (FAILED(hr)) return hr;
    hr = device_->CreatePixelShader(kScenePS, sizeof(kScenePS), nullptr, &pixelShader_);
    if (FAILED(hr)) return hr;
    constexpr D3D11_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    hr = device_->CreateInputLayout(elements, 2, kSceneVS, sizeof(kSceneVS), &layout_);
    if (FAILED(hr)) return hr;
    hr = device_->CreateVertexShader(kTerrainVS, sizeof(kTerrainVS), nullptr, &terrainVertexShader_);
    if (FAILED(hr)) return hr;
    hr = device_->CreatePixelShader(kTerrainPS, sizeof(kTerrainPS), nullptr, &terrainPixelShader_);
    if (FAILED(hr)) return hr;
    constexpr D3D11_INPUT_ELEMENT_DESC terrainElements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    hr = device_->CreateInputLayout(terrainElements, 3, kTerrainVS, sizeof(kTerrainVS), &terrainLayout_);
    if (FAILED(hr)) return hr;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MinLOD = 0; sampler.MaxLOD = D3D11_FLOAT32_MAX; // PS 逐 cell 夹取两级，再自行插值。
    hr = device_->CreateSamplerState(&sampler, &terrainSampler_);
    if (FAILED(hr)) return hr;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    hr = device_->CreateRasterizerState(&raster, &rasterizer_);
    if (FAILED(hr)) return hr;
    // 地表过渡层：颜色按贴图 alpha 混合，目标 alpha 保持不透明（截图约定 alpha=255）。
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device_->CreateBlendState(&blend, &overlayBlend_);
    if (FAILED(hr)) return hr;
    // 过渡层与基础层同一组顶点坐标，深度相等时也要通过。
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr = device_->CreateDepthStencilState(&depth, &terrainDepth_);
    if (FAILED(hr)) return hr;
    hr = Buffer(nullptr, sizeof(XMFLOAT4X4), D3D11_BIND_CONSTANT_BUFFER, &constants_);
    if (FAILED(hr)) return hr;
    hr = Buffer(nullptr, sizeof(TextureParameters), D3D11_BIND_CONSTANT_BUFFER, &terrainConstants_);
    if (FAILED(hr)) return hr;
    hr = device_->CreateVertexShader(kModelVS, sizeof(kModelVS), nullptr, &modelVertexShader_);
    if (FAILED(hr)) return hr;
    hr = device_->CreatePixelShader(kModelPS, sizeof(kModelPS), nullptr, &modelPixelShader_);
    if (FAILED(hr)) return hr;
    // 槽 0 每顶点；槽 1 每实例（跳过 model 编号，3 行变换 + 颜色）。
    constexpr D3D11_INPUT_ELEMENT_DESC modelElements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 4, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 20, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 36, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 52, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
    static_assert(sizeof(War3RenderModelInstanceV2) == 64 && sizeof(War3RenderModelVertexV2) == 32);
    hr = device_->CreateInputLayout(modelElements, 7, kModelVS, sizeof(kModelVS), &modelLayout_);
    if (FAILED(hr)) return hr;
    D3D11_SAMPLER_DESC modelSampler{};
    modelSampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    modelSampler.AddressU = modelSampler.AddressV = modelSampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    modelSampler.MaxAnisotropy = 1;
    modelSampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    modelSampler.MaxLOD = D3D11_FLOAT32_MAX;
    // 粒子保持原三线性；模型预览使用8×各向异性，四种TEXS寻址从模型desc继承。
    hr = device_->CreateSamplerState(&modelSampler, &particleSampler_);
    if (FAILED(hr)) return hr;
    modelSampler.Filter = D3D11_FILTER_ANISOTROPIC;
    modelSampler.MaxAnisotropy = 8;
    hr = device_->CreateSamplerState(&modelSampler, &modelSampler_);
    if (FAILED(hr)) return hr;
    hr = Buffer(nullptr, sizeof(ModelLayerConstants), D3D11_BIND_CONSTANT_BUFFER, &modelConstants_);
    if (FAILED(hr)) return hr;
    hr = Buffer(nullptr, sizeof(EnvironmentConstants), D3D11_BIND_CONSTANT_BUFFER, &environmentConstants_);
    if (FAILED(hr)) return hr;
    D3D11_SAMPLER_DESC shadowSampler{};
    shadowSampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    shadowSampler.AddressU = shadowSampler.AddressV = shadowSampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    shadowSampler.MaxAnisotropy = 1;
    shadowSampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    shadowSampler.MaxLOD = D3D11_FLOAT32_MAX; // 边框为 0：阴影图外无阴影
    hr = device_->CreateSamplerState(&shadowSampler, &shadowSampler_);
    if (FAILED(hr)) return hr;
    hr = SetEnvironment(DefaultEnvironment());
    if (FAILED(hr)) return hr;
    // 过滤模式 2..6 的混合；目标 alpha 保持不变（截图约定不透明）。0/1 不混合。
    for (std::size_t mode = 2; mode < modelBlend_.size(); ++mode) {
        D3D11_BLEND_DESC desc{};
        auto& target = desc.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        switch (mode) {
        case 2: target.SrcBlend = D3D11_BLEND_SRC_ALPHA; target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; break;
        case 3: case 4: target.SrcBlend = D3D11_BLEND_SRC_ALPHA; target.DestBlend = D3D11_BLEND_ONE; break;
        case 5: target.SrcBlend = D3D11_BLEND_ZERO; target.DestBlend = D3D11_BLEND_SRC_COLOR; break;
        default: target.SrcBlend = D3D11_BLEND_DEST_COLOR; target.DestBlend = D3D11_BLEND_SRC_COLOR; break;
        }
        hr = device_->CreateBlendState(&desc, &modelBlend_[mode]);
        if (FAILED(hr)) return hr;
    }
    D3D11_DEPTH_STENCIL_DESC readOnly{};
    readOnly.DepthEnable = TRUE;
    readOnly.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    readOnly.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr = device_->CreateDepthStencilState(&readOnly, &modelDepthRead_);
    if (FAILED(hr)) return hr;
    D3D11_DEPTH_STENCIL_DESC always{};
    always.DepthEnable = FALSE;
    always.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    always.DepthFunc = D3D11_COMPARISON_ALWAYS;
    hr = device_->CreateDepthStencilState(&always, &overlayDepth_);
    if (FAILED(hr)) return hr;
    std::vector<War3RenderVertexV2> grid;
    for (int index = -16; index <= 16; ++index) {
        const float position = static_cast<float>(index) * 128.0f;
        const float color = index == 0 ? 0.5f : 0.24f;
        grid.push_back({position, -2048, 0, 0.18f, color, 0.30f});
        grid.push_back({position, 2048, 0, 0.18f, color, 0.30f});
        grid.push_back({-2048, position, 0, color, 0.22f, 0.24f});
        grid.push_back({2048, position, 0, color, 0.22f, 0.24f});
    }
    gridCount_ = static_cast<UINT>(grid.size());
    return Buffer(grid.data(), static_cast<UINT>(grid.size() * sizeof(grid[0])), D3D11_BIND_VERTEX_BUFFER, &grid_);
}
HRESULT D3D11Scene::SetTarget(HWND window, UINT width, UINT height) {
    if (window == window_ && width == width_ && height == height_ && targetView_) return S_OK;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    targetView_.Reset(); target_.Reset(); depthView_.Reset(); depth_.Reset();
    HRESULT hr = S_OK;
    if (window != window_) swap_.Reset();
    if (window) {
        if (swap_) {
            hr = swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        } else {
            Ptr<IDXGIDevice> dxgi;
            Ptr<IDXGIAdapter> adapter;
            Ptr<IDXGIFactory> factory;
            hr = device_.As(&dxgi);
            if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
            if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
            if (FAILED(hr)) return hr;
            DXGI_SWAP_CHAIN_DESC desc{};
            desc.BufferDesc.Width = width; desc.BufferDesc.Height = height;
            desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.OutputWindow = window;
            desc.Windowed = TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = factory->CreateSwapChain(device_.Get(), &desc, &swap_);
            if (SUCCEEDED(hr)) factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        }
        if (SUCCEEDED(hr)) hr = swap_->GetBuffer(0, IID_PPV_ARGS(&target_));
    } else {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = device_->CreateTexture2D(&desc, nullptr, &target_);
    }
    if (FAILED(hr)) return hr;
    hr = device_->CreateRenderTargetView(target_.Get(), nullptr, &targetView_);
    if (FAILED(hr)) return hr;
    D3D11_TEXTURE2D_DESC desc{};
    target_->GetDesc(&desc);
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = device_->CreateTexture2D(&desc, nullptr, &depth_);
    if (SUCCEEDED(hr)) hr = device_->CreateDepthStencilView(depth_.Get(), nullptr, &depthView_);
    if (FAILED(hr)) return hr;
    window_ = window; width_ = width; height_ = height;
    return S_OK;
}
// 已给多级时原样上传；只给 mip0 时由 GPU 生成完整链（模型远处采样用；地表只采样声明的级别）。
HRESULT D3D11Scene::CreateTexture(const TextureRgba& source, Ptr<ID3D11ShaderResourceView>& view) {
    if (source.mips.empty()) return E_INVALIDARG;
    const auto& top = source.mips.front();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = top.width; desc.Height = top.height;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    Ptr<ID3D11Texture2D> texture;
    if (source.mips.size() > 1 || (top.width == 1 && top.height == 1)) {
        desc.MipLevels = static_cast<UINT>(source.mips.size());
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> data;
        data.reserve(source.mips.size());
        for (const auto& mip : source.mips) data.push_back({mip.rgba.data(), mip.width * 4, 0});
        auto hr = device_->CreateTexture2D(&desc, data.data(), &texture);
        if (FAILED(hr)) return hr;
        return device_->CreateShaderResourceView(texture.Get(), nullptr, &view);
    }
    desc.MipLevels = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    auto hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) return hr;
    context_->UpdateSubresource(texture.Get(), 0, nullptr, top.rgba.data(), top.width * 4, 0);
    hr = device_->CreateShaderResourceView(texture.Get(), nullptr, &view);
    if (FAILED(hr)) return hr;
    context_->GenerateMips(view.Get());
    return S_OK;
}
HRESULT D3D11Scene::SetOverlay(const std::vector<War3RenderVertexV2>& vertices) {
    overlay_.Reset(); overlayCount_ = 0;
    if (vertices.empty()) return S_OK;
    const auto hr = Buffer(vertices.data(), static_cast<UINT>(vertices.size() * sizeof(War3RenderVertexV2)),
        D3D11_BIND_VERTEX_BUFFER, &overlay_);
    if (SUCCEEDED(hr)) overlayCount_ = static_cast<UINT>(vertices.size());
    return hr;
}
HRESULT D3D11Scene::SetEnvironment(const Environment& environment) try {
    const auto& v = environment.values;
    Ptr<ID3D11ShaderResourceView> shadow;
    if (v.shadowWidth != 0) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = v.shadowWidth; desc.Height = v.shadowHeight;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial{environment.shadow.data(), v.shadowWidth, 0};
        Ptr<ID3D11Texture2D> texture;
        auto hr = device_->CreateTexture2D(&desc, &initial, &texture);
        if (FAILED(hr)) return hr;
        hr = device_->CreateShaderResourceView(texture.Get(), nullptr, &shadow);
        if (FAILED(hr)) return hr;
    }
    EnvironmentConstants constants{};
    const auto length = std::sqrt(v.towardLight[0] * v.towardLight[0] + v.towardLight[1] * v.towardLight[1]
        + v.towardLight[2] * v.towardLight[2]);
    for (int i = 0; i < 3; ++i) {
        constants.towardLight[i] = v.towardLight[i] / length;
        constants.diffuse[i] = v.lightColor[i];
        constants.ambient[i] = v.ambientColor[i];
        constants.fogColor[i] = v.fogColor[i];
    }
    constants.fogMode = static_cast<float>(v.fogMode);
    constants.fogStart = v.fogStart; constants.fogEnd = v.fogEnd; constants.fogDensity = v.fogDensity;
    if (shadow) {
        constants.shadowOrigin[0] = v.shadowOriginX; constants.shadowOrigin[1] = v.shadowOriginY;
        constants.shadowScale[0] = 1.0f / (static_cast<float>(v.shadowWidth) * v.shadowCellSize);
        constants.shadowScale[1] = 1.0f / (static_cast<float>(v.shadowHeight) * v.shadowCellSize);
        constants.shadowDarkness = v.shadowDarkness;
    }
    context_->UpdateSubresource(environmentConstants_.Get(), 0, nullptr, &constants, 0, 0);
    shadow_ = std::move(shadow);
    return S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
}
void D3D11Scene::DrawTerrainBatches(bool translucent) {
    context_->IASetInputLayout(terrainLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(War3RenderTerrainVertexV2), offset = 0;
    auto* vertexBuffer = vertices_.Get();
    context_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    context_->IASetIndexBuffer(indices_.Get(), DXGI_FORMAT_R32_UINT, 0);
    context_->VSSetShader(terrainVertexShader_.Get(), nullptr, 0);
    auto* sampler = terrainSampler_.Get();
    context_->PSSetSamplers(0, 1, &sampler);
    auto* parameters = terrainConstants_.Get();
    context_->PSSetConstantBuffers(1, 1, &parameters);
    context_->OMSetDepthStencilState(translucent ? modelDepthRead_.Get() : terrainDepth_.Get(), 0);
    for (const auto& batch : batches_) {
        const bool textured = batch.textureIndex != WAR3_RENDER_HEIGHT_COLOR_TEXTURE;
        const bool isTranslucent = textured && (batch.textureIndex & WAR3_RENDER_TERRAIN_TRANSLUCENT) != 0;
        if (isTranslucent != translucent) continue;
        const bool blended = isTranslucent || (textured && (batch.textureIndex & WAR3_RENDER_TERRAIN_ALPHA_BLEND) != 0);
        const auto slot = batch.textureIndex & ~(WAR3_RENDER_TERRAIN_ALPHA_BLEND | WAR3_RENDER_TERRAIN_TRANSLUCENT);
        if (textured) {
            auto textureParameters = textureParameters_[slot];
            textureParameters.alphaBlend = blended ? 1.0f : 0.0f;
            textureParameters.isTranslucent = isTranslucent ? 1.0f : 0.0f;
            if (isTranslucent) textureParameters.tapLimit = 1.0f; // 水面等透明批次保留原精确路径。
            context_->UpdateSubresource(terrainConstants_.Get(), 0, nullptr, &textureParameters, 0, 0);
        }
        auto* resource = textured ? textures_[slot].Get() : nullptr;
        context_->PSSetShaderResources(0, 1, &resource);
        context_->PSSetShader(textured ? terrainPixelShader_.Get() : pixelShader_.Get(), nullptr, 0);
        context_->OMSetBlendState(blended ? overlayBlend_.Get() : nullptr, nullptr, 0xffffffff);
        context_->DrawIndexed(batch.indexCount, batch.firstIndex, 0);
    }
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context_->OMSetDepthStencilState(nullptr, 0);
    ID3D11ShaderResourceView* empty = nullptr;
    context_->PSSetShaderResources(0, 1, &empty);
}
HRESULT D3D11Scene::SetMesh(const Mesh& mesh) try {
    Ptr<ID3D11Buffer> vertices, indices;
    std::vector<Ptr<ID3D11ShaderResourceView>> textures;
    std::vector<TextureParameters> parameters;
    auto batches = mesh.batches;
    textures.reserve(mesh.textures.size());
    parameters.reserve(mesh.textures.size());
    // 所有资源先创建到局部所有者；任一步失败都不替换现有 GPU 场景。
    for (const auto& source : mesh.textures) {
        Ptr<ID3D11ShaderResourceView> view;
        const auto hr = CreateTexture(source, view);
        if (FAILED(hr)) return hr;
        textures.push_back(std::move(view));
        parameters.push_back({static_cast<float>(source.mips.front().width), static_cast<float>(source.mips.front().height),
            static_cast<float>(source.atlasColumns), static_cast<float>(source.atlasRows),
            static_cast<float>(source.maxSampleMip), 0, 0, source.maxSampleMip > 0 ? 4.0f : 1.0f});
    }
    if (!mesh.indices.empty()) {
        const auto* data = mesh.terrain ? static_cast<const void*>(mesh.terrainVertices.data()) : mesh.vertices.data();
        const auto bytes = mesh.terrain ? mesh.terrainVertices.size() * sizeof(War3RenderTerrainVertexV2)
            : mesh.vertices.size() * sizeof(War3RenderVertexV2);
        auto hr = Buffer(data, static_cast<UINT>(bytes), D3D11_BIND_VERTEX_BUFFER, &vertices);
        if (FAILED(hr)) return hr;
        hr = Buffer(mesh.indices.data(), static_cast<UINT>(mesh.indices.size() * sizeof(std::uint32_t)), D3D11_BIND_INDEX_BUFFER, &indices);
        if (FAILED(hr)) return hr;
    }
    vertices_ = std::move(vertices); indices_ = std::move(indices);
    textures_.swap(textures); textureParameters_.swap(parameters); batches_.swap(batches); terrain_ = mesh.terrain;
    indexCount_ = static_cast<UINT>(mesh.indices.size());
    return S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
} catch (const std::length_error&) {
    return E_INVALIDARG;
}
HRESULT D3D11Scene::Draw(const War3RenderCameraV2& camera, std::vector<std::uint8_t>* capture, const std::atomic<bool>& closing,
    float (&viewProjection)[16], DrawTimings* timings) {
    using Clock = std::chrono::steady_clock;
    const auto nanoseconds = [](Clock::duration duration) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
    };
    const auto drawStart = Clock::now();
    DrawTimings measured;
    const auto target = XMVectorSet(camera.targetX, camera.targetY, camera.targetZ, 1);
    const auto eye = XMVectorAdd(target, XMVectorSet(camera.distance * std::cos(camera.pitchRadians) * std::cos(camera.yawRadians),
        camera.distance * std::cos(camera.pitchRadians) * std::sin(camera.yawRadians), camera.distance * std::sin(camera.pitchRadians), 0));
    XMFLOAT4X4 matrix;
    XMStoreFloat4x4(&matrix, XMMatrixLookAtRH(eye, target, XMVectorSet(0, 0, 1, 0)) *
        XMMatrixPerspectiveFovRH(XM_PIDIV4, static_cast<float>(width_) / static_cast<float>(height_), 1.0f, 1000000.0f));
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &matrix, 0, 0);
    std::memcpy(viewProjection, &matrix, sizeof(matrix));
    constexpr float background[]{0.045f, 0.060f, 0.075f, 1};
    context_->ClearRenderTargetView(targetView_.Get(), background);
    context_->ClearDepthStencilView(depthView_.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    auto* view = targetView_.Get();
    context_->OMSetRenderTargets(1, &view, depthView_.Get());
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_.Get());
    const bool terrain = terrain_ && indexCount_ != 0;
    context_->IASetInputLayout(terrain ? terrainLayout_.Get() : layout_.Get());
    context_->VSSetShader(terrain ? terrainVertexShader_.Get() : vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    auto* constants = constants_.Get();
    context_->VSSetConstantBuffers(0, 1, &constants);
    // 环境常量（模型光照、雾、阴影）供地表与模型两类着色器共用。
    auto* environment = environmentConstants_.Get();
    context_->VSSetConstantBuffers(3, 1, &environment);
    context_->PSSetConstantBuffers(3, 1, &environment);
    auto* shadowSampler = shadowSampler_.Get();
    context_->PSSetSamplers(2, 1, &shadowSampler);
    auto* shadow = shadow_.Get();
    context_->PSSetShaderResources(1, 1, &shadow);
    const UINT stride = static_cast<UINT>(terrain ? sizeof(War3RenderTerrainVertexV2) : sizeof(War3RenderVertexV2)), offset = 0;
    auto* vertexBuffer = indexCount_ ? vertices_.Get() : grid_.Get();
    context_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    if (indexCount_) {
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetIndexBuffer(indices_.Get(), DXGI_FORMAT_R32_UINT, 0);
        if (terrain) DrawTerrainBatches(false);
        else context_->DrawIndexed(indexCount_, 0, 0);
    } else {
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        context_->Draw(gridCount_, 0);
    }
    // 顺序：不透明地形与过渡层 → 不透明模型 → 半透明地形（水面）→ 模型混合层。
    DrawModels(0);
    DrawParticles(0);
    DrawModelLayers(0);
    if (terrain) DrawTerrainBatches(true);
    DrawModels(1);
    DrawParticles(1);
    DrawModelLayers(1);
    if (overlayCount_ != 0) {
        // 编辑叠加线：基础着色器（顶点色、无雾），不做深度测试，始终可见。
        const UINT lineStride = sizeof(War3RenderVertexV2), lineOffset = 0;
        auto* lines = overlay_.Get();
        context_->IASetInputLayout(layout_.Get());
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        context_->VSSetConstantBuffers(0, 1, &constants);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        context_->OMSetDepthStencilState(overlayDepth_.Get(), 0);
        context_->IASetVertexBuffers(0, 1, &lines, &lineStride, &lineOffset);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        context_->Draw(overlayCount_, 0);
        context_->OMSetDepthStencilState(nullptr, 0);
    }
    measured.drawCpuNs = nanoseconds(Clock::now() - drawStart);
    if (capture) {
        const auto readbackStart = Clock::now();
#ifdef WAR3_RENDER_TEST_FAULTS
        if (ConsumeTestFault(3)) return E_FAIL;
#endif
        D3D11_TEXTURE2D_DESC desc{};
        target_->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        Ptr<ID3D11Texture2D> staging;
        auto hr = device_->CreateTexture2D(&desc, nullptr, &staging);
        if (FAILED(hr)) return hr;
        context_->CopyResource(staging.Get(), target_.Get());
        context_->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            if (closing.load()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr != DXGI_ERROR_WAS_STILL_DRAWING) break;
            if (std::chrono::steady_clock::now() >= deadline) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            Sleep(1);
        }
        if (FAILED(hr)) return hr;
        // 尺寸由 ABI 上限约束；分配失败也必须先 Unmap。
        try {
            capture->resize(static_cast<std::size_t>(width_) * height_ * 4);
            for (UINT row = 0; row < height_; ++row) std::memcpy(capture->data() + static_cast<std::size_t>(row) * width_ * 4,
                static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch, static_cast<std::size_t>(width_) * 4);
        } catch (...) { context_->Unmap(staging.Get(), 0); throw; }
        context_->Unmap(staging.Get(), 0);
        measured.readbackCpuNs = nanoseconds(Clock::now() - readbackStart);
    }
    const auto presentStart = Clock::now();
    measured.presentResult = swap_ ? swap_->Present(0, 0) : device_->GetDeviceRemovedReason();
    const auto completed = Clock::now();
    measured.presentCpuNs = swap_ ? nanoseconds(completed - presentStart) : 0;
    measured.completedSteadyNs = nanoseconds(completed.time_since_epoch());
    if (timings) *timings = measured;
    return measured.presentResult;
}
D3D11Scene::~D3D11Scene() {
    // 附加层只释放自己拥有的资源，不能清空共享立即上下文的世界状态。
    for (auto& layer : additionalModels_) layer = {};
    if (context_ && !borrowedContext_) { context_->ClearState(); context_->Flush(); }
}
}
