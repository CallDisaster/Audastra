#pragma once

#include "War3MapCore/RenderPluginV2.h"
#include "War3MapCore/Render/MdxPose.h"
#include "War3MapCore/Render/SharedSceneVector.h"
#include "War3MapCore/Types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace War3RenderCore { class CompiledMaterialTextures; }
#include <vector>

namespace War3Map::Render {

class OwnedStaticModelAsset;

// 一个摆放实例需要的模型：路径已由调用方按对象数据（模型字段、变体数）确定。
struct ModelSelectionDescription {
    float circleScale = 0, circleHeight = 0; // 物编ussc/uslz；不由模型AABB推算。
    bool circleScaleKnown = false, circleOnWater = false;
    bool scalesProjectiles = false; // uscb/scaleBull属于投射物，不能改变选择圈比例。
    float rawPlacementZ = 0, flyHeightOffset = 0;
    bool heightPreviewApproximate = false; // fly仅rawZ+moveHeight，不修改存盘坐标。
};
struct ModelInstanceRequest {
    std::string modelPath;          // 含 .mdx 扩展名的虚拟路径
    std::string replaceableTexture; // 替换贴图 ID ≥ 3（树木等）使用的贴图路径；空则该层按白色
    std::uint32_t teamColor = 0;    // 队伍色/光晕编号 0..15
    std::array<float, 3> position{};
    float angle = 0;                // 绕 Z 轴，弧度
    std::array<float, 3> scale{1, 1, 1};
    std::array<float, 3> tint{1, 1, 1};
    bool previewGhost = false;      // 编辑器临时实例：独立材质变体，使用半透明混合；不写地图。
    std::string animationProperties; // 继承物编uani/animProps；按完整词选择动作，不改变静态资产身份。
    std::optional<ModelSelectionDescription> selection; // 宿主显示描述，不影响资源身份或精确拾取。
    // 编辑器原创辅助几何，独立typed资产身份；modelPath仍是原实际资源描述，不送入伪MDX读取器。
    std::shared_ptr<const OwnedStaticModelAsset> ownedStaticAsset;
};

// 已在世界坐标烘焙好的几何（悬崖等逐顶点贴合地形、无法用实例变换表达的网格）。
// 每块顶点数 ≤ 65536，全部并入一个恒等变换的模型；贴图按路径与普通模型共用缓存。
struct BakedModelMesh {
    std::vector<War3RenderModelVertexV2> vertices;
    std::vector<std::uint16_t> indices;
    std::string texturePath;
};

struct WorldModelLimits {
    std::size_t maxModelBytes = 16 * 1024 * 1024;
    std::size_t maxTextureBytes = 16 * 1024 * 1024; // 单个 BLP 文件
    std::uint32_t maxTextureDimension = 512;         // 更大的贴图按 2 倍逐级缩小
    std::uint64_t maxTotalTextureBytes = 160ull * 1024 * 1024;
    std::uint32_t maxVertices = 2097152, maxIndices = 6291456, maxLayers = 131072, maxMeshes = 65536;
    std::uint32_t maxModels = 16384, maxInstances = 0, maxTextures = 4096; // 0：没有场景总数门禁。
    std::uint64_t maxInstanceBytes = 64ull * 1024 * 1024; // 实例与来源索引的实际显示内存预算。
    bool allowRigidStaticPreview = false; // 明确申请MDX800无控制器静态identity子集；默认严格。
};

struct ModelTextureRgba {
    std::string path;
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
    std::string ownedIdentity; // 非资源路径；仅FreezeOwnedStaticModelAsset设置，旧资源贴图保持空。
};

struct OwnedModelMesh {
    std::vector<War3RenderModelVertexV2> vertices;
    std::vector<std::uint16_t> indices;
    std::uint32_t textureIndex = WAR3_RENDER_MODEL_NO_TEXTURE;
    std::uint32_t filterMode = 0, flags = 1;
    float alpha = 1;
};

// 不可由caller伪造SHA；factory防御性复制全部buffer，不借用外部可写vector。
class OwnedStaticModelAsset {
public:
    [[nodiscard]] std::string_view IdentitySha256() const noexcept { return identity_; }
    [[nodiscard]] std::span<const OwnedModelMesh> Meshes() const noexcept { return meshes_; }
    [[nodiscard]] std::span<const ModelTextureRgba> Textures() const noexcept { return textures_; }
    [[nodiscard]] std::uint64_t OwnedBytes() const noexcept { return bytes_; }
private:
    OwnedStaticModelAsset() = default;
    std::string identity_;
    std::vector<OwnedModelMesh> meshes_;
    std::vector<ModelTextureRgba> textures_;
    std::uint64_t bytes_ = 0;
    friend Result<std::shared_ptr<const OwnedStaticModelAsset>> FreezeOwnedStaticModelAsset(
        std::span<const OwnedModelMesh>, std::span<const ModelTextureRgba>, const WorldModelLimits&);
};
[[nodiscard]] Result<std::shared_ptr<const OwnedStaticModelAsset>> FreezeOwnedStaticModelAsset(
    std::span<const OwnedModelMesh> meshes, std::span<const ModelTextureRgba> textures,
    const WorldModelLimits& limits = {});
// 仅描述来源TGA平面尺寸；未知时为8单位小marker，不表示精确占地。高度/锚点/旋转均预览近似。
[[nodiscard]] Result<std::shared_ptr<const OwnedStaticModelAsset>> CreateCheckerBlockerAsset(
    std::optional<std::array<float, 2>> sourcePlanarFootprint, const WorldModelLimits& limits = {});
// 唯一资产匹配规则；普通资源规范路径保持旧值，owned身份使用非法资源路径的独立typed前缀。
[[nodiscard]] std::string ModelAssetKey(const ModelInstanceRequest& request);
[[nodiscard]] std::string ModelTextureAssetKey(const ModelTextureRgba& texture);

// 显式 CPU 刚性姿态请求：同一场景中所有摆放模型共享这一时刻；不推测默认动作或时钟。
// GEOA/KGAO 阶梯轨迹按显式动作帧确定可见度；其它可见度语义仍明确拒绝。
struct ModelSceneRigidPoseRequest {
    std::size_t sequenceIndex = 0;
    std::uint64_t sequenceElapsedMs = 0;
    std::uint64_t globalElapsedMs = 0;
    // 模型局部空间相机仅用于单实例“通用算法预览”；多实例各有不同局部视点。
    std::optional<MdxPoseCamera> camera;
};

// 可直接描述为 War3RenderModelSceneV2 的 CPU 场景；实例已按模型排序。
struct ModelTextureBinding {
    std::uint32_t textureIndex = WAR3_RENDER_MODEL_NO_TEXTURE, samplerFlags = 0;
    bool reachable = false;
};
struct WorldModelScene {
    SharedSceneVector<War3RenderModelVertexV2> vertices;
    SharedSceneVector<std::uint16_t> indices;
    SharedSceneVector<War3RenderModelLayerV2> layers;
    SharedSceneVector<War3RenderModelMeshV2> meshes;
    SharedSceneVector<War3RenderModelV2> models;
    std::vector<War3RenderModelInstanceV2> instances;
    // 与 instances 平行：来源请求序号；悬崖等烘焙网格为 kBakedInstance（不参与点选）。
    std::vector<std::uint32_t> instanceRequests;
    // 与 models 平行：局部 AABB {minX, minY, minZ, maxX, maxY, maxZ}。
    SharedSceneVector<std::array<float, 6>> modelBounds;
    SharedSceneVector<ModelTextureRgba> textures;
    std::optional<ModelSceneRigidPoseRequest> rigidPose; // 几何对应的显式姿态；空为原静态路径
    std::size_t requestedInstances = 0;
    std::size_t skippedInstances = 0;   // 模型不可用或超出预算而未显示的实例
    std::size_t untexturedLayers = 0;   // 贴图不可用、按白色显示的层
    std::size_t rigidStaticPreviewAssets = 0, rigidStaticPreviewInstances = 0; // 本次构建已接纳资产/显示实例；预览近似。
    std::vector<Diagnostic> warnings;   // 按路径汇总，不逐实例重复
    // 仅新动画几何构建入口填写；旧静态/刚性入口保持空。不是插件 ABI。
    SharedSceneVector<std::uint32_t> vertexGroups; // 与 vertices 平行，网格内局部组号
    SharedSceneVector<std::uint32_t> meshGeosets;  // 与 meshes 平行，原 geoset 序号（含静态保留）；仅烘焙为 UINT32_MAX
    SharedSceneVector<std::string> modelPaths;    // 与 models 平行，规范化模型路径；烘焙为空
    SharedSceneVector<std::string> modelAssetKeys; // 与models平行，typed资产身份，不能传给资源读取器。
    // 每个几何变体独立绑定 TEXS ID→常驻纹理，队伍色/替换贴图不会串；空表示旧静态资产。
    SharedSceneVector<std::shared_ptr<const std::vector<ModelTextureBinding>>> modelTextureBindings;
};

// 持有 ABI 描述符及其指向的 mip 描述；scene 指针在本对象存活期间有效（移动不失效）。
struct ModelSceneDescriptor {
    std::vector<War3RenderMipRgbaV2> mips;
    std::vector<War3RenderTextureMipChainV2> chains;
    War3RenderModelSceneV2 scene{};
};
[[nodiscard]] ModelSceneDescriptor DescribeModelScene(const WorldModelScene& scene);
[[nodiscard]] ModelSceneDescriptor DescribeModelTextures(std::span<const ModelTextureRgba> textures);
[[nodiscard]] std::string ModelTexturePath(const MdxTexture& texture, const ModelInstanceRequest& request);
[[nodiscard]] Result<ModelTextureRgba> DecodeModelTexture(std::string path, std::span<const std::byte> bytes,
    std::uint32_t maxDimension);

using ModelFileReader = std::function<Result<std::vector<std::byte>>(std::string_view virtualPath)>;
// 每个不同的模型路径（及其替换贴图、队伍色组合）只读取、解析一次。读取/解析/预算失败只跳过
// 相关实例并记入 warnings，不中止整个场景；参数本身无效（非有限坐标等）才返回失败。
// 请求 rigidPose 时，已实现的四元数样条与相机节点只按通用算法预览，并在 warnings
// 中保留来源诊断；未知姿态语义或超预算仍失败。模型局部相机只支持单实例。
// 成功场景不混入静态姿态几何；烘焙网格仍保持原位。此接口供未来 UI/Agent 构建独立预览场景。
[[nodiscard]] Result<WorldModelScene> BuildWorldModelScene(std::span<const ModelInstanceRequest> requests,
    const ModelFileReader& read, const WorldModelLimits& limits = {}, std::span<const BakedModelMesh> baked = {},
    std::optional<ModelSceneRigidPoseRequest> rigidPose = std::nullopt);

// 共享加载结果：动画服务拥有解析/编译资产；几何构建只借用，不重复解析。
// animated=true 必须由整模型普通或显式刚性静态编译成功后设置；保留全部有材质的 geoset，
// 不把某帧透明度烘进层/顶点。false 是完整旧静态模型，不是按 geoset 降级。
struct ModelGeometryAsset {
    std::shared_ptr<const MdxModel> model;
    bool animated = false;
    std::shared_ptr<const War3RenderCore::CompiledMaterialTextures> materialTextures;
};
using ModelGeometryProvider = std::function<ModelGeometryAsset(std::string_view)>;
// 可选静态解码缓存。读取器仍先提供本次真实原字节；provider须核对来源/摘要/解码策略。
// 返回空表示正常解码；结果仍计入当前场景的尺寸与累计贴图内存预算。
using ModelDecodedTextureProvider = std::function<std::shared_ptr<const ModelTextureRgba>(
    std::string_view, std::span<const std::byte>, std::uint32_t)>;
[[nodiscard]] Result<WorldModelScene> BuildAnimatedModelGeometry(
    std::span<const ModelInstanceRequest> requests, const ModelFileReader& read,
    const ModelGeometryProvider& assets, const WorldModelLimits& limits = {},
    std::span<const BakedModelMesh> baked = {}, const ModelDecodedTextureProvider& decoded = {});

constexpr std::uint32_t kBakedInstance = UINT32_MAX;

// 行主序 3×4 实例变换（绕 Z 旋转 × 缩放 + 平移），与场景构建一致；供拖动等局部更新复用。
[[nodiscard]] std::array<float, 12> ModelInstanceTransform(const ModelInstanceRequest& request);

// 摆放变化后原位重排实例（移动、删除、增加已载入的模型变体），不重新读取模型与贴图。
// previous 为构建当前场景所用请求；next 中每个请求的模型变体（路径 + 队伍色 + 替换贴图）
// 必须已在 previous 中出现过：已显示则复用其模型，此前不可用则同样跳过。出现新变体、
// 超出实例预算或参数无效时返回 false，场景保持不变，调用方改为重新构建。
[[nodiscard]] bool ReplaceModelInstances(WorldModelScene& scene, std::span<const ModelInstanceRequest> previous,
    std::span<const ModelInstanceRequest> next, const WorldModelLimits& limits = {});

// 地形编辑后替换烘焙网格（悬崖），不重新读取摆放模型。所需贴图必须已在场景中（按路径），
// 烘焙数据必须位于各数组开头（BuildWorldModelScene 的布局）；否则返回 false，场景保持不变。
// 实例的模型序号随烘焙模型的增减整体平移，实例与请求的对应关系不变。
[[nodiscard]] bool ReplaceBakedMeshes(WorldModelScene& scene, std::span<const BakedModelMesh> baked,
    const WorldModelLimits& limits = {});

// 队伍色/光晕贴图路径（原版 ReplaceableTextures 目录）。
[[nodiscard]] std::string TeamColorTexture(std::uint32_t index);
[[nodiscard]] std::string TeamGlowTexture(std::uint32_t index);

} // namespace War3Map::Render
