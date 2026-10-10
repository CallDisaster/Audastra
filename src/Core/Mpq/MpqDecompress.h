#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace War3Map::Mpq {

// 解压单个 MPQ 扇区。
//   input          扇区在归档中的原始字节（已解密）。
//   expectedSize   该扇区解压后的字节数。
//   implodeOnly    该文件使用整文件 PKWARE implode 标志（无压缩掩码字节）。
// 规则（与 StormLib 一致）：
//   * 若 input.size() >= expectedSize，扇区实际未压缩，直接拷贝。
//   * implodeOnly 为真时对整段做 PKWARE DCL explode。
//   * 否则首字节为压缩掩码：0x02=zlib/deflate，0x08=PKWARE implode。
//   * 0x01=Huffman，0x40/0x80=单/双声道 ADPCM，0x41/0x81=Huffman 后接 ADPCM。
//     音频链逐级受 expectedSize 约束，最终 PCM 长度必须与声明相同。
//   * bzip2 或其它组合掩码暂不支持，fail-closed 报错。
[[nodiscard]] Result<std::vector<std::byte>> DecompressSector(
    std::span<const std::byte> input,
    std::uint32_t expectedSize,
    bool implodeOnly);

// 原始 DEFLATE / zlib 流解压（RFC 1950/1951）。自动识别并跳过 2 字节 zlib 头。
[[nodiscard]] Result<std::vector<std::byte>> Inflate(
    std::span<const std::byte> input,
    std::uint32_t expectedSize);

// PKWARE Data Compression Library “explode”（对应 implode 的解压）。
[[nodiscard]] Result<std::vector<std::byte>> PkwareExplode(
    std::span<const std::byte> input,
    std::uint32_t expectedSize);

// 自研 DEFLATE 压缩器：输出完整 zlib 流（0x78 0x9C + deflate + adler32）。
// 使用 LZ77（哈希链，32KB 窗口）+ 固定 Huffman 块。
// 产出保证能被本项目 Inflate 正确还原（写入器会做扇区级自校验）。
[[nodiscard]] std::vector<std::byte> Deflate(std::span<const std::byte> input);

} // namespace War3Map::Mpq
