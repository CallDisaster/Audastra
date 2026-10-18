#include "War3MapCore/Render/TerrainMinimapPostprocess.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using namespace War3Map;
using namespace War3Map::Render;
using Rgba = std::array<unsigned, 4>;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

TerrainMinimapPostprocessResult Take(Result<TerrainMinimapPostprocessResult> result) {
    if (!result.Succeeded()) {
        std::string message = "postprocess fixture failed";
        for (const auto& d : result.diagnostics) message += " | " + d.code + ":" + d.message;
        throw std::runtime_error(message);
    }
    Require(result.value.has_value(), "成功必须返回完整图像");
    return std::move(*result.value);
}

void Rejected(const Result<TerrainMinimapPostprocessResult>& result, const char* code) {
    Require(!result.Succeeded() && !result.value, "失败不能返回部分图像");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& d) {
        return d.code == code;
    }), "必须命中指定拒绝诊断");
}

void Set(TerrainMinimapPostprocessInput& input, std::uint32_t x, std::uint32_t y, const Rgba& rgba) {
    const auto at = (static_cast<std::size_t>(y) * input.width + x) * 4;
    for (unsigned c = 0; c < 4; ++c) input.rgba[at + c] = static_cast<std::byte>(rgba[c]);
}

TerrainMinimapPostprocessInput Solid(std::uint32_t width, std::uint32_t height, const Rgba& rgba) {
    TerrainMinimapPostprocessInput input{width, height, std::vector<std::byte>(static_cast<std::size_t>(width) * height * 4)};
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) Set(input, x, y, rgba);
    return input;
}

Rgba Pixel(const Image::DecodedImage& image, std::uint32_t x, std::uint32_t y) {
    const auto at = (static_cast<std::size_t>(y) * image.width + x) * 4;
    Rgba result{};
    for (unsigned c = 0; c < 4; ++c) result[c] = std::to_integer<unsigned>(image.rgba[at + c]);
    return result;
}

TerrainMinimapPostprocessSettings Unfiltered() {
    TerrainMinimapPostprocessSettings settings;
    settings.medianRadius = 0; settings.blurRadius = 0; settings.noisePercent = 0;
    return settings;
}

void TestSmallHandComputedScaleAndOrientation() {
    auto input = Solid(2, 2, {0, 0, 0, 0});
    Set(input, 0, 0, {11, 12, 13, 14}); Set(input, 1, 0, {21, 22, 23, 24});
    Set(input, 0, 1, {31, 32, 33, 34}); Set(input, 1, 1, {41, 42, 43, 44});
    const auto result = Take(PostprocessTerrainMinimap(input, Unfiltered()));
    Require(result.preNoise.Valid() && result.finalImage.Valid(), "两输出必须为有效RGBA");
    Require(result.preNoise.width == 256 && result.preNoise.height == 256, "固定256画布");
    Require(result.facts.stepX16_16 == 512 && result.facts.stepY16_16 == 512, "2x2最近邻步长应为512");
    Require(Pixel(result.preNoise, 0, 0) == Rgba{31, 32, 33, 34}, "最终首行来自源底行");
    Require(Pixel(result.preNoise, 127, 127) == Rgba{31, 32, 33, 34}, "左下源像素覆盖128x128");
    Require(Pixel(result.preNoise, 128, 127) == Rgba{41, 42, 43, 44}, "横向nearest边界");
    Require(Pixel(result.preNoise, 0, 128) == Rgba{11, 12, 13, 14}, "纵向nearest边界及alpha保留");
    Require(Pixel(result.preNoise, 255, 255) == Rgba{21, 22, 23, 24}, "最终末行来自源顶行");
    Require(result.finalImage.rgba == result.preNoise.rgba, "禁噪时两张图逐字一致");
    Require(result.facts.noiseSource == TerrainMinimapNoiseSource::Disabled && result.facts.noiseSamplesUsed == 0,
        "禁噪metadata明确没有消费随机样本");
}

