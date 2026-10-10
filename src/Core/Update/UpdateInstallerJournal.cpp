#include "UpdateInstallerInternal.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Update/SemanticVersion.h"
#include "../Serialization/StrictJson.h"

#include <sodium.h>

#include <algorithm>
#include <array>

namespace War3Map::Update::Detail {
namespace {

using Json = nlohmann::json;
bool Hex(std::string_view text, std::size_t size) {
    return text.size() == size && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
Json Root(const RootIdentity& identity) { return {{"volume", identity.volume}, {"index", identity.index}}; }
RootIdentity ReadRoot(const Json& value) {
    if (!value.is_object() || value.size() != 2 || !value.at("volume").is_number_unsigned()
        || !value.at("index").is_number_unsigned()) throw std::runtime_error("invalid root identity");
    return {value.at("volume").get<std::uint64_t>(), value.at("index").get<std::uint64_t>()};
}
const std::array phases{"preparing", "staged", "old-rename-pending", "old-renamed", "activate-pending", "activated",
    "launch-pending", "awaiting-health", "committed", "rollback-pending", "rollback-ready",
    "rollback-current-rename-pending", "rollback-current-renamed", "rollback-activate-pending", "rolled-back", "abandoned"};

} // namespace

Status WriteJournal(const InstallJournal& journal) {
    try {
        Json child = nullptr;
        if (journal.child) child = {{"pid", journal.child->processId}, {"creation_time_100ns", journal.child->creationTime100ns}};
        const Json object{{"format", "audastra-update-journal"}, {"schema", 1}, {"transaction", journal.transactionId},
            {"install", IO::PathUtf8(journal.install)}, {"phase", journal.phase}, {"current_version", journal.currentVersion},
            {"target_version", journal.targetVersion}, {"source_commit", journal.sourceCommit}, {"executable", journal.executable},
            {"manifest_sha256", journal.manifestSha256}, {"baseline_sha256", journal.baselineSha256},
            {"original_inventory", journal.originalInventory}, {"staged_inventory", journal.stagedInventory},
            {"rollback_inventory", journal.rollbackInventory}, {"failed_inventory", journal.failedInventory},
            {"original_id", Root(journal.originalId)}, {"next_id", Root(journal.nextId)}, {"rollback_id", Root(journal.rollbackId)},
            {"child", std::move(child)}, {"committed_time_100ns", journal.committedTime100ns},
            {"rollback_attempt", journal.rollbackAttempt},
            {"startup_arguments", journal.startupArguments}, {"reason_code", journal.reasonCode}, {"reason", journal.reason}};
        const auto bytes = object.dump();
        std::array<unsigned char, 8> random{};
        std::array<char, 17> suffix{};
        if (sodium_init() < 0) return Status::Failure("Update", "UPDATE_RANDOM_FAILED", "无法初始化更新记录随机源。");
        randombytes_buf(random.data(), random.size());
        sodium_bin2hex(suffix.data(), suffix.size(), random.data(), random.size());
        const auto temporary = journal.transaction / std::filesystem::path(std::string("journal-") + suffix.data() + ".tmp");
        const auto written = WriteNewFile(temporary, std::as_bytes(std::span(bytes.data(), bytes.size())));
        if (!written.Succeeded()) return written;
        const auto parent = LockSafeDirectory(journal.transaction);
        if (!parent.Succeeded()) return {parent.diagnostics};
        const auto final = journal.transaction / L"journal.json";
        const auto attributes = GetFileAttributesW(final.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            const auto old = OpenSafeReadFile(final, kMaxUpdateManifestBytes);
            if (!old.Succeeded()) return {old.diagnostics};
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return Status::Failure("Update", "UPDATE_JOURNAL_WRITE_FAILED", "无法确认旧事务日志身份。");
        }
        if (!MoveFileExW(temporary.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return Status::Failure("Update", "UPDATE_JOURNAL_WRITE_FAILED", "事务日志未能持久替换；保留全部恢复证据。");
        return Status::Ok();
    } catch (const std::exception&) {
        return Status::Failure("Update", "UPDATE_JOURNAL_WRITE_FAILED", "无法完整持久记录更新阶段。");
    }
}

Result<InstallJournal> ReadJournal(const std::filesystem::path& transaction) {
    using R = Result<InstallJournal>;
    try {
        const auto normalized = LockSafeDirectory(transaction);
        if (!normalized.Succeeded()) return {std::nullopt, normalized.diagnostics};
        auto lease = OpenSafeReadFile(normalized.value->path / L"journal.json", kMaxUpdateManifestBytes);
        if (!lease.Succeeded()) return {std::nullopt, std::move(lease.diagnostics)};
        const auto bytes = ReadLeaseBytes(*lease.value);
        if (!bytes.Succeeded()) return {std::nullopt, bytes.diagnostics};
        Serialization::StrictJsonLimits limits; limits.maxDepth = 8; limits.maxNodes = 2048; limits.rejectFloatingPoint = true;
        const auto parsed = Serialization::ParseStrictJson(*bytes.value, limits);
        if (!parsed.Succeeded()) return {std::nullopt, parsed.diagnostics};
        const auto& object = *parsed.value;
        const std::set<std::string> allowed{"format", "schema", "transaction", "install", "phase", "current_version",
            "target_version", "source_commit", "executable", "manifest_sha256", "baseline_sha256", "original_inventory",
            "staged_inventory", "rollback_inventory", "failed_inventory", "original_id", "next_id", "rollback_id", "child",
            "committed_time_100ns", "rollback_attempt", "startup_arguments", "reason_code", "reason"};
        if (!object.is_object() || object.size() != allowed.size()) throw std::runtime_error("journal schema");
        for (const auto& [key, value] : object.items()) { (void)value; if (!allowed.contains(key)) throw std::runtime_error("journal field"); }
        if (object.at("format") != "audastra-update-journal" || object.at("schema") != 1) throw std::runtime_error("journal format");
        InstallJournal journal;
        journal.transactionId = object.at("transaction").get<std::string>();
        if (!Hex(journal.transactionId, 32)) throw std::runtime_error("journal transaction");
        const auto prefix = std::string(".audastra-update-") + journal.transactionId;
        if (normalized.value->path.filename().native() != std::filesystem::path(prefix + ".transaction").native())
            throw std::runtime_error("journal owned directory");
        journal.transaction = normalized.value->path;
        const auto installText = object.at("install").get<std::string>();
        const auto install = NormalizeLocalPath(std::filesystem::path(std::u8string_view(
            reinterpret_cast<const char8_t*>(installText.data()), installText.size())), true);
        if (!install.Succeeded() || install.value->parent_path() != journal.transaction.parent_path()
            || *install.value == install.value->root_path() || install.value->filename().native().starts_with(L".audastra-update-"))
            throw std::runtime_error("journal install scope");
        journal.install = *install.value;
        const auto parent = journal.transaction.parent_path();
        journal.next = parent / std::filesystem::path(prefix + ".next");
        journal.backup = parent / std::filesystem::path(prefix + ".backup");
        journal.failed = parent / std::filesystem::path(prefix + ".failed");
        journal.rollback = parent / std::filesystem::path(prefix + ".rollback");
        journal.phase = object.at("phase").get<std::string>();
        if (std::find(phases.begin(), phases.end(), journal.phase) == phases.end()) throw std::runtime_error("journal phase");
        journal.currentVersion = object.at("current_version").get<std::string>();
        journal.targetVersion = object.at("target_version").get<std::string>();
        journal.sourceCommit = object.at("source_commit").get<std::string>();
        journal.executable = object.at("executable").get<std::string>();
        if (!ParseSemanticVersion(journal.currentVersion).Succeeded() || !ParseSemanticVersion(journal.targetVersion).Succeeded()
            || !Hex(journal.sourceCommit, 40) || !ValidateUpdateRelativePath(journal.executable).Succeeded())
            throw std::runtime_error("journal signed identity");
        const auto readHash = [&](std::string_view field, bool emptyAllowed = false) {
            const auto value = object.at(std::string(field)).get<std::string>();
            if (!(emptyAllowed && value.empty()) && !Hex(value, 64)) throw std::runtime_error("journal hash");
            return value;
        };
        journal.manifestSha256 = readHash("manifest_sha256"); journal.baselineSha256 = readHash("baseline_sha256", true);
        journal.originalInventory = readHash("original_inventory"); journal.stagedInventory = readHash("staged_inventory", true);
        journal.rollbackInventory = readHash("rollback_inventory", true); journal.failedInventory = readHash("failed_inventory", true);
        journal.originalId = ReadRoot(object.at("original_id")); journal.nextId = ReadRoot(object.at("next_id"));
        journal.rollbackId = ReadRoot(object.at("rollback_id"));
        const auto& child = object.at("child");
        if (!child.is_null()) {
            if (!child.is_object() || child.size() != 2 || !child.at("pid").is_number_unsigned()
                || !child.at("creation_time_100ns").is_number_unsigned() || child.at("pid").get<std::uint64_t>() > MAXDWORD)
                throw std::runtime_error("journal child");
            journal.child = UpdateProcessIdentity{child.at("pid").get<std::uint32_t>(), child.at("creation_time_100ns").get<std::uint64_t>()};
            if (!journal.child->processId || !journal.child->creationTime100ns) throw std::runtime_error("journal child identity");
        }
        if (!object.at("committed_time_100ns").is_number_unsigned()) throw std::runtime_error("journal timestamp");
        journal.committedTime100ns = object.at("committed_time_100ns").get<std::uint64_t>();
        if (!object.at("rollback_attempt").is_number_unsigned() || object.at("rollback_attempt").get<std::uint64_t>() > 64)
            throw std::runtime_error("journal rollback attempt");
        journal.rollbackAttempt = object.at("rollback_attempt").get<std::uint32_t>();
        if (journal.rollbackAttempt) journal.rollback = parent / std::filesystem::path(prefix + ".rollback-" + std::to_string(journal.rollbackAttempt));
        journal.startupArguments = object.at("startup_arguments").get<std::vector<std::string>>();
        if (journal.startupArguments.size() > 64) throw std::runtime_error("journal arguments");
        for (const auto& argument : journal.startupArguments) if (argument.size() > 1024 || argument.find('\0') != std::string::npos
            || argument.starts_with("--update-health-")) throw std::runtime_error("journal arguments");
        journal.reasonCode = object.at("reason_code").get<std::string>(); journal.reason = object.at("reason").get<std::string>();
        if (journal.reasonCode.size() > 128 || journal.reason.size() > 4096) throw std::runtime_error("journal reason");
        return R::Success(std::move(journal));
    } catch (const std::exception&) { return R::Failure("Update", "UPDATE_JOURNAL_INVALID", "事务日志格式、身份或范围不可信；保留目录，不执行恢复。"); }
}

Result<InstallMutex> AcquireInstallMutex(const std::filesystem::path& install) {
    auto name = install.lexically_normal().native();
    if (!name.empty()) CharUpperBuffW(name.data(), static_cast<DWORD>(name.size()));
    const auto hash = Sha256(std::as_bytes(std::span(name.data(), name.size())));
    if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
    const auto mutexName = L"Local\\Audastra.Update." + std::wstring(hash.value->begin(), hash.value->end());
    NativeHandle mutex(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
    if (!mutex.Valid()) return Result<InstallMutex>::Failure("Update", "UPDATE_INSTALL_MUTEX_FAILED", "无法锁定更新事务。");
    const auto wait = WaitForSingleObject(mutex.Get(), 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
        return Result<InstallMutex>::Failure("Update", "UPDATE_INSTALL_BUSY", "该目录已有更新/恢复事务运行。");
    return Result<InstallMutex>::Success(InstallMutex(std::move(mutex)));
}

UpdateInstallReceipt Receipt(const InstallJournal& journal, UpdateInstallOutcome outcome) {
    UpdateInstallReceipt receipt;
    receipt.outcome = outcome; receipt.transactionId = journal.transactionId;
    receipt.currentVersion = journal.currentVersion; receipt.targetVersion = journal.targetVersion;
    receipt.transactionDirectory = journal.transaction; receipt.installDirectory = journal.install;
    receipt.backupDirectory = journal.backup; receipt.failedDirectory = journal.failed; receipt.childProcess = journal.child;
    receipt.healthVerified = outcome == UpdateInstallOutcome::Committed;
    receipt.userFilesPreserved = outcome == UpdateInstallOutcome::Committed || outcome == UpdateInstallOutcome::RolledBack;
    receipt.reasonCode = journal.reasonCode; receipt.reason = journal.reason;
    return receipt;
}

} // namespace War3Map::Update::Detail
