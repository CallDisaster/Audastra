#include "War3MapCore/Image/BlpJpegEncode.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Image;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::uint32_t U32(const std::vector<std::byte>& bytes, std::size_t offset) {
    return std::to_integer<std::uint32_t>(bytes[offset])
        | (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8)
        | (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16)
        | (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24);
}
std::uint8_t U8(const std::vector<std::byte>& bytes, std::size_t offset) { return std::to_integer<std::uint8_t>(bytes[offset]); }

DecodedImage Gradient() {
    DecodedImage image{256, 256, std::vector<std::byte>(256u * 256u * 4u, std::byte{255})};
    for (std::uint32_t y = 0; y < image.height; ++y) for (std::uint32_t x = 0; x < image.width; ++x) {
        const auto pixel = (static_cast<std::size_t>(y) * image.width + x) * 4;
        image.rgba[pixel + 0] = static_cast<std::byte>(x); // R
        image.rgba[pixel + 1] = static_cast<std::byte>(y); // G
        image.rgba[pixel + 2] = static_cast<std::byte>(x ^ y); // B
        image.rgba[pixel + 3] = std::byte{255};
    }
    return image;
}

void TestContainerAndMarkers() {
    const auto encoded = EncodeBlpJpegSingleMip(Gradient());
    Require(encoded.Succeeded() && encoded.value, "BLP JPEG encoding failed");
    const auto& bytes = encoded.value->bytes;
    Require(bytes.size() == 160 + encoded.value->jpegHeaderBytes + encoded.value->jpegMipBytes, "length accounting mismatch");
    Require(bytes.size() <= 1024u * 1024u && U32(bytes, 4) == 0 && U32(bytes, 8) == 0
        && U32(bytes, 12) == 256 && U32(bytes, 16) == 256 && U32(bytes, 20) == 3 && U32(bytes, 24) == 0,
        "BLP fixed header contract mismatch");
    Require(U32(bytes, 156) == encoded.value->jpegHeaderBytes && U32(bytes, 28) == 160 + encoded.value->jpegHeaderBytes
        && U32(bytes, 92) == encoded.value->jpegMipBytes && U32(bytes, 28) + U32(bytes, 92) == bytes.size(),
        "BLP shared header and single mip offsets mismatch");
    for (std::size_t level = 1; level < 16; ++level)
        Require(U32(bytes, 28 + level * 4) == 0 && U32(bytes, 92 + level * 4) == 0, "later mip declaration is nonzero");
    const auto shared = static_cast<std::size_t>(160);
    Require(bytes[shared] == std::byte{0xff} && bytes[shared + 1] == std::byte{0xd8}, "shared JPEG header lacks SOI");
    Require(bytes[shared + 2] == std::byte{0xff} && bytes[shared + 3] == std::byte{0xdb}
        && U8(bytes, shared + 4) == 0 && U8(bytes, shared + 5) == 67 && U8(bytes, shared + 6) == 0
        && U8(bytes, shared + 7) == 20, "quality40 DQT contract mismatch");
    constexpr std::array<std::uint8_t, 64> base{
        16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
        14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
        18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
        49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
    constexpr std::array<int, 64> zig{
        0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42,
        3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
        10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
        21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};
    for (std::size_t natural = 0; natural < 64; ++natural) {
        const auto scaled = static_cast<std::uint8_t>(std::clamp((static_cast<int>(base[natural]) * 125 + 50) / 100, 1, 255));
        Require(U8(bytes, shared + 7 + zig[natural]) == scaled, "DQT full quality40 table mismatch");
    }
    const auto payload = static_cast<std::size_t>(U32(bytes, 28));
    Require(bytes[payload] == std::byte{0xff} && bytes[payload + 1] == std::byte{0xc0}
        && U8(bytes, payload + 4) == 8 && U8(bytes, payload + 9) == 4,
        "mip0 SOF0 component contract mismatch");
    for (unsigned component = 0; component < 4; ++component)
        Require(U8(bytes, payload + 10 + component * 3) == component + 1
            && U8(bytes, payload + 11 + component * 3) == 0x11 && U8(bytes, payload + 12 + component * 3) == 0,
            "SOF0 BGRA component or sampling order mismatch");
    const auto sofLength = static_cast<std::size_t>((U8(bytes, payload + 2) << 8) | U8(bytes, payload + 3));
    Require(bytes[payload + 2 + sofLength] == std::byte{0xff} && bytes[payload + 3 + sofLength] == std::byte{0xda},
        "SOF0 is not followed by SOS");
    Require(bytes.back() == std::byte{0xd9} && bytes[bytes.size() - 2] == std::byte{0xff}, "mip0 lacks final EOI");
    const auto decoded = DecodeBlp(bytes);
    Require(decoded.Succeeded() && decoded.value->Valid() && decoded.value->width == 256 && decoded.value->height == 256,
        "encoded BLP did not pass the existing decoder");
    for (std::size_t pixel = 3; pixel < decoded.value->rgba.size(); pixel += 4)
        Require(decoded.value->rgba[pixel] == std::byte{255}, "alphaBits=0 did not decode opaque");
}

void TestFailClosedInputs() {
    auto image = Gradient();
    image.rgba[3] = std::byte{254};
    Require(!EncodeBlpJpegSingleMip(image).Succeeded(), "nonopaque input was accepted");
    auto limits = BlpJpegEncodeLimits{}; limits.quality = 41;
    Require(!EncodeBlpJpegSingleMip(Gradient(), limits).Succeeded(), "non-q40 input was accepted");
    image = Gradient(); image.width = 128;
    Require(!EncodeBlpJpegSingleMip(image).Succeeded(), "non-256 input was accepted");
}

void TestRawBgraColorAndCapacityLimits() {
    auto source = Gradient();
    constexpr std::array<std::uint8_t, 4> expected{9, 100, 231, 255};
    for (std::size_t pixel = 0; pixel < source.rgba.size() / 4; ++pixel)
        for (std::size_t channel = 0; channel < 4; ++channel)
            source.rgba[pixel * 4 + channel] = static_cast<std::byte>(expected[channel]);
    const auto encoded = EncodeBlpJpegSingleMip(source);
    Require(encoded.Succeeded(), "constant asymmetric RGB fixture must encode");
    const auto decoded = DecodeBlp(encoded.value->bytes);
    Require(decoded.Succeeded(), "constant fixture must decode");
    // Qdc=20，常色的重建误差界为20/(2*8)+半字节舍入，最多2 code；
    // 独立R/B值同时拒绝交换分量及错误YCbCr/CMYK变换。
    for (std::size_t pixel = 0; pixel < source.rgba.size() / 4; ++pixel) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto actual = static_cast<int>(std::to_integer<std::uint8_t>(decoded.value->rgba[pixel * 4 + channel]));
            const auto error = actual - static_cast<int>(expected[channel]);
            Require(error >= -2 && error <= 2, "raw BGRA DC reconstruction exceeded analytic q40 constant-color bound");
        }
        Require(decoded.value->rgba[pixel * 4 + 3] == std::byte{255}, "opaque constant alpha");
    }
    auto limits = BlpJpegEncodeLimits{}; limits.maxOutputBytes = 8;
    auto refused = EncodeBlpJpegSingleMip(source, limits);
    Require(!refused.Succeeded() && !refused.value && refused.diagnostics.front().code == "BLP_JPEG_OUTPUT_BUDGET",
        "small output cap must reject without publishing a partial stream");
    limits = {}; limits.maxWorkingBytes = 4u * 1024u * 1024u;
    Require(EncodeBlpJpegSingleMip(source, limits).Succeeded(), "normal retained input fits the fixed working cap");
    source.rgba.reserve(2u * 1024u * 1024u);
    refused = EncodeBlpJpegSingleMip(source, limits);
    Require(!refused.Succeeded() && !refused.value && refused.diagnostics.front().code == "BLP_JPEG_WORKING_BUDGET",
        "input capacity rather than just pixel size must count toward peak");
}

} // namespace

int main() {
    try {
        TestContainerAndMarkers(); TestFailClosedInputs(); TestRawBgraColorAndCapacityLimits();
        std::cout << "BlpJpegEncode: container, decoder and fail-closed cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BlpJpegEncode: " << error.what() << '\n'; return 1;
    }
}