void TestBlackPaddingAndOddFlipRectangle() {
    const auto result = Take(PostprocessTerrainMinimap(Solid(3, 1, {90, 80, 70, 0}), Unfiltered()));
    Require(result.facts.contentWidth == 256 && result.facts.contentHeight == 85, "整比短边向下整除");
    Require(result.facts.contentRectangle == std::array<std::uint32_t, 4>{0, 86, 256, 85},
        "85行内容翻转后上黑86行、下黑85行");
    Require(Pixel(result.preNoise, 0, 85) == Rgba{0, 0, 0, 255}, "上padding必须opaque black");
    Require(Pixel(result.preNoise, 0, 86) == Rgba{90, 80, 70, 0}, "内容alpha不能被padding不透明规则覆盖");
    Require(Pixel(result.preNoise, 255, 170) == Rgba{90, 80, 70, 0}, "内容矩形末行");
    Require(Pixel(result.finalImage, 255, 171) == Rgba{0, 0, 0, 255}, "下padding必须opaque black");
    for (std::uint32_t y = 0; y < 256; ++y) for (std::uint32_t x = 0; x < 256; ++x)
        if (y < 86 || y >= 171) Require(Pixel(result.finalImage, x, y) == Rgba{0, 0, 0, 255}, "全padding均须不透明");
}

void TestFixedPointStepRejectsDoubleNegativeControl() {
    auto input = Solid(3, 2, {10, 20, 30, 255});
    for (std::uint32_t x = 0; x < 3; ++x) Set(input, x, 1, {200, 210, 220, 255});
    const auto result = Take(PostprocessTerrainMinimap(input, Unfiltered()));
    Require(result.facts.contentHeight == 170 && result.facts.stepY16_16 == 771, "先截断16.16步长");
    // 85*771=65535，尚在源第0行；每像素精确比例或double floor(85*2/170)已经到第1行。
    constexpr auto doubleSourceY = static_cast<std::uint32_t>(85.0 * 2.0 / 170.0);
    static_assert(doubleSourceY == 1);
    Require(Pixel(result.preNoise, 0, 127) == Rgba{10, 20, 30, 255}, "定点量化负控不能改成double nearest");
    Require(Pixel(result.preNoise, 0, 126) == Rgba{200, 210, 220, 255}, "下一个定点步才跨源行");
}

void TestMedianUpperMedianAndAlphaIndependent() {
    auto input = Solid(256, 256, {100, 100, 100, 68});
    for (std::uint32_t y = 0; y < 5; ++y) for (std::uint32_t x = 0; x < 5; ++x) {
        const auto n = y * 5 + x;
        Set(input, 98 + x, 98 + y, {n, 24 - n, 200, 19});
    }
    for (std::uint32_t y = 0; y < 4; ++y) for (std::uint32_t x = 0; x < 4; ++x) {
        const auto n = y * 4 + x;
        Set(input, x, y, {n, 15 - n, 50, 77});
    }
    auto settings = Unfiltered(); settings.medianRadius = 2;
    const auto result = Take(PostprocessTerrainMinimap(input, settings));
    Require(Pixel(result.preNoise, 100, 155) == Rgba{12, 12, 200, 19}, "25个RGB分别排序取第12项");
    Require(Pixel(result.preNoise, 1, 254) == Rgba{8, 8, 50, 77}, "边缘16样本取upper median第8项，不补边");
    Require(result.facts.medianRadius == 2 && result.facts.blurRadius == 0, "独立median阶段事实");
}

void TestGaussianWeightsAndInBoundsRenormalization() {
    auto input = Solid(256, 256, {0, 0, 0, 73});
    Set(input, 100, 100, {255, 255, 255, 29}); Set(input, 0, 0, {255, 255, 255, 41});
    auto settings = Unfiltered(); settings.blurRadius = 0.5;
    const auto result = Take(PostprocessTerrainMinimap(input, settings));
    // a=exp(-2), b=exp(-4)，完整权重和1+4a+4b；角落只有1+2a+b。
    Require(Pixel(result.preNoise, 100, 155) == Rgba{157, 157, 157, 29}, "Gaussian中心255/(1+4a+4b)截断157");
    Require(Pixel(result.preNoise, 101, 155) == Rgba{21, 21, 21, 73}, "Gaussian正邻255a/(1+4a+4b)截断21");
    Require(Pixel(result.preNoise, 101, 154) == Rgba{2, 2, 2, 73}, "Gaussian斜邻255b/(1+4a+4b)截断2");
    Require(Pixel(result.preNoise, 0, 255) == Rgba{197, 197, 197, 41}, "Gaussian边缘按图内权重归一化截断197");
    Require(Pixel(result.preNoise, 1, 255) == Rgba{23, 23, 23, 73}, "Gaussian边缘邻点归一化截断23");
}

