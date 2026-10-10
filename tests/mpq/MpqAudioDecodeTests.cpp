#include "Mpq/MpqAudioDecode.h"
#include "Mpq/MpqDecompress.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::byte> Hex(std::string_view text) {
    Require(text.size() % 2 == 0, "fixture hex length");
    const auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        throw std::runtime_error("fixture hex digit");
    };
    std::vector<std::byte> result;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        result.push_back(static_cast<std::byte>((nibble(text[i]) << 4) | nibble(text[i + 1])));
    }
    return result;
}

void CheckSector(std::string_view packed, const std::vector<std::byte>& expected, const char* name) {
    const auto input = Hex(packed);
    const auto result = War3Map::Mpq::DecompressSector(input, static_cast<std::uint32_t>(expected.size()), false);
    if (!result.Succeeded()) {
        std::cerr << name << ": decode failed";
        for (const auto& diagnostic : result.diagnostics) std::cerr << " | " << diagnostic.code << ": " << diagnostic.message;
        std::cerr << '\n';
        throw std::runtime_error(name);
    }
    if (*result.value != expected) {
        const auto& actual = *result.value;
        std::size_t firstDifference = 0;
        while (firstDifference < actual.size() && firstDifference < expected.size()
            && actual[firstDifference] == expected[firstDifference]) ++firstDifference;
        std::cerr << name << ": expected=" << expected.size() << " actual=" << actual.size()
            << " firstDifference=" << firstDifference;
        if (firstDifference < expected.size()) std::cerr << " expectedByte=" << static_cast<unsigned>(expected[firstDifference]);
        if (firstDifference < actual.size()) std::cerr << " actualByte=" << static_cast<unsigned>(actual[firstDifference]);
        std::cerr << '\n';
        throw std::runtime_error(name);
    }
}

// 原创 PCM：单声道固定 1000，双声道交替 1000/-1000，跳变波形 ((i*4091)%60000)-30000。
// 静态压缩字节/有损 PCM 期望由本机 StormLib SCompCompress/SCompDecompress 独立生成。
// ADPCM 有损：即使输入是常值，量化输出也可能有 ±1，不能把原始 PCM 当解码期望。
// 不含 Blizzard 音频，也不在测试运行时依赖 StormLib。
void TestAuthoredPcmFixtures() {
    const auto stereoOracle = Hex(
        "e80318fce80318fce80318fce80318fce80318fce80318fce80318fce80318fce80318fce80318fce80318fce80318fc"
        "e80318fce80318fce80318fce80318fce80318fce80318fce80318fce80318fce80318fce80318fce80318fce80318fc"
        "e80318fce80318fce80318fce80318fce80318fce80318fce90319fce80318fc"
    );
    const auto monoOracle = Hex(
        "e803e803e803e803e803e803e803e803e803e803e803e803e803e803e803e803e803e803e803e803e803e803e803e803"
        "e803e803e803e803e803e803e903e803e903e803e903e803e903e803e803e803e803e803e803e803e803e803e803e803"
        "e803e803e803e803e803e803e803e803e803e803e803e803e803e803e803e803"
    );
    CheckSector("400004e803808080808080808080808080808080808080808080808080808080808000400040004000400000000000000008080808080808080808080808080808080808", monoOracle, "mono-40 oracle fixture");
    CheckSector("4107194a41fff67ddff77ddff77ddff77ddff77ddff77ddff77ddff739e594534e39e79c73161111111111111111914e01", monoOracle, "mono-41 oracle fixture");
    CheckSector("800004e80318fc8080808080808080808080808080808080808080808080808080808080808080808080808080808080808080808080808080808080808080808000004040", stereoOracle, "stereo-80 oracle fixture");
    CheckSector("8107194a41ff761aa3d3f8bbaeebbaaeebbaaeebbaaeebbaaeebbaaeebbaaeebbaaeebbaaeebbaaeebbaaeebbaaeebbaaeebbaaeebba2ee794fa09", stereoOracle, "stereo-81 oracle fixture");
    const auto ramp = Hex(
        "d08ae69a9bab58ba51cb0eda07ebc4f9bd0a721b2f2a283be549de5a9b699f919e9970af45c34cd5c3da99f32501760d"
        "1f2015318040844e5b609370979896a068b63dca3fd0a4e07af9060757130026f636614665543c66408e3f9611ace6bf"
        "e8c54dd623efaffc0009a91b9f2c0a3c0e4ae55b1d6c2194209cf2b1c7c5c9cb2edc14eba901fa0da32099310441084f"
        "df6017711b991aa1ecb6c1cac3d028e1fef98a07db1384267a37e546e954c066c48ec39695ac6ac06cc6d1d6a7ef33fd"
        "84092d1c232d8e3c924a695ca16ca594a49c76b24bc64dccb2dc98eb2d027e0e27211d3288418c4f63619b719f999ea1"
        "70b745cb47d1ace192f0270778132126"
    );
    CheckSector("400004d08a818107010e090e090e09010e090e090e8181814b0008080800040808040404040c0c81814b0008080008040808040404040c81814b0008080008040808040404040c0c81814b0008080008080408040404040c0c81814b0008080008040808040404040c81814b0008080008040808040404040c0c81814b0008080008080408040404040c0c81814b000808000808040804", ramp, "ramp-40 oracle fixture");
    CheckSector("4107194a81eeb462597637f5ead537f5eab52ccb4816910c220000cc6519c9225940040080cb329245b280080000735946b24816010100602ecb4816c9022200005c96912c920544000098cb329245b208080000735946b2481601814e07", ramp, "ramp-41 oracle fixture");
}

