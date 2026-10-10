#include "UpdateInstallerInternal.h"
#include "War3MapCore/Update/SemanticVersion.h"

#include <sodium.h>

#include <algorithm>
#include <array>

namespace War3Map::Update::Detail {
namespace {

using R = Result<UpdateInstallReceipt>;
Status Failure(std::string code, std::string message) {
    return Status::Failure("Update", std::move(code), std::move(message));
}
void Reason(InstallJournal& journal, const Status& status) {
    if (!status.diagnostics.empty()) {
        journal.reasonCode = status.diagnostics.front().code;
        journal.reason = status.diagnostics.front().message;
    }
}
Status RejectPendingTransactions(const std::filesystem::path& install) {
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(install.parent_path())) {
        if (++entries > 100000) return Failure("UPDATE_PARENT_COUNT_BUDGET", "安装父目录过大，无法完整确认未恢复事务。");
        const auto name = entry.path().filename().native();
        if (!name.starts_with(L".audastra-update-") || !name.ends_with(L".transaction")) continue;
        auto journal = ReadJournal(entry.path());
        if (!journal.Succeeded()) return Failure("UPDATE_RECOVERY_REQUIRED", "存在未知更新事务记录，请先恢复，未启动新的替换。");
        if (Within(journal.value->install, install) && Within(install, journal.value->install)
            && journal.value->phase != "committed" && journal.value->phase != "rolled-back" && journal.value->phase != "abandoned")
            return Failure("UPDATE_RECOVERY_REQUIRED", "该安装目录有未完成更新，必须先按精确事务记录恢复。");
    }
    return Status::Ok();
}
Status PreserveCheck(const TreeInventory& original, const TreeInventory& stage,
    const std::set<std::wstring, WindowsPathLess>& targets) {
    std::size_t expectedFiles = targets.size();
    for (const auto& [key, file] : original.files) {
        if (targets.contains(key)) continue;
        ++expectedFiles;
        const auto current = stage.files.find(key);
        if (current == stage.files.end() || current->second.sha256 != file.sha256 || current->second.bytes != file.bytes)
            return Failure("UPDATE_STAGE_USER_CONTENT_MISMATCH", "新镜像未完整保留原用户文件，不切换目录。");
    }
    if (stage.files.size() != expectedFiles) return Failure("UPDATE_STAGE_EXTRA_CONTENT", "镜像出现未知文件，不切换目录。");
    auto expectedDirectories = original.directories;
    for (const auto& file : targets) for (auto end = file.find(L'/'); end != std::wstring::npos; end = file.find(L'/', end + 1))
        expectedDirectories.insert(file.substr(0, end));
    if (stage.directories != expectedDirectories) return Failure("UPDATE_STAGE_DIRECTORY_MISMATCH", "镜像缺少空用户目录或出现未知目录。");
    return Status::Ok();
}

} // namespace

