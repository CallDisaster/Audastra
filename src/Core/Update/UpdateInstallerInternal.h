#pragma once

#include "War3MapCore/Update/UpdateInstaller.h"
#include "War3MapCore/Update/UpdateArchive.h"
#include "UpdatePath.h"

#include <functional>
#include <map>
#include <set>

namespace War3Map::Update::Detail {

struct TreeInventory {
    std::map<std::wstring, UpdateFileEntry, WindowsPathLess> files;
    std::set<std::wstring, WindowsPathLess> directories;
    std::uint64_t bytes = 0;
    std::string sha256;
};
struct RootIdentity {
    std::uint64_t volume = 0, index = 0;
    [[nodiscard]] bool operator==(const RootIdentity&) const noexcept = default;
};
struct SignedInput {
    VerifiedUpdateManifest verified;
    std::vector<std::byte> raw, signature;
    FileReadLease manifestLease, signatureLease;
};
struct InstallJournal {
    std::string transactionId, phase;
    std::filesystem::path install, transaction, next, backup, failed, rollback;
    std::string currentVersion, targetVersion, sourceCommit, executable;
    std::string manifestSha256, baselineSha256, originalInventory, stagedInventory;
    std::string rollbackInventory, failedInventory;
    RootIdentity originalId, nextId, rollbackId;
    std::optional<UpdateProcessIdentity> child;
    std::uint64_t committedTime100ns = 0;
    std::uint32_t rollbackAttempt = 0;
    std::vector<std::string> startupArguments;
    std::string reasonCode, reason;
};
struct HealthChild {
    NativeHandle process, thread, pipeRead;
    UpdateProcessIdentity identity;
    std::string challenge;
};
class InstallMutex {
public:
    explicit InstallMutex(NativeHandle handle) : handle_(std::move(handle)) {}
    ~InstallMutex() { if (handle_.Valid()) ReleaseMutex(handle_.Get()); }
    InstallMutex(const InstallMutex&) = delete;
    InstallMutex& operator=(const InstallMutex&) = delete;
    InstallMutex(InstallMutex&&) noexcept = default;
    InstallMutex& operator=(InstallMutex&&) = delete;
private:
    NativeHandle handle_;
};
// 仅src内部的原创测试接缝；产品公开API与updater命令行不接受故障注入参数。
struct InstallerTestHooks {
    std::function<Status(std::string_view)> checkpoint;
    std::function<Result<bool>(const InstallJournal&)> restartProcessProbe;
    bool leaveForRecovery = false;
};
struct BackupCleanupTestHooks {
    std::function<Status(const std::filesystem::path&)> beforeDelete;
    // 原创回归在预检完成、文件删除接受及目录租约释放时制造精确竞争；不对外公开。
    std::function<Status(std::string_view, const std::filesystem::path&, const std::filesystem::path&)> checkpoint;
};
struct TreeDeletionReceipt {
    Status status;
    std::size_t filesMarkedForDeletion = 0;
    std::size_t directoriesMarkedForDeletion = 0;
    std::size_t fileAttributeChanges = 0;
    bool rootRemoved = false;
    [[nodiscard]] bool MutationStarted() const noexcept {
        return filesMarkedForDeletion != 0 || directoriesMarkedForDeletion != 0 || fileAttributeChanges != 0;
    }
};

[[nodiscard]] Result<TreeInventory> InspectUpdateTree(const std::filesystem::path& root);
[[nodiscard]] Result<RootIdentity> ReadRootIdentity(const std::filesystem::path& root);
[[nodiscard]] Status CopyInventory(const std::filesystem::path& source,
    const std::filesystem::path& destination, const TreeInventory& inventory, bool replace = false,
    const std::set<std::wstring, WindowsPathLess>& excluded = {});
[[nodiscard]] Status EnsureOwnedDirectories(const std::filesystem::path& root,
    const std::vector<std::string>& directories);
[[nodiscard]] Status WriteNewFile(const std::filesystem::path& file, std::span<const std::byte> bytes);
[[nodiscard]] Result<std::vector<std::byte>> ReadLeaseBytes(FileReadLease& lease);
[[nodiscard]] Result<SignedInput> ReadSignedInput(const std::filesystem::path& manifest,
    const std::filesystem::path& signature, const TrustedUpdateKey& key);
[[nodiscard]] bool Within(const std::filesystem::path& child, const std::filesystem::path& root);
[[nodiscard]] Status RenameOwnedRoot(const std::filesystem::path& from,
    const std::filesystem::path& to, const RootIdentity& expected);
[[nodiscard]] Status VerifyProgramTree(const std::filesystem::path& root, const VerifiedUpdateManifest& target);
[[nodiscard]] TreeDeletionReceipt DeleteOwnedTree(const std::filesystem::path& root, const TreeInventory& expected,
    const RootIdentity& expectedId, const BackupCleanupTestHooks& hooks);
[[nodiscard]] Status ExtractUpdatePackageToStage(const VerifiedUpdateManifest& target,
    FileReadLease& package, const std::filesystem::path& stage);

[[nodiscard]] Status WaitForParent(const UpdateProcessIdentity& identity, std::uint32_t timeoutMs);
[[nodiscard]] Status CheckUpdaterOutsideInstall(const std::filesystem::path& install);
[[nodiscard]] Result<HealthChild> LaunchHealthChild(const InstallJournal& journal,
    const VerifiedUpdateManifest& target);
[[nodiscard]] Status WaitForHealth(HealthChild& child, const InstallJournal& journal, std::uint32_t timeoutMs);
[[nodiscard]] Result<bool> ProcessStillRunning(const UpdateProcessIdentity& identity);
[[nodiscard]] Result<bool> InstallExecutableStillRunning(const InstallJournal& journal);
[[nodiscard]] Status RestartOldVersion(const InstallJournal& journal, const VerifiedUpdateManifest* baseline,
    const std::function<Result<bool>(const InstallJournal&)>& processProbe = {});

[[nodiscard]] Status WriteJournal(const InstallJournal& journal);
[[nodiscard]] Result<InstallJournal> ReadJournal(const std::filesystem::path& transaction);
[[nodiscard]] Result<InstallMutex> AcquireInstallMutex(const std::filesystem::path& install);
[[nodiscard]] UpdateInstallReceipt Receipt(const InstallJournal& journal, UpdateInstallOutcome outcome);
[[nodiscard]] Result<UpdateInstallReceipt> RollbackJournal(InstallJournal& journal,
    const VerifiedUpdateManifest& target, const VerifiedUpdateManifest* baseline, const InstallerTestHooks& hooks = {});
[[nodiscard]] Result<UpdateInstallReceipt> InstallUpdateWithHooks(const UpdateInstallRequest& request,
    const TrustedUpdateKey& key, const InstallerTestHooks& hooks);
[[nodiscard]] Result<UpdateBackupCleanupReceipt> CleanupUpdateBackupsWithHooks(const std::filesystem::path& install,
    const TrustedUpdateKey& key, const BackupCleanupTestHooks& hooks);

} // namespace War3Map::Update::Detail
