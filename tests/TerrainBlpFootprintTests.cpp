#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/Image/BlpJpegEncode.h"
#include "War3MapCore/MapData/TerrainDoodadFootprint.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message;
        for (const auto& diagnostic : result.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
        throw std::runtime_error(message.empty() ? "missing result" : message);
    }
    return std::move(*result.value);
}
void U32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes[at + index] = static_cast<std::byte>((value >> (8 * index)) & 255);
}
Bytes Palette(unsigned alphaBits = 8) {
    Bytes bytes(1180 + 6 + (6 * alphaBits + 7) / 8);
    std::copy_n(reinterpret_cast<const std::byte*>("BLP1"), 4, bytes.begin());
    U32(bytes, 4, 1); U32(bytes, 8, alphaBits); U32(bytes, 12, 3); U32(bytes, 16, 2);
    U32(bytes, 28, 1180); U32(bytes, 92, static_cast<std::uint32_t>(bytes.size() - 1180));
    // 文件的首行是黑、蓝、绿；次行是红、蓝绿、蓝红。调色板本身的alpha不参与。
    constexpr std::array<unsigned, 6> colors{0, 0x000001, 0x000100, 0x010000, 0x000101, 0x010001};
    for (unsigned index = 0; index < colors.size(); ++index) {
        U32(bytes, 156 + 4 * index, colors[index] | 0x7f000000u);
        bytes[1180 + index] = static_cast<std::byte>(index);
    }
    if (alphaBits == 8) for (unsigned index = 0; index < 6; ++index) bytes[1186 + index] = static_cast<std::byte>(index * 17);
    if (alphaBits == 4) { bytes[1186] = std::byte{0x10}; bytes[1187] = std::byte{0x32}; bytes[1188] = std::byte{0x54}; }
    if (alphaBits == 1) bytes[1186] = std::byte{0x2a};
    return bytes;
}
void PaletteProjectionAndTightBudget() {
    for (unsigned bits : {0u, 1u, 4u, 8u}) {
        const auto bytes = Palette(bits), original = bytes;
        Image::BlpDecodeLimits limits; limits.maxDimension = 256; limits.maxWorkingBytes = bytes.size() + 24;
        const auto decoded = Take(Image::DecodeBlpBudgeted(bytes, limits));
        Require(decoded.workingBytes == bytes.size() + 24 && decoded.image.Valid(), "palette charges input and output before allocation");
        for (unsigned index = 0; index < 6; ++index) {
            const auto alpha = bits == 0 ? 255u : bits == 1 ? (index % 2 ? 255u : 0u) : index * 17;
            Require(std::to_integer<unsigned>(decoded.image.rgba[index * 4 + 3]) == alpha, "packed alpha has an independent literal expectation");
        }
        auto owned = Take(DecodeDoodadFootprintBlpBudgeted(bytes, 24));
        const auto address = owned.footprint.pixels.data();
        auto image = std::move(owned);
        Require(image.footprint.pixels.data() == address && image.decodedBytes == 24
            && image.footprint.topOrigin && !image.footprint.uniform, "moving BLP owner retains the exact borrowed pixels");
        TerrainHeader terrain; terrain.width = terrain.height = 5;
        DoodadPlacement object; object.position = {128,128,0}; object.scale = {1,1,1};
        std::vector<std::uint8_t> actual(256, 0x40), expected(256, 0x40);
        const auto status = AddDoodadFootprint(actual, terrain, object, image.footprint, 2, -1.0f);
        Require(status.Succeeded(), "BLP uses the same native rectangular projector");
        // 原版零角度3×2图片投影为2×3；不能用转换后的RGBA或alpha推导期望。
        constexpr std::array<std::uint8_t,6> pixels{0,0x31,4,6,2,0x35};
        for (std::size_t index = 0; index < pixels.size(); ++index)
            expected[(3 + index / 2) * 16 + 4 + index % 2] |= pixels[index];
        Require(actual == expected && bytes == original, "BLP row order and channels match literal native pathing including transparent RGB");
        --limits.maxWorkingBytes;
        Require(!Image::DecodeBlpBudgeted(bytes, limits).Succeeded()
            && !DecodeDoodadFootprintBlpBudgeted(bytes, 23).Succeeded(), "one byte less refuses without a partial image");
    }
}
void StrictBoundsAndUniformity() {
    const auto source = Palette();
    const auto refuse = [&](Bytes bytes) { Require(!DecodeDoodadFootprintBlpBudgeted(bytes, 4u * 1024u * 1024u).Succeeded(), "malformed BLP refuses"); };
    auto bytes = source; U32(bytes, 92, 1); refuse(bytes); // file still has pixels; declared mip does not.
    bytes = source; U32(bytes, 28, 156); refuse(bytes);
    bytes = source; U32(bytes, 8, 2); refuse(bytes);
    bytes = source; U32(bytes, 12, 257); refuse(bytes);
    bytes = source; bytes.pop_back(); refuse(bytes);
    bytes = source; std::fill(bytes.begin() + 1180, bytes.begin() + 1186, std::byte{1});
    Require(!Take(DecodeDoodadFootprintBlpBudgeted(bytes, 24)).footprint.uniform, "different alpha prevents the native full-pixel uniform fast path");
    std::fill(bytes.begin() + 1186, bytes.end(), std::byte{});
    Require(Take(DecodeDoodadFootprintBlpBudgeted(bytes, 24)).footprint.uniform, "equal complete BGRA is uniform, even transparent blue");
    Image::BlpDecodeLimits limits; limits.maxWorkingBytes += 1;
    Require(!Image::DecodeBlpBudgeted(source, limits).Succeeded(), "caller cannot widen the hard decoder budget");
}
void JpegUsesMeasuredPeakAndDeclaredDimensions() {
    Image::DecodedImage input; input.width = input.height = 256;
    input.rgba.resize(256 * 256 * 4, std::byte{255});
    for (std::size_t at = 0; at < input.rgba.size(); at += 4) {
        input.rgba[at] = std::byte{32}; input.rgba[at + 1] = std::byte{64}; input.rgba[at + 2] = std::byte{96};
    }
    const auto encoded = Take(Image::EncodeBlpJpegSingleMip(input)).bytes;
    Image::BlpDecodeLimits limits; limits.maxDimension = 256;
    const auto generous = Take(Image::DecodeBlpBudgeted(encoded, limits));
    // 256²、四分量1×1采样：原BLP + 去160字节包装后的JPEG + 64KiB固定预留
    // + 4×65536×2系数 + 4×65536组件像素 + 65536单平面。RGBA阶段峰值较小。
    const auto expectedPeak = encoded.size() + (encoded.size() - 160) + 917504;
    Require(generous.workingBytes == expectedPeak, "JPEG peak matches independently counted component geometry");
    limits.maxWorkingBytes = expectedPeak;
    const auto exact = Take(Image::DecodeBlpBudgeted(encoded, limits));
    Require(exact.image.rgba == generous.image.rgba && exact.workingBytes > encoded.size() + exact.image.rgba.size(),
        "JPEG budget includes coefficients and planes, not only output pixels");
    const auto extra = exact.workingBytes - encoded.size();
    const auto footprint = Take(DecodeDoodadFootprintBlpBudgeted(encoded, extra));
    Require(footprint.decodedBytes == extra && footprint.footprint.pixels[0] == exact.image.rgba[2]
        && footprint.footprint.pixels[2] == exact.image.rgba[0], "JPEG footprint converts RGBA in place and charges the measured peak");
    --limits.maxWorkingBytes;
    Require(!Image::DecodeBlpBudgeted(encoded, limits).Succeeded(), "JPEG peak minus one refuses before coefficient allocation");
    auto mismatched = encoded; U32(mismatched, 12, 255);
    Require(!DecodeDoodadFootprintBlpBudgeted(mismatched, extra).Succeeded(), "JPEG dimensions cannot override the footprint declaration");
}
} // namespace
int main() {
    try {
        PaletteProjectionAndTightBudget(); StrictBoundsAndUniformity(); JpegUsesMeasuredPeakAndDeclaredDimensions();
        std::cout << "Budgeted native BLP footprints passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
