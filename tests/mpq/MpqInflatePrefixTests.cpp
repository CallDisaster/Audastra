#include "Mpq/MpqDecompress.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
using Bytes = std::vector<std::byte>;
struct Stream {
    Bytes bytes;
    unsigned offset = 0;
    void Bits(unsigned value, unsigned length) {
        for (unsigned i = 0; i < length; ++i) {
            if (!offset) bytes.push_back(std::byte{0});
            bytes.back() |= std::byte(((value >> i) & 1u) << offset);
            offset = (offset + 1) % 8;
        }
    }
    void Code(unsigned value, unsigned length) {
        for (unsigned i = length; i; --i) Bits((value >> (i - 1)) & 1, 1);
    }
    void Fixed(unsigned symbol) {
        if (symbol < 144) Code(symbol + 48, 8);
        else if (symbol < 256) Code(symbol - 144 + 400, 9);
        else if (symbol < 280) Code(symbol - 256, 7);
        else Code(symbol - 280 + 192, 8);
    }
    void Align() { offset = 0; }
};
void Require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
void Check(const Bytes& compressed, const Bytes& expected) {
    const auto decoded = War3Map::Mpq::Inflate(compressed, static_cast<std::uint32_t>(expected.size()));
    Require(decoded.Succeeded() && decoded.value && *decoded.value == expected, "decoded bytes differ");
}
void FixedAndStored() {
    // 独立按规范写码，覆盖全部字面量、短结束码和查看位后切换未压缩块的对齐。
    for (const bool stored : {false, true}) {
        Stream stream;
        stream.Bits(stored ? 0 : 1, 1); stream.Bits(1, 2);
        Bytes expected;
        for (unsigned i = 0; i < 256; ++i) {
            stream.Fixed(i); expected.push_back(std::byte(i));
        }
        stream.Fixed(256);
        if (stored) {
            stream.Bits(1, 1); stream.Bits(0, 2); stream.Align();
            stream.Bits(2, 16); stream.Bits(0xfffd, 16);
            stream.Bits('H', 8); stream.Bits('i', 8);
            expected.push_back(std::byte{'H'}); expected.push_back(std::byte{'i'});
        }
        Check(stream.bytes, expected);
    }
    Check({std::byte{3}, std::byte{0}}, {}); // 原始位流结尾没有九位可查看。
    Stream invalid;
    invalid.Bits(1, 1); invalid.Bits(1, 2); invalid.Fixed(286);
    Require(!War3Map::Mpq::Inflate(invalid.bytes, 1).Succeeded(), "reserved length accepted");
}
void DynamicLongCode() {
    Stream stream;
    stream.Bits(1, 1); stream.Bits(2, 2);
    stream.Bits(0, 5); stream.Bits(0, 5); stream.Bits(15, 4);
    constexpr std::array<unsigned, 19> order{16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    // 码长字母表：0=0、1=10、15=11。
    for (auto symbol : order) stream.Bits(symbol == 0 ? 1 : symbol == 1 || symbol == 15 ? 2 : 0, 3);
    for (unsigned symbol = 0; symbol < 258; ++symbol) {
        if (symbol == 65) stream.Code(2, 2);
        else if (symbol == 256) stream.Code(3, 2);
        else stream.Code(0, 1);
    }
    // 稀疏表的短码 A=0，结束码=100000000000000；后者必须走原长码路径。
    for (unsigned i = 0; i < 100; ++i) stream.Code(0, 1);
    stream.Code(1 << 14, 15);
    Check(stream.bytes, Bytes(100, std::byte{'A'}));
    stream.bytes.pop_back();
    Require(!War3Map::Mpq::Inflate(stream.bytes, 100).Succeeded(), "truncated long code accepted");
}
void BackReferences() {
    // 共用编码器之外还有上面的独立位流；这里覆盖距离一的重叠复制和混合字面量。
    for (unsigned length : {1u, 7u, 127u, 4096u, 32769u}) {
        Bytes expected(length, std::byte{'x'});
        for (unsigned i = 0; i < length; i += 71) expected[i] = std::byte(i % 256);
        Check(War3Map::Mpq::Deflate(expected), expected);
    }
}
}
int main() {
    try { FixedAndStored(); DynamicLongCode(); BackReferences(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