void TestMedianRunsBeforeGaussian() {
    auto input = Solid(256, 256, {0, 0, 0, 255});
    for (std::uint32_t y = 99; y <= 101; ++y) for (std::uint32_t x = 99; x <= 101; ++x)
        Set(input, x, y, {255, 255, 255, 255});
    auto settings = Unfiltered(); settings.medianRadius = 2; settings.blurRadius = 0.5;
    const auto result = Take(PostprocessTerrainMinimap(input, settings));
    Require(Pixel(result.preNoise, 100, 155) == Rgba{0, 0, 0, 255}, "3x3亮块先经5x5中值消除，再高斯仍0");
    // 负控若先高斯，亮区扩至5x5；随后中心25个样本至少21个为正，upper median会大于0。
    for (std::size_t at = 0; at < result.preNoise.rgba.size(); at += 4)
        Require(result.preNoise.rgba[at] == std::byte{0}, "median在Gaussian之前且输入阶段不可原地污染");
}

void TestCallerU15NoiseAndExactPreNoise() {
    const auto input = Solid(256, 256, {100, 200, 240, 17});
    auto settings = Unfiltered(); settings.noisePercent = 10;
    settings.noiseSamples = std::vector<std::uint16_t>(256u * 256u, 0);
    (*settings.noiseSamples)[1] = 32767; (*settings.noiseSamples)[2] = 16383;
    const auto result = Take(PostprocessTerrainMinimap(input, settings));
    Require(Pixel(result.preNoise, 0, 255) == Rgba{100, 200, 240, 17}, "噪点前精确图不能被原地噪点改写");
    Require(Pixel(result.finalImage, 0, 255) == Rgba{90, 180, 216, 17}, "u15=0时RGB共用0.9因子");
    Require(Pixel(result.finalImage, 1, 255) == Rgba{110, 220, 255, 17}, "u15=32767时RGB共用1.1因子并clamp");
    Require(Pixel(result.finalImage, 2, 255) == Rgba{99, 199, 239, 17}, "中间u15因子向零截断而非round");
    Require(result.facts.noiseSource == TerrainMinimapNoiseSource::CallerU15Samples
        && !result.facts.noiseSeed && result.facts.noiseSamplesUsed == 65536, "固定样本来源/数量metadata");
}

void TestSeedRepeatabilityAndGlobalRandIsolation() {
    const auto input = Solid(1, 128, {128, 128, 128, 255});
    auto settings = Unfiltered(); settings.noisePercent = 100; settings.noiseSeed = 0;
    const auto first = Take(PostprocessTerrainMinimap(input, settings));
    const auto second = Take(PostprocessTerrainMinimap(input, settings));
    Require(first.preNoise.rgba == second.preNoise.rgba && first.finalImage.rgba == second.finalImage.rgba,
        "显式seed=0也应逐字可重复");
    Require(first.facts.contentRectangle == std::array<std::uint32_t, 4>{127, 0, 2, 256}, "1x128高瘦图可非零等比表示");
    Require(first.facts.noiseSource == TerrainMinimapNoiseSource::Lcg32V1 && first.facts.noiseSeed == 0
        && first.facts.noiseSamplesUsed == 512, "固定LCG-v1标记不能冒充CRT随机流");
    // seed0首次更新1013904223，高15位7735；128*(2*7735/32767)截断为60。
    Require(Pixel(first.finalImage, 127, 255) == Rgba{60, 60, 60, 255}, "seed算法有固定首样本合同");
    Require(Pixel(first.finalImage, 126, 255) == Rgba{0, 0, 0, 255}, "padding不消费随机数也不加噪点");
    std::srand(4321);
    const auto expectedFirst = std::rand(), expectedSecond = std::rand();
    std::srand(4321);
    Require(std::rand() == expectedFirst, "CRT测试自身需有稳定起点");
    const auto isolated = Take(PostprocessTerrainMinimap(input, settings));
    Require(std::rand() == expectedSecond, "组件不能消费或重置全局CRT rand状态");
    Require(isolated.finalImage.rgba == first.finalImage.rgba, "CRT状态不影响显式seed图像");
}

