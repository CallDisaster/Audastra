#pragma once

namespace War3Map { class ProjectSaveControl; }

#include "War3MapCore/Types.h"
#include "War3MapCore/Mpq/MpqWriter.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Mpq {

// MPQ 块表标志位（MPQ format v0/v1 共用；严格旁路服务当前只接受 format v0）。
inline constexpr std::uint32_t kFileImplode = 0x00000100;    // 整文件 PKWARE DCL implode
inline constexpr std::uint32_t kFileCompress = 0x00000200;   // 每扇区带压缩掩码字节的多重压缩
inline constexpr std::uint32_t kFileEncrypted = 0x00010000;  // 数据经典加密
inline constexpr std::uint32_t kFileFixKey = 0x00020000;     // 加密 key 随文件位置/大小调整
inline constexpr std::uint32_t kFilePatch = 0x00100000;      // 增量补丁（本读取器不支持）
inline constexpr std::uint32_t kFileSingleUnit = 0x01000000; // 整文件作为单个块，不分扇区
inline constexpr std::uint32_t kFileSectorCrc = 0x04000000;  // 扇区偏移表尾部附带 CRC 表
inline constexpr std::uint32_t kFileExists = 0x80000000;     // 块有效存在

// 单个可命名文件的元信息（来自块表 + 可选的 (listfile)）。
struct MpqFileInfo {
    std::string name;                 // 已知名字（来自 (listfile) 或显式查询）
    std::uint32_t compressedSize = 0; // 归档内占用字节
    std::uint32_t fileSize = 0;       // 解压后字节
    std::uint32_t flags = 0;          // 块表标志
    bool hasName = false;
};

// 仅在 (listfile) 能完整、唯一地映射全部有效 block 时返回的成员描述。
// blockIndex 用于证明没有匿名块或多个名称指向同一 block；普通调用方不应依赖其稳定性。
struct MpqStrictFileInfo {
    std::string name;
    std::uint32_t blockIndex = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t fileSize = 0;
    std::uint32_t flags = 0;
};

// 哈希表 / 块表的只读副本（诊断与表审计使用，不暴露内部布局）。
inline constexpr std::uint32_t kMpqHashEmpty = 0xFFFFFFFF;
inline constexpr std::uint32_t kMpqHashDeleted = 0xFFFFFFFE;

// platform 字段按磁盘顺序是 platform 字节 + reserved 字节；小端读成 u16 后低字节才是 platform。
// 世界编辑器写出的地图常把 reserved 置为 0xFF（整体 0xFF00），游戏查找只看 platform 字节。
struct MpqHashEntry {
    std::uint32_t name1 = 0;
    std::uint32_t name2 = 0;
    std::uint16_t locale = 0;
    std::uint16_t platform = 0;
    std::uint32_t blockIndex = 0;
};

[[nodiscard]] constexpr std::uint8_t MpqPlatformByte(std::uint16_t field) noexcept {
    return static_cast<std::uint8_t>(field & 0x00FFu);
}
[[nodiscard]] constexpr std::uint8_t MpqReservedByte(std::uint16_t field) noexcept {
    return static_cast<std::uint8_t>(field >> 8);
}

// MPQ 约定的元数据成员：(attributes) 记录每个块的 CRC / 时间戳，(signature) 是弱签名。
// 两者通常不写进 (listfile)，内容依赖其余成员的字节，任何重建都会让它们过期。
// 参数须为 canonical 名称（大写、反斜杠）或原样小写名称。
[[nodiscard]] bool IsMpqMetadataMemberName(std::string_view name) noexcept;

struct MpqBlockEntry {
    std::uint32_t filePos = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t fileSize = 0;
    std::uint32_t flags = 0;
};

