#include "UpdateInstallerInternal.h"

#include <algorithm>

namespace War3Map::Update::Detail {
namespace {

using R = Result<UpdateInstallReceipt>;
Result<std::optional<RootIdentity>> MaybeRoot(const std::filesystem::path& root) {
    if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return Result<std::optional<RootIdentity>>::Success(std::nullopt);
        return Result<std::optional<RootIdentity>>::Failure("Update", "UPDATE_ROOT_INSPECTION_FAILED", "更新目录存在性未知。");
    }
    const auto identity = ReadRootIdentity(root);
    if (!identity.Succeeded()) return {std::nullopt, identity.diagnostics};
    return Result<std::optional<RootIdentity>>::Success(*identity.value);
}
R Pending(InstallJournal& journal, std::string code, std::string reason) {
    if (journal.reasonCode.empty()) journal.reasonCode = std::move(code);
    if (journal.reason.empty()) journal.reason = std::move(reason);
    else if (journal.reason.size() + reason.size() < 4090 && journal.reason.find(reason) == std::string::npos) journal.reason += " " + reason;
    journal.phase = "rollback-pending";
    const auto written = WriteJournal(journal);
    if (!written.Succeeded() && journal.reason.size() < 3900) journal.reason += " 恢复状态未能持久写入，请保留原日志。";
    return R::Success(Receipt(journal, UpdateInstallOutcome::RollbackPending));
}
Status RecoveryProcessesExited(const InstallJournal& journal) {
    if (journal.child) {
        const auto running = ProcessStillRunning(*journal.child);
        if (!running.Succeeded() || *running.value)
            return Status::Failure("Update", "UPDATE_CHILD_STILL_RUNNING", "新版本仍运行或身份未知；待其自然退出后恢复，不结束进程。");
    }
    const auto running = InstallExecutableStillRunning(journal);
    if (!running.Succeeded() || *running.value)
        return Status::Failure("Update", "UPDATE_CHILD_STILL_RUNNING", "安装、备份或失败根可能仍有本程序实例；待自然退出后恢复。");
    return Status::Ok();
}
void RestartAndExplain(UpdateInstallReceipt& receipt, const InstallJournal& journal,
    const VerifiedUpdateManifest* baseline, const InstallerTestHooks& hooks) {
    const auto restarted = RestartOldVersion(journal, baseline, hooks.restartProcessProbe);
    receipt.oldVersionRestarted = restarted.Succeeded();
    if (restarted.Succeeded() || restarted.diagnostics.empty()) return;
    const auto& diagnostic = restarted.diagnostics.front();
    if (receipt.reasonCode.empty()) receipt.reasonCode = diagnostic.code;
    if (!receipt.reason.empty()) receipt.reason += ' ';
    receipt.reason += diagnostic.code + ": " + diagnostic.message;
}
R Restored(InstallJournal& journal, const VerifiedUpdateManifest* baseline, const InstallerTestHooks& hooks) {
    journal.phase = "rolled-back";
    const auto written = WriteJournal(journal);
    if (!written.Succeeded()) return Pending(journal, "UPDATE_JOURNAL_WRITE_FAILED", "原目录已恢复，但终态未能持久记录。");
    auto receipt = Receipt(journal, UpdateInstallOutcome::RolledBack);
    RestartAndExplain(receipt, journal, baseline, hooks);
    return R::Success(std::move(receipt));
}

} // namespace