void TestTallNarrowAndAspectFailures() {
    const auto settings = Unfiltered();
    const auto tall = Take(PostprocessTerrainMinimap(Solid(1, 256, {7, 8, 9, 255}), settings));
    Require(tall.facts.contentRectangle == std::array<std::uint32_t, 4>{127, 0, 1, 256}, "1x256内容可表示");
    Require(Pixel(tall.preNoise, 127, 0) == Rgba{7, 8, 9, 255} && Pixel(tall.preNoise, 128, 0) == Rgba{0, 0, 0, 255},
        "奇数横padding保持floor居中");
    Rejected(PostprocessTerrainMinimap(Solid(1, 257, {0, 0, 0, 255}), settings), "TERRAIN_MINIMAP_POSTPROCESS_ASPECT_UNREPRESENTABLE");
    Rejected(PostprocessTerrainMinimap(Solid(257, 1, {0, 0, 0, 255}), settings), "TERRAIN_MINIMAP_POSTPROCESS_ASPECT_UNREPRESENTABLE");
}

void TestFailClosedDimensionsArithmeticAndBudget() {
    const auto settings = Unfiltered();
    Rejected(PostprocessTerrainMinimap({}, settings), "TERRAIN_MINIMAP_POSTPROCESS_DIMENSIONS");
    Rejected(PostprocessTerrainMinimap({1, 0, {}}, settings), "TERRAIN_MINIMAP_POSTPROCESS_DIMENSIONS");
    Rejected(PostprocessTerrainMinimap({0, 1, {}}, settings), "TERRAIN_MINIMAP_POSTPROCESS_DIMENSIONS");
    Rejected(PostprocessTerrainMinimap({1, 1, {}}, settings), "TERRAIN_MINIMAP_POSTPROCESS_RGBA_SIZE");
    Rejected(PostprocessTerrainMinimap({1, 1, std::vector<std::byte>(5)}, settings), "TERRAIN_MINIMAP_POSTPROCESS_RGBA_SIZE");
    Rejected(PostprocessTerrainMinimap({std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(), {}}, settings),
        "TERRAIN_MINIMAP_POSTPROCESS_SIZE_OVERFLOW");
    // 声明128MiB输入本身已耗尽预算，必须在分配/访问正文之前拒绝，不要求真的分配大图。
    Rejected(PostprocessTerrainMinimap({8192, 4096, {}}, settings), "TERRAIN_MINIMAP_POSTPROCESS_BUDGET");
    const auto small = Solid(1, 1, {1, 2, 3, 255});
    Rejected(PostprocessTerrainMinimap(small, settings, {0}), "TERRAIN_MINIMAP_POSTPROCESS_LIMITS");
    Rejected(PostprocessTerrainMinimap(small, settings, {128u * 1024u * 1024u + 1}), "TERRAIN_MINIMAP_POSTPROCESS_LIMITS");
    const auto baseline = Take(PostprocessTerrainMinimap(small, settings));
    Rejected(PostprocessTerrainMinimap(small, settings, {baseline.facts.estimatedPeakBytes - 1}), "TERRAIN_MINIMAP_POSTPROCESS_BUDGET");
    auto retained = small; retained.rgba.reserve(1024u * 1024u);
    Rejected(PostprocessTerrainMinimap(retained, settings, {baseline.facts.estimatedPeakBytes}), "TERRAIN_MINIMAP_POSTPROCESS_BUDGET");
    const auto actual = Take(PostprocessTerrainMinimap(small, settings));
    Require(actual.facts.observedContainerCapacityBytes <= 128u * 1024u * 1024u
        && actual.facts.retainedInputCapacityBytes == small.rgba.capacity(), "真实capacity与固定metadata在128MiB内计费");
}