// 调用方可为安全敏感读取收紧解析期预算；默认值保持普通 reader 的既有上限。
// 限制在 header scan、表分配与 strict map 构建之前生效。
struct MpqOpenLimits {
    std::uint64_t maxArchivePrefixBytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t maxDeclaredArchiveBytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint32_t maxHashTableEntries = 1u << 20;
    std::uint32_t maxBlockTableEntries = 1u << 20;
    std::size_t maxStrictMemberCount =
        (std::numeric_limits<std::size_t>::max)();
};

// 只读 MPQ format v0 归档读取器。完全自研：加密表、哈希、解密、头/哈希表/块表、
// 扇区拆分与解压分发全部在 Core 内实现，不依赖任何第三方库或 Qt。
//
// 设计原则：
//   * 源归档只读，绝不写回原文件。
//   * Fail-closed：遇到不支持的结构或压缩类型宁可报错，不产出损坏数据。
class MpqArchive {
public:
    MpqArchive(MpqArchive&&) noexcept;
    MpqArchive& operator=(MpqArchive&&) noexcept;
    MpqArchive(const MpqArchive&) = delete;
    MpqArchive& operator=(const MpqArchive&) = delete;
    ~MpqArchive();

    // 打开归档。自动跳过 WC3 地图的 512 字节 "HM3W" 头，扫描 512 对齐的 MPQ 签名。
    [[nodiscard]] static Result<MpqArchive> OpenReadOnly(
        const std::filesystem::path& archivePath);

    // 从借入的 Windows GENERIC_READ 文件句柄打开；内部复制句柄，调用方仍保有原句柄。
    // 该入口用于让事务层的 identity/SHA lease 与实际 MPQ reader 绑定同一文件对象。
    [[nodiscard]] static Result<MpqArchive> OpenReadOnlyHandle(
        void* fileHandle,
        const std::filesystem::path& diagnosticPath = {},
        const MpqOpenLimits& limits = {});

    // 拥有嵌套归档的完整原字节，复用同一解析/解压器；不建立临时文件。
    // 输入最多 64 MiB，调用方从外层读取时仍须先使用 ReadFileBounded。
    [[nodiscard]] static Result<MpqArchive> OpenReadOnlyBytes(
        std::vector<std::byte> bytes,
        const std::filesystem::path& diagnosticPath = {},
        const MpqOpenLimits& limits = {});

    // 按名字（如 "war3map.w3u"）判断是否存在。名字大小写不敏感，'/' 等价于 '\'。
    [[nodiscard]] bool HasFile(std::string_view name) const;

    // 按名字读取并完整解压。找不到或解压失败返回 Failure。
    [[nodiscard]] Result<std::vector<std::byte>> ReadFile(std::string_view name,
        const ProjectSaveControl* control = nullptr) const;

    // 完整读取，先检查命中块的解压大小与归档内大小均不超过 maxBytes，再读数据/解压。
    // 超限失败，不返回截断内容；零预算只接受真实空成员。预算分别限制输入/输出字节，
    // 不表示解码器全部工作内存之和。归档打开时的表/清单预算由 MpqOpenLimits 管理。
    [[nodiscard]] Result<std::vector<std::byte>> ReadFileBounded(
        std::string_view name, std::size_t maxBytes) const;

    // 读取成员的原始块（不解密、不解压），供重建归档时原样复用。块与位置有关（FixKey 加密）
    // 或为补丁块时失败，调用方应回退到解压后重压。结果记录本归档的扇区大小。
    [[nodiscard]] Result<MpqRawBlock> ReadRawBlock(std::string_view name) const;

    // 只读取文件开头至多 maxBytes 字节（分扇区文件只解压需要的扇区）。用于读取音频等文件头：
    // 游戏 WAV 的后续扇区是有损 ADPCM 压缩，首扇区为无损压缩。
    [[nodiscard]] Result<std::vector<std::byte>> ReadFileHead(std::string_view name, std::size_t maxBytes) const;

    // 若归档含 (listfile)，返回其中列出的全部名字（去重、稳定排序）。
    [[nodiscard]] std::vector<std::string> ListFiles() const;

