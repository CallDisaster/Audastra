#pragma once

#include "War3MapCore/Update/UpdatePlan.h"

namespace War3Map::Update {

struct UpdateProcessIdentity {
    std::uint32_t processId = 0;
    std::uint64_t creationTime100ns = 0;
    [[nodiscard]] bool operator==(const UpdateProcessIdentity&) const noexcept = default;
};

enum class UpdateInstallOutcome { Committed, RolledBack, RollbackPending, Abandoned };

struct UpdateInstallRequest {
    std::filesystem::path installDirectory;
    std::filesystem::path packagePath;
    std::filesystem::path manifestPath;
    std::filesystem::path signaturePath;
    std::filesystem::path baselineManifestPath;
    std::filesystem::path baselineSignaturePath;
    std::string currentVersion;
    std::string executableRelativePath = "war3-map-editor.exe";
    std::optional<UpdateProcessIdentity> parentProcess;
    std::uint32_t parentExitTimeoutMs = 120000;
    std::uint32_t healthTimeoutMs = 60000;
    std::vector<std::string> startupArguments;
};

struct UpdateInstallReceipt {
    UpdateInstallOutcome outcome = UpdateInstallOutcome::Abandoned;
    std::string transactionId;
    std::string currentVersion;
    std::string targetVersion;
    std::filesystem::path transactionDirectory;
    std::filesystem::path installDirectory;
    std::filesystem::path backupDirectory;
    std::filesystem::path failedDirectory;
    std::optional<UpdateProcessIdentity> childProcess;
    bool healthVerified = false;
    bool userFilesPreserved = false;
    bool oldVersionRestarted = false;
    std::string reasonCode;
    std::string reason;
};

// PID与creation time共同绑定；0表示读取当前进程。仅查询，不结束任何进程。
[[nodiscard]] Result<UpdateProcessIdentity> ReadUpdateProcessIdentity(std::uint32_t processId = 0);
// installer重新锁定原始签名输入/ZIP和当前树，不接受外部已验证DTO或过期计划。
// 调用程序及其已加载模块必须在安装根外；所有input也必须在安装根外。
[[nodiscard]] Result<UpdateInstallReceipt> InstallUpdate(
    const UpdateInstallRequest& request, const TrustedUpdateKey& trustedKey);
// 只恢复精确transaction目录。child仍运行或身份/内容未知时返回RollbackPending并保留树。
[[nodiscard]] Result<UpdateInstallReceipt> RecoverUpdate(
    const std::filesystem::path& transactionDirectory, const TrustedUpdateKey& trustedKey);

struct UpdateBaselineLocation {
    std::filesystem::path manifestPath;
    std::filesystem::path signaturePath;
    std::filesystem::path transactionDirectory;
    std::string manifestSha256;
};
// 只读发现先前commit的原始签名baseline；是路径提示，安装器仍重新读取验证。
// 无ownedtxn时可读取根旁置update-manifest.json/.sig，并核所有signed当前文件SHA。
// 根旁置输入仅供只读发现；上层必须复制到安装树外，安装入口仍重新验签。
// 无可信pair明确返回UPDATE_BASELINE_NOT_FOUND，绝不使用未签名portable manifest。
[[nodiscard]] Result<UpdateBaselineLocation> DiscoverInstalledUpdateBaseline(
    const std::filesystem::path& installDirectory, std::string_view currentVersion, const TrustedUpdateKey& trustedKey);

struct UpdateBackupCleanupReceipt {
    std::size_t removedBackups = 0;
    std::size_t retainedBackups = 0;
    // 删除开始前拒绝的未知备份；本次清理没有修改这些目录。
    std::size_t retainedUnknown = 0;
    // 已有原生修改、随后未能完整删除的自有备份；不得混计为retainedUnknown。
    std::size_t partiallyCleanedBackups = 0;
    // 成功接受的原生删除标记数，目录数包含备份根；不表示物理空间已释放。
    std::size_t filesMarkedForDeletion = 0;
    std::size_t directoriesMarkedForDeletion = 0;
    // 成功写入文件属性的次数，包含删除失败时成功恢复原属性的写入。
    std::size_t fileAttributeChanges = 0;
    // 仅列出本次已核为自有、但清理未完成的备份原路径，不列安装根或普通未知目录。
    std::vector<std::filesystem::path> partialBackupDirectories;
};
// 下次真实启动后调用；保留最近1份已commit备份。未知/改变/正在恢复的目录一律保留。
// 部分清理返回value和Error UPDATE_CLEANUP_PARTIAL；Succeeded为false，value仍有准确回执。
// removedBackups仅计已确认根路径消失的备份，不把接受删除标记当成根已移除。
[[nodiscard]] Result<UpdateBackupCleanupReceipt> CleanupUpdateBackups(
    const std::filesystem::path& installDirectory, const TrustedUpdateKey& trustedKey);

// Qt在真实event loop与MainWindow ready后回复。pipe仅由updater HANDLE_LIST显式继承。
// 回复含transaction、challenge、自身PID/creationTime、编译version/commit；写完关闭pipe。
[[nodiscard]] Status ReplyUpdateHealth(std::uintptr_t pipeWriteHandle,
    std::string_view transactionId, std::string_view challengeHex,
    std::string_view compiledVersion, std::string_view compiledCommit);

} // namespace War3Map::Update
