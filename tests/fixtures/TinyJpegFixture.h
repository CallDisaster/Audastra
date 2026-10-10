#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace War3Map::TestFixtures {

struct TinyJpegFixture {
    std::vector<unsigned char> bytes;
    std::size_t entropyOffset = 0;
};

// 原创灰度 128 JPEG：每个 8x8 MCU 只有 DC category=0、AC EOB 两个零 bit。
// restart=true 时每 MCU 用 RST0..7 分隔，其余位按 JPEG 规则填 1。
inline TinyJpegFixture MakeTinyGrayJpeg(std::uint16_t width, std::uint16_t height, bool restart = false) {
    TinyJpegFixture result{{0xff, 0xd8}, 0};
    auto& jpeg = result.bytes;
    const auto segment = [&](unsigned char marker, const std::vector<unsigned char>& data) {
        jpeg.push_back(0xff); jpeg.push_back(marker);
        const auto length = data.size() + 2;
        jpeg.push_back(static_cast<unsigned char>(length >> 8));
        jpeg.push_back(static_cast<unsigned char>(length & 255));
        jpeg.insert(jpeg.end(), data.begin(), data.end());
    };
    std::vector<unsigned char> quant(65, 1); quant[0] = 0;
    segment(0xdb, quant);
    segment(0xc0, {8, static_cast<unsigned char>(height >> 8), static_cast<unsigned char>(height & 255),
        static_cast<unsigned char>(width >> 8), static_cast<unsigned char>(width & 255), 1, 1, 0x11, 0});
    std::vector<unsigned char> huffman;
    for (const auto type : {0, 1}) {
        huffman.push_back(static_cast<unsigned char>(type << 4));
        huffman.push_back(1);
        for (int index = 1; index < 16; ++index) huffman.push_back(0);
        huffman.push_back(0);
    }
    segment(0xc4, huffman);
    if (restart) segment(0xdd, {0, 1});
    segment(0xda, {1, 1, 0, 0, 63, 0});
    result.entropyOffset = jpeg.size();
    const auto units = ((static_cast<unsigned>(width) + 7) / 8) * ((static_cast<unsigned>(height) + 7) / 8);
    unsigned bits = 0;
    for (unsigned unit = 0; unit < units; ++unit) {
        bits += 2;
        if (bits == 8) { jpeg.push_back(0); bits = 0; }
        if (restart || unit + 1 == units) {
            if (bits != 0) jpeg.push_back(static_cast<unsigned char>((1u << (8 - bits)) - 1));
            bits = 0;
            if (restart && unit + 1 < units) jpeg.insert(jpeg.end(), {0xff, static_cast<unsigned char>(0xd0 + unit % 8)});
        }
    }
    jpeg.insert(jpeg.end(), {0xff, 0xd9});
    return result;
}

} // namespace War3Map::TestFixtures