void TestFilterAndRandomSourceFailures() {
    const auto input = Solid(1, 1, {100, 200, 240, 255});
    auto settings = Unfiltered(); settings.medianRadius = 1;
    Rejected(PostprocessTerrainMinimap(input, settings), "TERRAIN_MINIMAP_POSTPROCESS_FILTER_UNSUPPORTED");
    settings = Unfiltered(); settings.medianRadius = -1;
    Rejected(PostprocessTerrainMinimap(input, settings), "TERRAIN_MINIMAP_POSTPROCESS_FILTER_UNSUPPORTED");
    settings = Unfiltered(); settings.blurRadius = 1;
    Rejected(PostprocessTerrainMinimap(input, settings), "TERRAIN_MINIMAP_POSTPROCESS_FILTER_UNSUPPORTED");
    settings.blurRadius = std::numeric_limits<double>::quiet_NaN();
    Rejected(PostprocessTerrainMinimap(input, settings), "TERRAIN_MINIMAP_POSTPROCESS_FILTER_UNSUPPORTED");
    for (const auto percent : {-1.0, 101.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        settings = Unfiltered(); settings.noisePercent = percent;
        Rejected(PostprocessTerrainMinimap(input, settings), "TERRAIN_MINIMAP_POSTPROCESS_NOISE_INVALID");
    }
    Rejected(PostprocessTerrainMinimap(input), "TERRAIN_MINIMAP_POSTPROCESS_NOISE_SOURCE_REQUIRED");
    settings = Unfiltered(); settings.noisePercent = 10; settings.noiseSamples = std::vector<std::uint16_t>(65536, 0); settings.noiseSeed = 1;
    Rejected(PostprocessTerrainMinimap(input, settings), "TERRAIN_MINIMAP_POSTPROCESS_NOISE_SOURCE_AMBIGUOUS");
    settings.noiseSeed.reset(); settings.noiseSamples->pop_back();
    Rejected(PostprocessTerrainMinimap(input, settings), "TERRAIN_MINIMAP_POSTPROCESS_NOISE_SAMPLE_COUNT");
    settings.noiseSamples->push_back(32768);
    Rejected(PostprocessTerrainMinimap(input, settings), "TERRAIN_MINIMAP_POSTPROCESS_NOISE_SAMPLE_INVALID");
    // 未消费的样本仍要计capacity并验证合同，不能借noise=0藏入无界或非法数据。
    settings.noisePercent = 0;
    Rejected(PostprocessTerrainMinimap(input, settings), "TERRAIN_MINIMAP_POSTPROCESS_NOISE_SAMPLE_INVALID");
    settings.noiseSamples->back() = 0; settings.noiseSamples->reserve(1024u * 1024u);
    const auto baseline = Take(PostprocessTerrainMinimap(input, Unfiltered()));
    Rejected(PostprocessTerrainMinimap(input, settings, {baseline.facts.estimatedPeakBytes}), "TERRAIN_MINIMAP_POSTPROCESS_BUDGET");
}

} // namespace

int main() {
    try {
        TestSmallHandComputedScaleAndOrientation();
        TestBlackPaddingAndOddFlipRectangle();
        TestFixedPointStepRejectsDoubleNegativeControl();
        TestMedianUpperMedianAndAlphaIndependent();
        TestGaussianWeightsAndInBoundsRenormalization();
        TestMedianRunsBeforeGaussian();
        TestCallerU15NoiseAndExactPreNoise();
        TestSeedRepeatabilityAndGlobalRandIsolation();
        TestTallNarrowAndAspectFailures();
        TestFailClosedDimensionsArithmeticAndBudget();
        TestFilterAndRandomSourceFailures();
        std::cout << "terrain minimap postprocess algorithm tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
