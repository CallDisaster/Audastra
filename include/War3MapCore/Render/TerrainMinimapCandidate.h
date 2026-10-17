#pragma once

#include "War3MapCore/Render/TerrainInspection.h"
#include "War3MapCore/Render/TerrainTextureAtlas.h"

#include <optional>

namespace War3Map::Render {

// full-w3e / declared-w3i-margins；后者仅是声明边距政策，未验证1.27a native crop。
enum class TerrainMinimapRangePolicy { FullW3e, DeclaredW3iMargins };

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

struct TerrainMinimapManifest {
    static constexpr std::uint32_t algorithmVersion = 3; // v3增加显式声明边距，默认像素策略保持。
    static constexpr const char* samplingPolicy = "original-mips-cell-clamped-trilinear-unorm-v1";
    static constexpr const char* encodingPolicy = "opaque-full-chain-shared-adaptive-palette-v1";
    TerrainInspectionBinding binding;
    std::string currentW3eSha256, reserializedW3eSha256;
    // 完整 W3E 范围 [west,south,east,north]，不采用 W3I 可玩区或相机裁剪。
    std::array<double, 4> worldExtent{};
    std::array<double, 4> renderExtent{}; // 实际选区；worldExtent 始终保留完整 W3E。
    TerrainMinimapRangePolicy rangePolicy = TerrainMinimapRangePolicy::FullW3e;
    static constexpr std::uint32_t rangePolicyVersion = 1;
    TerrainMinimapW3iFacts w3i;
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
// cliff layer，无坡道/水/荒芜/边界/MapEdge/未知位。合法 cliff palette 索引不等于悬崖。
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
