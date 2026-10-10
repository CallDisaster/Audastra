#include "MpqDecompress.h"
#include "MpqAudioDecode.h"

namespace War3Map::Mpq {

Result<std::vector<std::byte>> DecompressSector(
    std::span<const std::byte> input,
    std::uint32_t expectedSize,
    bool implodeOnly) {
    if (expectedSize == 0) {
        return Result<std::vector<std::byte>>::Success({});
    }
    // 扇区未压缩存储：归档内字节数不小于解压后大小时按原样拷贝。
    if (input.size() >= expectedSize) {
        std::vector<std::byte> raw(
            input.begin(),
            input.begin() + static_cast<std::ptrdiff_t>(expectedSize));
        return Result<std::vector<std::byte>>::Success(std::move(raw));
    }
    if (input.empty()) {
        return Result<std::vector<std::byte>>::Failure(
            "Mpq", "SECTOR_EMPTY", "compressed sector is empty");
    }

    // 整文件 implode 标志：无压缩掩码字节，直接 explode。
    if (implodeOnly) {
        return PkwareExplode(input, expectedSize);
    }

    // 多重压缩：首字节为压缩掩码。
    const auto mask = static_cast<unsigned char>(input[0]);
    const std::span<const std::byte> payload = input.subspan(1);
    switch (mask) {
    case 0x02: // zlib / DEFLATE —— WC3 物体/触发器数据主力
        return Inflate(payload, expectedSize);
    case 0x08: // PKWARE DCL implode
        return PkwareExplode(payload, expectedSize);
    case 0x01: // Blizzard 自适应 Huffman
    case 0x40: // Blizzard ADPCM mono
    case 0x80: // Blizzard ADPCM stereo
    case 0x41: // Huffman → ADPCM mono
    case 0x81: // Huffman → ADPCM stereo
        return DecodeAudioSector(payload, expectedSize, mask);
    default:
        // 0x10 bzip2 及未明确接入的组合掩码仍不支持，保持失败封闭。
        return Result<std::vector<std::byte>>::Failure(
            "Mpq", "SECTOR_UNSUPPORTED_COMPRESSION",
            "unsupported MPQ compression mask 0x" +
                std::string(1, "0123456789ABCDEF"[(mask >> 4) & 0xF]) +
                std::string(1, "0123456789ABCDEF"[mask & 0xF]));
    }
}

} // namespace War3Map::Mpq
