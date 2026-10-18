#include "War3MapCore/Render/TerrainMinimapPostprocess.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace War3Map::Render {
namespace {

constexpr const char* kSubsystem = "TerrainMinimapPostprocess";
constexpr TerrainMinimapPostprocessLimits kHard{};
constexpr std::uint32_t kCanvasSize = 256;
constexpr std::size_t kCanvasBytes = 256u * 256u * 4u;
// 固定元数据、局部排序窗口和有界失败诊断的保守预留，不是分配器/RSS统计。
constexpr std::size_t kFixedReserve = 64u * 1024u;
static_assert(sizeof(TerrainMinimapPostprocessResult) + sizeof(TerrainMinimapPostprocessInput)
    + sizeof(TerrainMinimapPostprocessSettings) + sizeof(TerrainMinimapPostprocessLimits) < kFixedReserve / 2);

struct Failure { const char* code; const char* message; };

[[noreturn]] void Fail(const char* code, const char* message) { throw Failure{code, message}; }

std::size_t Product(std::size_t a, std::size_t b) {
    if (b && a > std::numeric_limits<std::size_t>::max() / b)
        Fail("TERRAIN_MINIMAP_POSTPROCESS_SIZE_OVERFLOW", "图像尺寸或容量乘法溢出。");
    return a * b;
}

void Charge(std::size_t& bytes, std::size_t addition) {
    if (addition > std::numeric_limits<std::size_t>::max() - bytes)
        Fail("TERRAIN_MINIMAP_POSTPROCESS_SIZE_OVERFLOW", "图像容量加法溢出。");
    bytes += addition;
}

void CheckBudget(std::size_t bytes, const TerrainMinimapPostprocessLimits& limits) {
    if (bytes > limits.maxWorkingBytes)
        Fail("TERRAIN_MINIMAP_POSTPROCESS_BUDGET", "输入保活、阶段图像或元数据超过工作预算。");
}

std::size_t InputCapacity(const TerrainMinimapPostprocessInput& input,
    const TerrainMinimapPostprocessSettings& settings) {
    std::size_t bytes = input.rgba.capacity();
    if (settings.noiseSamples) Charge(bytes, Product(settings.noiseSamples->capacity(), sizeof(std::uint16_t)));
    return bytes;
}

void ValidateSettings(const TerrainMinimapPostprocessSettings& settings, std::size_t contentPixels) {
    if ((settings.medianRadius != 0 && settings.medianRadius != 2)
        || !std::isfinite(settings.blurRadius) || (settings.blurRadius != 0 && settings.blurRadius != 0.5))
        Fail("TERRAIN_MINIMAP_POSTPROCESS_FILTER_UNSUPPORTED", "仅支持禁用或半径2中值，以及禁用或sigma .5高斯。");
    if (!std::isfinite(settings.noisePercent) || settings.noisePercent < 0 || settings.noisePercent > 100)
        Fail("TERRAIN_MINIMAP_POSTPROCESS_NOISE_INVALID", "噪点百分比必须为有限的0至100。");
    if (settings.noiseSamples && settings.noiseSeed)
        Fail("TERRAIN_MINIMAP_POSTPROCESS_NOISE_SOURCE_AMBIGUOUS", "固定u15样本和seed不能同时声明。");
    if (settings.noiseSamples) {
        if (settings.noiseSamples->size() != contentPixels)
            Fail("TERRAIN_MINIMAP_POSTPROCESS_NOISE_SAMPLE_COUNT", "u15样本数必须恰好等于缩放后内容像素数。");
        for (const auto sample : *settings.noiseSamples) if (sample > 32767)
            Fail("TERRAIN_MINIMAP_POSTPROCESS_NOISE_SAMPLE_INVALID", "u15噪点样本超出0至32767。");
    }
    if (settings.noisePercent > 0 && !settings.noiseSamples && !settings.noiseSeed)
        Fail("TERRAIN_MINIMAP_POSTPROCESS_NOISE_SOURCE_REQUIRED", "启用噪点必须由调用者提供u15样本或显式seed。");
}

std::size_t Offset(std::uint32_t x, std::uint32_t y, std::uint32_t width) {
    // 仅用于已验证的最多256x256阶段图。
    return (static_cast<std::size_t>(y) * width + x) * 4;
}

unsigned Channel(const std::vector<std::byte>& rgba, std::size_t at) {
    return std::to_integer<unsigned>(rgba[at]);
}

void Scale(const TerrainMinimapPostprocessInput& input, const TerrainMinimapPostprocessFacts& facts,
    std::vector<std::byte>& target) {
    // 源宽高至多uint32；(src<<16)和256*step均在uint64范围内。预先整除步长，
    // 不能改成每像素(x*src/dst)或double，那会改变16.16量化边界。
    for (std::uint32_t y = 0; y < facts.contentHeight; ++y) {
        const auto sourceY = static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * facts.stepY16_16) >> 16);
        for (std::uint32_t x = 0; x < facts.contentWidth; ++x) {
            const auto sourceX = static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * facts.stepX16_16) >> 16);
            const auto source = (static_cast<std::size_t>(sourceY) * input.width + sourceX) * 4;
            const auto dest = Offset(x, y, facts.contentWidth);
            std::copy_n(input.rgba.data() + source, 4, target.data() + dest);
        }
    }
}

