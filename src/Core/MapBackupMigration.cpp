#include "War3MapCore/MapBackupMigration.h"
#include "MapBackupStorage.h"

#include <set>

namespace War3Map {
namespace {
using namespace BackupStorage;
bool ValidPath(const std::filesystem::path& path) {
    return !path.empty() && path.is_absolute() && path.native().size() <= 4096
        && !path.native().starts_with(L"\\\\.\\") && !path.native().starts_with(L"\\\\?\\GLOBALROOT")
        && std::none_of(path.native().begin(), path.native().end(), [](wchar_t c) { return c < 32; });
}
bool SameObject(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && FileId(a) == FileId(b);
}
struct Migration {
    const MapBackupMigrationOptions& options;
    Result<MapBackupMigration>& result;
    bool Active() const { return !options.stop.stop_requested() && std::chrono::steady_clock::now() < options.deadline; }
    void Warn(std::string code, std::string message) {
        result.value->complete = false;
        BackupStorage::Warn(result.diagnostics, std::move(code), std::move(message));
    }
    bool Allow(std::uint64_t bytes = 0, std::size_t entries = 0) {
        auto& out = *result.value;
        if (!Active() || bytes > options.maxReadBytes - out.bytesRead || entries > options.maxEntries - out.examinedEntries) {
            Warn("BACKUP_MIGRATION_LIMIT", "备份迁移达到期限、取消或条目/字节预算；其余旧备份保留。"); return false;
        }
        out.bytesRead += bytes; out.examinedEntries += entries; return true;
    }
    void Skip(const std::filesystem::path& name, std::string code, std::string reason) {
        ++result.value->skipped;
        Warn(std::move(code), std::move(reason) + "：" + Utf8(name));
    }
};
// 析构不访问路径，只撤回持有句柄的本次新建物；既有同名文件和记录永不删除。
struct TargetTransaction {
    Migration& migration;
    FileHandle file{INVALID_HANDLE_VALUE}, receipt{INVALID_HANDLE_VALUE};
    bool createdFile = false, createdReceipt = false, committed = false;
    ~TargetTransaction() {
        if (committed) return;
        try {
            if (createdReceipt && !receipt.Delete()) {
                migration.Warn("BACKUP_MIGRATION_ROLLBACK_INCOMPLETE", "目标归属记录无法撤回，保留新旧副本供恢复。"); return;
            }
            receipt.Close();
            if (createdFile && !file.Delete()) migration.Warn("BACKUP_MIGRATION_ROLLBACK_INCOMPLETE", "目标临时副本无法删除，旧备份仍保留。");
        } catch (...) { /* 不让诊断分配失败破坏源备份。 */ }
    }
};
bool OpenDirectory(const std::filesystem::path& path, FileHandle& handle,
    BY_HANDLE_FILE_INFORMATION& info, std::filesystem::path& actual) {
    handle.Reset(CreateFileW(WindowsApiPath(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.Valid()) return false;
    if (!DirectoryHandle(handle.Get(), info)) { SetLastError(ERROR_INVALID_DATA); return false; }
    const auto resolved = FinalDirectory(handle.Get());
    if (!resolved.Succeeded()) return false;
    actual = *resolved.value;
    return true;
}
void RemoveEmptyDirectory(const std::filesystem::path& path, const BY_HANDLE_FILE_INFORMATION& expected) {
    FileHandle handle(CreateFileW(WindowsApiPath(path).c_str(), FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION actual{};
    // 非空时原版调用失败；不递归，也不接管被换掉的目录。
    if (handle.Valid() && DirectoryHandle(handle.Get(), actual) && SameObject(actual, expected)) (void)handle.Delete();
}

bool MoveOne(Migration& migration, const std::filesystem::path& from, const std::filesystem::path& to,
    const std::wstring& name, const std::string& key, ProjectSaveControl& control) {
    const auto skip = [&](std::string code, std::string reason) { migration.Skip(name, std::move(code), std::move(reason)); return false; };
    const auto backup = name.substr(0, name.size() - 5);
    const auto source = from / backup, target = to / backup;
    FileHandle metadata(CreateFileW(WindowsApiPath(from / L".forge-backup-owners" / name).c_str(), GENERIC_READ | DELETE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!migration.Allow(kMaxRecordBytes)) return false;
    const auto record = metadata.Valid() ? ReadRecord(metadata.Get()) : std::nullopt;
    FileHandle file(CreateFileW(WindowsApiPath(source).c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    BY_HANDLE_FILE_INFORMATION before{};
    if (!record || !file.Valid() || !RegularFile(file.Get(), before) || !MatchesRecord(*record, key, source, before))
        return skip("BACKUP_MIGRATION_OWNER_UNCONFIRMED", "归属、原生身份或占用无法确认，保留旧备份");
    const auto size = FileSize(before);
    // 内容核对、复制、目标核对各预留一次；失败不退还，也不因重复尝试低估I/O。
    if (size > (migration.options.maxReadBytes - migration.result.value->bytesRead) / 3) {
        migration.Warn("BACKUP_MIGRATION_LIMIT", "备份内容超过剩余读取预算，旧备份保留。"); return false;
    }
    if (!migration.Allow(size * 3)) return false;
    const auto hash = Sha256Handle(file.Get(), &control);
    if (!hash.Succeeded() || !hash.value || record->at("sha256") != *hash.value)
        return skip("BACKUP_MIGRATION_HASH_MISMATCH", "旧备份摘要不符或读取未完成");
    TargetTransaction transaction{migration};
    transaction.file.Reset(CreateFileW(WindowsApiPath(target).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    transaction.createdFile = transaction.file.Valid();
    if (!transaction.createdFile) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            return skip("BACKUP_MIGRATION_TARGET_UNAVAILABLE", "无法创建目标备份");
        transaction.file.Reset(CreateFileW(WindowsApiPath(target).c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    }
    BY_HANDLE_FILE_INFORMATION targetInfo{};
    if (!transaction.file.Valid() || !RegularFile(transaction.file.Get(), targetInfo))
        return skip("BACKUP_MIGRATION_CONFLICT", "目标同名文件不可核验，不覆盖");
    if (transaction.createdFile && CopySource(file.Get(), transaction.file.Get(), &control))
        return skip("BACKUP_MIGRATION_COPY_FAILED", "复制或刷盘失败");
    const auto copied = Sha256Handle(transaction.file.Get(), &control);
    if (!copied.Succeeded() || !copied.value || !RegularFile(transaction.file.Get(), targetInfo))
        return skip("BACKUP_MIGRATION_TARGET_UNAVAILABLE", "无法读取或核对新目录里的同名文件");
    if (*copied.value != *hash.value || FileSize(targetInfo) != size)
        return transaction.createdFile
            ? skip("BACKUP_MIGRATION_COPY_FAILED", "复制后的备份与旧文件不同，旧文件保留")
            : skip("BACKUP_MIGRATION_CONFLICT", "新目录已有同名的不同文件");
    const auto receiptPath = to / L".forge-backup-owners" / name;
    transaction.receipt.Reset(CreateFileW(WindowsApiPath(receiptPath).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    transaction.createdReceipt = transaction.receipt.Valid();
    if (transaction.createdReceipt) {
        auto updated = *record;
        updated["volume"] = targetInfo.dwVolumeSerialNumber;
        updated["fileId"] = FileId(targetInfo);
        if (WriteRecord(transaction.receipt.Get(), updated)) return skip("BACKUP_MIGRATION_RECORD_FAILED", "目标归属登记失败");
    } else {
        if (!migration.Allow(kMaxRecordBytes)) return false;
        transaction.receipt.Reset(CreateFileW(WindowsApiPath(receiptPath).c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        const auto existing = transaction.receipt.Valid() ? ReadRecord(transaction.receipt.Get()) : std::nullopt;
        if (!existing || !MatchesRecord(*existing, key, target, targetInfo) || existing->at("sha256") != *copied.value)
            return skip("BACKUP_MIGRATION_RECORD_CONFLICT", "目标归属记录冲突，不覆盖");
    }
    BY_HANDLE_FILE_INFORMATION after{};
    if (!migration.Allow() || !RegularFile(file.Get(), after) || !SameFileState(before, after))
        return skip("BACKUP_MIGRATION_SOURCE_CHANGED", "源备份变化或迁移中断");
    const auto removed = RemoveBackupPair(file, metadata);
    if (removed == BackupPairRemoval::RecoveryRequired) {
        transaction.committed = true; // 保留已核验的恢复副本，不能再回滚目标。
        return skip("BACKUP_MIGRATION_ROLLBACK_INCOMPLETE", "旧份待删除状态无法撤销，目标核验备份已保留");
    }
    if (removed != BackupPairRemoval::Removed) return skip("BACKUP_MIGRATION_SOURCE_DELETE_FAILED", "源备份或归属记录无法一起删除，已撤回旧份删除");
    transaction.committed = true;
    ++migration.result.value->moved;
    if (!transaction.createdFile) ++migration.result.value->alreadyPresent;
    return true;
}
} // namespace

Result<MapBackupMigration> MigrateMapBackups(const std::filesystem::path& from,
    const std::filesystem::path& to, const MapBackupMigrationOptions& options) {
    using namespace BackupStorage;
    if (!ValidPath(from) || !ValidPath(to) || options.maxEntries > kMaxBackupRecords
        || options.maxReadBytes > 64ull * 1024 * 1024 * 1024 || options.keepBackups > 1000
        || options.deadline == std::chrono::steady_clock::time_point::max())
        return Fail<MapBackupMigration>("BACKUP_MIGRATION_OPTIONS_INVALID", "备份迁移需要绝对目录和有限的条目、字节、保留数量及期限。");
    auto result = Result<MapBackupMigration>::Success(MapBackupMigration{});
    Migration migration{options, result};
    if (from.lexically_normal() == to.lexically_normal()) return result;
    if (!migration.Allow()) return result;
    try {
        FileHandle sourceRoot(INVALID_HANDLE_VALUE), sourceIndexRoot(INVALID_HANDLE_VALUE);
        FileHandle targetRoot(INVALID_HANDLE_VALUE), targetIndexRoot(INVALID_HANDLE_VALUE);
        BY_HANDLE_FILE_INFORMATION sourceInfo{}, sourceIndexInfo{}, targetInfo{}, targetIndexInfo{};
        std::filesystem::path source, sourceIndex, target, targetIndex;
        if (!OpenDirectory(from, sourceRoot, sourceInfo, source)) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                migration.Warn("BACKUP_MIGRATION_SOURCE_UNAVAILABLE", "旧备份目录不可确认，所有备份保留。");
            return result;
        }
        if (!OpenDirectory(source / L".forge-backup-owners", sourceIndexRoot, sourceIndexInfo, sourceIndex)) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                migration.Warn("BACKUP_MIGRATION_INDEX_UNAVAILABLE", "旧备份归属索引不可确认，未移动文件。");
            return result;
        }
        std::error_code error;
        std::filesystem::create_directories(WindowsApiPath(to), error);
        if (error || !OpenDirectory(to, targetRoot, targetInfo, target)) {
            migration.Warn("BACKUP_MIGRATION_TARGET_UNAVAILABLE", "新备份目录不可用，旧备份保留。"); return result;
        }
        if (SameObject(sourceInfo, targetInfo)) return result;
        std::filesystem::create_directories(WindowsApiPath(target / L".forge-backup-owners"), error);
        if (error || !OpenDirectory(target / L".forge-backup-owners", targetIndexRoot, targetIndexInfo, targetIndex)) {
            migration.Warn("BACKUP_MIGRATION_TARGET_UNAVAILABLE", "新备份归属索引不可用。"); return result;
        }
        ProjectSaveControl* current = nullptr;
        ProjectSaveControl control([&](ProjectSaveStage, std::uint64_t) { if (!migration.Active()) (void)current->RequestCancellation(); });
        current = &control;
        std::vector<std::wstring> receipts;
        std::filesystem::directory_iterator iterator(WindowsApiPath(sourceIndex), error), end;
        while (!error && iterator != end) {
            if (!migration.Allow(0, 1)) break;
            receipts.push_back(iterator->path().filename().wstring());
            iterator.increment(error);
        }
        if (error) migration.Warn("BACKUP_MIGRATION_ENUMERATION_FAILED", "旧备份索引枚举未完成，其余文件保留。");
        std::set<std::string> keys;
        for (const auto& name : receipts) {
            if (!migration.Allow()) break;
            const auto key = InspectionReceiptKey(name);
            if (!key) { migration.Skip(name, "BACKUP_MIGRATION_OWNER_UNCONFIRMED", "归属记录名称无效"); continue; }
            if (MoveOne(migration, source, target, name, *key, control)) keys.insert(*key);
        }
        for (const auto& key : keys) {
            if (!migration.Allow()) break;
            result.value->pruned += PruneBackups(target, targetIndex, key, {}, options.keepBackups,
                result.diagnostics, [&](std::uint64_t bytes, std::size_t entries) { return migration.Allow(bytes, entries); }, &control).size();
        }
        if (!result.diagnostics.empty()) result.value->complete = false;
        sourceIndexRoot.Close();
        // 至少完成一份有归属的迁移，才有证据回收这个空索引；不接管来历未知的空目录。
        if (result.value->moved) RemoveEmptyDirectory(sourceIndex, sourceIndexInfo);
        sourceRoot.Close(); RemoveEmptyDirectory(source, sourceInfo);
    } catch (const std::exception&) {
        migration.Warn("BACKUP_MIGRATION_FAILED", "备份迁移异常中止；未提交的旧备份保留，请核对目标恢复副本。");
    }
    return result;
}
} // namespace War3Map