Result<UpdateInstallReceipt> InstallUpdateWithHooks(const UpdateInstallRequest& request,
    const TrustedUpdateKey& key, const InstallerTestHooks& hooks) {
    std::optional<InstallJournal> journal;
    std::optional<SignedInput> target, baseline;
    std::optional<InstallMutex> installMutex;
    try {
        if (!key.configured) return R::Failure("Update", "UPDATE_TRUST_NOT_CONFIGURED", "尚未配置可信发布公钥，拒绝安装。");
        if (!ParseSemanticVersion(request.currentVersion).Succeeded()
            || !ValidateUpdateRelativePath(request.executableRelativePath).Succeeded() || !request.executableRelativePath.ends_with(".exe")
            || request.parentExitTimeoutMs == 0 || request.parentExitTimeoutMs > 300000
            || request.healthTimeoutMs == 0 || request.healthTimeoutMs > 300000 || request.startupArguments.size() > 64)
            return R::Failure("Update", "UPDATE_INSTALL_ARGUMENT_INVALID", "安装参数、版本、入口或有界等待时间无效。");
        std::size_t argumentBytes = 0;
        for (const auto& argument : request.startupArguments) {
            if (argument.size() > 1024 || argument.find('\0') != std::string::npos || argument.starts_with("--update-health-"))
                return R::Failure("Update", "UPDATE_INSTALL_ARGUMENT_INVALID", "启动参数超限或试图覆盖健康协议。");
            argumentBytes += argument.size();
        }
        auto normalized = NormalizeLocalPath(request.installDirectory, true);
        if (!normalized.Succeeded()) return {std::nullopt, std::move(normalized.diagnostics)};
        const auto install = *normalized.value;
        if (install == install.root_path() || install.filename().native().starts_with(L".audastra-update-")
            || install.native().size() + argumentBytes + request.executableRelativePath.size() > 30000)
            return R::Failure("Update", "UPDATE_INSTALL_ROOT_REJECTED", "安装根或完整启动命令范围无效。");
        const auto outside = CheckUpdaterOutsideInstall(install);
        if (!outside.Succeeded()) return {std::nullopt, outside.diagnostics};
        auto mutex = AcquireInstallMutex(install);
        if (!mutex.Succeeded()) return {std::nullopt, std::move(mutex.diagnostics)};
        installMutex.emplace(std::move(*mutex.value));
        auto signedTarget = ReadSignedInput(request.manifestPath, request.signaturePath, key);
        if (!signedTarget.Succeeded()) return {std::nullopt, std::move(signedTarget.diagnostics)};
        target.emplace(std::move(*signedTarget.value));
        if (std::none_of(target->verified.Manifest().files.begin(), target->verified.Manifest().files.end(),
            [&](const auto& file) { return file.path == request.executableRelativePath; }))
            return R::Failure("Update", "UPDATE_EXECUTABLE_UNSIGNED", "新版本启动入口不在签名程序文件清单。");
        if (request.baselineManifestPath.empty() != request.baselineSignaturePath.empty())
            return R::Failure("Update", "UPDATE_BASELINE_INPUT_INCOMPLETE", "旧可信清单与签名必须成对提供。");
        if (!request.baselineManifestPath.empty()) {
            auto signedBaseline = ReadSignedInput(request.baselineManifestPath, request.baselineSignaturePath, key);
            if (!signedBaseline.Succeeded()) return {std::nullopt, std::move(signedBaseline.diagnostics)};
            baseline.emplace(std::move(*signedBaseline.value));
        }
        auto package = OpenSafeReadFile(request.packagePath, kMaxUpdatePackageBytes);
        if (!package.Succeeded()) return {std::nullopt, std::move(package.diagnostics)};
        std::vector<std::filesystem::path> inputs{target->manifestLease.path, target->signatureLease.path, package.value->path};
        if (baseline) { inputs.push_back(baseline->manifestLease.path); inputs.push_back(baseline->signatureLease.path); }
        for (const auto& input : inputs) if (Within(input, install))
            return R::Failure("Update", "UPDATE_INPUT_INSIDE_INSTALL", "签名输入或包位于待改名安装树内，拒绝替换。");
        const auto verifiedPackage = VerifyUpdatePackage(target->verified, package.value->path);
        if (!verifiedPackage.Succeeded()) return {std::nullopt, verifiedPackage.diagnostics};
        if (request.parentProcess) {
            const auto waited = WaitForParent(*request.parentProcess, request.parentExitTimeoutMs);
            if (!waited.Succeeded()) return {std::nullopt, waited.diagnostics};
        }
        const auto parent = LockSafeDirectory(install.parent_path());
        if (!parent.Succeeded()) return {std::nullopt, parent.diagnostics};
        const auto pending = RejectPendingTransactions(install);
        if (!pending.Succeeded()) return {std::nullopt, pending.diagnostics};
        const auto plan = BuildUpdateInstallPlan(target->verified, baseline ? &baseline->verified : nullptr, install, request.currentVersion);
        if (!plan.Succeeded()) return {std::nullopt, plan.diagnostics};
        const auto original = InspectUpdateTree(install);
        const auto identity = ReadRootIdentity(install);
        if (!original.Succeeded()) return {std::nullopt, original.diagnostics};
        if (!identity.Succeeded()) return {std::nullopt, identity.diagnostics};
        if (original.value->sha256 != plan.value->inventorySha256)
            return R::Failure("Update", "UPDATE_INVENTORY_CHANGED", "安装树已在计划后变化，拒绝替换。");
        InstallJournal state;
        std::array<unsigned char, 16> transactionBytes{}; std::array<char, 33> transactionText{};
        randombytes_buf(transactionBytes.data(), transactionBytes.size());
        sodium_bin2hex(transactionText.data(), transactionText.size(), transactionBytes.data(), transactionBytes.size());
        state.transactionId = transactionText.data(); state.install = install; state.phase = "preparing";
        const auto prefix = std::string(".audastra-update-") + state.transactionId;
        state.transaction = install.parent_path() / std::filesystem::path(prefix + ".transaction");
        state.next = install.parent_path() / std::filesystem::path(prefix + ".next");
        state.backup = install.parent_path() / std::filesystem::path(prefix + ".backup");
        state.failed = install.parent_path() / std::filesystem::path(prefix + ".failed");
        state.rollback = install.parent_path() / std::filesystem::path(prefix + ".rollback");
        state.currentVersion = request.currentVersion; state.targetVersion = target->verified.Manifest().version;
        state.sourceCommit = target->verified.Manifest().sourceCommit; state.executable = request.executableRelativePath;
        state.manifestSha256 = target->verified.ManifestSha256(); state.originalInventory = original.value->sha256;
        state.originalId = *identity.value; state.startupArguments = request.startupArguments;
        if (baseline) state.baselineSha256 = baseline->verified.ManifestSha256();
        const auto running = InstallExecutableStillRunning(state);
        if (!running.Succeeded()) return {std::nullopt, running.diagnostics};
        if (*running.value) return R::Failure("Update", "UPDATE_INSTALL_EXECUTABLE_RUNNING", "安装树仍有本程序进程运行，不强制结束。");
        if (!CreateDirectoryW(state.transaction.c_str(), nullptr)) return R::Failure("Update", "UPDATE_TRANSACTION_CREATE_FAILED",
            "无法独占新建同卷更新事务目录；请检查安装目录写入权限。");
        journal.emplace(std::move(state));
        for (const auto& [name, bytes] : std::array{
            std::pair{L"target-manifest.json", std::span<const std::byte>(target->raw)},
            std::pair{L"target-manifest.sig", std::span<const std::byte>(target->signature)}}) {
            const auto written = WriteNewFile(journal->transaction / name, bytes);
            if (!written.Succeeded()) return {std::nullopt, written.diagnostics};
        }
        if (baseline) {
            const auto raw = WriteNewFile(journal->transaction / L"baseline-manifest.json", baseline->raw);
            if (!raw.Succeeded()) return {std::nullopt, raw.diagnostics};
            const auto sig = WriteNewFile(journal->transaction / L"baseline-manifest.sig", baseline->signature);
            if (!sig.Succeeded()) return {std::nullopt, sig.diagnostics};
        }
        auto persisted = WriteJournal(*journal);
        if (!persisted.Succeeded()) return {std::nullopt, persisted.diagnostics};
        const auto abort = [&](const Status& error) -> R {
            Reason(*journal, error);
            if (hooks.leaveForRecovery) return {std::nullopt, error.diagnostics};
            return RollbackJournal(*journal, target->verified, baseline ? &baseline->verified : nullptr, hooks);
        };
        const auto checkpoint = [&](std::string_view phase) { return hooks.checkpoint ? hooks.checkpoint(phase) : Status::Ok(); };
        if (!CreateDirectoryW(journal->next.c_str(), nullptr)) return abort(Failure("UPDATE_STAGE_CREATE_FAILED", "无法独占创建同卷镜像目录。"));
        const auto nextIdentity = ReadRootIdentity(journal->next);
        if (!nextIdentity.Succeeded()) return abort({nextIdentity.diagnostics});
        journal->nextId = *nextIdentity.value;
        std::set<std::wstring, WindowsPathLess> targets;
        std::vector<std::string> directories;
        for (const auto& file : target->verified.Manifest().files) {
            targets.emplace(file.path.begin(), file.path.end());
            for (auto end = file.path.find('/'); end != std::string::npos; end = file.path.find('/', end + 1))
                directories.push_back(file.path.substr(0, end));
        }
        auto copied = CopyInventory(install, journal->next, *original.value, false, targets);
        if (!copied.Succeeded()) return abort(copied);
        copied = EnsureOwnedDirectories(journal->next, directories);
        if (!copied.Succeeded()) return abort(copied);
        const auto extracted = ExtractUpdatePackageToStage(target->verified, *package.value, journal->next);
        if (!extracted.Succeeded()) return abort(extracted);
        const auto staged = InspectUpdateTree(journal->next);
        if (!staged.Succeeded()) return abort({staged.diagnostics});
        const auto preserved = PreserveCheck(*original.value, *staged.value, targets);
        if (!preserved.Succeeded()) return abort(preserved);
        const auto afterCopy = InspectUpdateTree(install);
        if (!afterCopy.Succeeded()) return abort({afterCopy.diagnostics});
        if (afterCopy.value->sha256 != journal->originalInventory)
            return abort(Failure("UPDATE_INVENTORY_CHANGED", "用户文件在镜像构建期间变化，保留原树并拒绝替换。"));
        journal->stagedInventory = staged.value->sha256; journal->phase = "staged";
        persisted = WriteJournal(*journal); if (!persisted.Succeeded()) return abort(persisted);
        auto hook = checkpoint("after-stage"); if (!hook.Succeeded()) return abort(hook);
        journal->phase = "old-rename-pending"; persisted = WriteJournal(*journal); if (!persisted.Succeeded()) return abort(persisted);
        hook = checkpoint("before-old-rename"); if (!hook.Succeeded()) return abort(hook);
        const auto beforeRename = InspectUpdateTree(install);
        if (!beforeRename.Succeeded() || beforeRename.value->sha256 != journal->originalInventory)
            return abort(Failure("UPDATE_INVENTORY_CHANGED", "原树在切换前变化，未覆盖用户内容。"));
        auto renamed = RenameOwnedRoot(install, journal->backup, journal->originalId);
        if (!renamed.Succeeded()) return abort(renamed);
        journal->phase = "old-renamed"; persisted = WriteJournal(*journal); if (!persisted.Succeeded()) return abort(persisted);
        hook = checkpoint("after-old-rename"); if (!hook.Succeeded()) return abort(hook);
        const auto backup = InspectUpdateTree(journal->backup);
        if (!backup.Succeeded() || backup.value->sha256 != journal->originalInventory)
            return abort(Failure("UPDATE_INVENTORY_CHANGED", "旧目录改名后盘点不符，恢复它并保留新增内容。"));
        journal->phase = "activate-pending"; persisted = WriteJournal(*journal); if (!persisted.Succeeded()) return abort(persisted);
        hook = checkpoint("before-activation"); if (!hook.Succeeded()) return abort(hook);
        renamed = RenameOwnedRoot(journal->next, install, journal->nextId);
        if (!renamed.Succeeded()) return abort(renamed);
        journal->phase = "activated"; persisted = WriteJournal(*journal); if (!persisted.Succeeded()) return abort(persisted);
        hook = checkpoint("after-activation"); if (!hook.Succeeded()) return abort(hook);
        const auto activated = InspectUpdateTree(install);
        if (!activated.Succeeded() || activated.value->sha256 != journal->stagedInventory)
            return abort(Failure("UPDATE_ACTIVATION_CHANGED", "激活镜像在真实启动前变化，未授予健康。"));
        journal->phase = "launch-pending"; persisted = WriteJournal(*journal); if (!persisted.Succeeded()) return abort(persisted);
        auto child = LaunchHealthChild(*journal, target->verified);
        if (!child.Succeeded()) return abort({child.diagnostics});
        journal->child = child.value->identity; journal->phase = "awaiting-health";
        persisted = WriteJournal(*journal); if (!persisted.Succeeded()) return abort(persisted);
        const auto healthy = WaitForHealth(*child.value, *journal, request.healthTimeoutMs);
        if (!healthy.Succeeded()) return abort(healthy);
        const auto finalProgram = VerifyProgramTree(install, target->verified);
        if (!finalProgram.Succeeded()) return abort(finalProgram);
        FILETIME now{}; GetSystemTimeAsFileTime(&now);
        journal->committedTime100ns = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        journal->phase = "committed";
        persisted = WriteJournal(*journal); if (!persisted.Succeeded()) return abort(persisted);
        return R::Success(Receipt(*journal, UpdateInstallOutcome::Committed));
    } catch (const std::exception&) {
        if (journal && target) {
            journal->reasonCode = "UPDATE_INSTALL_FAILED"; journal->reason = "更新异常中断，按持久身份恢复并保留用户内容。";
            if (!hooks.leaveForRecovery) return RollbackJournal(*journal, target->verified, baseline ? &baseline->verified : nullptr, hooks);
        }
        return R::Failure("Update", "UPDATE_INSTALL_FAILED", "更新未完整执行；保留全部已有目录和事务证据。");
    }
}

} // namespace War3Map::Update::Detail

namespace War3Map::Update {
Result<UpdateInstallReceipt> InstallUpdate(const UpdateInstallRequest& request, const TrustedUpdateKey& trustedKey) {
    return Detail::InstallUpdateWithHooks(request, trustedKey, {});
}
} // namespace War3Map::Update
