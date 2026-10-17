#pragma once

#include "War3MapCore/Render/TerrainInspection.h"
#include "War3MapCore/Render/TerrainTextureAtlas.h"

namespace War3Map::Render {

struct TerrainMinimapInput {
    TerrainInspectionBinding binding;
    // 当前修订已经序列化的完整 W3E；本函数内部重新 Parse/Serialize 并核对实际 SHA。
    std::vector<std::byte> currentW3e;
    // 与当前 W3E groundTilesets 同序；宿主必须冻结快照，不能经别名并发改写。
    std::vector<std::shared_ptr<const TerrainAtlasSnapshot>> groundAtlases;
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
    // capacity 计费含容器对象、字符串 capacity+1（SSO 仍保守计费），去重 atlas 指针。
    // 不含分配器元数据、共享指针控制块、CNG provider 系统内存，也不是进程 RSS。
    std::size_t retainedInputCapacityBytes = 0;
    std::size_t estimatedExtraPeakBytes = 0, estimatedOutputBytes = 0, estimatedPeakBytes = 0;
    // 以下为成功时实际可观察的容器 capacity 账，不冒充已测得的完整峰值。
    std::size_t parsedTerrainCapacityBytes = 0, reserializedW3eCapacityBytes = 0;
    std::size_t meshCapacityBytes = 0, rasterCapacityBytes = 0;
    std::size_t outputCapacityBytes = 0, observedContainerCapacityBytes = 0;
    std::size_t maximumObservedHashScratchBytes = 0;
};

struct TerrainMinimapManifest {
    static constexpr std::uint32_t algorithmVersion = 1;
    static constexpr const char* samplingPolicy = "original-mips-cell-clamped-trilinear-unorm-v1";
    TerrainInspectionBinding binding;
    std::string currentW3eSha256, reserializedW3eSha256;
    // 完整 W3E 范围 [west,south,east,north]，不采用 W3I 可玩区或相机裁剪。
    std::array<double, 4> worldExtent{};
    // 256 画布上的连续边界 [left,top,width,height]；像素中心落在半开范围才采样。
    std::array<double, 4> contentRectangle{};
    std::size_t sourceTiles = 0, meshQuads = 0, meshBatches = 0;
    std::string meshSha256, outputRgbaSha256, manifestSha256;
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
    TerrainMinimapManifest manifest;
};

// 独立、只读 CPU 候选，不执行 I/O/GPU/编码/保存。仅接受 classic v11 平坦干地，统一
// cliff layer，无坡道/水/荒芜/边界/MapEdge/未知位。合法 cliff palette 索引不等于悬崖。
// 内部从当前 W3E 构建 Strict unlit 完整 GroundMesh，逐批次索引追溯原 tile 与全部层。
// 每层按唯一三角重心插值 UV；原始连续 mip 按 quad 导数及输出像素足迹选择 LOD，
// 在每个仍有整数 cell 边界的 mip 内夹到该 cell 首末 texel 再双线性/三线性采样。
// 这是明确的 CPU 策略，不声称等价于 GPU 隐式 LOD、4 tap 或各向异性采样。
// base 忽略源 alpha，overlay 为 straight alpha；在 UNORM 字节数值空间用 double 混合，
// 最终才 lround 到 byte，不隐式进行 sRGB/linear 转换。不拓宽任何现有保存门禁。
[[nodiscard]] Result<TerrainMinimapCandidate> BuildTerrainMinimapCandidate(
    const TerrainMinimapInput& input, const TerrainMinimapLimits& limits = {});

} // namespace War3Map::Render
