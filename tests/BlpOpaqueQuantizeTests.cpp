#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/Image/BlpMipChain.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace War3Map;
using namespace War3Map::Image;

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) throw std::runtime_error("unexpected codec refusal");
    return std::move(*result.value);
}

void Rejected(const Result<BlpOpaqueQuantized>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value, "failure leaked partial output");
    Require(!result.diagnostics.empty() && result.diagnostics.front().code == code, "unexpected refusal code");
}

void Put(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes[offset + shift / 8] = static_cast<std::byte>((value >> shift) & 255u);
}

std::uint32_t U32(const std::vector<std::byte>& bytes, std::size_t offset) {
    Require(offset + 4 <= bytes.size(), "header read overflow");
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[offset + shift / 8])) << shift;
    return value;
}

DecodedImage MakeImage(std::uint32_t width, std::uint32_t height) {
    DecodedImage result{width, height};
    result.rgba.resize(static_cast<std::size_t>(width) * height * 4, std::byte{255});
    return result;
}

void Pixel(DecodedImage& image, std::size_t index, unsigned r, unsigned g, unsigned b, unsigned a = 255) {
    const std::array<unsigned, 4> values{r, g, b, a};
    for (unsigned channel = 0; channel < 4; ++channel)
        image.rgba[index * 4 + channel] = static_cast<std::byte>(values[channel]);
}

void GoldenHeader(std::vector<std::byte>& bytes, unsigned width, unsigned height, bool alpha) {
    bytes[0] = std::byte{'B'}; bytes[1] = std::byte{'L'}; bytes[2] = std::byte{'P'}; bytes[3] = std::byte{'1'};
    Put(bytes, 4, 1); Put(bytes, 8, alpha ? 8u : 0u);
    Put(bytes, 12, width); Put(bytes, 16, height);
    Put(bytes, 20, alpha ? 4u : 5u); Put(bytes, 24, 1);
}

void LegacyIndependentByteGoldens() {
    auto source = MakeImage(2, 2);
    Pixel(source, 0, 255, 0, 0); Pixel(source, 1, 0, 255, 0);
    Pixel(source, 2, 0, 0, 255); Pixel(source, 3, 255, 255, 255);
    // 独立手写 BLP1：RGB 字典序 blue,green,gray128,red,white；平均 mip 为 gray128。
    std::vector<std::byte> golden(1185, std::byte{0});
    GoldenHeader(golden, 2, 2, false);
    Put(golden, 28, 1180); Put(golden, 32, 1184);
    Put(golden, 92, 4); Put(golden, 96, 1);
    const std::array<std::array<unsigned, 3>, 5> bgr{{{255, 0, 0}, {0, 255, 0},
        {128, 128, 128}, {0, 0, 255}, {255, 255, 255}}};
    for (std::size_t index = 0; index < bgr.size(); ++index)
        for (unsigned channel = 0; channel < 3; ++channel)
            golden[156 + index * 4 + channel] = static_cast<std::byte>(bgr[index][channel]);
    const std::array<unsigned, 5> indices{3, 1, 0, 4, 2};
    for (std::size_t index = 0; index < indices.size(); ++index)
        golden[1180 + index] = static_cast<std::byte>(indices[index]);
    Require(Take(EncodeBlpPaletted(source)) == golden, "legacy opaque BLP byte golden changed");
    const auto encoded = Take(EncodeBlpOpaqueQuantized(source));
    Require(encoded.bytes == golden && !encoded.quantized && encoded.method == BlpOpaquePaletteMethod::Exact
        && encoded.paletteColorCount == 5 && encoded.mipCount == 2 && encoded.totalMipPixels == 5,
        "exact new path differs from independent golden/statistics");

    auto transparent = MakeImage(1, 1); Pixel(transparent, 0, 10, 20, 30, 17);
    std::vector<std::byte> alphaGolden(1182, std::byte{0});
    GoldenHeader(alphaGolden, 1, 1, true);
    Put(alphaGolden, 28, 1180); Put(alphaGolden, 92, 2);
    alphaGolden[156] = std::byte{30}; alphaGolden[157] = std::byte{20}; alphaGolden[158] = std::byte{10};
    alphaGolden[1181] = std::byte{17};
    Require(Take(EncodeBlpPaletted(transparent)) == alphaGolden, "legacy alpha BLP byte golden changed");
    Rejected(EncodeBlpOpaqueQuantized(transparent), "BLP_OPAQUE_ALPHA");
}

