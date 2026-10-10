#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace War3Map::Mpq {

// 写出器的默认扇区大小（512 << 3）。可以按源归档的扇区大小写出（YDWE 保存的地图为 64 KB），
// 这样未改动成员的分扇区块都能原样复用。
inline constexpr std::uint16_t kMpqWriterSectorShift = 3;
inline constexpr std::uint32_t kMpqWriterSectorSize = 512u << kMpqWriterSectorShift;
// 允许的扇区位移范围：512 << 3（4 KB）到 512 << 15（16 MB）。
inline constexpr std::uint16_t kMpqMaxSectorShift = 15;

// 源归档中一个成员的原始块：与块在归档中的位置无关（未用 FixKey 加密），可以原样写进新归档，
// 不解压、不重压。读取见 MpqArchive::ReadRawBlock。
struct MpqRawBlock {
    std::vector<std::byte> bytes; // 块在归档中的全部字节（扇区偏移表 + 扇区，或单块数据）
    std::uint32_t fileSize = 0;   // 解压后大小
    std::uint32_t flags = 0;      // 块表标志
    std::uint32_t sectorSize = 0; // 源归档的扇区大小；分扇区块只能写进扇区大小相同的归档
};

// 待写入归档的单个文件。
struct MpqBuildFile {
    std::string name;               // 归档内路径（如 "war3map.w3u"）
    std::vector<std::byte> data;    // 文件内容（解压后）；提供 raw 时不使用，可为空
    bool compress = true;           // 是否尝试 DEFLATE 压缩（失败则存 raw）
    std::optional<MpqRawBlock> raw; // 原样复用的源块（未改动成员）
};

// 构建并写出一个合法的 MPQ format v0 归档。
//   outPath        输出文件路径。
//   files          待写入的文件列表。
//   leadingHeader  可选的前置头（如 WC3 地图的 512 字节 HM3W 头）；为空则无前置。
// 写入规则：
//   * 文件数据不加密（WE/YDWE 可正常读取未加密文件）。
//   * 每扇区（512 << sectorShift 字节）尝试 Deflate 压缩；若压缩后≥原大小或自校验失败则存 raw。
//   * 提供 raw 的文件直接写入原块字节与标志（不含 FixKey / 补丁标志、分扇区块的扇区大小与本次相同，否则拒绝）。
//   * 哈希表/块表使用经典加密。
//   * 自动追加 (listfile)。
//   * outPath 必须不存在，并以 exclusive-create 打开；绝不截断既有文件。
//   * 拒绝重复/不安全名称、调用方提供的 (listfile)、非 512 对齐前缀及 uint32 溢出。
[[nodiscard]] Status WriteMpqArchive(
    const std::filesystem::path& outPath,
    std::span<const MpqBuildFile> files,
    std::span<const std::byte> leadingHeader = {},
    std::uint16_t sectorShift = kMpqWriterSectorShift);

} // namespace War3Map::Mpq