void Median(const std::vector<std::byte>& source, std::vector<std::byte>& target,
    std::uint32_t width, std::uint32_t height) {
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        std::array<std::array<unsigned, 25>, 3> values{};
        std::size_t count = 0;
        const auto beginY = y > 2 ? y - 2 : 0;
        const auto beginX = x > 2 ? x - 2 : 0;
        const auto endY = std::min(height - 1, y + 2), endX = std::min(width - 1, x + 2);
        for (auto sy = beginY; sy <= endY; ++sy) for (auto sx = beginX; sx <= endX; ++sx) {
            const auto at = Offset(sx, sy, width);
            for (unsigned c = 0; c < 3; ++c) values[c][count] = Channel(source, at + c);
            ++count;
        }
        const auto dest = Offset(x, y, width);
        for (unsigned c = 0; c < 3; ++c) {
            std::sort(values[c].begin(), values[c].begin() + static_cast<std::ptrdiff_t>(count));
            target[dest + c] = static_cast<std::byte>(values[c][count / 2]);
        }
        target[dest + 3] = source[dest + 3];
    }
}

void Gaussian(const std::vector<std::byte>& source, std::vector<std::byte>& target,
    std::uint32_t width, std::uint32_t height) {
    // exp(-2)及exp(-4)的double常量。顺序固定为dy再dx，边缘只累加图内权重。
    constexpr std::array<double, 3> weights{1.0, 0.1353352832366126918939994949724844034,
        0.01831563888873418029371802127324124221};
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        std::array<double, 3> sums{};
        double totalWeight = 0;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const auto sx = static_cast<int>(x) + dx, sy = static_cast<int>(y) + dy;
            if (sx < 0 || sy < 0 || sx >= static_cast<int>(width) || sy >= static_cast<int>(height)) continue;
            const auto weight = weights[static_cast<std::size_t>(dx * dx + dy * dy)];
            const auto at = Offset(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy), width);
            for (unsigned c = 0; c < 3; ++c) sums[c] += Channel(source, at + c) * weight;
            totalWeight += weight;
        }
        const auto dest = Offset(x, y, width);
        for (unsigned c = 0; c < 3; ++c)
            target[dest + c] = static_cast<std::byte>(static_cast<unsigned>(std::clamp(sums[c] / totalWeight, 0.0, 255.0)));
        target[dest + 3] = source[dest + 3];
    }
}

std::uint16_t NextU15(std::uint32_t& state) {
    // uint32 wrap为LCG32-v1合同；只修改本次调用局部state，seed=0也是有效确定序列。
    state = state * 1664525u + 1013904223u;
    return static_cast<std::uint16_t>(state >> 17);
}

void Noise(std::vector<std::byte>& content, const TerrainMinimapPostprocessSettings& settings) {
    const double percent = settings.noisePercent / 100.0;
    auto state = settings.noiseSeed.value_or(0);
    for (std::size_t pixel = 0; pixel < content.size() / 4; ++pixel) {
        const auto sample = settings.noiseSamples ? (*settings.noiseSamples)[pixel] : NextU15(state);
        const double factor = (1.0 - percent) + static_cast<double>(sample) / 32767.0 * (2.0 * percent);
        for (unsigned c = 0; c < 3; ++c) {
            const auto value = std::clamp(Channel(content, pixel * 4 + c) * factor, 0.0, 255.0);
            content[pixel * 4 + c] = static_cast<std::byte>(static_cast<unsigned>(value));
        }
    }
}

void Canvas(const std::vector<std::byte>& content, const TerrainMinimapPostprocessFacts& facts,
    Image::DecodedImage& target) {
    target.width = target.height = kCanvasSize;
    target.rgba.resize(kCanvasBytes, std::byte{0});
    for (std::size_t at = 3; at < kCanvasBytes; at += 4) target.rgba[at] = std::byte{255};
    const auto offsetX = (kCanvasSize - facts.contentWidth) / 2;
    const auto offsetY = (kCanvasSize - facts.contentHeight) / 2;
    for (std::uint32_t y = 0; y < facts.contentHeight; ++y) {
        const auto source = Offset(0, y, facts.contentWidth);
        const auto dest = Offset(offsetX, kCanvasSize - 1 - (offsetY + y), kCanvasSize);
        std::copy_n(content.data() + source, static_cast<std::size_t>(facts.contentWidth) * 4, target.rgba.data() + dest);
    }
}

Result<TerrainMinimapPostprocessResult> FailureResult(const char* code, const char* message) {
    Result<TerrainMinimapPostprocessResult> result;
    result.diagnostics.push_back({DiagnosticSeverity::Error, kSubsystem, code, message, {}});
    return result;
}

} // namespace

