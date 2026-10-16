#pragma once
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3RenderCore/PoseBatch.h"
#include "War3RenderCore/MaterialTextures.h"
#include "../Particles/ParticleTypes.h"
#include <unordered_map>

namespace War3Map::Render {
// 逐请求来源需求常驻于scene，统一按实际容量进入帧侧固定存储；不是逐帧复制。
[[nodiscard]] inline std::uint64_t AnimationRequestGeometryBytes(std::size_t capacity) noexcept {
    return std::uint64_t(capacity) * sizeof(std::uint8_t);
}
[[nodiscard]] inline std::uint64_t AnimationRequestGeometryBytes(const AnimatedModelScene& scene) noexcept {
    return AnimationRequestGeometryBytes(scene.requestGeometryRequired.capacity());
}
inline War3RenderCore::NodeKeySamplingPolicy AnimationNodeKeyPolicy(const AnimationLimits& limits) noexcept {
    return limits.allowSourceOrderNodePreview && limits.playbackPolicy == AnimationPlaybackPolicy::InteractivePreview
        ? War3RenderCore::NodeKeySamplingPolicy::SourceOrderPreview : War3RenderCore::NodeKeySamplingPolicy::Strict;
}
inline War3RenderCore::MaterialAlphaKeySamplingPolicy AnimationMaterialAlphaKeyPolicy(const AnimationLimits& limits) noexcept {
    return limits.allowIdenticalMaterialAlphaKeysPreview && limits.playbackPolicy == AnimationPlaybackPolicy::InteractivePreview
        ? War3RenderCore::MaterialAlphaKeySamplingPolicy::IdenticalDuplicatesPreview
        : War3RenderCore::MaterialAlphaKeySamplingPolicy::Strict;
}
struct AnimationVisibilityBounds {
    std::array<double, 6> box{};
    double worldPadding = 0; // 世界空间重力及粒子 billboard 尺寸，不随实例缩小而丢失。
    bool available = false;
};
// KMTF 贴图ID或 TXAN 坐标任一逐帧变化，都需要逐帧发布材质状态。
[[nodiscard]] inline bool AnimatedMaterialState(const War3RenderCore::CompiledMaterialTextures* textures) noexcept {
    return textures && (textures->HasAnimatedTexture() || textures->HasAnimatedUv());
}
struct AnimationAsset {
    std::shared_ptr<const MdxModel> parsed;
    War3RenderCore::CompiledModel compiled;
    std::shared_ptr<const War3RenderCore::CompiledMaterialTextures> materialTextures;
    std::uint32_t evaluatorIndex = UINT32_MAX;
    War3RenderCore::AssetSource source;
    std::string unavailableCode;
    std::shared_ptr<const Particles::Asset> particles;
    std::vector<std::uint32_t> emitterNodes;
    std::vector<float> particleLayerAlpha;
    std::shared_ptr<const std::vector<std::uint32_t>> particleMaterialFlags; // 只含保留的emitter，资产与绑定共享不可变表。
    AnimationParticleCapability particleCapability;
    std::string originalParticleRejectionCode; // 静态读取结果；驻留重建不能复用上一轮 PREM 准入原因。
    bool drawable = false, cameraDependent = false, particleUnfoggedPreview = false;
    std::shared_ptr<const std::vector<std::uint32_t>> collisionNodes;
    AnimationVisibilityBounds visibilityBounds; // 原字节中的 MODL/SEQS 声明，读取后不可变。
    std::shared_ptr<const OwnedStaticModelAsset> ownedStaticAsset; // 原创静态辅助资产，与MDX/compiled互斥。
};
// 新材质通道的固定owner增量：前/当前帧vector本体与资产数组中的shared_ptr成员。
// 即便粒子关闭，空容器仍存在；payload表/输出capacity由粒子host另行收费。
[[nodiscard]] inline std::uint64_t AnimationParticleMaterialOwnerBytes(std::size_t assetCapacity) noexcept {
    return 2ull*sizeof(std::vector<std::uint32_t>)
        + std::uint64_t(assetCapacity)*sizeof(std::shared_ptr<const std::vector<std::uint32_t>>);
}
struct AnimationParticleHost;
struct AnimationPremHost;
struct AnimationVisibilityIndex;
struct AnimationCatalogIdentityIndex;
// 私有构造且只通过const shared_ptr公开。只保存已经接受的静态解析/解码内容，
// 不保存动画服务、compiled身份或预览scene；来源变化时使用方直接走正常解析。
class AnimationAssetSnapshot {
    friend class ModelAnimationService;
    struct Model {
        std::string sourceLayer, digest;
        std::shared_ptr<const MdxModel> parsed;
    };
    struct Texture {
        std::string sourceLayer, digest;
        std::uint32_t index = 0;
    };
    AnimationAssetSnapshot() = default;
    std::unordered_map<std::string, Model> models_;
    std::unordered_map<std::string, Texture> textures_;
    SharedSceneVector<ModelTextureRgba> decoded_;
    std::uint32_t maxTextureDimension_ = 0;
    bool allowRigidStaticPreview_ = false;
};
// 记录共用静态加载阶段真正访问的正/负来源；不是整个来源树的快照。
// 正项共享已经冻结的原字节，负项保留原拒绝；命中前必须用正式reader重新核对。
struct AnimationStaticResourceAccess {
    std::string path;
    std::uint64_t budget = 0;
    std::shared_ptr<const AnimationResource> resource;
    std::vector<Diagnostic> diagnostics;
};
// 只含静态缓冲，类型本身没有实例、动作时刻、相机、帧或求值器字段。
struct AnimationStaticGeometry {
    struct Geometry {
        SharedSceneVector<War3RenderModelVertexV2> vertices;
        SharedSceneVector<std::uint16_t> indices;
        SharedSceneVector<War3RenderModelLayerV2> layers;
        SharedSceneVector<War3RenderModelMeshV2> meshes;
        SharedSceneVector<War3RenderModelV2> models;
        SharedSceneVector<std::array<float, 6>> modelBounds;
        SharedSceneVector<ModelTextureRgba> textures;
        SharedSceneVector<std::uint32_t> vertexGroups, meshGeosets;
        SharedSceneVector<std::string> modelPaths, modelAssetKeys;
        SharedSceneVector<std::shared_ptr<const std::vector<ModelTextureBinding>>> modelTextureBindings;
        std::size_t untexturedLayers = 0, rigidStaticPreviewAssets = 0;
        std::vector<Diagnostic> warnings;
    } geometry;
    SharedSceneVector<War3RenderSkinnedModelVertexV2> vertices;
    SharedSceneVector<War3RenderSkinnedModelMeshV2> meshes;
    SharedSceneVector<War3RenderMeshColorV2> meshColors;
    SharedSceneVector<AnimationDrawLayerBinding> drawLayerBindings;
    SharedSceneVector<War3RenderCore::Range32> modelLayerRanges;
    SharedSceneVector<War3RenderSkinnedModelV2> models;
    SharedSceneVector<AnimationPickModel> pickModels;
    SharedSceneVector<War3RenderCore::Range32> pickMeshGroups;
    SharedSceneVector<AnimationPickGroupBounds> pickGroupBounds;
    std::vector<War3RenderCore::AssetSource> sources;
    bool dynamicColors = false, dynamicMaterialAlphas = false, dynamicMaterialStates = false;
    [[nodiscard]] static AnimationStaticGeometry Capture(const AnimatedModelScene& source);
    [[nodiscard]] AnimatedModelScene Materialize() const;
    void ApplyTo(AnimatedModelScene& target) const;
};
// 只在同一正式catalog消费；编号在那个catalog内编译后不再重绑。
// 不持有原服务/scene或任何实例运行状态，也不能与另一份同base的包拼接。
class AnimationPreparedAssets {
    friend class ModelAnimationService;
    AnimationPreparedAssets() = default;
    std::uint64_t baseSceneVersion_ = 0, baseDocumentRevision_ = 0, generation_ = 0;
    std::string sourceIdentity_, assetKey_, replacementKey_;
    std::uint32_t teamColor_ = 0, assetIndex_ = UINT32_MAX, modelIndex_ = UINT32_MAX;
    bool newAsset_ = false, collisionNodesRequired_ = false;
    AnimationAsset asset_;
    // 完整合并只在准备阶段完成；增量快照用于正式消费时再次检查当前预算。
    // 两者均只有静态缓冲，应用时不覆盖候选的实例表或运行状态。
    AnimationStaticGeometry mergedGeometry_, addedGeometry_;
    std::vector<std::uint32_t> modelAssets_;
    // 与准备时的资产池同序，仅拥有已编译不可变材质元数据；不携带实例时钟。
    std::vector<std::shared_ptr<const War3RenderCore::CompiledMaterialTextures>> materialTextures_;
    std::vector<AnimationStaticResourceAccess> accesses_;
    std::unordered_map<std::string, std::shared_ptr<const AnimationResource>> resources_;
    std::uint64_t sourceBytes_ = 0, resourceBytes_ = 0;
};
struct ModelAnimationService::State {
    std::shared_ptr<AnimatedModelScene> scene;
    std::shared_ptr<const AnimationCatalogIdentityIndex> catalogIdentity;
    std::vector<AnimationAsset> assets;
    std::vector<std::string> assetPaths;
    std::unordered_map<std::string,std::shared_ptr<const AnimationResource>> resources;
    std::uint64_t sourceBytes = 0, resourceBytes = 0;
    AnimationLimits limits;
    std::vector<AnimationStaticResourceAccess> staticResourceAccesses; // 静态准备后清空，不参与播放。
    std::vector<std::uint32_t> requestAssets, modelAssets;
    std::vector<AnimationInstanceState> initial;
    std::unique_ptr<War3RenderCore::PoseBatchEvaluator> evaluator;
    std::shared_ptr<AnimationParticleHost> particles;
    std::shared_ptr<AnimationPremHost> prem;
    std::shared_ptr<AnimationVisibilityIndex> visibility;
    std::uint64_t frameBytes = 0;
    std::uint64_t maxVisibilityBytes = 64ull * 1024 * 1024;
    std::uint64_t maxGeosetIssues = 0;
    std::uint64_t maxMaterialIssues = 0;
    std::uint64_t maxCollisionMatrices = 0;
    bool collisionNodesRequired = false;
    bool sharedPoseRanges = false;
    std::shared_ptr<const AnimationFrame> standFrame;
    std::vector<AnimationPoseLink> standPoses; // 按(几何模型,实际Stand序列)缓存；片段属于 standFrame。
    std::vector<std::uint32_t> standPoseIndices, modelStandPoses; // 几何实例→缓存；模型→首个缓存（仅用于动态槽尺寸）。
    std::vector<AnimationPoseLink> activeSlots;
    std::vector<std::vector<std::uint32_t>> modelSlots;
    std::uint32_t sharedGroupCount = 0, sharedAlphaCount = 0, sharedLayerAlphaCount = 0;
    std::uint64_t staticPoseVersion = 0;
    std::uint64_t epoch = 0;
    std::uint32_t batchWidth = 64;
    bool retainUnsupportedStatic = false;
    std::array<War3RenderCore::PoseRequest, 64> requests;
    std::array<std::uint32_t, 64> renderedIndices{};
};
[[nodiscard]] Status AnimationFailure(std::string code, std::string message);
[[nodiscard]] Result<std::shared_ptr<const AnimationCatalogIdentityIndex>> EnsureAnimationCatalogIdentityIndex(
    const AnimatedModelScene& scene, std::shared_ptr<const AnimationCatalogIdentityIndex> previous,
    std::uint64_t maxBytes);
[[nodiscard]] Status CheckAnimationStaticAppend(const AnimatedModelScene& current,
    const AnimatedModelScene& added, const WorldModelLimits& limits);
void AppendAnimationStaticGeometry(AnimatedModelScene& target, const AnimatedModelScene& added);
[[nodiscard]] bool AnimationLocalDataFailure(War3RenderCore::Code code) noexcept;
[[nodiscard]] bool AnimationLocalDataFailure(std::string_view code) noexcept;
[[nodiscard]] Status CheckAnimationState(const AnimationInstanceState& state);
[[nodiscard]] Result<AnimationInstanceState> MakeAnimationInitialState(const AnimationInstanceSpec& input,
    const AnimationAsset& asset, bool retainUnsupported, std::vector<Diagnostic>& warnings);
[[nodiscard]] Status AdvanceAnimationStates(std::vector<AnimationInstanceState>& states,
    const AnimatedModelScene& scene, const AnimationAdvance& advance);
[[nodiscard]] Result<War3RenderCore::LocalCamera> ToAnimationLocalCamera(
    const War3RenderCore::LocalCamera& camera, const std::array<float, 12>& transform);
[[nodiscard]] std::uint64_t AnimationBitsHash(const void* data, std::size_t bytes) noexcept;
[[nodiscard]] Status FillAnimationLayerAlphas(const AnimatedModelScene& scene, std::uint32_t model,
    War3RenderCore::Range32 range, std::span<float> output, std::span<const float> evaluated = {});
// evaluated/uvTransforms 为同一姿势的原 material/layer 序输出；为空时发布静态贴图与单位坐标变换。
[[nodiscard]] Status FillAnimationMaterialStates(const AnimatedModelScene& scene, std::uint32_t model,
    War3RenderCore::Range32 range, std::span<War3RenderMaterialStateV2> output,
    std::span<const std::uint32_t> evaluated = {}, std::span<const War3RenderCore::UvTransform> uvTransforms = {});
[[nodiscard]] bool AnimationHasVisibleLayers(const AnimatedModelScene& scene, const AnimationFrame& frame,
    const AnimationPoseLink& pose, std::uint32_t model) noexcept;
[[nodiscard]] Status PrepareAnimationPickBounds(const AnimatedModelScene& scene, AnimationFrame& frame);
} // namespace War3Map::Render
