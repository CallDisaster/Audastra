#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace War3Map::TerrainSaveTests {

// 原创type2夹具转为相同文件行序的type10；此处只编码，不调用产品解码器制造预期。
// 重复包和原样包按整图编码，可跨行，保留BGRA与descriptor原点位。
inline std::vector<std::byte> EncodeFootprintTgaRle(std::span<const std::byte> input) {
    if (input.size() < 18 || input[2] != std::byte{2})
        throw std::runtime_error("RLE fixture requires a complete original type2 header");
    const auto word = [&](std::size_t offset) {
        return std::to_integer<unsigned>(input[offset]) | (std::to_integer<unsigned>(input[offset + 1]) << 8);
    };
    const auto pixelBytes = std::to_integer<unsigned>(input[16]) / 8;
    const std::size_t count = std::size_t(word(12)) * word(14);
    if ((pixelBytes != 3 && pixelBytes != 4) || !count || input.size() != 18 + count * pixelBytes)
        throw std::runtime_error("RLE fixture requires a complete 24/32-bit pixel matrix without footer");
    std::vector<std::byte> result(input.begin(), input.begin() + 18); result[2] = std::byte{10};
    const auto same = [&](std::size_t a, std::size_t b) {
        return std::equal(input.begin() + 18 + a * pixelBytes, input.begin() + 18 + (a + 1) * pixelBytes,
            input.begin() + 18 + b * pixelBytes);
    };
    for (std::size_t at = 0; at < count;) {
        std::size_t run = 1;
        while (run < 128 && at + run < count && same(at, at + run)) ++run;
        if (run > 1) {
            result.push_back(std::byte(0x80u | unsigned(run - 1)));
            result.insert(result.end(), input.begin() + 18 + at * pixelBytes, input.begin() + 18 + (at + 1) * pixelBytes);
            at += run;
        } else {
            const auto first = at++;
            while (at - first < 128 && at < count && (at + 1 == count || !same(at, at + 1))) ++at;
            result.push_back(std::byte(at - first - 1));
            result.insert(result.end(), input.begin() + 18 + first * pixelBytes, input.begin() + 18 + at * pixelBytes);
        }
    }
    return result;
}

} // namespace War3Map::TerrainSaveTests