Result<UpdateInstallReceipt> RollbackJournal(InstallJournal& journal,
    const VerifiedUpdateManifest& target, const VerifiedUpdateManifest* baseline, const InstallerTestHooks& hooks) {
    try {
        const auto installed = MaybeRoot(journal.install), backup = MaybeRoot(journal.backup), failed = MaybeRoot(journal.failed);
        if (!installed.Succeeded() || !backup.Succeeded() || !failed.Succeeded())
            return Pending(journal, "UPDATE_RECOVERY_IDENTITY_UNKNOWN", "目录身份未知，保留全部当前/备份/失败镜像。");
        if (*installed.value && **installed.value == journal.originalId && !*backup.value) {
            journal.phase = "abandoned";
            const auto written = WriteJournal(journal);
            if (!written.Succeeded()) return Pending(journal, "UPDATE_JOURNAL_WRITE_FAILED", "原目录未切换，但无法记录安全放弃。");
            auto receipt = Receipt(journal, UpdateInstallOutcome::Abandoned);
            receipt.userFilesPreserved = true;
            RestartAndExplain(receipt, journal, baseline, hooks);
            return R::Success(std::move(receipt));
        }
        if (!*backup.value || **backup.value != journal.originalId)
            return Pending(journal, "UPDATE_BACKUP_IDENTITY_CHANGED", "原备份身份不符，不覆盖或删除任何目录。");
        if (*installed.value && *failed.value && **installed.value == journal.rollbackId && **failed.value == journal.nextId) {
            // 最后一次回滚改名成功、终态日志未落盘的断电边界；三个目录均完整证明才收口。
            const auto source = InspectUpdateTree(journal.backup), restored = InspectUpdateTree(journal.install), failure = InspectUpdateTree(journal.failed);
            const auto remaining = MaybeRoot(journal.rollback);
            if ((journal.phase != "rollback-activate-pending" && journal.phase != "rollback-pending")
                || journal.rollbackInventory.empty() || journal.failedInventory.empty()
                || !source.Succeeded() || !restored.Succeeded() || !failure.Succeeded()
                || source.value->sha256 != journal.originalInventory || restored.value->sha256 != journal.rollbackInventory
                || failure.value->sha256 != journal.failedInventory || !remaining.Succeeded() || *remaining.value)
                return Pending(journal, "UPDATE_ROLLBACK_TERMINAL_UNKNOWN", "回滚已改名，但原备份、恢复树或失败树盘点不符，保留全部内容。");
            return Restored(journal, baseline, hooks);
        }
        if (!*installed.value && !*failed.value) {
            // 旧目录已改名而新目录未激活。直接搬回同一原目录；包括晚到的用户写入。
            const auto restored = RenameOwnedRoot(journal.backup, journal.install, journal.originalId);
            if (!restored.Succeeded()) return Pending(journal, "UPDATE_BACKUP_RESTORE_FAILED", "原备份暂时无法移回，等待占用自然结束。");
            journal.rollbackId = journal.originalId;
            return Restored(journal, baseline, hooks);
        }
        if (!*installed.value && *failed.value) {
            const auto exited = RecoveryProcessesExited(journal);
            if (!exited.Succeeded()) return Pending(journal, exited.diagnostics.front().code, exited.diagnostics.front().message);
            const auto source = InspectUpdateTree(journal.backup);
            if (!source.Succeeded() || source.value->sha256 != journal.originalInventory)
                return Pending(journal, "UPDATE_ROLLBACK_CONTENT_UNKNOWN", "两次回滚改名之间的原备份已变化或盘点未知，保留三个镜像。");
            const auto rollback = MaybeRoot(journal.rollback);
            const auto failureInventory = InspectUpdateTree(journal.failed);
            const auto rollbackInventory = InspectUpdateTree(journal.rollback);
            if (!rollback.Succeeded() || !*rollback.value || **rollback.value != journal.rollbackId
                || **failed.value != journal.nextId || !failureInventory.Succeeded() || !rollbackInventory.Succeeded()
                || failureInventory.value->sha256 != journal.failedInventory || rollbackInventory.value->sha256 != journal.rollbackInventory)
                return Pending(journal, "UPDATE_ROLLBACK_IMAGE_UNKNOWN", "回滚两个改名之间的镜像身份或盘点未知，保留它们。");
            const auto activated = RenameOwnedRoot(journal.rollback, journal.install, journal.rollbackId);
            if (!activated.Succeeded()) return Pending(journal, "UPDATE_ROLLBACK_ACTIVATE_FAILED", "完整回滚镜像暂时无法恢复到原路径。");
            return Restored(journal, baseline, hooks);
        }
        if (!*installed.value || **installed.value != journal.nextId || *failed.value)
            return Pending(journal, "UPDATE_CURRENT_IDENTITY_CHANGED", "当前目录或失败备份身份不确定，停止自动回滚。");
        const auto exited = RecoveryProcessesExited(journal);
        if (!exited.Succeeded()) return Pending(journal, exited.diagnostics.front().code, exited.diagnostics.front().message);
        const auto source = InspectUpdateTree(journal.backup), current = InspectUpdateTree(journal.install);
        if (!source.Succeeded() || !current.Succeeded() || source.value->sha256 != journal.originalInventory)
            return Pending(journal, "UPDATE_ROLLBACK_CONTENT_UNKNOWN", "备份已改变或当前用户树无法完整读取，保留全部内容。");
        const auto program = VerifyProgramTree(journal.install, target);
        if (!program.Succeeded()) return Pending(journal, "UPDATE_ROLLBACK_CONTENT_UNKNOWN", "当前程序内容不再匹配已激活签名，保留证据。");
        const auto oldRollback = MaybeRoot(journal.rollback);
        bool usePrepared = false;
        if (!oldRollback.Succeeded()) return Pending(journal, "UPDATE_ROLLBACK_IMAGE_UNKNOWN", "旧回滚镜像存在性未知。");
        if (*oldRollback.value) {
            const auto prepared = InspectUpdateTree(journal.rollback);
            usePrepared = **oldRollback.value == journal.rollbackId && prepared.Succeeded()
                && !journal.rollbackInventory.empty() && prepared.value->sha256 == journal.rollbackInventory
                && current.value->sha256 == journal.failedInventory;
            if (!usePrepared) {
                // 不删除半成品或被外部修改的旧镜像；精确新attempt保留全部旧证据。
                if (++journal.rollbackAttempt > 64) return Pending(journal, "UPDATE_ROLLBACK_ATTEMPT_BUDGET", "回滚尝试达到64次，保留镜像待检查。");
                const auto prefix = std::string(".audastra-update-") + journal.transactionId;
                journal.rollback = journal.install.parent_path() / std::filesystem::path(prefix + ".rollback-" + std::to_string(journal.rollbackAttempt));
                journal.rollbackId = {}; journal.rollbackInventory.clear();
            }
        }
        if (!usePrepared) {
            journal.phase = "rollback-pending";
            const auto beforeCreate = WriteJournal(journal);
            if (!beforeCreate.Succeeded()) return Pending(journal, "UPDATE_JOURNAL_WRITE_FAILED", "无法记录新恢复镜像，未修改当前树。");
            if (!CreateDirectoryW(journal.rollback.c_str(), nullptr))
                return Pending(journal, "UPDATE_ROLLBACK_STAGE_FAILED", "无法独占新建恢复镜像；当前/备份均保留。");
            const auto rollbackId = ReadRootIdentity(journal.rollback);
            if (!rollbackId.Succeeded()) return Pending(journal, "UPDATE_ROLLBACK_STAGE_FAILED", "恢复镜像身份未知。");
            journal.rollbackId = *rollbackId.value;
            const auto owned = WriteJournal(journal);
            if (!owned.Succeeded()) return Pending(journal, "UPDATE_JOURNAL_WRITE_FAILED", "无法记录恢复镜像身份。");
            const auto oldCopied = CopyInventory(journal.backup, journal.rollback, *source.value);
            if (!oldCopied.Succeeded()) return Pending(journal, "UPDATE_ROLLBACK_COPY_FAILED", "备份复制未完成，保留原数据和半成品。");
            std::set<std::wstring, WindowsPathLess> programs;
            for (const auto& file : target.Manifest().files) programs.emplace(file.path.begin(), file.path.end());
            const auto merged = CopyInventory(journal.install, journal.rollback, *current.value, true, programs);
            if (!merged.Succeeded()) return Pending(journal, "UPDATE_ROLLBACK_USER_MERGE_FAILED", "晚期用户文件发生冲突或无法复制，当前树保持。");
            const auto mergedInventory = InspectUpdateTree(journal.rollback);
            if (!mergedInventory.Succeeded()) return Pending(journal, "UPDATE_ROLLBACK_IMAGE_UNKNOWN", "恢复镜像盘点不完整。");
            // 逐项证明当前清单外用户内容进入恢复镜像；旧备份/失败新树仍保留。
            for (const auto& [key, file] : current.value->files) {
                if (programs.contains(key)) continue;
                const auto copied = mergedInventory.value->files.find(key);
                if (copied == mergedInventory.value->files.end() || copied->second.bytes != file.bytes || copied->second.sha256 != file.sha256)
                    return Pending(journal, "UPDATE_ROLLBACK_USER_MERGE_FAILED", "晚期用户内容未完整进入恢复镜像。");
            }
            if (!PreservedOriginsMatch(*current.value, *mergedInventory.value, programs))
                return Pending(journal, "UPDATE_ROLLBACK_USER_MERGE_FAILED", "晚期用户文件或目录的下载标记未完整进入恢复镜像。");
            journal.rollbackInventory = mergedInventory.value->sha256; journal.failedInventory = current.value->sha256;
        }
        const auto beforeCurrentRename = InspectUpdateTree(journal.install), beforeBackup = InspectUpdateTree(journal.backup);
        if (!beforeCurrentRename.Succeeded() || !beforeBackup.Succeeded() || beforeBackup.value->sha256 != journal.originalInventory
            || beforeCurrentRename.value->sha256 != journal.failedInventory)
            return Pending(journal, "UPDATE_ROLLBACK_INVENTORY_CHANGED", "恢复复制期间用户又写入，拒绝切换并保留所有版本。");
        journal.phase = "rollback-ready";
        auto written = WriteJournal(journal);
        if (!written.Succeeded()) return Pending(journal, "UPDATE_JOURNAL_WRITE_FAILED", "恢复镜像尚未持久记录。");
        journal.phase = "rollback-current-rename-pending";
        written = WriteJournal(journal);
        if (!written.Succeeded()) return Pending(journal, "UPDATE_JOURNAL_WRITE_FAILED", "当前目录改名前日志失败。");
        auto renamed = RenameOwnedRoot(journal.install, journal.failed, journal.nextId);
        if (!renamed.Succeeded()) return Pending(journal, "UPDATE_ROLLBACK_CURRENT_BUSY", "新版本当前目录仍被占用，未强制结束任何进程。");
        journal.phase = "rollback-current-renamed";
        written = WriteJournal(journal);
        const auto afterRename = InspectUpdateTree(journal.failed);
        if (!written.Succeeded() || !afterRename.Succeeded() || afterRename.value->sha256 != journal.failedInventory) {
            (void)RenameOwnedRoot(journal.failed, journal.install, journal.nextId);
            return Pending(journal, "UPDATE_ROLLBACK_INVENTORY_CHANGED", "新树改名后有变化，尝试移回并保留最新用户内容。");
        }
        if (hooks.checkpoint) {
            const auto checkpoint = hooks.checkpoint("after-rollback-current-rename");
            if (!checkpoint.Succeeded()) {
                if (hooks.leaveForRecovery) return {std::nullopt, checkpoint.diagnostics};
                return Pending(journal, "UPDATE_ROLLBACK_INTERRUPTED", "当前树已移到失败根，保留日志供下次按进程和完整盘点恢复。");
            }
        }
        journal.phase = "rollback-activate-pending"; written = WriteJournal(journal);
        if (!written.Succeeded()) {
            (void)RenameOwnedRoot(journal.failed, journal.install, journal.nextId);
            return Pending(journal, "UPDATE_JOURNAL_WRITE_FAILED", "恢复激活前日志失败，尝试移回新树。");
        }
        renamed = RenameOwnedRoot(journal.rollback, journal.install, journal.rollbackId);
        if (!renamed.Succeeded()) {
            (void)RenameOwnedRoot(journal.failed, journal.install, journal.nextId);
            return Pending(journal, "UPDATE_ROLLBACK_ACTIVATE_FAILED", "旧版本镜像未能激活，尝试保留新树于原位。");
        }
        if (hooks.checkpoint) {
            const auto checkpoint = hooks.checkpoint("after-rollback-activation");
            if (!checkpoint.Succeeded()) {
                if (hooks.leaveForRecovery) return {std::nullopt, checkpoint.diagnostics};
                return Pending(journal, "UPDATE_ROLLBACK_INTERRUPTED", "恢复镜像已改名，保留日志供下次按完整身份回读。");
            }
        }
        return Restored(journal, baseline, hooks);
    } catch (const std::exception&) { return Pending(journal, "UPDATE_RECOVERY_FAILED", "恢复异常中断，保留当前/备份/失败镜像。"); }
}

} // namespace War3Map::Update::Detail

