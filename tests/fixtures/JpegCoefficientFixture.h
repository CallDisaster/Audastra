#pragma once

#include <cstdint>
#include <vector>

namespace War3Map::TestFixtures {

// 原创 JPEG 边界输入生成器。逐块固定 DC 差分；可加一个填满 AC 的渐进扫描。
// 故意允许测试指定非法类别/过大系数，用于证明解码器在窄化和移位前拒绝输入。
struct JpegCoefficientSpec {
    unsigned blocks = 1;
    unsigned dcCategory = 0;
    int dcDifference = 0;
    unsigned dcAl = 0;
    bool progressive = false;
    bool denseAc = false;
    unsigned acCategory = 10;
    unsigned acAl = 0;
    std::uint16_t quant = 1;
};

inline std::vector<unsigned char> MakeCoefficientJpeg(const JpegCoefficientSpec& spec) {
    std::vector<unsigned char> jpeg{0xff, 0xd8};
    const auto segment = [&](unsigned char marker, const std::vector<unsigned char>& data) {
        const auto length = data.size() + 2;
        jpeg.insert(jpeg.end(), {0xff, marker, static_cast<unsigned char>(length >> 8),
            static_cast<unsigned char>(length & 255)});
        jpeg.insert(jpeg.end(), data.begin(), data.end());
    };
    std::vector<unsigned char> quant{static_cast<unsigned char>(spec.quant > 255 ? 0x10 : 0)};
    for (unsigned index = 0; index < 64; ++index) {
        if (spec.quant > 255) quant.push_back(static_cast<unsigned char>(spec.quant >> 8));
        quant.push_back(static_cast<unsigned char>(spec.quant & 255));
    }
    segment(0xdb, quant);
    const auto width = spec.blocks * 8;
    segment(static_cast<unsigned char>(spec.progressive ? 0xc2 : (spec.quant > 255 ? 0xc1 : 0xc0)),
        {8, 0, 8, static_cast<unsigned char>(width >> 8), static_cast<unsigned char>(width & 255), 1, 1, 0x11, 0});
    std::vector<unsigned char> huffman;
    for (const auto type : {0u, 1u}) {
        huffman.push_back(static_cast<unsigned char>(type << 4));
        huffman.push_back(1); // 唯一符号码为 0，保留全 1 padding 码。
        for (unsigned index = 1; index < 16; ++index) huffman.push_back(0);
        huffman.push_back(static_cast<unsigned char>(type == 0 ? spec.dcCategory : (spec.denseAc ? spec.acCategory : 0)));
    }
    segment(0xc4, huffman);
    unsigned byte = 0, count = 0;
    const auto bit = [&](unsigned value) {
        byte = (byte << 1) | (value & 1);
        if (++count == 8) {
            jpeg.push_back(static_cast<unsigned char>(byte));
            if (byte == 255) jpeg.push_back(0);
            count = 0; byte = 0;
        }
    };
    const auto bits = [&](std::uint64_t value, unsigned widthBits) {
        for (unsigned index = widthBits; index > 0; --index) bit(static_cast<unsigned>((value >> (index - 1)) & 1));
    };
    const auto finish = [&] { while (count != 0) bit(1); };
    segment(0xda, {1, 1, 0, 0, static_cast<unsigned char>(spec.progressive ? 0 : 63), static_cast<unsigned char>(spec.dcAl)});
    const auto amplitude = spec.dcDifference >= 0 ? static_cast<std::uint64_t>(spec.dcDifference)
        : static_cast<std::uint64_t>(static_cast<std::int64_t>(spec.dcDifference) + (std::int64_t{1} << spec.dcCategory) - 1);
    for (unsigned block = 0; block < spec.blocks; ++block) {
        bit(0); bits(amplitude, spec.dcCategory);
        if (!spec.progressive) bit(0); // AC EOB
    }
    finish();
    if (spec.denseAc) {
        segment(0xda, {1, 1, 0, 1, 63, static_cast<unsigned char>(spec.acAl)});
        for (unsigned block = 0; block < spec.blocks; ++block) {
            for (unsigned coefficient = 1; coefficient < 64; ++coefficient) {
                bit(0); bits((std::uint64_t{1} << spec.acCategory) - 1, spec.acCategory);
            }
        }
        finish();
    }
    jpeg.insert(jpeg.end(), {0xff, 0xd9});
    return jpeg;
}

} // namespace War3Map::TestFixtures
