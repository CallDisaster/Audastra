#pragma once

#include "War3MapCore/Image/BlpImage.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace War3Map::Render {

// 逐格取色完成后的任意 RGBA8888 图；行优先、每行无填充。组件不解释地图/世界朝向。
struct TerrainMinimapPostprocessInput {
    std::uint32_t width = 0, height = 0;
    std::vector<std::byte> rgba;
};

struct TerrainMinimapPostprocessSettings {
    // 只实现本组件明确的 5x5 中值及 sigma=.5 的 3x3 高斯；0 可独立禁用某一步。
    std::int32_t medianRadius = 2; // 仅 0 或 2
    double blurRadius = 0.5; // 仅 0 或 .5，单位为 sigma
    double noisePercent = 10.0; // 有限的 [0,100]
    // 有噪点时必须二选一。样本按补边/翻转前的内容图逐行排列，恰好每像素一个 [0,32767]。
    // seed 使用本组件固定的 LCG32-v1，不复现原版编辑器的进程级 CRT 随机流。
    std::optional<std::vector<std::uint16_t>> noiseSamples;
    std::optional<std::uint32_t> noiseSeed;
};

struct TerrainMinimapPostprocessLimits {
    // 默认也是硬上限，只能收紧。包含输入及样本 capacity、全部图像阶段和固定元数据预留。
    // 不包含分配器元数据或进程 RSS；没有哈希/编码/I/O 工作缓冲。
    std::size_t maxWorkingBytes = 128u * 1024u * 1024u;
};

enum class TerrainMinimapNoiseSource { Disabled, CallerU15Samples, Lcg32V1 };

struct TerrainMinimapPostprocessFacts {
    static constexpr std::uint32_t algorithmVersion = 1;
    static constexpr const char* samplingPolicy = "nearest-16.16-truncated-step-v1";
    static constexpr const char* filterPolicy = "rgb-upper-median5-gaussian3-sigma0.5-truncate-v1";
    static constexpr const char* noisePolicy = "rgb-shared-u15-factor-truncate-clamp-v1";
    std::uint32_t sourceWidth = 0, sourceHeight = 0;
    std::uint32_t contentWidth = 0, contentHeight = 0;
    std::uint64_t stepX16_16 = 0, stepY16_16 = 0;
    // 最终上下翻转后的画布内容半开矩形 [x,y,width,height]，均为整数像素。
    std::array<std::uint32_t, 4> contentRectangle{};
    std::int32_t medianRadius = 0;
    double blurRadius = 0, noisePercent = 0;
    TerrainMinimapNoiseSource noiseSource = TerrainMinimapNoiseSource::Disabled;
    std::optional<std::uint32_t> noiseSeed;
    std::size_t noiseSamplesUsed = 0;
    std::size_t retainedInputCapacityBytes = 0;
    std::size_t estimatedPeakBytes = 0;
    // 成功时的真实容器 capacity 加固定预留，不冒充测量到的完整分配峰值。
    std::size_t observedContainerCapacityBytes = 0;
};

struct TerrainMinimapPostprocessResult {
    // 两图均为256x256；preNoise是同一次缩放/中值/高斯结果补黑边并翻转后的精确RGBA。
    Image::DecodedImage preNoise;
    Image::DecodedImage finalImage;
    TerrainMinimapPostprocessFacts facts;
};

// 纯 CPU：等比整除尺寸 -> 16.16 nearest -> 5x5 upper median -> sigma .5 Gaussian
// -> 每像素同因子RGB噪点 -> opaque black居中补边 -> 上下翻转。RGB直接在字节数值空间
// 滤波，不做sRGB变换；内容alpha仅最近邻采样并保留，不参与滤波/噪点，padding alpha=255。
// 任一尺寸为0、整比短边变0、RGBA长度错误、未知滤波设置或预算不足均拒绝，无部分结果。
// 不读取地图/产品parser、不编码BLP、不接保存，不认证与WorldEdit的像素或随机流一致性。
[[nodiscard]] Result<TerrainMinimapPostprocessResult> PostprocessTerrainMinimap(
    const TerrainMinimapPostprocessInput& input,
    const TerrainMinimapPostprocessSettings& settings = {},
    const TerrainMinimapPostprocessLimits& limits = {});

} // namespace War3Map::Render
