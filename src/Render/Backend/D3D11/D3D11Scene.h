#pragma once
#include "../../Scene/ModelSceneData.h"
#include "../../Scene/ModelSceneLayerData.h"
#include "../../Particles/ParticleInput.h"
#include "War3MapCore/RenderPluginEditorVisibility.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

namespace War3Map::Render {
struct Mesh {
    std::vector<War3RenderVertexV2> vertices;
    std::vector<War3RenderTerrainVertexV2> terrainVertices;
    std::vector<std::uint32_t> indices;
    std::vector<TextureRgba> textures;
    std::vector<War3RenderTerrainBatchV2> batches;
    std::uint64_t revision = 0;
    bool terrain = false;
};
// 场景环境的 CPU 副本（已由插件入口校验）：values.shadow 不使用，像素在 shadow。
struct Environment {
    War3RenderEnvironmentV2 values{};
    std::vector<std::uint8_t> shadow;
};
// 未调用 setEnvironment 时的中性灰光（与地表 TerrainLighting 默认值一致）、无雾、无阴影。
[[nodiscard]] Environment DefaultEnvironment();
struct EditorVisibility {
    War3RenderEditorPresentationStatusV2 status{sizeof(War3RenderEditorPresentationStatusV2)};
    std::vector<std::vector<std::pair<UINT, UINT>>> modelRuns; // 原实例编号的可见连续区间，不重排实例
    std::vector<std::uint8_t> particleDrawVisible;
};
// CPU接收前按原场景布局预算拆分draw；无效输入不修改output。与GPU安装复用同一拥有数据。
[[nodiscard]] bool CopyEditorVisibility(const ModelScene& models, const ParticleFrameData* particles,
    const War3RenderEditorPresentationTargetV2& target, const War3RenderEditorVisibilityUpdateV2& input,
    EditorVisibility& output, const SkinPose* currentPose = nullptr);
// 仅渲染线程调用；不接触 Qt 或地图文档。
class D3D11Scene {
public:
    // CPU 提交、GPU 等待读回和 Present 独立计时；不冒称 GPU 执行时长。
    struct DrawTimings {
        std::uint64_t drawCpuNs = 0, presentCpuNs = 0, readbackCpuNs = 0, completedSteadyNs = 0;
        HRESULT presentResult = S_OK;
    };
    HRESULT Initialize(bool warp);
    HRESULT SetTarget(HWND window, UINT width, UINT height);
    HRESULT SetMesh(const Mesh& mesh);
    HRESULT SetModels(const ModelScene& models, const SkinPose* latestPose = nullptr);
    HRESULT SetSkinPose(const SkinPose& pose);
    // 基础重绑定：调用方已通过共享校验及资产身份CAS确认verifiedStaticLayout。
    // 仅换动态两槽与实例区间；有呈现掩码或活跃粒子时拒绝，须由后续联合事务接线。
    HRESULT RebindSkinRoster(const SkinPose& pose, const ModelScene& verifiedStaticLayout);
    struct PreparedInstanceRoster;
    HRESULT PrepareInstanceRoster(std::shared_ptr<const SkinPose> pose, const ModelScene& verifiedStaticLayout,
        std::shared_ptr<const ParticleFrameData> particles, std::shared_ptr<const EditorVisibility> visibility,
        std::unique_ptr<PreparedInstanceRoster>& output);
    HRESULT PrepareAssetRoster(std::shared_ptr<const ModelScene> models, std::shared_ptr<const SkinPose> pose,
        std::shared_ptr<const ParticleSceneData> particleAssets, std::shared_ptr<const ParticleFrameData> particles,
        std::shared_ptr<const EditorVisibility> visibility, std::unique_ptr<PreparedInstanceRoster>& output);
    // 仅在插件基准CAS成功后的渲染线程调用；所有分配/D3D失败均已发生在Prepare。
    void PublishInstanceRoster(PreparedInstanceRoster& prepared) noexcept;
    [[nodiscard]] War3RenderSkinStatusV2 SkinStatus() const noexcept;
    HRESULT SetParticleModels(const ModelScene& models, const SkinPose& pose,
        const ParticleSceneData& particles, const ParticleFrameData& frame);
    HRESULT SetParticleFrame(const SkinPose& pose, const ParticleFrameData& frame);
    void ClearParticles() noexcept;
    [[nodiscard]] War3RenderParticleStatusV2 ParticleStatus() const noexcept;
    void SetModelLayers(const ModelSceneLayers& layers);
    [[nodiscard]] std::vector<War3RenderModelLayerStatusV2> ModelLayerStatuses() const;
    [[nodiscard]] UINT ModelInstanceCount() const noexcept { return modelInstanceCount_; }
    [[nodiscard]] War3RenderEditorPresentationStatusV2 EditorVisibilityStatus() const noexcept {
        return editorVisibility_ ? editorVisibility_->status
            : War3RenderEditorPresentationStatusV2{sizeof(War3RenderEditorPresentationStatusV2)};
    }
    [[nodiscard]] const std::array<char, 256>& AdapterName() const noexcept { return adapterName_; }
    [[nodiscard]] UINT FeatureLevel() const noexcept { return featureLevel_; }
    [[nodiscard]] bool SoftwareDevice() const noexcept { return softwareDevice_; }
    HRESULT SetEditorVisibility(std::shared_ptr<const EditorVisibility> visibility) noexcept;
    void ClearEditorVisibility() noexcept;
#ifdef WAR3_RENDER_TEST_FAULTS
    // 仅测试 DLL 拥有的单次提交边界故障；生产 ABI 和二进制均不包含控制入口。
    void SetTestFaultSource(std::atomic<std::uint32_t>* source) noexcept { testFault_ = source; }
    [[nodiscard]] std::array<std::uint64_t, 7> TestAssetResources() const noexcept {
        const auto id = [](const auto* value) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value)); };
        return {modelPages_.size(), modelTextures_.size(), particleTextures_.size(),
            modelPages_.empty() ? 0 : id(modelPages_[0].vertices.Get()),
            modelPages_.empty() ? 0 : id(modelPages_[0].indices.Get()),
            modelTextures_.empty() ? 0 : id(modelTextures_[0].Get()),
            particleTextures_.empty() ? 0 : id(particleTextures_[0].Get())};
    }