void CheckAllMipLayout(const BlpOpaqueQuantized& encoded, unsigned width, unsigned height) {
    const auto& bytes = encoded.bytes;
    Require(U32(bytes, 4) == 1 && U32(bytes, 8) == 0 && U32(bytes, 20) == 5, "output is not opaque paletted BLP1");
    std::size_t next = 1180, total = 0;
    unsigned levels = 0;
    for (; levels < 16; ++levels) {
        const auto count = static_cast<std::size_t>(width) * height;
        Require(U32(bytes, 28 + levels * 4) == next && U32(bytes, 92 + levels * 4) == count,
            "mip layout is not contiguous original-sized payload");
        Require(next + count <= bytes.size(), "mip exceeds output");
        for (std::size_t pixel = 0; pixel < count; ++pixel)
            Require(std::to_integer<unsigned>(bytes[next + pixel]) < encoded.paletteColorCount, "invalid shared palette index");
        next += count; total += count;
        if (width == 1 && height == 1) { ++levels; break; }
        width = std::max(1u, width / 2); height = std::max(1u, height / 2);
    }
    Require(next == bytes.size() && total == encoded.totalMipPixels && levels == encoded.mipCount, "mip statistics/layout mismatch");
    for (unsigned level = levels; level < 16; ++level)
        Require(U32(bytes, 28 + level * 4) == 0 && U32(bytes, 92 + level * 4) == 0, "unused mip declaration not zero");
    const auto chain = Take(DecodeBlpMipChain(bytes));
    Require(chain.validatedMipCount == levels && chain.additionalMips.size() + 1 == levels, "not every encoded mip decoded");
}

void HandComputedFullChainPalette() {
    auto source = MakeImage(257, 1);
    for (unsigned index = 0; index < 257; ++index)
        Pixel(source, index, index % 8, (index / 8) % 8, index / 64);
    Require(!EncodeBlpPaletted(source).Succeeded(), "legacy >256-color lossless refusal removed");
    const auto before = source.rgba;
    const auto encoded = Take(EncodeBlpOpaqueQuantized(source));
    // 257 不同原色全部在一个 5bit bucket；未量化的 9 层共512像素。
    // 独立手算全链 RGB 加权和为(1916,1804,772)，四舍五入得到(4,4,2)。
    // 仅统计 mip0 会得到(3,3,2)，故该例能区分完整原链和 top-only 量化。
    Require(encoded.quantized && encoded.method == BlpOpaquePaletteMethod::WeightedMedianCut5Bit
        && encoded.paletteColorCount == 1 && encoded.occupiedHistogramBins == 1
        && encoded.mipCount == 9 && encoded.totalMipPixels == 512, "hand example palette statistics wrong");
    Require(encoded.bytes[156] == std::byte{2} && encoded.bytes[157] == std::byte{4}
        && encoded.bytes[158] == std::byte{4}, "full-chain hand-computed palette mean wrong");
    for (std::size_t offset = 1180; offset < encoded.bytes.size(); ++offset)
        Require(encoded.bytes[offset] == std::byte{0}, "single-color full-chain payload should use index zero");
    Require(source.rgba == before, "quantizer mutated borrowed source pixels");
    CheckAllMipLayout(encoded, 257, 1);
}

void GradientExercisesSplitAndDeterminism() {
    auto source = MakeImage(64, 64);
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x)
        Pixel(source, y * 64 + x, x * 4, y * 4, (x + y) * 2);
    const auto encoded = Take(EncodeBlpOpaqueQuantized(source));
    const auto again = Take(EncodeBlpOpaqueQuantized(source));
    Require(encoded.bytes == again.bytes && encoded.paletteColorCount == again.paletteColorCount
        && encoded.occupiedHistogramBins == again.occupiedHistogramBins, "weighted split/tie order is not deterministic");
    Require(encoded.quantized && encoded.paletteColorCount > 64 && encoded.paletteColorCount <= 256
        && encoded.occupiedHistogramBins > 256 && encoded.mipCount == 7
        && encoded.totalMipPixels == 5461, "gradient did not exercise bounded multi-box quantization");
    const auto top = Take(DecodeBlp(encoded.bytes));
    std::uint64_t absoluteError = 0;
    for (std::size_t pixel = 0; pixel < 4096; ++pixel) {
        Require(top.rgba[pixel * 4 + 3] == std::byte{255}, "opaque output lost alpha");
        for (unsigned channel = 0; channel < 3; ++channel) {
            const auto a = std::to_integer<int>(top.rgba[pixel * 4 + channel]);
            const auto b = std::to_integer<int>(source.rgba[pixel * 4 + channel]);
            absoluteError += static_cast<std::uint64_t>(a > b ? a - b : b - a);
        }
    }
    // 独立质量下限：渐变平均每通道误差必须低于24，排除合法但退化为单色的容器。
    Require(absoluteError < 4096u * 3u * 24u, "gradient quantization unexpectedly collapsed image detail");
    CheckAllMipLayout(encoded, 64, 64);
}

