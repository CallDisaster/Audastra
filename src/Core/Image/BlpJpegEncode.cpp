#include "War3MapCore/Image/BlpJpegEncode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>

namespace War3Map::Image {
namespace {

constexpr std::size_t kBlpHeaderBytes = 156;
constexpr std::size_t kJpegHeaderPrefixBytes = 4;

struct CapacityFailure {};

constexpr std::array<int, 64> kZigZag{
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42,
    3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};

constexpr std::array<std::uint8_t, 64> kLumaQuant{
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99};

constexpr std::array<std::uint8_t, 17> kDcBits{0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 12> kDcValues{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr std::array<std::uint8_t, 17> kAcBits{0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
constexpr std::array<std::uint8_t, 162> kAcValues{
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
    0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
    0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
    0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

struct HuffCode { std::uint16_t code = 0; std::uint8_t bits = 0; };

std::array<HuffCode, 256> MakeHuffman(const std::array<std::uint8_t, 17>& counts,
    const auto& values) {
    std::array<HuffCode, 256> result{};
    std::uint16_t code = 0;
    std::size_t value = 0;
    for (std::uint8_t bits = 1; bits <= 16; ++bits) {
        for (std::uint8_t index = 0; index < counts[bits]; ++index) {
            result[values[value++]] = {code, bits};
            ++code;
        }
        code = static_cast<std::uint16_t>(code << 1);
    }
    return result;
}

class Bytes {
public:
    void Limit(std::size_t limit) noexcept { limit_ = limit; }
    void ReserveLimit() {
        bytes_.reserve(limit_);
        if (bytes_.capacity() > limit_) throw CapacityFailure{};
    }
    void U8(std::uint8_t value) { Ensure(1); bytes_.push_back(static_cast<std::byte>(value)); }
    void U16(std::uint16_t value) { U8(static_cast<std::uint8_t>(value >> 8)); U8(static_cast<std::uint8_t>(value)); }
    void Marker(std::uint8_t marker) { U8(0xff); U8(marker); }
    void Segment(std::uint8_t marker, std::span<const std::byte> data) {
        Ensure(data.size() + 4); Marker(marker); U16(static_cast<std::uint16_t>(data.size() + 2));
        bytes_.insert(bytes_.end(), data.begin(), data.end());
    }
    std::vector<std::byte> Take() { return std::move(bytes_); }
    std::size_t Size() const noexcept { return bytes_.size(); }
private:
    void Ensure(std::size_t amount) const { if (amount > limit_ || bytes_.size() > limit_ - amount) throw CapacityFailure{}; }
    std::vector<std::byte> bytes_;
    std::size_t limit_ = std::numeric_limits<std::size_t>::max();
};

class EntropyWriter {
public:
    explicit EntropyWriter(Bytes& out) : out_(out) {}
    void Bits(std::uint16_t value, std::uint8_t count) {
        buffer_ = (buffer_ << count) | value;
        bits_ = static_cast<std::uint8_t>(bits_ + count);
        while (bits_ >= 8) {
            const auto byte = static_cast<std::uint8_t>((buffer_ >> (bits_ - 8)) & 0xffu);
            out_.U8(byte); if (byte == 0xff) out_.U8(0);
            bits_ = static_cast<std::uint8_t>(bits_ - 8);
        }
    }
    void Code(HuffCode code) { Bits(code.code, code.bits); }
    void Finish() { if (bits_) Bits(static_cast<std::uint16_t>((1u << (8 - bits_)) - 1), static_cast<std::uint8_t>(8 - bits_)); }
private:
    Bytes& out_; std::uint32_t buffer_ = 0; std::uint8_t bits_ = 0;
};

void PutU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8) bytes[offset + shift / 8] = static_cast<std::byte>(value >> shift);
}

const std::array<std::array<double, 8>, 8>& CosTable() {
    static const auto table = [] {
        std::array<std::array<double, 8>, 8> result{};
        constexpr double pi = 3.14159265358979323846;
        for (std::size_t frequency = 0; frequency < 8; ++frequency)
            for (std::size_t sample = 0; sample < 8; ++sample)
                result[frequency][sample] = std::cos((2 * sample + 1) * frequency * pi / 16.0);
        return result;
    }();
    return table;
}

std::uint8_t QualityValue(std::uint8_t base, std::uint8_t quality) {
    const int scale = quality < 50 ? 5000 / std::max<int>(1, quality) : 200 - quality * 2;
    return static_cast<std::uint8_t>(std::clamp((static_cast<int>(base) * scale + 50) / 100, 1, 255));
}

std::array<std::uint8_t, 64> Quant(std::uint8_t quality) {
    std::array<std::uint8_t, 64> table{};
    for (std::size_t index = 0; index < table.size(); ++index) table[index] = QualityValue(kLumaQuant[index], quality);
    return table;
}

int Category(int value) {
    int absolute = std::abs(value), bits = 0; while (absolute) { absolute >>= 1; ++bits; } return bits;
}

std::uint16_t MagnitudeBits(int value, int bits) {
    if (value >= 0) return static_cast<std::uint16_t>(value);
    return static_cast<std::uint16_t>(value - 1 + (1 << bits));
}

void EncodeValue(EntropyWriter& writer, const std::array<HuffCode, 256>& dc,
    const std::array<HuffCode, 256>& ac, int& previous, const std::array<int, 64>& block) {
    const int delta = block[0] - previous; previous = block[0];
    const int dcBits = Category(delta); writer.Code(dc[dcBits]);
    if (dcBits) writer.Bits(MagnitudeBits(delta, dcBits), static_cast<std::uint8_t>(dcBits));
    int zeroes = 0;
    for (int index = 1; index < 64; ++index) {
        if (!block[index]) { ++zeroes; continue; }
        while (zeroes >= 16) { writer.Code(ac[0xf0]); zeroes -= 16; }
        const int bits = Category(block[index]);
        writer.Code(ac[(zeroes << 4) | bits]); writer.Bits(MagnitudeBits(block[index], bits), static_cast<std::uint8_t>(bits));
        zeroes = 0;
    }
    if (zeroes) writer.Code(ac[0]);
}

std::array<int, 64> DctBlock(const DecodedImage& image, std::uint32_t bx, std::uint32_t by,
    std::uint32_t channel, const std::array<std::uint8_t, 64>& quant) {
    std::array<int, 64> result{};
    const auto& cosine = CosTable();
    for (std::uint32_t v = 0; v < 8; ++v) for (std::uint32_t u = 0; u < 8; ++u) {
        double sum = 0;
        for (std::uint32_t y = 0; y < 8; ++y) for (std::uint32_t x = 0; x < 8; ++x) {
            const auto sx = std::min<std::uint32_t>(image.width - 1, bx * 8 + x);
            const auto sy = std::min<std::uint32_t>(image.height - 1, by * 8 + y);
            const auto sourceChannel = std::array<std::uint32_t, 4>{2, 1, 0, 3}[channel];
            const auto value = std::to_integer<std::uint8_t>(image.rgba[(static_cast<std::size_t>(sy) * image.width + sx) * 4 + sourceChannel]);
            sum += (static_cast<double>(value) - 128.0) * cosine[u][x] * cosine[v][y];
        }
        const double cu = u ? 1.0 : 1.0 / std::sqrt(2.0), cv = v ? 1.0 : 1.0 / std::sqrt(2.0);
        const double coefficient = 0.25 * cu * cv * sum / quant[v * 8 + u];
        result[kZigZag[v * 8 + u]] = static_cast<int>(coefficient >= 0 ? std::floor(coefficient + 0.5) : std::ceil(coefficient - 0.5));
    }
    return result;
}

void EmitDqt(Bytes& bytes, const std::array<std::uint8_t, 64>& quant) {
    std::array<std::byte, 65> segment{}; segment[0] = std::byte{0};
    for (int natural = 0; natural < 64; ++natural)
        segment[kZigZag[natural] + 1] = static_cast<std::byte>(quant[natural]);
    bytes.Segment(0xdb, segment);
}

void EmitDht(Bytes& bytes) {
    std::vector<std::byte> segment; segment.reserve(210);
    const auto append = [&](std::uint8_t tc, const auto& counts, const auto& values) {
        segment.push_back(static_cast<std::byte>(tc));
        for (std::uint8_t i = 1; i <= 16; ++i) segment.push_back(static_cast<std::byte>(counts[i]));
        for (const auto value : values) segment.push_back(static_cast<std::byte>(value));
    };
    append(0x00, kDcBits, kDcValues); append(0x10, kAcBits, kAcValues); bytes.Segment(0xc4, segment);
}

} // namespace

Result<BlpJpegSingleMip> EncodeBlpJpegSingleMip(const DecodedImage& image, const BlpJpegEncodeLimits& limits) {
    using Output = Result<BlpJpegSingleMip>;
    const auto fail = [](const char* code, const char* message) { return Output::Failure("Image.BlpJpeg", code, message); };
    const BlpJpegEncodeLimits hard;
    if (limits.maxDimension > hard.maxDimension || limits.maxPixels > hard.maxPixels
        || limits.maxWorkingBytes > hard.maxWorkingBytes || limits.maxOutputBytes > hard.maxOutputBytes
        || limits.quality != 40) return fail("BLP_JPEG_LIMITS", "BLP JPEG 限额只能收紧，质量固定为40。");
    if (image.width != 256 || image.height != 256 || image.width > limits.maxDimension || image.height > limits.maxDimension)
        return fail("BLP_JPEG_DIMENSIONS", "原版小地图 JPEG 入口只接受256×256。");
    const auto pixels = static_cast<std::size_t>(image.width) * image.height;
    if (pixels > limits.maxPixels || image.rgba.size() != pixels * 4) return fail("BLP_JPEG_PIXELS", "RGBA载荷不完整或超过预算。");
    for (std::size_t pixel = 0; pixel < pixels; ++pixel)
        if (image.rgba[pixel * 4 + 3] != std::byte{255}) return fail("BLP_JPEG_ALPHA", "BLP JPEG quality40入口只接受opaque图像。");
    constexpr std::size_t blocksPerAxis = 32;
    const auto quant = Quant(40); const auto dc = MakeHuffman(kDcBits, kDcValues); const auto ac = MakeHuffman(kAcBits, kAcValues);
    const auto outputPeak = limits.maxOutputBytes > std::numeric_limits<std::size_t>::max() / 3
        ? std::numeric_limits<std::size_t>::max() : limits.maxOutputBytes * 3;
    const auto extraPeak = outputPeak > std::numeric_limits<std::size_t>::max() - 256 * 1024
        ? std::numeric_limits<std::size_t>::max() : outputPeak + 256 * 1024;
    const auto estimatedWorking = image.rgba.capacity() > std::numeric_limits<std::size_t>::max() - extraPeak
        ? std::numeric_limits<std::size_t>::max() : image.rgba.capacity() + extraPeak;
    if (estimatedWorking > limits.maxWorkingBytes) return fail("BLP_JPEG_WORKING_BUDGET", "JPEG编码工作预算不足。");
    try {
        Bytes header; header.Limit(std::min<std::size_t>(limits.maxOutputBytes, 64u * 1024u)); header.ReserveLimit();
        header.Marker(0xd8); EmitDqt(header, quant); EmitDht(header);
        const auto sharedHeader = header.Take();
        Bytes frame;
        const auto frameBudget = limits.maxOutputBytes > kBlpHeaderBytes + kJpegHeaderPrefixBytes + sharedHeader.size()
            ? limits.maxOutputBytes - kBlpHeaderBytes - kJpegHeaderPrefixBytes - sharedHeader.size() : 0;
        frame.Limit(frameBudget);
        frame.ReserveLimit();
        std::array<std::byte, 18> sof{}; sof[0] = std::byte{8}; sof[1] = std::byte{1}; sof[2] = std::byte{0}; sof[3] = std::byte{1}; sof[4] = std::byte{0}; sof[5] = std::byte{4};
        for (int i = 0; i < 4; ++i) { sof[6 + i * 3] = static_cast<std::byte>(i + 1); sof[7 + i * 3] = std::byte{0x11}; sof[8 + i * 3] = std::byte{0}; }
        frame.Segment(0xc0, std::span<const std::byte>(sof));
        std::array<std::byte, 12> sos{}; sos[0] = std::byte{4};
        for (int i = 0; i < 4; ++i) { sos[1 + i * 2] = static_cast<std::byte>(i + 1); sos[2 + i * 2] = std::byte{0}; }
        sos[9] = std::byte{0}; sos[10] = std::byte{63}; sos[11] = std::byte{0}; frame.Segment(0xda, sos);
        EntropyWriter writer(frame); std::array<int, 4> previous{};
        for (std::uint32_t by = 0; by < blocksPerAxis; ++by) for (std::uint32_t bx = 0; bx < blocksPerAxis; ++bx)
            for (std::uint32_t channel = 0; channel < 4; ++channel) {
                const auto block = DctBlock(image, bx, by, channel, quant);
                EncodeValue(writer, dc, ac, previous[channel], block);
            }
        writer.Finish(); frame.Marker(0xd9);
        const auto frameBytes = frame.Take();
        const auto mipOffset = kBlpHeaderBytes + kJpegHeaderPrefixBytes + sharedHeader.size();
        const auto totalBytes = mipOffset + frameBytes.size();
        if (totalBytes > limits.maxOutputBytes || mipOffset > std::numeric_limits<std::uint32_t>::max()
            || frameBytes.size() > std::numeric_limits<std::uint32_t>::max())
            return fail("BLP_JPEG_OUTPUT_BUDGET", "BLP JPEG输出超过预算或偏移范围。");
        std::vector<std::byte> output;
        output.reserve(totalBytes);
        if (output.capacity() > limits.maxOutputBytes) return fail("BLP_JPEG_OUTPUT_BUDGET", "BLP输出实际capacity超过预算。");
        output.resize(kBlpHeaderBytes + kJpegHeaderPrefixBytes, std::byte{0});
        std::copy(sharedHeader.begin(), sharedHeader.end(), std::back_inserter(output));
        output.insert(output.end(), frameBytes.begin(), frameBytes.end());
        output[0] = std::byte{'B'}; output[1] = std::byte{'L'}; output[2] = std::byte{'P'}; output[3] = std::byte{'1'};
        PutU32(output, 4, 0); PutU32(output, 8, 0); PutU32(output, 12, image.width); PutU32(output, 16, image.height);
        PutU32(output, 20, 3); PutU32(output, 24, 0); PutU32(output, 156, static_cast<std::uint32_t>(sharedHeader.size()));
        PutU32(output, 28, static_cast<std::uint32_t>(mipOffset)); PutU32(output, 92, static_cast<std::uint32_t>(frameBytes.size()));
        if (output.size() != totalBytes) return fail("BLP_JPEG_INTERNAL", "BLP JPEG输出长度内部不一致。");
        BlpJpegSingleMip result;
        result.bytes = std::move(output); result.width = image.width; result.height = image.height;
        result.jpegHeaderBytes = sharedHeader.size(); result.jpegMipBytes = frameBytes.size();
        return Output::Success(std::move(result));
    } catch (const CapacityFailure&) { return fail("BLP_JPEG_OUTPUT_BUDGET", "JPEG编码输出增长超过预算。"); }
      catch (const std::bad_alloc&) { return fail("BLP_JPEG_ALLOCATION", "JPEG编码分配失败。"); }
      catch (const std::length_error&) { return fail("BLP_JPEG_ALLOCATION", "JPEG编码容量异常。"); }
}

} // namespace War3Map::Image