// 每种分布选择最高初始权重的字节重复 128 次，再加入其下一字节 8 次。
void TestAllHuffmanDistributions() {
    {
        auto expected = std::vector<std::byte>(128, std::byte{0x00});
        expected.insert(expected.end(), 8, std::byte{0x01});
        CheckSector("0100ffffffffffffffffffffffffffffffff0810a812", expected, "huff-0 distribution");
    }
    {
        auto expected = std::vector<std::byte>(128, std::byte{0x00});
        expected.insert(expected.end(), 8, std::byte{0x01});
        CheckSector("01017777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777777700000000003102", expected, "huff-1 distribution");
    }
    {
        auto expected = std::vector<std::byte>(128, std::byte{0x20});
        expected.insert(expected.end(), 8, std::byte{0x21});
        CheckSector("0102aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa31c618638c31c618638ce502", expected, "huff-2 distribution");
    }
    {
        auto expected = std::vector<std::byte>(128, std::byte{0x00});
        expected.insert(expected.end(), 8, std::byte{0x01});
        CheckSector("0103ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb66ddbb66ddb9503", expected, "huff-3 distribution");
    }
    {
        auto expected = std::vector<std::byte>(128, std::byte{0x00});
        expected.insert(expected.end(), 8, std::byte{0x01});
        CheckSector("0104dbb66ddbb66ddbb66ddbb66ddbb66ddbb66ddbb66ddbb66ddbb66ddbb66ddbb66ddbb66ddbb66ddbb66ddbb66ddbb66d6ddbb6ae", expected, "huff-4 distribution");
    }
    {
        auto expected = std::vector<std::byte>(128, std::byte{0x00});
        expected.insert(expected.end(), 8, std::byte{0x01});
        CheckSector("01052222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222244444444cf08", expected, "huff-5 distribution");
    }
    {
        auto expected = std::vector<std::byte>(128, std::byte{0x04});
        expected.insert(expected.end(), 8, std::byte{0x05});
        CheckSector("010692244992244992244992244992244992244992244992244992244992244992244992244992244992244992244992244973ce39e79ca6", expected, "huff-6 distribution");
    }
    {
        auto expected = std::vector<std::byte>(128, std::byte{0x4e});
        expected.insert(expected.end(), 8, std::byte{0x4f});
        CheckSector("0107666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666662525252525252525a502", expected, "huff-7 distribution");
    }
    {
        auto expected = std::vector<std::byte>(128, std::byte{0x5e});
        expected.insert(expected.end(), 8, std::byte{0x5f});
        CheckSector("0108de7befbdf7de7befbdf7de7befbdf7de7befbdf7de7befbdf7de7befbdf7de7befbdf7de7befbdf7de7befbdf7de7befbdf7de7befbdf7de7befbdf7de7befbdf7de7befbdf7de7befbdf7de7befbdf7b163c78e1d3b76ecd85105", expected, "huff-8 distribution");
    }
}

void ExpectAudioError(std::string_view packed, std::uint32_t size, std::uint8_t mask, std::string_view code) {
    const auto data = Hex(packed);
    const auto result = War3Map::Mpq::DecodeAudioSector(data, size, mask);
    Require(!result.Succeeded() && !result.value.has_value(), "invalid audio must not return partial bytes");
    Require(!result.diagnostics.empty() && result.diagnostics.front().code == code, "audio failure diagnostic");
}

