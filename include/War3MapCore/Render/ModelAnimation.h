#pragma once
#include "War3MapCore/Render/AnimationPreviewBudget.h"

#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/RenderPluginModelColor.h"
#include "War3MapCore/RenderPluginMaterialAlpha.h"
#include "War3MapCore/RenderPluginMaterialState.h"
#include "War3RenderCore/PoseTypes.h"
#include "War3RenderCore/MaterialChannels.h"

#include <memory>
#include <optional>

namespace War3Map::Render {

// 本文件均为进程内 C++ 服务对象，不能直接按字节发送到 IPC/插件。所有结果仅 PreviewApproximate。
// 资源适配器必须在同一次真实读取中确定来源，不能用虚拟路径猜 MPQ 层。
// digest 为读取字节的 SHA-256；游戏/地图适配器计算，合成测试可注入自己的确定来源。
struct AnimationResource {
    std::vector<std::byte> bytes;
    std::string sourceLayer;
    std::string digest;
};
using AnimationResourceReader = std::function<Result<AnimationResource>(std::string_view, std::uint64_t)>;

struct AnimationInstanceSpec {
    War3RenderCore::InstanceIdentity identity;
    std::string hostKey; // row_key 或预览编号，仅属进程内元数据
    ModelInstanceRequest model;
    std::string action = "Stand"; // 真实 SEQS 全名，或完整首词（数字变体优先）
    std::optional<std::uint32_t> sequenceIndex;
    std::optional<std::uint64_t> elapsedMs; // 未给时选动作中点，不假定起点可求值
    std::uint64_t globalMs = 0;
    double speed = 1.0;
    bool playing = false;
};
struct AnimationSession {
    std::uint64_t generation = 0; // 地图/来源切换必须新值，禁止固定为 0
    std::uint64_t documentRevision = 0;
    std::string sourceIdentity;   // 地图 SHA/来源集合身份；不能只给一个路径
};
struct AnimationParticleLimits {
    std::uint32_t maxParticles = 65536, maxRibbonSegments = 32768, maxPerEmitter = 4096;
    std::uint32_t maxAnchorSamples = 6000, maxAnchorNodeVisits = 4194304;
    std::uint32_t maxVertices = 1048576, maxDraws = 65536;
    // 世界预览每个获准实例固定预留的10ms步数；慢帧先老化已有状态，不补历史发射。
    std::uint32_t maxWorldFrameSteps = 8;
};
// 时间求值策略与视锥、坏部件保留策略独立。严格回放默认不变；
// 交互预览直接进入当前时刻，冻结恢复不补历史发射，不能作为原版回放证明。
enum class AnimationPlaybackPolicy { StrictReplay, InteractivePreview };
struct AnimationLimits {
    WorldModelLimits geometry;
    std::uint32_t maxInstances = 0; // 兼容旧调用字段，不作为地图总实例数门禁；容量按实际帧内存核算。
    std::uint64_t maxAssetBytes = 256ull * 1024 * 1024;
    std::uint64_t maxFrameBytes = 64ull * 1024 * 1024;
    std::uint64_t maxActivePoseBytes = 16ull * 1024 * 1024; // 共享 Stand 模式中的动态姿势池，不是实例总数。
    std::uint64_t maxVisibilityBytes = 64ull * 1024 * 1024; // 独立持久空间索引/构建临时内存；不限制地图实例数。
    // 世界浏览：可表达的整模型保留静态，坏资产跳过，局部采样失败仅隐藏该实例。
    // 来源/身份/预算和宿主输入错误仍整体失败；不做按 geoset 降级。
    // 世界浏览可启用；独立预览/命令默认关闭，不能将静态画面冒充动作验收。
    bool retainUnsupportedStatic = false;
    bool enableParticles = false; // 宿主显式选择；旧插件/旧调用不暗中改变模型预览语义。
    bool allowRigidStaticPreview = false; // 明确申请无控制器/空矩阵组预览近似；不提供保存兼容资格。
    AnimationParticleLimits particles;
    AnimationPlaybackPolicy playbackPolicy = AnimationPlaybackPolicy::StrictReplay;
    // 仅显式交互预览采用普通节点原序近似；默认/严格回放仍拒绝逆序，不提供保存资格。
    bool allowSourceOrderNodePreview = false;
    // 仅交互预览接受位相同的 KMTA 重复帧；严格回放/保存门禁仍拒绝。
    bool allowIdenticalMaterialAlphaKeysPreview = false;
    // 基础 PREM 子模型层，独立有界预览；默认旧服务不读取子资源。
    bool enablePremPreview = false;
};
// 所有编辑器交互入口共用已核实的 1.27a 按动作原序取键政策；不能用于
// 严格回放、保存或兼容性认证。几何、粒子、内存预算及坏模型保留仍由宿主显式选择。
[[nodiscard]] inline AnimationLimits InteractiveAnimationPreviewLimits() {
    AnimationLimits limits;
    limits.playbackPolicy = AnimationPlaybackPolicy::InteractivePreview;
    limits.allowSourceOrderNodePreview = true;
    limits.allowIdenticalMaterialAlphaKeysPreview = true;
    limits.allowRigidStaticPreview = true;
    return limits;
}
struct AnimationInstanceState {
    War3RenderCore::InstanceIdentity identity;
    std::uint32_t sequenceIndex = 0;
    std::uint64_t elapsedMs = 0, globalMs = 0;
    double speed = 1.0, fractionalMs = 0.0;
    bool playing = false;
    std::array<float, 12> transform{};
    std::array<float, 3> tint{1, 1, 1};
};
struct AnimationEdit {
    War3RenderCore::InstanceIdentity identity;
    std::optional<std::uint32_t> sequenceIndex;
    std::optional<std::uint64_t> elapsedMs, globalMs;
    std::optional<double> speed;
    std::optional<bool> playing;
    std::optional<std::array<float, 12>> transform;
    std::optional<std::array<float, 3>> tint;
};
// 与实际 D3D11 透视相机和物理绘制尺寸绑定；不以逻辑控件尺寸或距离近似视锥。
struct AnimationView {
    War3RenderCameraV2 camera{};
    std::uint32_t physicalWidth = 0, physicalHeight = 0;
    float conservativeMargin = 64.0f; // 世界单位；扩张六个视锥平面，边缘实例保守放行。
};
struct AnimationAdvance {
    std::uint64_t deltaMs = 0; // 显式时间；单次至多 1000 ms，落后时间不偷偷补成巨步
    bool stepPaused = false;
    std::optional<bool> playAll;
    // 用户点“播放”：已停在末帧的不循环动作从头开始。相机刷新等内部播放命令不设置，避免反复重播。
    bool restartFinished = false;
    std::vector<AnimationEdit> edits;
    // 世界空间相机，由服务按每实例变换求模型局部视点/上向量。
    std::optional<War3RenderCore::LocalCamera> worldCamera;
    std::optional<AnimationView> worldView; // 未提供时维持无视锥过滤的旧调用语义。
    std::optional<AnimationParticleFrameBudget> previewParticleBudget; // 仅InteractivePreview；每帧owned软预算。
};
// 每条实际姿势直接拥有有界警告；全局256条摘要不承担实例归属。
// 缓存Stand可显式重绑目标owner，但始终保存原缓存姿势的实际动作/时钟。
struct AnimationPoseWarnings {
    static constexpr std::size_t Capacity = 8;
    std::array<War3RenderCore::PoseDiagnostic, Capacity> records{};
    // 与 records 平行；只有实际 KMTA 兼容警告才有有效 materialIndex。
    std::array<War3RenderCore::MaterialAlphaPreviewWarning, Capacity> materialRecords{};
    std::uint32_t count = 0;
    std::uint64_t omitted = 0;
    std::uint32_t requestIndex = UINT32_MAX, sequenceIndex = UINT32_MAX;
    War3RenderCore::InstanceIdentity identity;
    War3RenderCore::AssetIdentity asset;
    std::uint64_t evaluationEpoch = 0, poseVersion = 0;
    std::uint64_t elapsedMs = 0, globalMs = 0;
    bool cachedStand = false;
};
struct AnimationPoseLink {
    War3RenderCore::InstanceIdentity identity;
    War3RenderCore::AssetIdentity asset;
    std::uint64_t evaluationEpoch = 0, poseVersion = 0;
    std::uint32_t requestIndex = 0, renderedInstance = 0;
    std::uint32_t firstGroup = 0, groupCount = 0;
    std::uint32_t firstAlpha = 0, alphaCount = 0;
    War3RenderCore::Range32 materialAlphas; // 本帧 owned 层池；顺序是当前模型实际 draw 层。
    War3RenderCore::Range32 materialStates; // 与绝对层 alpha 同一实际 draw 层布局，属于当前帧。
    bool animated = false; // 只有本帧成功求值的实例为 true
    bool hidden = false; // 采样失败：整实例透明，poseVersion=0，不伪造静态或动画成功
    War3RenderCore::PoseDiagnostic rejection; // hidden 时绑定本实例的原始拒绝
    std::string rejectionCode, rejectionMessage; // 粒子导致整实例隐藏时保留原始组件诊断。
    std::uint64_t matrixHash = 0, alphaHash = 0, colorHash = 0, materialAlphaHash = 0, materialStateHash = 0; // FNV-1a 数值指纹，不是资源 SHA
    bool partial = false, noVisibleGeometry = false; // 局部 GEOA 隐藏不代表骨架失败。
    War3RenderCore::Range32 geosetIssues; // 本帧 owned 列表；索引是原 MDX geoset 序。
    War3RenderCore::Range32 materialIssues; // 精确 material/layer/原字节位置，不假装节点诊断。
    War3RenderCore::Range32 collisionMatrices; // 与本模型原 collisionShapes 序平行，数据属于本帧。
    bool staticForBudget = false; // 实际 Stand 求值保留；不是 bind pose、隐藏或动画成功。
    bool outsideView = false; // 本帧未求值；仍保留身份/时钟，不能计为预算省略或采样失败。
    std::uint32_t staticSequenceIndex = UINT32_MAX; // 实际缓存 SEQS；缺Stand时显式记录真实首动作。
    std::uint32_t standPoseIndex = UINT32_MAX; // (几何模型,Stand序列)缓存身份，不与模型序号混用。
    std::uint64_t staticElapsedMs = 0, staticGlobalMs = 0;
    AnimationPoseWarnings warnings; // sizeof(link)已纳入完整帧/Stand/活动槽预算。
};
struct AnimationGeosetIssue {
    std::uint32_t geosetIndex = War3RenderCore::NoIndex;
    War3RenderCore::PoseDiagnostic diagnostic;
};
struct AnimationMaterialIssue {
    War3RenderCore::MaterialLayerIssue source;
};
struct AnimationDrawLayerBinding {
    std::uint32_t materialLayer = War3RenderCore::NoIndex; // CompiledModel 的唯一 material/layer 序。
    float previewAlphaScale = 1.0f; // 仅幽灵预览系数；绝不再次乘静态层 alpha 或 GEOA。
    std::uint32_t staticSamplerFlags = 3; // 原创/烘焙沿旧固定WRAP；MDX显式模式来自真实TEXS flags。
    bool operator==(const AnimationDrawLayerBinding&) const noexcept = default;
};
struct AnimationPickGroupBounds {
    std::uint32_t group = 0; // mesh 内组号，与真实 shader 索引一致。
    std::array<float, 6> box{};
};
struct AnimationPickModel {
    std::shared_ptr<const MdxModel> source; // 复用同一解析资产；不复制完整原网格。
    std::shared_ptr<const std::vector<std::uint32_t>> collisionNodes; // 原 shape 序→已编译拓扑序。
    std::shared_ptr<const OwnedStaticModelAsset> ownedStaticAsset; // 显式sealed原创网格；非MDX source替身。
};
struct AnimationCollisionMatrix {
    War3RenderGroupMatrixV2 matrix{}; // 单节点模型局部列式3×4，绝非平均后的矩阵组。
    bool available = false;
};
struct AnimationPickBounds {
    std::array<double, 6> box{}; // 当前可显示/可选择 mesh 与已证碰撞的保守世界盒，仅粗筛。
    bool available = false, collisionIncomplete = false;
};
struct AnimationParticleIssue {
    std::string kind; // PRE2 / RIBB；来源索引在同类块之间连续，包含被跳过的记录。
    std::uint32_t sourceIndex = 0;
    std::optional<std::uint32_t> nodeId;
    std::string code, message;
};
struct AnimationParticleCapability {
    std::uint32_t particle2Count = 0, ribbonCount = 0, premCount = 0;
    bool available = false;
    std::string rejectionCode;
    // 同一资产的所有实例共享不可变来源诊断，不按实例复制整份列表。
    std::shared_ptr<const std::vector<AnimationParticleIssue>> skippedEmitters;
    std::uint32_t premPreparedCount = 0; // 独立子模型层实际准入；available仍只表示PRE2/RIBB。
};
struct AnimationParticleLink {
    War3RenderCore::InstanceIdentity identity;
    War3RenderCore::AssetIdentity asset;
    std::uint32_t requestIndex = 0;
    bool simulated = false, hidden = false, budgetLimited = false;
    bool outsideView = false, frozen = false;
    bool partial = false;
    std::uint32_t unsupportedPremCount = 0;
    std::uint64_t simulationTimeMs = 0, rejectedElapsedMs = 0, rejectedGlobalMs = 0;
    std::uint64_t skippedTimelineMs = 0; // 世界慢帧/冻结快进跳过的历史发射时间；预览近似。
    std::uint32_t particleCount = 0, ribbonSegmentCount = 0;
    std::uint32_t firstVertex = 0, vertexCount = 0, firstDraw = 0, drawCount = 0;
    std::uint64_t spawned = 0, suppressed = 0, stateHash = 0, geometryHash = 0;
    std::string rejectionCode, rejectionMessage, resetReason;
};
struct AnimationParticleCheckpoint; // 服务拥有的不可变模拟状态，不暴露内部模拟器类型。
// 单次 Prepare 的 CPU 测量；不参与数值指纹或姿势身份，不能据此宣称 GPU/可见帧完成。
// ns 使用 steady_clock；phase 可独立汇总，particlesTotal 包含其 simulation/geometry 子项。
struct AnimationFrameTimings {
    std::uint64_t prepareNs = 0, stateAdvanceNs = 0, poseEvaluationNs = 0, posePackingNs = 0;
    std::uint64_t particlesTotalNs = 0, particleSimulationNs = 0, particleGeometryNs = 0;
    std::uint64_t pickBoundsNs = 0, fingerprintNs = 0;
    std::uint64_t visibilityNs = 0, visibilityNodesVisited = 0, visibilityBoundsTests = 0;
    std::uint64_t candidateInstances = 0, eligiblePoseInstances = 0, poseEvaluationRequests = 0;
    std::uint64_t particleSimulationRequests = 0, particleAnchorRequests = 0, particleAnchorNodeVisits = 0;
    std::uint64_t particleSkippedTimelineMs = 0;
    // 仅实际成功的输出数值：不含版本号、时钟、隐藏占位或 Stand 重用的计数变化。
    std::uint64_t outputMotionHash = 0, particleStateHash = 0;
    std::uint64_t minElapsedMs = 0, maxElapsedMs = 0, minGlobalMs = 0, maxGlobalMs = 0;
};
struct AnimationPremFrame;
struct AnimationFrame {
    std::uint64_t sceneVersion = 0, contentVersion = 0, documentRevision = 0;
    std::optional<War3RenderCore::LocalCamera> worldCamera;
    std::optional<AnimationView> worldView;
    std::vector<std::uint8_t> requestVisibility; // 原始 specs 序；空=旧调用未过滤，0=视野外，1=保守放行。
    std::vector<AnimationInstanceState> states; // 原始请求序；暂停/取消后从已接受帧恢复
    std::vector<War3RenderSkinnedModelInstanceV2> instances; // 按 model 排序
    std::vector<War3RenderGroupMatrixV2> groups;
    std::vector<float> alphas; // 按实际 mesh，不是 geoset 序
    std::vector<War3RenderMeshColorV2> colors; // 与alphas平行，属于当前contentVersion
    std::vector<float> layerAlphas; // 绝对层 alpha，复制求值结果；同一帧拥有，不借 scratch。
    std::vector<War3RenderMaterialLayerSliceV2> layerSlices; // 与 instances 平行，可共享真实 Stand 片段。
    std::vector<War3RenderMaterialStateV2> materialStates;
    std::vector<War3RenderMaterialLayerSliceV2> materialStateSlices;
    std::vector<AnimationPoseLink> poses;
    std::vector<AnimationGeosetIssue> geosetIssues;
    std::vector<AnimationMaterialIssue> materialIssues;
    std::vector<AnimationCollisionMatrix> collisionMatrices;
    std::vector<AnimationPickBounds> pickBounds; // 与 poses 平行，同一 contentVersion。
    std::vector<War3RenderParticleVertexV2> particleVertices;
    std::vector<War3RenderParticleDrawV2> particleDraws;
    std::vector<AnimationParticleLink> particleLinks; // 原始请求身份；包括启用后被局部拒绝的请求。
    std::shared_ptr<const AnimationParticleCheckpoint> particleCheckpoint;
    std::vector<War3RenderCore::PoseDiagnostic> diagnostics;
    std::uint64_t omittedDiagnostics = 0;
    std::size_t animatedInstances = 0, retainedStaticInstances = 0, hiddenInstances = 0;
    std::size_t budgetStaticInstances = 0;
    std::size_t budgetParticleInstances = 0; // 对应模拟省略，不代表模型隐藏或资源有效。
    std::size_t budgetParticleEmitters = 0; // 实际支持但本帧整实例省略的PRE2/RIBB，不含PREM/无效记录/视野外。
    std::optional<AnimationParticleFrameBudget> previewParticleBudget;
    std::size_t offscreenInstances = 0, offscreenPoseInstances = 0, offscreenParticleInstances = 0;
    std::size_t visibilityUnknownInstances = 0; // 无法证明保守包络的请求显式放行。
    AnimationFrameTimings timings;
    bool sharedPoseRanges = false; // 显式插件 mode2；旧连续 mode1 不暗中放宽。
    bool dynamicMaterialAlphas = false, dynamicColors = false; // 材质描述符的显式模式；空 roster 也保留。
    bool dynamicMaterialStates = false;
    std::shared_ptr<const AnimationPremFrame> prem;
    [[nodiscard]] War3RenderSkinUpdateV2 Describe() const noexcept;
    [[nodiscard]] War3RenderDynamicColoredSkinUpdateV2 DescribeColored() const noexcept;
    [[nodiscard]] War3RenderMaterialSkinUpdateV2 DescribeMaterial() const noexcept;
    [[nodiscard]] War3RenderMaterialStateSkinUpdateV2 DescribeMaterialState() const noexcept;
    [[nodiscard]] War3RenderParticleUpdateV2 DescribeParticles(const War3RenderSkinUpdateV2* modelPose) const noexcept;
};
struct AnimationUnavailableInstance {
    War3RenderCore::InstanceIdentity identity;
    War3RenderCore::AssetIdentity asset;
    std::uint32_t requestIndex = 0;
    std::string code; // 资产加载/颜色/几何拒绝；路径及宿主键由原始 specs 索引确定
};
struct AnimatedModelScene {
    std::shared_ptr<const AnimatedModelScene> premGeometry;
    WorldModelScene geometry;
    SharedSceneVector<War3RenderSkinnedModelVertexV2> vertices;
    SharedSceneVector<War3RenderSkinnedModelMeshV2> meshes;
    // 与全局网格平行的不可变 GEOA RGB；空数组仅代表旧场景全白。姿势更新不改颜色。
    SharedSceneVector<War3RenderMeshColorV2> meshColors;
    SharedSceneVector<AnimationDrawLayerBinding> drawLayerBindings; // 与 geometry.layers 平行，创建时映射一次。
    SharedSceneVector<War3RenderCore::Range32> modelLayerRanges; // 模型实际 draw 层的全局静态范围。
    SharedSceneVector<War3RenderSkinnedModelV2> models;
    SharedSceneVector<AnimationPickModel> pickModels; // 与 models 平行；烘焙没有来源模型。
    SharedSceneVector<War3RenderCore::Range32> pickMeshGroups; // 与 meshes 平行。
    SharedSceneVector<AnimationPickGroupBounds> pickGroupBounds; // 不随姿势重建顶点。
    SharedSceneVector<AnimationInstanceSpec> specs;
    // 与 specs 平行，由实际资产 drawable 冻结；0 只表示资产本身无需实体网格，
    // 不能由 geometry 中缺少实例反推。Create/目录增量都从资产重新构建。
    SharedSceneVector<std::uint8_t> requestGeometryRequired;
    SharedSceneVector<std::vector<MdxSequence>> sequences; // 与 specs 平行；仅增删实例或编辑定义时分离。
    std::vector<War3RenderCore::AssetSource> sources;
    std::vector<AnimationUnavailableInstance> unavailableInstances;
    SharedSceneVector<ModelTextureRgba> particleTextures;
    std::vector<AnimationParticleCapability> particleCapabilities; // 与 specs 平行，禁用时仍公开声明计数。
    std::uint64_t sceneVersion = 0, generation = 0, documentRevision = 0;
    std::string sourceIdentity;
    std::size_t animatedInstances = 0, retainedStaticInstances = 0; // 创建时能力；成功帧数量见 AnimationFrame
    bool dynamicColors = false; // 任何活动KGAC均要求动态RGB能力，即使首帧恰好是白色
    bool dynamicMaterialAlphas = false; // 真实 KMTA 要求显式能力，即使当前 alpha 正好等于静态值。
    bool dynamicMaterialStates = false; // KMTF 数值必须经过同帧显式材质渠道，不能伪造静态成功。
    bool particlesEnabled = false;
    std::size_t particleInstanceCount = 0; // 创建时可运行粒子的请求数；与某帧是否有顶点无关。
    std::size_t budgetStaticInstances = 0; // Create 的容量估计，实际调度以 Frame 为准。
    SharedSceneVector<AnimationPickBounds> standPickBounds; // 与Stand姿势缓存平行；预算外实例按standPoseIndex取局部盒。
    bool premEnabled = false; // PREM 子层独立开关；不能用 PRE2/RIBB 开关推断。
};
struct AnimatedSceneDescriptor {
    ModelSceneDescriptor textures;
    War3RenderSkinUpdateV2 update{};
    War3RenderDynamicColoredSkinUpdateV2 coloredUpdate{};
    War3RenderMaterialSkinUpdateV2 materialUpdate{};
    War3RenderMaterialStateSkinUpdateV2 materialStateUpdate{};
    War3RenderSkinnedModelSceneV2 scene{};
    // 返回值移动可能改变 update 地址；调用 ABI 前必须 Refresh。
    void Refresh(const AnimatedModelScene& geometry, const AnimationFrame& frame);
};

class ModelAnimationService;
class AnimationAssetSnapshot; // 不可变的解析/解码来源缓存，不含compiled身份、实例、帧或材质。
class AnimationPreparedAssets; // 正式catalog绑定的只读静态资产；不含实例身份、时钟或可变求值状态。
// 仅记录单次CPU准备调用的真实区间，不代表GPU完成、来源验证或UI排队时间。
// storage的四个子项属于storageNs；其余阶段互不叠加。未执行的阶段保持0。
struct AnimationInstanceDeltaTimings {
    std::uint64_t cloneNs = 0, identityNs = 0, assetNs = 0, orderingNs = 0;
    // assetNs的子项：真实预取来源复核，以及编译身份/预算校验和静态快照应用。
    std::uint64_t preparedSourceVerifyNs = 0, preparedGeometryApplyNs = 0;
    std::uint64_t storageNs = 0, storageLayoutNs = 0, storageEvaluatorNs = 0;
    std::uint64_t storageStandNs = 0, storageParticlesNs = 0;
    std::uint64_t stateRemapNs = 0, particleRemapNs = 0, firstFrameNs = 0, renderedRemapNs = 0;
    std::uint64_t totalNs = 0;
    bool preparedAssetHit = false;
    std::string preparedAssetMissReason;
};
struct AnimationInstanceDelta {
    AnimationSession session; // 同一来源代次的新地图修订。
    std::vector<AnimationInstanceSpec> additions;
    std::vector<War3RenderCore::InstanceIdentity> removals;
    std::vector<AnimationEdit> edits;
    struct RowRemap { War3RenderCore::InstanceIdentity identity; std::string hostKey; };
    std::vector<RowRemap> rows; // 行重排不改变存活实例身份。
};
class AnimationInstanceDeltaPlan {
public:
    [[nodiscard]] bool AssetsChanged() const noexcept { return assetsChanged_; }
    [[nodiscard]] std::uint64_t BaseSceneVersion() const noexcept { return baseSceneVersion_; }
private:
    friend class ModelAnimationService;
    std::shared_ptr<ModelAnimationService> prepared_;
    AnimationInstanceDelta delta_;
    std::uint64_t baseSceneVersion_ = 0, baseDocumentRevision_ = 0;
    std::vector<std::uint32_t> oldToNewRequests_;
    bool assetsChanged_ = false;
};
struct AnimationInstanceDeltaResult {
    std::shared_ptr<ModelAnimationService> service;
    std::shared_ptr<const AnimationFrame> frame;
    std::vector<std::uint32_t> oldToNewRequests, oldToNewRendered, oldToNewParticleBindings;
    bool assetsChanged = false;
};

// 单所有者、Qt-free。只有 Create 读取资产/建立材质；Prepare 只推进状态、求值、复制帧包。
// previous 必须为本 service 产物。worker 取消时丢弃结果；下一次仍以最后被接受的 previous 开始。
// 这样暂停/切图不会让未显示的半帧偷偷推进时钟。输出拥有数据，不借求值器 span 跨帧。
class ModelAnimationService {
public:
    ~ModelAnimationService();
    [[nodiscard]] static Result<std::shared_ptr<ModelAnimationService>> Create(
        AnimationSession session, std::span<const AnimationInstanceSpec> instances,
        const AnimationResourceReader& read, const AnimationLimits& limits = {},
        std::span<const BakedModelMesh> baked = {});
    [[nodiscard]] std::shared_ptr<const AnimatedModelScene> Scene() const noexcept;
    [[nodiscard]] AnimationPlaybackPolicy PlaybackPolicy() const noexcept;
    [[nodiscard]] Result<std::shared_ptr<const AnimationFrame>> Prepare(
        const AnimationFrame* previous, const AnimationAdvance& advance = {});
    // 两阶段：第一步加载新增资产；第二步在单worker边界按当时最新接受帧重基。
    // 均返回独立候选，不修改当前服务；旧Prepare身份/数量门保持严格。
    [[nodiscard]] Result<std::shared_ptr<const AnimationInstanceDeltaPlan>> PrepareInstanceCatalog(
        AnimationInstanceDelta delta, const AnimationResourceReader& read,
        std::shared_ptr<const AnimationAssetSnapshot> preloaded = {}, AnimationInstanceDeltaTimings* timings = nullptr,
        std::shared_ptr<const AnimationPreparedAssets> prepared = {}) const;
    // 提前准备一个正式模型变体。只对捕获的只读catalog调用；消费时仍核验真实来源与预算。
    // 包只适用于相同基础catalog，不能从ghost获取或跨catalog重编号compiled身份。
    [[nodiscard]] Result<std::shared_ptr<const AnimationPreparedAssets>> PrepareStaticAssets(
        const ModelInstanceRequest& request, const AnimationResourceReader& read,
        std::shared_ptr<const AnimationAssetSnapshot> preloaded = {}) const;
    // 在初始化/求值worker边界捕获。使用方仍读取正式来源并按path/layer/SHA/策略匹配；
    // 命中只免重复解析/解码，不复用预览几何、透明材质、时钟或来源读取资格。
    [[nodiscard]] Result<std::shared_ptr<const AnimationAssetSnapshot>> CaptureAssetSnapshot() const;
    // 仅在当前求值worker边界捕获。目录可共享已冻结scene，不含当前可变求值scratch；
    // 后台加载新增资产先建立独占目录，原世界继续按独立worker推进。
    [[nodiscard]] Result<std::shared_ptr<const ModelAnimationService>> CaptureInstanceCatalog() const;
    [[nodiscard]] Result<AnimationInstanceDeltaResult> PrepareInstanceDelta(
        const AnimationFrame& latest, const AnimationInstanceDeltaPlan& plan,
        const AnimationAdvance& advance = {}, AnimationInstanceDeltaTimings* timings = nullptr) const;
private:
    struct State;
    friend Status CreateAnimationPrem(State& state, const AnimationResourceReader& read);
    friend Status PrepareAnimationPrem(State& state, AnimationFrame& frame,
        const AnimationFrame* previous, const AnimationAdvance& advance);
    [[nodiscard]] static Status PrepareStandStorage(State& state, std::uint64_t frameBudget, std::uint64_t activeBudget);
    [[nodiscard]] static Status InitializeStorage(State& state, const AnimationLimits& limits,
        const AnimationResourceReader& read, AnimationInstanceDeltaTimings* timings = nullptr);
    [[nodiscard]] static Result<std::shared_ptr<ModelAnimationService>> CreateWithResident(
        AnimationSession session, std::span<const AnimationInstanceSpec> instances,
        const AnimationResourceReader& read, const AnimationLimits& limits,
        std::span<const BakedModelMesh> baked, const State* resident,
        std::shared_ptr<const AnimationAssetSnapshot> preloaded = {});
    [[nodiscard]] static Result<std::unique_ptr<State>> BuildStaticAssets(
        AnimationSession session, std::span<const AnimationInstanceSpec> instances,
        const AnimationResourceReader& read, const AnimationLimits& limits,
        std::span<const BakedModelMesh> baked, const State* resident,
        std::shared_ptr<const AnimationAssetSnapshot> preloaded = {});
    explicit ModelAnimationService(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

struct AnimationDiagnosticBinding {
    std::uint64_t sceneVersion = 0, generation = 0, documentRevision = 0, contentVersion = 0;
    std::uint64_t evaluationEpoch = 0, poseVersion = 0;
    std::uint32_t requestIndex = 0;
    War3RenderCore::InstanceIdentity identity;
    bool operator==(const AnimationDiagnosticBinding&) const noexcept = default;
};
struct AnimationPoseWarningView {
    std::span<const War3RenderCore::PoseDiagnostic> records; // 仅在所属frame存活期间有效。
    std::uint64_t omitted = 0;
    std::uint32_t sequenceIndex = UINT32_MAX;
    std::uint64_t elapsedMs = 0, globalMs = 0;
    bool cachedStand = false;
    std::span<const War3RenderCore::MaterialAlphaPreviewWarning> materialRecords;
};
// Query和UI聚合共用同一owner/来源/版本/采样时钟验证；不按模型或nodeId猜归属。
// 无记录返回空view；不把成功warning升级为hidden/partial或地图缺陷。
[[nodiscard]] Result<AnimationPoseWarningView> ReadAnimationPoseWarnings(
    const AnimatedModelScene& scene, const AnimationFrame& frame, const AnimationPoseLink& pose);
struct AnimationDiagnosticIssue {
    std::string stage, kind, code, message;
    std::optional<std::uint32_t> sourceIndex, nodeId;
    std::optional<std::uint32_t> geosetIndex;
    std::optional<std::uint32_t> materialIndex, layerIndex;
    std::optional<std::uint64_t> sourceByteOffset;
};
struct AnimationDiagnosticPage {
    AnimationDiagnosticBinding binding;
    std::string modelPath, hostKey;
    std::optional<War3RenderCore::AssetSource> source;
    War3RenderCore::AssetIdentity asset;
    bool hasFrame = false, animated = false, hidden = false, unavailable = false, partial = false;
    bool particlesAvailable = false, particlesSimulated = false;
    std::uint32_t premDeclaredCount = 0, premPreparedCount = 0;
    std::uint64_t elapsedMs = 0, globalMs = 0;
    std::uint32_t offset = 0, total = 0;
    bool truncated = false;
    std::vector<AnimationDiagnosticIssue> issues;
    bool noVisibleGeometry = false;
    bool staticForBudget = false;
    std::optional<std::uint32_t> displayedSequenceIndex;
    std::optional<std::uint64_t> displayedElapsedMs, displayedGlobalMs; // 不把请求时钟当缓存姿势时间。
};
// 单所有者，只查询宿主已接受的服务帧，不是另一个帧接受器或 renderer 完成回执。
// Create 一次索引不可变场景；SetFrame 只替换引用。Query 不扫描整场景，不逐帧拼接文字。
// hidden 表示服务帧局部拒绝，不代表编辑器眼睛开关或插件 presentation mask。
class AnimationDiagnosticQuery {
public:
    [[nodiscard]] static Result<std::shared_ptr<AnimationDiagnosticQuery>> Create(
        std::shared_ptr<const AnimatedModelScene> scene);
    [[nodiscard]] Status SetFrame(std::shared_ptr<const AnimationFrame> acceptedFrame);
    [[nodiscard]] Result<AnimationDiagnosticBinding> Binding(std::uint32_t requestIndex,
        War3RenderCore::InstanceIdentity expectedIdentity) const;
    [[nodiscard]] Result<std::optional<War3RenderCore::AssetSource>> Source(std::uint32_t requestIndex,
        War3RenderCore::InstanceIdentity expectedIdentity) const;
    [[nodiscard]] Result<AnimationDiagnosticPage> Query(const AnimationDiagnosticBinding& expected,
        std::uint32_t offset = 0, std::uint32_t limit = 16) const; // limit: 1..64
private:
    struct Index { std::uint32_t pose = UINT32_MAX, source = UINT32_MAX, unavailable = UINT32_MAX; };
    std::shared_ptr<const AnimatedModelScene> scene_;
    std::shared_ptr<const AnimationFrame> frame_;
    std::vector<Index> indices_;
};

[[nodiscard]] std::optional<std::uint32_t> FindAnimationSequence(
    std::span<const MdxSequence> sequences, std::string_view action, std::string_view requiredProperties = {});
[[nodiscard]] War3RenderCameraV2 FitAnimationSceneCamera(const AnimatedModelScene& scene);
[[nodiscard]] War3RenderCore::LocalCamera AnimationWorldCamera(const War3RenderCameraV2& camera);
// 仅复制点选所需包围盒/实例，绝不复制大顶点/贴图池。点选仍是预览 AABB，不是骨骼精确命中。
[[nodiscard]] std::shared_ptr<WorldModelScene> AnimationPickingScene(const AnimatedModelScene& scene,
    const AnimationFrame& frame);
// 复用已复制的不可变模型目录；每帧仅更新实际可点选实例和身份映射。
void AnimationPickingInstances(const AnimatedModelScene& scene, const AnimationFrame& frame,
    WorldModelScene& output);
[[nodiscard]] std::string AnimationPathKey(std::string_view path);
// 宿主生命周期流水号，无系统时钟；独立 renderer 可以共用，数值允许有间隙。
[[nodiscard]] std::uint64_t NextAnimationSerial();

} // namespace War3Map::Render
