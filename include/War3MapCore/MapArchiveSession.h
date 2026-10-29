#pragma once

#include "War3MapCore/Types.h"

#include <chrono>
#include <span>
#include <stop_token>
#include <string_view>

namespace War3Map {

class ProjectSaveControl;

struct MemberReplacement {
    std::string name;
    std::vector<std::byte> data;
};

using MapArchiveMember = MemberReplacement;

struct ArchiveExactMember {
    std::string name;
    bool present = false;
    std::vector<std::byte> bytes;
};

struct ArchiveExactMemberLimits {
    std::size_t memberCount = 256;
    std::size_t pathBytes = 1024;
    std::size_t totalPathBytes = 8u * 1024u * 1024u;
    std::size_t memberBytes = 64u * 1024u * 1024u;
    std::size_t totalBytes = 64u * 1024u * 1024u;
};

struct ArchiveExactMemberIdentity {
    std::string name;
    bool present = false;
    std::optional<std::uint64_t> byteSize;
    std::optional<std::string> sha256;
};

struct ArchiveExactIdentityResult {
    std::string sourceSha256;
    std::vector<ArchiveExactMemberIdentity> members;
};

struct ArchiveSaveResult {
    std::filesystem::path path;
    std::string sha256;
    std::size_t memberCount = 0; // 不包含 writer 自动生成的 (listfile)。
    std::uint64_t fileSize = 0;  // 写出的归档字节数
};

// 完整回读验证后才将临时文件发布到独立目标；失败不留下部分目标，不覆盖既有文件。
[[nodiscard]] Result<ArchiveSaveResult> WriteVerifiedArchive(
    const std::filesystem::path& target, std::span<const MapArchiveMember> members,
    std::span<const std::byte> leadingHeader = {}, ProjectSaveControl* control = nullptr,
    bool finalPublication = true);

// 原地保存（写回打开的地图）的选项与结果。
struct InPlaceSaveOptions {
    // 原图备份目录，可跨卷。Core 为空时兼容使用“源目录/War3MapEditor Backups”；
    // 应用调用方显式传入用户目录或编辑器安装根目录/backups。
    std::filesystem::path backupDirectory;
    // 每个规范源路径保留的最近备份数；只清理归属记录、文件身份与内容均可核验的本服务备份。
    // 旧命名备份和用户改动过的文件不自动删除。0 表示不清理。
    std::size_t keepBackups = 10;
};

struct InPlaceSaveResult {
    ArchiveSaveResult written;                        // path 为源地图路径
    std::filesystem::path backup;                     // 保存前的原图
    std::vector<std::filesystem::path> prunedBackups; // 超出保留数被删除的旧备份
};

enum class MapBackupInspectionState { Unknown, Absent, ConfirmedAtLeastOne };

struct MapBackupInspectionOptions {
    // 总目录项包含与本服务无关的文件；只能收紧 4096 项 / 4096 收据 / 单收据 4096 B / 总 1 GiB。
    std::size_t maxEntries = 256;
    std::size_t maxReceipts = 32;
    std::size_t maxReceiptBytes = 4096;
    std::uint64_t maxReadBytes = 1024ull * 1024 * 1024;
    // 多根检查共用同一绝对期限与剩余额度。Win32 I/O 只能协作取消，不能被期限抢占。
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    std::stop_token stop;
};

struct MapBackupInspection {
    MapBackupInspectionState state = MapBackupInspectionState::Unknown;
    std::size_t examinedEntries = 0;
    std::size_t readReceipts = 0;
    // 读取预算账：receipt 计实际读数；backup 开始散列时预留锁定大小，取消 / 失败不退还。
    // 成功时等于完整读取字节；Unknown 可能为保守上界，不声称备份已经读完。元数据不计入。
    std::uint64_t bytesRead = 0;
    std::optional<std::filesystem::path> confirmedBackup;
};

// 只读检查一个已知旧备份位置；不扫描其他目录、不打开源地图、不申请 DELETE、不创建或迁移文件。
// 复用保存链的 owner 记录与同句柄原生身份 / 完整 SHA。确认第一份即停止，不报告完整份数。
// Absent 仅表示完整检查后没有本服务候选收据 / 孤立命名标记，不表示目录为空或其他历史位置也无备份。
// 占用、损坏、失效、取消及容量 / 期限耗尽均保留 Unknown 值和诊断；错误参数才返回失败。
[[nodiscard]] Result<MapBackupInspection> InspectMapBackupDirectory(
    const std::filesystem::path& directory, const MapBackupInspectionOptions& options = {});

// 编辑会话的只读源快照。保存候选不会修改此对象；调用方只在保存成功后切换基线。
// 保存时未改动的成员原样复用源归档的压缩块（不解压、不重压，回读时逐字节比较原始块），
// 改动的成员重新压缩并完整回读；源图只在租约开始时散列一次，租约期间源图不可写入。
// Members() 只列可命名成员。HasOpaqueMembers() 为真时，匿名块只能通过原字节
// 追加保存路径保留，不得把 Members() 当成完整清单另行重建。
// 此服务证明候选原始字节未丢失，不代表脚本编译、编辑器真保存或游戏运行通过。
class MapArchiveSession {
public:
    [[nodiscard]] static Result<MapArchiveSession> Open(const std::filesystem::path& path,
        const ProjectSaveControl* control = nullptr);
    // 只解压脚本审计所需的固定基础/触发器/物编成员；不读取不透明导入，不能保存。
    [[nodiscard]] static Result<MapArchiveSession> OpenScriptAudit(const std::filesystem::path& path);
    [[nodiscard]] Status VerifySourceUnchanged(const ProjectSaveControl* control = nullptr) const;
    [[nodiscard]] bool HasMember(std::string_view name) const;
    [[nodiscard]] Result<std::vector<std::byte>> ReadMember(std::string_view name,
        const ProjectSaveControl* control = nullptr) const;
    // 按明确名字在同一已验证只读源租约内捕获；不依赖 listfile 是否列出该名字。
    // 输出保持请求顺序和名字，present=false 才表示已证明缺失；真实空成员仍为 present=true。
    // 名字必须安全、唯一且非归档保留名。调用方只能收紧预算；失败不返回部分结果。
    // 此接口不改变 Members()/HasMember()/ReadMember() 的命名快照语义，也不扩大保存资格。
    [[nodiscard]] Result<std::vector<ArchiveExactMember>> CaptureExactMembers(
        std::span<const std::string> names, const ArchiveExactMemberLimits& limits = {}) const;
    // 强制绑定合法期望源SHA，复用同一精确捕获与预算，仅散列拥有的字节，不解析成员内容。
    // Present空文件返回0与标准空内容SHA；Missing返回false与两个nullopt。
    // 顺序/名字保持请求原值；源过期、不可读、预算或散列失败不返回部分身份。
    [[nodiscard]] Result<ArchiveExactIdentityResult> DescribeExactMemberIdentities(
        std::string_view expectedSourceSha256, std::span<const std::string> names,
        const ArchiveExactMemberLimits& limits = {}) const;
    [[nodiscard]] std::span<const MapArchiveMember> Members() const noexcept { return members_; }
    [[nodiscard]] bool HasOpaqueMembers() const noexcept { return opaqueMembers_; }
    [[nodiscard]] std::size_t OpaqueMemberCount() const noexcept { return opaqueMemberCount_; }
    // 源归档中存在的 (attributes) / (signature)。它们不进入 Members()，完整重建时丢弃
    // （内容会过期）；原字节追加保存路径下仍原样保留在源字节中。
    [[nodiscard]] std::span<const std::string> MetadataMembers() const noexcept { return metadataMembers_; }
    [[nodiscard]] std::span<const std::byte> Prefix() const noexcept { return prefix_; }
    // 产生保存快照，不更改当前会话或源文件。保留源租约身份；匿名追加路径不能改外头。
    // 只接受同长度头部，或无头归档新增一个经典 512 字节头部。
    [[nodiscard]] Result<MapArchiveSession> WithLeadingHeader(std::span<const std::byte> header) const;
    [[nodiscard]] const std::filesystem::path& SourcePath() const noexcept { return sourcePath_; }
    [[nodiscard]] const std::string& SourceSha256() const noexcept { return sourceSha256_; }
    // removals：不写入候选的成员（改名 / 移动后的旧路径、删除的资源）。含匿名成员的归档不能删除成员。
    [[nodiscard]] Result<ArchiveSaveResult> SaveCandidate(
        const std::filesystem::path& target,
        std::span<const MemberReplacement> replacements,
        std::span<const std::string> removals = {}) const;
    // 与候选保存共用源租约、成员准备与输入校验，不创建/发布归档或建立新会话。
    // target 为空时不检查输出路径。通过仅说明当前输入可准备，不保证写出/回读/备份。
    [[nodiscard]] Status PreflightSave(
        std::span<const MemberReplacement> replacements,
        std::span<const std::string> removals = {},
        const std::filesystem::path& target = {}) const;
    // 写回源地图：在同目录写出并回读验证临时候选（期间源图由租约锁定不可写入），替换前核对
    // 源图身份，先复制并核验原图备份，再用源卷恢复路径参与 ReplaceFileW 原子替换。
    // 失败时源图保持不变或从恢复位置移回；恢复失败时明确返回原图恢复路径和已核验备份路径。
    // 本对象不变；调用方用 AfterSave 以新文件建立会话。
    [[nodiscard]] Result<InPlaceSaveResult> SaveInPlace(
        std::span<const MemberReplacement> replacements,
        std::span<const std::string> removals = {},
        const InPlaceSaveOptions& options = {}) const;
    // 保存成功后以写出的文件为新基线：核对实际路径、大小、SHA-256 与成员清单，
    // 成员直接取自本快照与同一组替换 / 删除，不再整体读取解压。
    // 含匿名成员的归档通过同一产物校验后重新打开。
    [[nodiscard]] Result<MapArchiveSession> AfterSave(
        const ArchiveSaveResult& written,
        std::span<const MemberReplacement> replacements,
        std::span<const std::string> removals = {}) const;

private:
    [[nodiscard]] static Result<MapArchiveSession> OpenImpl(const std::filesystem::path&, const ProjectSaveControl*, bool scriptAuditOnly);
    [[nodiscard]] Result<ArchiveSaveResult> WriteCandidate(
        const std::filesystem::path& target,
        std::span<const MemberReplacement> replacements,
        std::span<const std::string> removals, bool hashSource, bool preflight = false) const;
    // 只核对源路径仍指向快照对应的文件（不散列）。
    [[nodiscard]] Status VerifySourceIdentity() const;
    // 与 WriteCandidate 相同顺序的成员清单：删除后按原顺序，替换就地，新增追加。
    [[nodiscard]] std::vector<MapArchiveMember> MembersAfter(
        std::span<const MemberReplacement> replacements, std::span<const std::string> removals) const;

    std::filesystem::path sourcePath_;
    std::string sourceSha256_;
    std::uint32_t volumeSerial_ = 0;
    std::uint64_t fileId_ = 0;
    std::vector<std::byte> prefix_;
    std::vector<std::byte> listfile_;
    std::vector<MapArchiveMember> members_;
    std::vector<std::string> metadataMembers_;
    bool opaqueMembers_ = false;
    bool scriptAuditOnly_ = false;
    std::size_t opaqueMemberCount_ = 0;
};

} // namespace War3Map
