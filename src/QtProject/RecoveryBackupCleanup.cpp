#include "RecoveryBackupCleanup.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>

#include <algorithm>
#include <cwchar>
#include <string>

namespace War3Map::QtProject {
namespace {
class BackupHandle {
public:
    explicit BackupHandle(HANDLE handle) : handle_(handle) {}
    ~BackupHandle() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    BackupHandle(const BackupHandle&) = delete;
    BackupHandle& operator=(const BackupHandle&) = delete;
    HANDLE Get() const { return handle_; }
private:
    HANDLE handle_;
};
Status Keep(const std::filesystem::path& path) {
    Status result;
    result.diagnostics.push_back({DiagnosticSeverity::Warning, "恢复", "RECOVERY_BACKUP_KEPT",
        "本次恢复点已写出；上一副本的独占身份无法确认或不能删除，因此保留该副本。", OriginSpan{path}});
    return result;
}
bool EqualPath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}
} // namespace

Status CleanupConfirmedRecoveryBackup(const ProjectSaveResult& written, std::string_view previousSha256) {
    if (!written.backupPath) return {};
    const auto& backup = *written.backupPath;
    if (previousSha256.size() != 64 || !std::all_of(previousSha256.begin(), previousSha256.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) return Keep(backup);
    std::error_code error;
    const auto target = std::filesystem::absolute(written.path, error).lexically_normal();
    if (error) return Keep(backup);
    const auto absoluteBackup = std::filesystem::absolute(backup, error).lexically_normal();
    if (error || !EqualPath(target.parent_path(), absoluteBackup.parent_path())
        || !absoluteBackup.filename().wstring().starts_with(target.filename().wstring() + L".backup-")
        || _wcsicmp(absoluteBackup.extension().c_str(), L".forge") != 0) return Keep(backup);
    // 新恢复点必须仍完整存在，并在删上一版期间拒绝写入和删除。
    // Store 成功之后的外部变化不能让最后一份有效恢复点被清掉。
    BackupHandle current(CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    BY_HANDLE_FILE_INFORMATION currentIdentity{};
    if (current.Get() == INVALID_HANDLE_VALUE || GetFileType(current.Get()) != FILE_TYPE_DISK
        || !GetFileInformationByHandle(current.Get(), &currentIdentity)
        || (currentIdentity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return Keep(backup);
    const auto currentSha = Sha256Handle(current.Get());
    if (!currentSha.Succeeded() || !currentSha.value || *currentSha.value != written.sha256) return Keep(backup);
    BackupHandle handle(CreateFileW(absoluteBackup.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (handle.Get() == INVALID_HANDLE_VALUE || GetFileType(handle.Get()) != FILE_TYPE_DISK) return Keep(backup);
    BY_HANDLE_FILE_INFORMATION identity{};
    if (!GetFileInformationByHandle(handle.Get(), &identity) || identity.nNumberOfLinks != 1
        || (identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return Keep(backup);
    const auto needed = GetFinalPathNameByHandleW(handle.Get(), nullptr, 0, FILE_NAME_NORMALIZED);
    if (!needed) return Keep(backup);
    std::wstring finalName(needed, L'\0');
    const auto length = GetFinalPathNameByHandleW(handle.Get(), finalName.data(), needed, FILE_NAME_NORMALIZED);
    if (!length || length >= needed) return Keep(backup);
    finalName.resize(length);
    if (finalName.starts_with(L"\\\\?\\UNC\\")) finalName = L"\\\\" + finalName.substr(8);
    else if (finalName.starts_with(L"\\\\?\\")) finalName.erase(0, 4);
    if (!EqualPath(absoluteBackup, std::filesystem::path(finalName).lexically_normal())) return Keep(backup);
    const auto actual = Sha256Handle(handle.Get());
    if (!actual.Succeeded() || !actual.value || *actual.value != previousSha256) return Keep(backup);
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(handle.Get(), FileDispositionInfo, &disposition, sizeof(disposition))) return Keep(backup);
    return {};
}

} // namespace War3Map::QtProject