namespace War3Map::Update {
namespace {

bool SameInstall(const std::filesystem::path& a, const std::filesystem::path& b) {
    return Detail::Within(a, b) && Detail::Within(b, a);
}
Result<Detail::SignedInput> JournalTarget(const Detail::InstallJournal& journal, const TrustedUpdateKey& key) {
    auto target = Detail::ReadSignedInput(journal.transaction / L"target-manifest.json", journal.transaction / L"target-manifest.sig", key);
    if (!target.Succeeded()) return target;
    if (target.value->verified.ManifestSha256() != journal.manifestSha256
        || target.value->verified.Manifest().version != journal.targetVersion
        || target.value->verified.Manifest().sourceCommit != journal.sourceCommit)
        return Result<Detail::SignedInput>::Failure("Update", "UPDATE_RECOVERY_MANIFEST_MISMATCH", "持久签名清单与事务身份不符。");
    return target;
}

} // namespace

Result<UpdateInstallReceipt> RecoverUpdate(const std::filesystem::path& transactionDirectory, const TrustedUpdateKey& key) {
    if (!key.configured) return Result<UpdateInstallReceipt>::Failure("Update", "UPDATE_TRUST_NOT_CONFIGURED", "尚未配置可信发布公钥，拒绝恢复更新。");
    try {
        auto journal = Detail::ReadJournal(transactionDirectory);
        if (!journal.Succeeded()) return {std::nullopt, std::move(journal.diagnostics)};
        auto mutex = Detail::AcquireInstallMutex(journal.value->install);
        if (!mutex.Succeeded()) return {std::nullopt, std::move(mutex.diagnostics)};
        const auto outside = Detail::CheckUpdaterOutsideInstall(journal.value->install);
        if (!outside.Succeeded()) return {std::nullopt, outside.diagnostics};
        auto target = JournalTarget(*journal.value, key);
        if (!target.Succeeded()) return {std::nullopt, std::move(target.diagnostics)};
        std::optional<Detail::SignedInput> baseline;
        if (!journal.value->baselineSha256.empty()) {
            auto old = Detail::ReadSignedInput(journal.value->transaction / L"baseline-manifest.json", journal.value->transaction / L"baseline-manifest.sig", key);
            if (!old.Succeeded()) return {std::nullopt, std::move(old.diagnostics)};
            if (old.value->verified.ManifestSha256() != journal.value->baselineSha256
                || old.value->verified.Manifest().version != journal.value->currentVersion
                || old.value->verified.TrustedKeySha256() != target.value->verified.TrustedKeySha256())
                return Result<UpdateInstallReceipt>::Failure("Update", "UPDATE_RECOVERY_BASELINE_MISMATCH", "恢复baseline签名/version/key不符。");
            baseline.emplace(std::move(*old.value));
        }
        if (journal.value->phase == "committed") {
            const auto current = Detail::ReadRootIdentity(journal.value->install);
            const auto program = Detail::VerifyProgramTree(journal.value->install, target.value->verified);
            if (!current.Succeeded() || *current.value != journal.value->nextId || !program.Succeeded())
                return Result<UpdateInstallReceipt>::Failure("Update", "UPDATE_COMMITTED_IDENTITY_CHANGED", "已commit安装的目录/程序身份已变化，未执行恢复。");
            return Result<UpdateInstallReceipt>::Success(Detail::Receipt(*journal.value, UpdateInstallOutcome::Committed));
        }
        if (journal.value->phase == "rolled-back" || journal.value->phase == "abandoned") {
            const auto current = Detail::ReadRootIdentity(journal.value->install);
            const auto expected = journal.value->phase == "abandoned" ? journal.value->originalId : journal.value->rollbackId;
            if (!current.Succeeded() || *current.value != expected)
                return Result<UpdateInstallReceipt>::Failure("Update", "UPDATE_RESTORED_IDENTITY_CHANGED", "已恢复目录身份变化，未再次改名。");
            auto receipt = Detail::Receipt(*journal.value, journal.value->phase == "abandoned" ? UpdateInstallOutcome::Abandoned : UpdateInstallOutcome::RolledBack);
            receipt.userFilesPreserved = true;
            return Result<UpdateInstallReceipt>::Success(std::move(receipt));
        }
        return Detail::RollbackJournal(*journal.value, target.value->verified, baseline ? &baseline->verified : nullptr);
    } catch (const std::exception&) { return Result<UpdateInstallReceipt>::Failure("Update", "UPDATE_RECOVERY_FAILED", "无法完整恢复，保留所有已有目录。"); }
}

Result<UpdateBaselineLocation> DiscoverInstalledUpdateBaseline(const std::filesystem::path& installDirectory,
    std::string_view currentVersion, const TrustedUpdateKey& key) {
    if (!key.configured) return Result<UpdateBaselineLocation>::Failure("Update", "UPDATE_TRUST_NOT_CONFIGURED", "尚未配置可信发布公钥，拒绝推断可信baseline。");
    try {
        const auto install = Detail::LockSafeDirectory(installDirectory);
        if (!install.Succeeded()) return {std::nullopt, install.diagnostics};
        std::optional<UpdateBaselineLocation> selected;
        std::uint64_t selectedTime = 0;
        std::size_t entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(install.value->path.parent_path())) {
            if (++entries > 100000) return Result<UpdateBaselineLocation>::Failure("Update", "UPDATE_PARENT_COUNT_BUDGET", "父目录超出baseline发现预算。");
            const auto name = entry.path().filename().native();
            if (!name.starts_with(L".audastra-update-") || !name.ends_with(L".transaction")) continue;
            const auto journal = Detail::ReadJournal(entry.path());
            if (!journal.Succeeded() || journal.value->phase != "committed" || journal.value->targetVersion != currentVersion
                || !SameInstall(journal.value->install, install.value->path)) continue;
            const auto target = JournalTarget(*journal.value, key);
            if (!target.Succeeded()) return {std::nullopt, target.diagnostics};
            const auto program = Detail::VerifyProgramTree(install.value->path, target.value->verified);
            if (!program.Succeeded()) return {std::nullopt, program.diagnostics};
            if (selected && selected->manifestSha256 != journal.value->manifestSha256)
                return Result<UpdateBaselineLocation>::Failure("Update", "UPDATE_BASELINE_AMBIGUOUS", "同版本存在不同签名baseline，拒绝推断。");
            if (!selected || journal.value->committedTime100ns > selectedTime) {
                selected = UpdateBaselineLocation{journal.value->transaction / L"target-manifest.json",
                    journal.value->transaction / L"target-manifest.sig", journal.value->transaction, journal.value->manifestSha256};
                selectedTime = journal.value->committedTime100ns;
            }
        }
        if (!selected) {
            // 首次离线portable的sidecar在payload ZIP冻结后旁置，不纳入自己的ZIP/hash清单。
            // 这里只读验签/全程序SHA；上层必须将原字节副本搬到安装树外，installer再验一次。
            const auto manifestPath = install.value->path / L"update-manifest.json";
            const auto signaturePath = install.value->path / L"update-manifest.json.sig";
            const auto manifestAttributes = GetFileAttributesW(manifestPath.c_str());
            const auto manifestError = GetLastError();
            const auto signatureAttributes = GetFileAttributesW(signaturePath.c_str());
            const auto signatureError = GetLastError();
            if (manifestAttributes == INVALID_FILE_ATTRIBUTES && signatureAttributes == INVALID_FILE_ATTRIBUTES
                && manifestError == ERROR_FILE_NOT_FOUND && signatureError == ERROR_FILE_NOT_FOUND)
                return Result<UpdateBaselineLocation>::Failure("Update", "UPDATE_BASELINE_NOT_FOUND", "未找到当前版本可信baseline；需当前Release的原字节清单及签名。");
            auto sidecar = Detail::ReadSignedInput(manifestPath, signaturePath, key);
            if (!sidecar.Succeeded()) return {std::nullopt, std::move(sidecar.diagnostics)};
            if (sidecar.value->verified.Manifest().version != currentVersion)
                return Result<UpdateBaselineLocation>::Failure("Update", "UPDATE_BASELINE_IDENTITY_MISMATCH",
                    "程序版本是 " + std::string(currentVersion) + "，同目录的签名版本清单却是 "
                    + sidecar.value->verified.Manifest().version + "。");
            const auto program = Detail::VerifyProgramTree(install.value->path, sidecar.value->verified);
            if (!program.Succeeded()) return {std::nullopt, program.diagnostics};
            selected = UpdateBaselineLocation{manifestPath, signaturePath, {}, sidecar.value->verified.ManifestSha256()};
        }
        return Result<UpdateBaselineLocation>::Success(std::move(*selected));
    } catch (const std::exception&) { return Result<UpdateBaselineLocation>::Failure("Update", "UPDATE_BASELINE_DISCOVERY_FAILED", "无法完整只读发现可信baseline。"); }
}