Result<TerrainMinimapPostprocessResult> PostprocessTerrainMinimap(const TerrainMinimapPostprocessInput& input,
    const TerrainMinimapPostprocessSettings& settings, const TerrainMinimapPostprocessLimits& limits) {
    try {
        if (!limits.maxWorkingBytes || limits.maxWorkingBytes > kHard.maxWorkingBytes)
            Fail("TERRAIN_MINIMAP_POSTPROCESS_LIMITS", "工作预算只能在非零128MiB硬上限内收紧。");
        if (!input.width || !input.height)
            Fail("TERRAIN_MINIMAP_POSTPROCESS_DIMENSIONS", "输入图像宽高必须非零。");
        const auto inputPixels = Product(input.width, input.height);
        const auto inputBytes = Product(inputPixels, 4);
        TerrainMinimapPostprocessResult output;
        auto& facts = output.facts;
        facts.retainedInputCapacityBytes = InputCapacity(input, settings);
        // 先预算检查再长度检查，巨大声明不需要分配或遍历正文即可明确拒绝。
        auto estimate = kFixedReserve;
        Charge(estimate, std::max(inputBytes, input.rgba.capacity()));
        if (settings.noiseSamples) Charge(estimate, Product(settings.noiseSamples->capacity(), sizeof(std::uint16_t)));
        CheckBudget(estimate, limits);
        if (input.rgba.size() != inputBytes)
            Fail("TERRAIN_MINIMAP_POSTPROCESS_RGBA_SIZE", "RGBA字节数与宽高不符。");
        facts.sourceWidth = input.width; facts.sourceHeight = input.height;
        if (input.width >= input.height) {
            facts.contentWidth = kCanvasSize;
            facts.contentHeight = static_cast<std::uint32_t>(static_cast<std::uint64_t>(kCanvasSize) * input.height / input.width);
        } else {
            facts.contentHeight = kCanvasSize;
            facts.contentWidth = static_cast<std::uint32_t>(static_cast<std::uint64_t>(kCanvasSize) * input.width / input.height);
        }
        if (!facts.contentWidth || !facts.contentHeight)
            Fail("TERRAIN_MINIMAP_POSTPROCESS_ASPECT_UNREPRESENTABLE", "等比整除后的短边为零，256画布无法表示该宽高比。");
        const auto contentPixels = Product(facts.contentWidth, facts.contentHeight);
        const auto contentBytes = Product(contentPixels, 4);
        Charge(estimate, Product(contentBytes, 2)); // 缩放/滤波 ping-pong，噪点原地执行
        Charge(estimate, Product(kCanvasBytes, 2));
        CheckBudget(estimate, limits);
        ValidateSettings(settings, contentPixels);
        facts.estimatedPeakBytes = estimate;
        facts.stepX16_16 = (static_cast<std::uint64_t>(input.width) << 16) / facts.contentWidth;
        facts.stepY16_16 = (static_cast<std::uint64_t>(input.height) << 16) / facts.contentHeight;
        const auto left = (kCanvasSize - facts.contentWidth) / 2;
        const auto topBeforeFlip = (kCanvasSize - facts.contentHeight) / 2;
        facts.contentRectangle = {left, kCanvasSize - topBeforeFlip - facts.contentHeight,
            facts.contentWidth, facts.contentHeight};
        facts.medianRadius = settings.medianRadius; facts.blurRadius = settings.blurRadius;
        facts.noisePercent = settings.noisePercent;
        if (settings.noisePercent > 0) {
            facts.noiseSource = settings.noiseSamples ? TerrainMinimapNoiseSource::CallerU15Samples : TerrainMinimapNoiseSource::Lcg32V1;
            facts.noiseSeed = settings.noiseSeed; facts.noiseSamplesUsed = contentPixels;
        }
        std::vector<std::byte> content(contentBytes), scratch(contentBytes);
        output.preNoise.rgba.reserve(kCanvasBytes); output.finalImage.rgba.reserve(kCanvasBytes);
        auto observed = kFixedReserve;
        Charge(observed, facts.retainedInputCapacityBytes);
        Charge(observed, content.capacity()); Charge(observed, scratch.capacity());
        Charge(observed, output.preNoise.rgba.capacity()); Charge(observed, output.finalImage.rgba.capacity());
        CheckBudget(observed, limits);
        facts.observedContainerCapacityBytes = observed;
        Scale(input, facts, content);
        if (settings.medianRadius > 0) { Median(content, scratch, facts.contentWidth, facts.contentHeight); content.swap(scratch); }
        if (settings.blurRadius > 0) { Gaussian(content, scratch, facts.contentWidth, facts.contentHeight); content.swap(scratch); }
        Canvas(content, facts, output.preNoise);
        if (settings.noisePercent > 0) Noise(content, settings);
        Canvas(content, facts, output.finalImage);
        Result<TerrainMinimapPostprocessResult> result;
        result.value = std::move(output);
        return result;
    } catch (const Failure& failure) {
        return FailureResult(failure.code, failure.message);
    } catch (const std::bad_alloc&) {
        return FailureResult("TERRAIN_MINIMAP_POSTPROCESS_ALLOCATION", "有界后处理分配失败，不返回部分图像。");
    } catch (const std::length_error&) {
        return FailureResult("TERRAIN_MINIMAP_POSTPROCESS_ALLOCATION", "有界后处理超出容器容量，不返回部分图像。");
    }
}

} // namespace War3Map::Render