void DownsampleColorsAlsoSelectTheQuantizer() {
    auto source = MakeImage(16, 16);
    // mip0 恰好256个颜色；首个平均层的(8+32x,8+32y,0)不在原来的16步长网格中。
    for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x)
        Pixel(source, y * 16 + x, x * 16, y * 16, 0);
    const auto lossless = EncodeBlpPaletted(source);
    Require(!lossless.Succeeded() && !lossless.diagnostics.empty()
        && lossless.diagnostics.front().code == "encode-palette", "legacy encoder stopped counting derived mip colors");
    const auto encoded = Take(EncodeBlpOpaqueQuantized(source));
    Require(encoded.method == BlpOpaquePaletteMethod::WeightedMedianCut5Bit && encoded.quantized
        && encoded.occupiedHistogramBins >= 320 && encoded.totalMipPixels == 341,
        "top-only color count bypassed full original mip palette selection");
    CheckAllMipLayout(encoded, 16, 16);
}

void OddSizeAndBudgetRefusals() {
    auto odd = MakeImage(3, 1);
    Pixel(odd, 0, 0, 0, 0); Pixel(odd, 1, 4, 8, 12); Pixel(odd, 2, 255, 255, 255);
    const auto encoded = Take(EncodeBlpOpaqueQuantized(odd));
    CheckAllMipLayout(encoded, 3, 1);
    const auto chain = Take(DecodeBlpMipChain(encoded.bytes));
    Require(chain.additionalMips.size() == 1 && chain.additionalMips[0].rgba
        == std::vector<std::byte>{std::byte{2}, std::byte{4}, std::byte{6}, std::byte{255}},
        "odd-size legacy floor-halving/2x2 average changed");
    auto square = MakeImage(2, 2);
    BlpOpaqueQuantizeLimits limits;
    limits.maxDimension = 513; Rejected(EncodeBlpOpaqueQuantized(square, limits), "BLP_OPAQUE_LIMITS");
    limits = {}; limits.maxPixels++; Rejected(EncodeBlpOpaqueQuantized(square, limits), "BLP_OPAQUE_LIMITS");
    limits = {}; limits.maxTotalMipPixels++; Rejected(EncodeBlpOpaqueQuantized(square, limits), "BLP_OPAQUE_LIMITS");
    limits = {}; limits.maxWorkingBytes++; Rejected(EncodeBlpOpaqueQuantized(square, limits), "BLP_OPAQUE_LIMITS");
    limits = {}; limits.maxOutputBytes++; Rejected(EncodeBlpOpaqueQuantized(square, limits), "BLP_OPAQUE_LIMITS");
    limits = {}; limits.maxDimension = 1; Rejected(EncodeBlpOpaqueQuantized(square, limits), "BLP_OPAQUE_DIMENSIONS");
    limits = {}; limits.maxPixels = 3; Rejected(EncodeBlpOpaqueQuantized(square, limits), "BLP_OPAQUE_PIXELS");
    limits = {}; limits.maxTotalMipPixels = 4; Rejected(EncodeBlpOpaqueQuantized(square, limits), "BLP_OPAQUE_MIP_BUDGET");
    limits = {}; limits.maxOutputBytes = 1184; Rejected(EncodeBlpOpaqueQuantized(square, limits), "BLP_OPAQUE_OUTPUT_BUDGET");
    limits = {}; limits.maxWorkingBytes = 0; Rejected(EncodeBlpOpaqueQuantized(square, limits), "BLP_OPAQUE_WORKING_BUDGET");
    limits = {}; limits.maxDimension = 2; limits.maxPixels = 4; limits.maxTotalMipPixels = 5; limits.maxOutputBytes = 1185;
    Require(Take(EncodeBlpOpaqueQuantized(square, limits)).totalMipPixels == 5, "honest narrowed budgets refused");
    DecodedImage huge{std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max()};
    Rejected(EncodeBlpOpaqueQuantized(huge), "BLP_OPAQUE_DIMENSIONS");
    Rejected(EncodeBlpOpaqueQuantized(DecodedImage{}), "BLP_OPAQUE_DIMENSIONS");
    square.rgba.pop_back(); Rejected(EncodeBlpOpaqueQuantized(square), "BLP_OPAQUE_PIXELS");
    auto maximum = MakeImage(512, 512);
    const auto maxEncoded = Take(EncodeBlpOpaqueQuantized(maximum));
    Require(maxEncoded.totalMipPixels == 349525 && maxEncoded.mipCount == 10, "hard boundary complete mip accounting wrong");
    CheckAllMipLayout(maxEncoded, 512, 512);
}
}

int main() {
    try {
        LegacyIndependentByteGoldens();
        HandComputedFullChainPalette();
        GradientExercisesSplitAndDeterminism();
        DownsampleColorsAlsoSelectTheQuantizer();
        OddSizeAndBudgetRefusals();
        std::cout << "blp-opaque-quantize: original contract regressions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "blp-opaque-quantize: " << error.what() << '\n';
        return 1;
    }
}
