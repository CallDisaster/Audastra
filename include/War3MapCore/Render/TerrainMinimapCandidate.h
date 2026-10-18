#pragma once

#include "War3MapCore/Render/TerrainInspection.h"
#include "War3MapCore/Render/TerrainTextureAtlas.h"

#include <array>
#include <cstdint>
#include <optional>

namespace War3Map::Render {

// 声明边距仅是源W3I政策，未验证1.27a native crop；新外缘域必须显式选择。
enum class TerrainMinimapRangePolicy { FullW3e, DeclaredW3iMargins, DeclaredW3iMarginsExteriorMapEdge, ClassicPlayable };

// 原版保存用的取色参数；资源捕获层从实际表/配置/贴图填入，不靠调用者的已验证开关。
struct ClassicMinimapTerrainColors {
    std::vector<std::array<std::uint8_t, 4>> cliffColors; // 与W3E cliff palette同序
    std::array<std::uint8_t, 4> waterColor{}; // Water.slk mm*，不是Smin_*
    double waterHeightOffset = 0;
    double deepLevelPathing = 0;
    bool waterConfigured = false;
    std::array<double, 4> cameraMargins{}; // UI配置L/R/B/T，tile单位
    bool cameraMarginsConfigured = false;
};

// 原版保存小地图的五项未决来源只能由独立原版实例/静态证据填入；这些字段不是
// “给一个默认值就通过”的开关。未验证或当前候选未接通时，Build 会返回稳定诊断。
// 当前实现只保留合同，不把水/对象/悬崖/滤波参数冒充成已实现的原版行为。
struct TerrainMinimapGenerationConfig {
    bool nativeReferenceVerified = false;
    // [MinimapGeneration] 已知字段：当前候选尚未接入中值/高斯/噪点执行链。
    std::optional<std::int32_t> medianRadius;
    std::optional<double> blurRadius;
    std::optional<double> noisePercent;
    std::optional<std::uint32_t> noiseSeed;
    // BoundaryTerrain 的实际 RGBA 及水色/深水阈值；来源未证实时必须保持空值。
    std::optional<std::array<std::uint8_t, 4>> boundaryColorRgba;
    std::optional<std::array<std::uint8_t, 4>> waterColorRgba;
    std::optional<double> waterDeepThreshold;
    // 对象层语义与 useMMColor=0 默认色；当前 TerrainMinimapInput 没有对象快照，故不执行。
    std::optional<bool> objectLayerIsDoodad;
    std::optional<std::array<std::uint8_t, 4>> objectDefaultColorRgba;
    std::optional<std::array<std::uint8_t, 4>> defaultGroundColorRgba;
    // 悬崖例外条件与可玩区域矩形；矩形仍须与同源 W3I 绑定，不能由地图边缘猜测。
    std::optional<bool> cliffExceptionResolved;
    std::optional<std::array<std::int32_t, 4>> playableTileRectangle;
};

struct TerrainMinimapInput {
    TerrainInspectionBinding binding;
    // 当前修订已经序列化的完整 W3E；本函数内部重新 Parse/Serialize 并核对实际 SHA。
    std::vector<std::byte> currentW3e;
    // 与当前 W3E groundTilesets 同序；宿主必须冻结快照，不能经别名并发改写。
    std::vector<std::shared_ptr<const TerrainAtlasSnapshot>> groundAtlases;
    TerrainMinimapRangePolicy rangePolicy = TerrainMinimapRangePolicy::FullW3e;
    // 显式声明边距策略才消费的源 W3I 原字；nullopt 与真实空成员分别拒绝。
    // 纯生成器自己计算 SHA/解析范围，不认证这些字节来自地图或当前会话。
    std::optional<std::vector<std::byte>> sourceW3i;
    // 旧调用保持空值并沿用当前严格域；非空只启用下列 fail-closed 合同验证。
    std::optional<TerrainMinimapGenerationConfig> generation;
    std::optional<ClassicMinimapTerrainColors> classicColors;
};

struct TerrainMinimapLimits {
    // 默认值也是硬上限，只能收紧。不会为了 256 输出而降低原格网格或省略过渡层。
    std::size_t maxTiles = 65536;
    std::size_t maxW3eBytes = 4 * 1024 * 1024;
    // 输入容器 capacity、唯一 atlas 及其原始解码链的保活账；不包含编码源文件内容。
    std::size_t maxRetainedInputBytes = 68 * 1024 * 1024;
    // 输入保活 + 额外工作 + 输出的保守估算；另保留 GroundMesh 原硬上限。
    std::size_t maxWorkingBytes = 128 * 1024 * 1024;
};

struct TerrainMinimapMipFacts {
    std::uint32_t width = 0, height = 0;
    std::size_t rgbaBytes = 0, rgbaCapacityBytes = 0;
    std::string decodedRgbaSha256; // 本次对真实 RGBA 字节计算；不代替 encoded source SHA
};

struct TerrainMinimapAtlasFacts {
    Resources::ResourceLocation declaredSource;
    std::string declaredEncodedSha256;
    std::size_t declaredEncodedBytes = 0;
    std::uint32_t sourceWidth = 0, sourceHeight = 0, columns = 0, rows = 4;
    std::uint32_t declaredValidatedMipCount = 0;
    std::vector<TerrainMinimapMipFacts> mips;
    std::vector<TerrainAtlasCell> cells;
    std::uint32_t originalMaxMip = 0, cellSafeMaxMip = 0, usableMaxMip = 0;
    double maximumRequestedLod = 0, maximumSampledLod = 0;
    bool missingOriginalMipsLimitedSampling = false;
    bool cellBoundaryLimitedSampling = false;
};

struct TerrainMinimapMemoryFacts {
    // 对既有 MapSettings 有界布局的保守解析/snapshot预留；分配器元数据仍不在口径内。
    static constexpr std::size_t settingsScratchFixedBytes = 64 * 1024, settingsScratchBytesPerSourceByte = 64;
    // capacity 计费含容器对象、字符串 capacity+1（SSO 仍保守计费），去重 atlas 指针。
    // 不含分配器元数据、共享指针控制块、CNG provider 系统内存，也不是进程 RSS。
    std::size_t retainedInputCapacityBytes = 0;
    std::size_t estimatedExtraPeakBytes = 0, estimatedOutputBytes = 0, estimatedPeakBytes = 0;
    // 以下为成功时实际可观察的容器 capacity 账，不冒充已测得的完整峰值。
    std::size_t parsedTerrainCapacityBytes = 0, reserializedW3eCapacityBytes = 0;
    std::size_t meshCapacityBytes = 0, rasterCapacityBytes = 0;
    std::size_t outputCapacityBytes = 0, observedContainerCapacityBytes = 0;
    std::size_t maximumObservedHashScratchBytes = 0;
    std::size_t estimatedSettingsScratchBytes = 0, parsedSettingsCapacityBytes = 0;
};

struct TerrainMinimapW3iFacts {
    // FullW3e 为 NotRequested/空 SHA；成功 crop 为 Parsed/source-archive。
    std::string state = "NotRequested", stage, sha256;
    std::size_t sourceBytes = 0;
    std::int32_t formatVersion = 0, playableWidth = 0, playableHeight = 0;
    std::array<float, 8> cameraBounds{};
    std::array<std::int32_t, 4> cameraMargins{}; // L/R/B/T，tile 单位；不是 native crop 证明。
};

struct TerrainMinimapMapEdgeFacts {
    // 仅独立外缘政策为Validated；摘要按原row-major每顶点单byte 0/1，不改源标志。
    static constexpr std::uint32_t policyVersion = 1;
    static constexpr const char* maskPolicy = "declared-w3i-margins-sw-anchor-exterior-terminal-clear-v1";
    std::string state = "NotRequested", maskSha256;
    std::size_t sourceTileAnchors = 0, declaredTileAnchors = 0, exteriorMapEdgeAnchors = 0, terminalClearVertices = 0;
};

struct TerrainMinimapManifest {
    static constexpr std::uint32_t algorithmVersion = 4; // v4增加独立声明外缘mask，像素策略保持。
    static constexpr const char* samplingPolicy = "original-mips-cell-clamped-trilinear-unorm-v1";
    static constexpr const char* encodingPolicy = "opaque-full-chain-shared-adaptive-palette-v1";
    TerrainInspectionBinding binding;
    std::string currentW3eSha256, reserializedW3eSha256;
    // 完整 W3E 范围 [west,south,east,north]，不采用 W3I 可玩区或相机裁剪。
    std::array<double, 4> worldExtent{};
    std::array<double, 4> renderExtent{}; // 实际选区；worldExtent 始终保留完整 W3E。
    TerrainMinimapRangePolicy rangePolicy = TerrainMinimapRangePolicy::FullW3e;
    static constexpr std::uint32_t rangePolicyVersion = 2;
    TerrainMinimapW3iFacts w3i;
    TerrainMinimapMapEdgeFacts mapEdge;
    // 256 画布上的连续边界 [left,top,width,height]；像素中心落在半开范围才采样。
    std::array<double, 4> contentRectangle{};
    std::size_t sourceTiles = 0, meshQuads = 0, meshBatches = 0;
    std::string meshSha256, outputRgbaSha256, outputBlpSha256, manifestSha256;
    Image::BlpOpaquePaletteMethod paletteMethod = Image::BlpOpaquePaletteMethod::Exact;
    bool paletteQuantized = false;
    std::uint32_t paletteColorCount = 0, occupiedHistogramBins = 0, encodedMipCount = 0;
    std::size_t encodedTotalMipPixels = 0, encodedBlpBytes = 0;
    std::vector<TerrainMinimapAtlasFacts> atlases; // 实际 mesh texture 槽顺序
    TerrainMinimapMemoryFacts memory;
    // sourceSha256 仅是宿主冻结身份；没有归档重读、编码源解码复验或 resolver winner 复验。
    bool archiveSourceIndependentlyVerified = false;
    bool encodedAtlasSourcesIndependentlyVerified = false;
    bool resourceWinnersIndependentlyVerified = false;
    bool nativeEditorOrGameVerified = false;
    bool formalSaveAuthorized = false;
};

struct TerrainMinimapCandidate {
    Image::DecodedImage image; // 固定 256×256，RGBA8888，左上原点，北向上，alpha 恒为 255
    Image::BlpOpaqueQuantized encoded; // 同次图像生成的完整 BLP1 候选；没有地图写入授权
    TerrainMinimapManifest manifest;
};

// 独立、只读 CPU 候选，不执行 I/O/GPU/保存。仅接受 classic v11 干地heightfield，统一
// cliff layer，无坡道/水/荒芜/边界/未知位；旧两政策仍拒MapEdge。独立外缘政策只接
// 完整W3I声明域外的SW锚点MapEdge且末行/列clear，不清bit，不豁免其它全网格拒域。
// 采样内域quad可共享带MapEdge的外部锚点；合法 cliff palette 索引不等于悬崖。
// 保留真实Z及其mesh身份；顶视unlit覆盖/UV/LOD不使用Z，不计算坡度明暗、阴影或遮挡。
// 普通高程坡面不等于Ramp flag或cliff层变化；生成域不授予正式保存的幅度资格。
// 内部从当前 W3E 构建 Strict unlit 完整 GroundMesh，逐批次索引追溯原 tile 与全部层。
// 每层按唯一三角重心插值 UV；原始连续 mip 按 quad 导数及输出像素足迹选择 LOD，
// 在每个仍有整数 cell 边界的 mip 内夹到该 cell 首末 texel 再双线性/三线性采样。
// 这是明确的 CPU 策略，不声称等价于 GPU 隐式 LOD、4 tap 或各向异性采样。
// base 忽略源 alpha，overlay 为 straight alpha；在 UNORM 字节数值空间用 double 混合，
// 最终才 lround 到 byte，不隐式进行 sRGB/linear 转换。不拓宽任何现有保存门禁。
// 最后经独立 opaque BLP 编码器生成全部 mip；量化策略、实际输出 SHA 与容量同样绑定。
[[nodiscard]] Result<TerrainMinimapCandidate> BuildTerrainMinimapCandidate(
    const TerrainMinimapInput& input, const TerrainMinimapLimits& limits = {});

} // namespace War3Map::Render