    // 返回 MPQ 签名前的真实前缀字节。不会假设前缀固定为 512 字节。
    [[nodiscard]] Result<std::vector<std::byte>> ReadArchivePrefix() const;

    [[nodiscard]] std::uint64_t ArchivePrefixSize() const noexcept;
    [[nodiscard]] std::uint64_t DeclaredArchiveSize() const noexcept;

    // Fail-closed 完整枚举：要求存在可读 (listfile)，每个有效 block 恰有一个安全的
    // canonical 名称，locale 与 platform 字节均为 0（reserved 字节不限），且不含 patch 文件。
    // 未列入 (listfile) 但存在的 (attributes) / (signature) 按约定名命名，排在 (listfile) 之后；
    // 调用方用 IsMpqMetadataMemberName 识别它们，并自行决定丢弃或拒绝。
    [[nodiscard]] Result<std::vector<MpqStrictFileInfo>> ListFilesStrict() const;

    // 仅用于保留原始 MPQ 字节的受限追加保存：验证清单中每个名字唯一指向一个
    // 活跃 block，同时允许其余 block 保持匿名。此结果不代表匿名内容可解码。
    [[nodiscard]] Result<std::vector<MpqStrictFileInfo>> ListFilesPreservingOpaque() const;

    // 将源归档逐字节复制到独占打开的候选句柄，仅追加替换已列出的现有成员。
    // 原数据区、哈希表及匿名 block 不移动；不能新增成员或改变清单。
    [[nodiscard]] Status WriteOpaquePreservingPatchToHandle(
        void* outputHandle, std::span<const MpqBuildFile> replacements) const;
    // 共用追加写出器的纯输入准备校验；不复制/发布归档，不证明写权限或回读结果。
    [[nodiscard]] Status ValidateOpaquePreservingPatchInputs(
        std::span<const MpqBuildFile> replacements) const;

    // 与源对象比较候选：原归档字节仅允许两个头字段改变，未替换的 block 元数据
    // 以及所有哈希项必须相同，所有已知成员须可正确回读。
    [[nodiscard]] Status VerifyOpaquePreservingPatch(
        const MpqArchive& candidate, std::span<const MpqBuildFile> replacements) const;

    // 将指定名字的文件解压提取到 outDir（保持相对路径，'\' 归一为子目录）。
    // 只提取实际存在的文件；缺失的文件记录诊断但不视为致命错误。
    [[nodiscard]] Status ExtractTo(
        const std::filesystem::path& outDir,
        std::span<const std::string> names) const;

    // 扇区大小（512 << 头中的扇区位移）。
    [[nodiscard]] std::uint32_t SectorSize() const noexcept;

    // 块表中有效文件数量（含无法命名的匿名块）。
    [[nodiscard]] std::size_t FileCount() const noexcept;

    // 表的只读副本与 (listfile) 原始名字（按出现顺序），供表审计与会话诊断使用。
    [[nodiscard]] std::vector<MpqHashEntry> HashTableEntries() const;
    [[nodiscard]] std::vector<MpqBlockEntry> BlockTableEntries() const;
    [[nodiscard]] const std::vector<std::string>& RawListfileNames() const noexcept;

private:
    [[nodiscard]] Result<std::vector<std::uint32_t>> PrepareOpaquePreservingPatchInputs(
        std::span<const MpqBuildFile> replacements) const;
    class Impl;
    explicit MpqArchive(std::unique_ptr<Impl> impl);
    [[nodiscard]] static Result<MpqArchive> OpenParsed(
        std::unique_ptr<Impl> impl, const MpqOpenLimits& limits);
    [[nodiscard]] Result<std::vector<MpqStrictFileInfo>> ListFilesValidated(bool allowOpaque) const;
    std::unique_ptr<Impl> impl_;
};

} // namespace War3Map::Mpq