#endif
    HRESULT SetEnvironment(const Environment& environment);
    // 只覆盖当前模型场景的实例变换与颜色（数量须一致）。
    HRESULT SetModelInstances(const std::vector<War3RenderModelInstanceV2>& instances);
    // 编辑叠加线（成对顶点的线段列表）；空清除。
    HRESULT SetOverlay(const std::vector<War3RenderVertexV2>& vertices);
    HRESULT Draw(const War3RenderCameraV2& camera, std::vector<std::uint8_t>* capture, const std::atomic<bool>& closing,
        float (&viewProjection)[16], DrawTimings* timings = nullptr);
    ~D3D11Scene();
private:
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    HRESULT Buffer(const void* data, UINT bytes, UINT bind, ID3D11Buffer** output);
    HRESULT CreateTexture(const TextureRgba& source, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view);
    HRESULT InitializeSkinning();
    HRESULT InitializeMaterialStateSamplers();
    static bool CountModelDrawLayers(const ModelScene& models, std::vector<std::uint32_t>& counts);
    static bool ValidMaterialPose(const SkinPose& pose, const std::vector<std::uint32_t>& counts, bool material) noexcept;
    static bool ValidMaterialStatePose(const SkinPose& pose, const std::vector<std::uint32_t>& counts,
        bool enabled, std::size_t textureCount) noexcept;
    // 仅GPU内部排布；外部72-byte实例合同和旧firstGroup/firstAlpha偏移保持。
    struct SkinGpuInstance {
        War3RenderSkinnedModelInstanceV2 instance;
        std::uint32_t firstLayerAlpha = 0;
        std::uint32_t firstMaterialState = 0;
    };
    static_assert(sizeof(SkinGpuInstance) == 80);
    static_assert(offsetof(SkinGpuInstance, firstLayerAlpha) == 72);
    static_assert(offsetof(SkinGpuInstance, firstMaterialState) == 76);
    struct SkinGpuBuffers {
        Ptr<ID3D11Buffer> instances, groups, alphas, colors, layerAlphas, materialStates;
        Ptr<ID3D11ShaderResourceView> groupView, alphaView, colorView, layerAlphaView, materialStateView;
        std::vector<SkinGpuInstance> instanceData; // 随两槽分配，热更新不再分配转换暂存。
        std::vector<War3RenderMaterialStateV2> materialStateData; // 驻留CPU状态用于连续run绑定，不读资产。
    };
    struct GeometryPage {
        Ptr<ID3D11Buffer> vertices, indices;
        UINT firstVertex = 0, firstIndex = 0;
    };
    struct PreparedStaticAssets {
        std::vector<GeometryPage> pages;
        std::vector<UINT> meshPages;
        std::vector<Ptr<ID3D11ShaderResourceView>> textures, particleTextures;
        std::vector<War3RenderModelLayerV2> layers;
        std::vector<War3RenderModelMeshV2> meshes;
        std::vector<War3RenderModelV2> models;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> meshGroups;
        std::vector<War3RenderMeshColorV2> meshColors;
        UINT vertexCount = 0, indexCount = 0;
        std::vector<std::uint32_t> modelLayerCounts;
        bool modelUpload = false, particleTextureUpload = false, dynamicColors = false, dynamicMaterialAlphas = false;
        bool dynamicMaterialStates = false;
    };
    HRESULT PrepareStaticAssets(const ModelScene& models, const ParticleSceneData* particles, PreparedStaticAssets& output);
    HRESULT CreateRosterBuffers(const SkinPose& pose, std::size_t modelCount,
        std::array<SkinGpuBuffers, 2>& buffers, std::vector<std::pair<UINT, UINT>>& ranges);
    HRESULT CreateSkinBuffers(const SkinPose& pose, SkinGpuBuffers& buffers);
    HRESULT PrepareSkinRoster(const SkinPose& pose, const ModelScene& verifiedStaticLayout,
        std::array<SkinGpuBuffers, 2>& buffers, std::vector<std::pair<UINT, UINT>>& ranges);
    void PublishSkinRoster(const SkinPose& pose, std::array<SkinGpuBuffers, 2>& buffers,
        std::vector<std::pair<UINT, UINT>>& ranges) noexcept;
    HRESULT InitializeParticles();
    struct ParticleGpuFrame {
        Ptr<ID3D11Buffer> vertices;
        UINT capacity = 0;
        std::vector<War3RenderParticleDrawV2> draws;
        std::vector<std::uint32_t> drawMaterialFlags;
    };
    HRESULT PrepareParticleFrame(const ParticleFrameData& frame, ParticleGpuFrame& candidate);
    void PublishParticleFrame(const ParticleFrameData& frame, ParticleGpuFrame& candidate) noexcept;
    void DrawParticles(int pass);