Result<UpdateBackupCleanupReceipt> Detail::CleanupUpdateBackupsWithHooks(const std::filesystem::path& installDirectory,
    const TrustedUpdateKey& key, const Detail::BackupCleanupTestHooks& hooks) {
    if (!key.configured) return Result<UpdateBackupCleanupReceipt>::Failure("Update", "UPDATE_TRUST_NOT_CONFIGURED", "尚未配置可信发布公钥，拒绝删除任何更新备份。");
    UpdateBackupCleanupReceipt receipt;
    try {
        auto normalized = Detail::NormalizeLocalPath(installDirectory, true);
        if (!normalized.Succeeded()) return {std::nullopt, std::move(normalized.diagnostics)};
        auto mutex = Detail::AcquireInstallMutex(*normalized.value);
        if (!mutex.Succeeded()) return {std::nullopt, std::move(mutex.diagnostics)};
        const auto parent = Detail::LockSafeDirectory(normalized.value->parent_path());
        if (!parent.Succeeded()) return {std::nullopt, parent.diagnostics};
        std::vector<Detail::InstallJournal> owned;
        std::size_t entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(parent.value->path)) {
            if (++entries > 100000) return Result<UpdateBackupCleanupReceipt>::Failure("Update", "UPDATE_PARENT_COUNT_BUDGET", "父目录超出清理预算，未继续删除。");
            const auto name = entry.path().filename().native();
            if (!name.starts_with(L".audastra-update-") || !name.ends_with(L".transaction")) continue;
            auto journal = Detail::ReadJournal(entry.path());
            if (!journal.Succeeded()) { ++receipt.retainedUnknown; continue; }
            if (!SameInstall(journal.value->install, *normalized.value)) continue;
            if (journal.value->phase != "committed" || !journal.value->committedTime100ns) { ++receipt.retainedUnknown; continue; }
            const auto target = JournalTarget(*journal.value, key);
            if (!target.Succeeded()) { ++receipt.retainedUnknown; continue; }
            if (GetFileAttributesW(journal.value->backup.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            const auto id = Detail::ReadRootIdentity(journal.value->backup);
            const auto inventory = Detail::InspectUpdateTree(journal.value->backup);
            if (!id.Succeeded() || *id.value != journal.value->originalId || !inventory.Succeeded()
                || inventory.value->sha256 != journal.value->originalInventory) { ++receipt.retainedUnknown; continue; }
            owned.push_back(std::move(*journal.value));
        }
        std::sort(owned.begin(), owned.end(), [](const auto& a, const auto& b) {
            return a.committedTime100ns != b.committedTime100ns ? a.committedTime100ns > b.committedTime100ns : a.transactionId > b.transactionId;
        });
        if (!owned.empty()) ++receipt.retainedBackups;
        receipt.partialBackupDirectories.reserve(owned.size());
        for (std::size_t index = 1; index < owned.size(); ++index) {
            const auto inventory = Detail::InspectUpdateTree(owned[index].backup);
            const auto id = Detail::ReadRootIdentity(owned[index].backup);
            if (!inventory.Succeeded() || !id.Succeeded() || *id.value != owned[index].originalId
                || inventory.value->sha256 != owned[index].originalInventory) { ++receipt.retainedUnknown; continue; }
            if (hooks.beforeDelete) {
                const auto checkpoint = hooks.beforeDelete(owned[index].backup);
                if (!checkpoint.Succeeded()) { ++receipt.retainedUnknown; continue; }
            }
            // 提前复制路径，避免删除开始后才因path分配异常丢失partial归属。
            auto partialPath = owned[index].backup;
            const auto deleted = Detail::DeleteOwnedTree(owned[index].backup, *inventory.value, owned[index].originalId, hooks);
            receipt.filesMarkedForDeletion += deleted.filesMarkedForDeletion;
            receipt.directoriesMarkedForDeletion += deleted.directoriesMarkedForDeletion;
            receipt.fileAttributeChanges += deleted.fileAttributeChanges;
            if (deleted.status.Succeeded() && deleted.rootRemoved) {
                ++receipt.removedBackups;
            } else if (!deleted.MutationStarted()) {
                ++receipt.retainedUnknown; // 全部预检完成前拒绝，该备份未被本次清理修改。
            } else {
                ++receipt.partiallyCleanedBackups;
                receipt.partialBackupDirectories.push_back(std::move(partialPath));
                auto failed = Result<UpdateBackupCleanupReceipt>::Failure("Update", "UPDATE_CLEANUP_PARTIAL",
                    "旧备份清理已发生部分修改，剩余内容保留；删除标记计数不表示物理空间已释放。");
                failed.diagnostics.insert(failed.diagnostics.end(), deleted.status.diagnostics.begin(), deleted.status.diagnostics.end());
                failed.value.emplace(std::move(receipt));
                return failed; // 部分失败后不继续删除其它旧备份。
            }
        }
        // 原raw签名txn目录继续保留，为后续升级提供可信baseline和审计证据。
        return Result<UpdateBackupCleanupReceipt>::Success(receipt);
    } catch (const std::exception&) {
        auto failed = Result<UpdateBackupCleanupReceipt>::Failure("Update",
            receipt.partiallyCleanedBackups ? "UPDATE_CLEANUP_PARTIAL" : "UPDATE_CLEANUP_FAILED",
            "清理未完整执行，已记录的操作计数保留，未继续修改剩余备份和事务。");
        failed.value.emplace(std::move(receipt));
        return failed;
    }
}

Result<UpdateBackupCleanupReceipt> CleanupUpdateBackups(const std::filesystem::path& installDirectory, const TrustedUpdateKey& key) {
    return Detail::CleanupUpdateBackupsWithHooks(installDirectory, key, {});
}

} // namespace War3Map::Update