void TestAdpcmControlsAndBounds() {
    // 0x81 保留当前声道且不产出样本；0x80 复制预测样本，随后再轮到另一个声道。
    const auto controls = Hex("000064009cff81008040");
    const auto decoded = War3Map::Mpq::DecodeAudioSector(controls, 10, 0x80);
    Require(decoded.Succeeded() && *decoded.value == Hex("64009cff88049cffc500"), "stereo channel control semantics");
    const auto saturation = Hex("000030758181818181813f7f3f7f3f7f3f7f3f7f3f7f");
    const auto samples = War3Map::Mpq::DecodeAudioSector(saturation, 26, 0x40);
    Require(samples.Succeeded() && *samples.value == Hex("3075ff7f0080ff7f0080ff7f0080ff7f0080ff7f0080ff7f0080"), "ADPCM prediction clamps both signed limits");
    ExpectAudioError("0020000080", 4, 0x40, "AUDIO_ADPCM_SHIFT");
    ExpectAudioError("000000", 4, 0x40, "AUDIO_ADPCM_HEADER");
    ExpectAudioError("0000000080", 5, 0x40, "AUDIO_ADPCM_HEADER");
    ExpectAudioError("0000000080", 2, 0x40, "AUDIO_ADPCM_CAPACITY");
    ExpectAudioError("0000000080", 6, 0x40, "AUDIO_ADPCM_SIZE");
    ExpectAudioError("000000", 16, 0x01, "AUDIO_HUFFMAN_TREE");
    ExpectAudioError("09", 16, 0x01, "AUDIO_HUFFMAN_HEADER");
    ExpectAudioError("00", 16, 0x01, "AUDIO_HUFFMAN_TRUNCATED");
    ExpectAudioError("", 0, 0x01, "AUDIO_OUTPUT_EMPTY");
    ExpectAudioError("0000000080", 4, 0xC0, "AUDIO_UNSUPPORTED_MASK");
}

void TestTruncationAndCapacity() {
    const auto packed = Hex("4107194a41fff67ddff77ddff77ddff77ddff77ddff77ddff77ddff739e594534e39e79c73161111111111111111914e01");
    for (std::size_t length = 1; length < packed.size(); ++length) {
        const auto result = War3Map::Mpq::DecompressSector(std::span(packed).first(length), 128, false);
        Require(!result.Succeeded() && !result.value.has_value(), "every truncated Huffman+ADPCM prefix must fail");
    }
    ExpectAudioError("00ffffffffffffffffffffffffffffffff0810a812", 135, 0x01, "AUDIO_HUFFMAN_CAPACITY");
    ExpectAudioError("00ffffffffffffffffffffffffffffffff0810a812", 137, 0x01, "AUDIO_HUFFMAN_SIZE");
    ExpectAudioError("07194a41fff67ddff77ddff77ddff77ddff77ddff77ddff77ddff739e594534e39e79c73161111111111111111914e01", 126, 0x41, "AUDIO_ADPCM_CAPACITY");
    ExpectAudioError("07194a41fff67ddff77ddff77ddff77ddff77ddff77ddff77ddff739e594534e39e79c73161111111111111111914e01", 130, 0x41, "AUDIO_ADPCM_SIZE");
}

void TestStoredAndUnsupportedSectors() {
    const std::array stored{std::byte{0x41}, std::byte{0x12}, std::byte{0x34}};
    const auto result = War3Map::Mpq::DecompressSector(stored, static_cast<std::uint32_t>(stored.size()), false);
    Require(result.Succeeded() && *result.value == std::vector<std::byte>(stored.begin(), stored.end()),
        "stored sector must copy literal bytes including leading 0x41");
    for (const auto mask : {0x01, 0x40, 0x80, 0x41, 0x81}) {
        const std::array truncated{std::byte{static_cast<unsigned char>(mask)}, std::byte{0x00}};
        const auto failed = War3Map::Mpq::DecompressSector(truncated, 16, false);
        Require(!failed.Succeeded() && !failed.value.has_value(), "truncated audio cannot return fake bytes");
    }
    for (const auto mask : {0x03, 0x10, 0xC0, 0xC1}) {
        const std::array unsupported{std::byte{static_cast<unsigned char>(mask)}, std::byte{0x00}};
        const auto failed = War3Map::Mpq::DecompressSector(unsupported, 16, false);
        Require(!failed.Succeeded() && failed.diagnostics.front().code == "SECTOR_UNSUPPORTED_COMPRESSION",
            "unimplemented compression combinations remain explicit failures");
    }
}

} // namespace

int main() {
    try {
        TestAuthoredPcmFixtures();
        TestAllHuffmanDistributions();
        TestAdpcmControlsAndBounds();
        TestTruncationAndCapacity();
        TestStoredAndUnsupportedSectors();
        std::cout << "MPQ audio decode and boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