#ifdef WAR3_RENDER_TEST_FAULTS
    bool ConsumeTestFault(std::uint32_t stage) noexcept {
        auto expected = stage;
        return testFault_ && testFault_->compare_exchange_strong(expected, 0);
    }
    std::atomic<std::uint32_t>* testFault_ = nullptr;
#endif
    void DrawModels(int pass); // 0 = 不透明/透明测试（写深度），1 = 混合层（只读深度）
    void DrawModelLayers(int pass);
    struct ModelLayerSlot {
        std::shared_ptr<const ModelSceneLayerData> requested, accepted;
        std::unique_ptr<D3D11Scene> gpu;
        HRESULT result = S_OK;
    };
    std::array<ModelLayerSlot, WAR3_RENDER_MAX_MODEL_SCENE_LAYERS> additionalModels_;
    std::size_t additionalModelCount_ = 0;
    bool borrowedContext_ = false;
    void DrawTerrainBatches(bool translucent); // false = 不透明与过渡层，true = 水面等半透明批次
    Ptr<ID3D11Device> device_;
    std::array<char, 256> adapterName_{};
    UINT featureLevel_ = 0;
    bool softwareDevice_ = false;
    Ptr<ID3D11DeviceContext> context_;
    Ptr<IDXGISwapChain> swap_;
    Ptr<ID3D11Texture2D> target_, depth_;
    Ptr<ID3D11RenderTargetView> targetView_;
    Ptr<ID3D11DepthStencilView> depthView_;
    Ptr<ID3D11VertexShader> vertexShader_;
    Ptr<ID3D11PixelShader> pixelShader_;
    Ptr<ID3D11InputLayout> layout_;
    Ptr<ID3D11VertexShader> terrainVertexShader_;
    Ptr<ID3D11PixelShader> terrainPixelShader_;
    Ptr<ID3D11InputLayout> terrainLayout_;
    Ptr<ID3D11SamplerState> terrainSampler_;
    struct TextureParameters {
        float width, height, columns, rows;
        float maxSampleMip, alphaBlend, unused[2]; // alphaBlend：1 = 输出贴图 alpha（过渡层）
    };
    static_assert(sizeof(TextureParameters) == 32);
    std::vector<TextureParameters> textureParameters_;
    std::vector<Ptr<ID3D11ShaderResourceView>> textures_;
    std::vector<War3RenderTerrainBatchV2> batches_;
    Ptr<ID3D11RasterizerState> rasterizer_;
    Ptr<ID3D11RasterizerState> groundDecalRasterizer_; // 显式GroundDecalPreview层独用，不改地形/模型。
    // 模型：顶点/索引/实例缓冲、贴图、每模型实例区间；按过滤模式分两遍绘制。
    Ptr<ID3D11VertexShader> modelVertexShader_;
    Ptr<ID3D11PixelShader> modelPixelShader_;
    Ptr<ID3D11InputLayout> modelLayout_;
    Ptr<ID3D11SamplerState> modelSampler_;
    std::array<Ptr<ID3D11SamplerState>, 4> materialStateSamplers_;
    Ptr<ID3D11Buffer> modelVertices_, modelIndices_, modelInstances_, modelConstants_;
    std::vector<GeometryPage> modelPages_;
    std::vector<UINT> modelMeshPages_;
    UINT modelVertexCount_ = 0, modelIndexCount_ = 0;
    std::array<Ptr<ID3D11BlendState>, 7> modelBlend_;
    Ptr<ID3D11DepthStencilState> modelDepthRead_;
    std::vector<Ptr<ID3D11ShaderResourceView>> modelTextures_;
    std::vector<War3RenderModelLayerV2> modelLayers_;
    std::vector<std::uint32_t> modelStaticSamplerFlags_; // 空=legacy3；与已接受的静态 draw 层同序。
    std::vector<War3RenderModelMeshV2> modelMeshes_;
    std::vector<War3RenderModelV2> modelRanges_;
    std::vector<std::pair<UINT, UINT>> modelInstanceRanges_; // 每模型 (首实例, 数量)
    UINT modelInstanceCount_ = 0;
    std::shared_ptr<const EditorVisibility> editorVisibility_;
    Ptr<ID3D11VertexShader> skinVertexShader_;
    Ptr<ID3D11InputLayout> skinLayout_;
    Ptr<ID3D11Buffer> skinMeshConstants_;
    std::array<SkinGpuBuffers, 2> skinBuffers_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> skinMeshGroups_;
    std::vector<War3RenderMeshColorV2> skinMeshColors_;
    std::vector<std::uint32_t> skinModelLayerCounts_;
    struct SkinMeshConstants {
        std::uint32_t firstGroup, localIndex, dynamicColors, dynamicMaterialAlphas;
        float rgb[3]; std::uint32_t localLayerIndex;
        std::uint32_t dynamicMaterialStates = 0, reserved[3]{};
    };
    static_assert(sizeof(SkinMeshConstants) == 48);
    UINT activeSkinBuffer_ = 0, skinGroupCount_ = 0, skinAlphaCount_ = 0, skinLayerAlphaCount_ = 0;
    bool skinned_ = false, skinDynamicColors_ = false, skinDynamicMaterialAlphas_ = false;
    bool skinDynamicMaterialStates_ = false;
    UINT skinMaterialStateCount_ = 0;
    std::uint64_t skinSceneVersion_ = 0, skinContentVersion_ = 0;
    std::uint64_t staticUploadCount_ = 0, matrixUploadCount_ = 0;
    Ptr<ID3D11VertexShader> particleVertexShader_;
    Ptr<ID3D11PixelShader> particlePixelShader_;
    Ptr<ID3D11InputLayout> particleLayout_;
    std::array<ParticleGpuFrame, 2> particleFrames_;
    std::vector<Ptr<ID3D11ShaderResourceView>> particleTextures_;
    UINT activeParticleFrame_ = 0;
    War3RenderParticleStatusV2 particleStatus_{sizeof(War3RenderParticleStatusV2)};
    Ptr<ID3D11Buffer> overlay_;
    Ptr<ID3D11DepthStencilState> overlayDepth_;
    UINT overlayCount_ = 0;
    struct ModelLayerConstants {
        float alpha; UINT filterMode, flags, textured;
    };
    static_assert(sizeof(ModelLayerConstants) == 16);
    // 场景环境：与 Scene.hlsl 的 Environment 常量布局一致。
    struct EnvironmentConstants {
        float towardLight[3], fogMode;
        float diffuse[3], fogStart;
        float ambient[3], fogEnd;
        float fogColor[3], fogDensity;
        float shadowOrigin[2], shadowScale[2];
        float shadowDarkness, padding[3];
    };
    static_assert(sizeof(EnvironmentConstants) == 96);
    Ptr<ID3D11Buffer> environmentConstants_;
    Ptr<ID3D11ShaderResourceView> shadow_;
    Ptr<ID3D11SamplerState> shadowSampler_;
    Ptr<ID3D11BlendState> overlayBlend_;
    Ptr<ID3D11DepthStencilState> terrainDepth_;
    Ptr<ID3D11Buffer> vertices_, indices_, grid_, constants_, terrainConstants_;
    UINT width_ = 0, height_ = 0, indexCount_ = 0, gridCount_ = 0;
    HWND window_ = nullptr;
    bool terrain_ = false;
};
struct D3D11Scene::PreparedInstanceRoster {
private:
    friend class D3D11Scene;
    std::array<SkinGpuBuffers, 2> buffers;
    std::vector<std::pair<UINT, UINT>> ranges;
    std::shared_ptr<const SkinPose> pose;
    ParticleGpuFrame particleFrame;
    std::shared_ptr<const ParticleFrameData> particles;
    std::shared_ptr<const EditorVisibility> visibility;
    std::unique_ptr<PreparedStaticAssets> assets;
};
}
