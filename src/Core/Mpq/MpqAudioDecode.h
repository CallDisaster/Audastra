#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace War3Map::Mpq {

// 解码 MPQ 音频压缩链。payload 不含掩码；每级输出受 expectedSize 约束。
// 仅支持 0x01 Huffman、0x40/0x80 单/双声道 ADPCM、0x41/0x81 二者组合。
[[nodiscard]] Result<std::vector<std::byte>> DecodeAudioSector(
    std::span<const std::byte> payload,
    std::uint32_t expectedSize,
    std::uint8_t mask);

} // namespace War3Map::Mpq
