#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/ProjectSaveControl.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <exception>
#include <string>
#include <utility>

#include <functional>

// 备份保存、只读检查、迁移与保留清理共享同一归属和原生身份规则。
namespace War3Map::BackupStorage {

constexpr std::size_t kMaxBackupRecords = 4096;
constexpr DWORD kMaxRecordBytes = 4096;
constexpr std::string_view kBackupSchema = "forge.map-backup.v1";
constexpr std::uint64_t kMaxInspectionBytes = 1024ull * 1024 * 1024;

template <typename T>
Result<T> Fail(std::string code, std::string message) {
    return Result<T>::Failure("MapArchiveSession", std::move(code), std::move(message));
}

template <typename T>
Result<T> Forward(std::vector<Diagnostic> diagnostics) {
    Result<T> result;
    result.diagnostics = std::move(diagnostics);
    return result;
}

inline void Warn(std::vector<Diagnostic>& diagnostics, std::string code, std::string message) {
    diagnostics.push_back({DiagnosticSeverity::Warning, "MapArchiveSession", std::move(code), std::move(message)});
}

inline std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

// 仅在系统调用边界使用扩展路径，源身份键和用户回执仍使用普通规范路径。
// 调用方先取得绝对路径；规范化点段和分隔符后再加前缀，保留本地/UNC语义。
inline std::filesystem::path WindowsApiPath(const std::filesystem::path& path) {
    auto normalized = path.lexically_normal();
    normalized.make_preferred();
    const auto name = normalized.wstring();
    if (name.starts_with(L"\\\\?\\") || !normalized.is_absolute()) return normalized;
    if (name.starts_with(L"\\\\")) return std::filesystem::path(L"\\\\?\\UNC\\" + name.substr(2));
    return std::filesystem::path(L"\\\\?\\" + name);
}

inline std::string FailureDetails(std::string_view stage, DWORD error, const std::filesystem::path& path) {
    return "（阶段 " + std::string(stage) + "，Win32 " + std::to_string(error) + "，路径 " + Utf8(path) + "）";
}

inline std::string DirectoryContents(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(WindowsApiPath(directory), error), end;
    std::string result = "；目录成员[";
    std::size_t count = 0;
    while (!error && iterator != end && count < 8) {
        if (count++) result += ", ";
        result += Utf8(iterator->path().filename());
        iterator.increment(error);
    }
    if (!error && iterator != end) result += ", ...";
    if (error) result += "枚举错误 " + std::to_string(error.value());
    return result + "]";
}

struct Win32Failure {
    std::string_view stage;
    DWORD error;
};

class FileHandle {
public:
    explicit FileHandle(HANDLE value) : value_(value) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() { Close(); }
    bool Valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
    HANDLE Get() const { return value_; }
    void Reset(HANDLE value) { Close(); value_ = value; }
    bool Delete() {
        return SetDelete(true);
    }
    bool SetDelete(bool remove) {
        FILE_DISPOSITION_INFO disposition{static_cast<BOOLEAN>(remove ? TRUE : FALSE)};
        return Valid() && SetFileInformationByHandle(value_, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
    }
    void Close() {
        if (Valid()) CloseHandle(value_);
        value_ = INVALID_HANDLE_VALUE;
    }
private:
    HANDLE value_;
};

enum class BackupPairRemoval { Removed, Preserved, RecoveryRequired };
inline BackupPairRemoval RemoveBackupPair(FileHandle& file, FileHandle& metadata) {
    if (!file.Delete()) return BackupPairRemoval::Preserved;
    if (!metadata.Delete()) return file.SetDelete(false) ? BackupPairRemoval::Preserved : BackupPairRemoval::RecoveryRequired;
    file.Close(); metadata.Close();
    return BackupPairRemoval::Removed;
}

inline std::uint64_t FileId(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}

inline std::uint64_t FileSize(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

inline bool RegularFile(HANDLE file, BY_HANDLE_FILE_INFORMATION& info) {
    if (!GetFileInformationByHandle(file, &info)) return false;
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    return true;
}

inline bool SameFileState(const BY_HANDLE_FILE_INFORMATION& before, const BY_HANDLE_FILE_INFORMATION& after) {
    return before.dwVolumeSerialNumber == after.dwVolumeSerialNumber && FileId(before) == FileId(after)
        && FileSize(before) == FileSize(after) && CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0;
}

inline Result<std::filesystem::path> FinalDirectory(HANDLE directory) {
    const DWORD needed = GetFinalPathNameByHandleW(directory, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!needed) return Fail<std::filesystem::path>("BACKUP_DIRECTORY_FAILED", "无法确认备份目录的实际路径，地图未保存。"
        + FailureDetails("directory-final-path-size", GetLastError(), {}));
    std::wstring name(needed, L'\0');
    const DWORD count = GetFinalPathNameByHandleW(directory, name.data(), needed, FILE_NAME_NORMALIZED);
    if (!count || count >= needed) return Fail<std::filesystem::path>("BACKUP_DIRECTORY_FAILED", "无法确认备份目录的实际路径，地图未保存。"
        + FailureDetails("directory-final-path", count ? ERROR_INSUFFICIENT_BUFFER : GetLastError(), {}));
    name.resize(count);
    if (name.starts_with(L"\\\\?\\UNC\\")) name = L"\\\\" + name.substr(8);
    else if (name.starts_with(L"\\\\?\\")) name.erase(0, 4);
    return Result<std::filesystem::path>::Success(std::filesystem::path(name));
}

inline std::uint64_t UtcTicks() {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

inline std::wstring UniqueSuffix() {
    static std::atomic<std::uint64_t> sequence{0};
    return std::to_wstring(UtcTicks()) + L"-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(sequence.fetch_add(1));
}

inline std::optional<Win32Failure> CopySource(HANDLE source, HANDLE backup,
    const ProjectSaveControl* control = nullptr) {
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(source, start, nullptr, FILE_BEGIN)) return Win32Failure{"backup-source-seek", GetLastError()};
    std::array<std::byte, 64 * 1024> buffer{};
    std::uint64_t copied = 0;
    for (;;) {
        if (control && !control->Checkpoint(ProjectSaveStage::Copy, copied))
            return Win32Failure{"backup-copy-cancelled", ERROR_CANCELLED};
        DWORD read = 0;
        if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return Win32Failure{"backup-source-read", GetLastError()};
        if (read == 0) break;
        DWORD offset = 0;
        while (offset < read) {
            DWORD written = 0;
            if (!WriteFile(backup, buffer.data() + offset, read - offset, &written, nullptr)) return Win32Failure{"backup-data-write", GetLastError()};
            if (written == 0) return Win32Failure{"backup-data-write", ERROR_WRITE_FAULT};
            offset += written;
        }
        copied += read;
    }
    if (!FlushFileBuffers(backup)) return Win32Failure{"backup-data-flush", GetLastError()};
    return std::nullopt;
}

// 索引不是按名字推测的授权：每份记录还绑定源路径、卷、文件 ID、大小和内容摘要。
inline std::optional<Win32Failure> WriteRecord(HANDLE file, const nlohmann::json& record) {
    const auto bytes = record.dump();
    if (bytes.size() > kMaxRecordBytes) return Win32Failure{"backup-record-size", ERROR_BUFFER_OVERFLOW};
    DWORD written = 0;
    if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) return Win32Failure{"backup-record-write", GetLastError()};
    if (written != bytes.size()) return Win32Failure{"backup-record-write", ERROR_WRITE_FAULT};
    if (!FlushFileBuffers(file)) return Win32Failure{"backup-record-flush", GetLastError()};
    return std::nullopt;
}

inline std::optional<nlohmann::json> ReadRecord(HANDLE file, DWORD maxBytes = kMaxRecordBytes,
    std::uint64_t* bytesRead = nullptr) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!RegularFile(file, info) || FileSize(info) == 0 || FileSize(info) > (std::min)(maxBytes, kMaxRecordBytes)) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(FileSize(info)), '\0');
    DWORD read = 0;
    const bool readOk = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE;
    if (bytesRead) *bytesRead += read;
    BY_HANDLE_FILE_INFORMATION after{};
    if (!readOk || read != bytes.size() || !RegularFile(file, after) || !SameFileState(info, after)) return std::nullopt;
    auto record = nlohmann::json::parse(bytes, nullptr, false);
    if (!record.is_object()) return std::nullopt;
    return record;
}

inline bool HexDigest(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

inline bool MatchesRecord(const nlohmann::json& record, const std::string& sourceKey, const std::filesystem::path& backup,
    const BY_HANDLE_FILE_INFORMATION& info) {
    try {
        // JSON 的整数转换可以接受负数或浮点；owner 字段必须先核实际类型和无符号范围。
        return record.at("schema").is_string() && record.at("schema").get<std::string>() == kBackupSchema
            && HexDigest(sourceKey) && record.at("sourceKey").is_string() && record.at("sourceKey") == sourceKey
            && record.at("backup").is_string()
            && record.at("sourceName").is_string() && !record.at("sourceName").get<std::string>().empty()
            && record.at("backup") == Utf8(backup.filename())
            && record.at("volume").is_number_unsigned() && record.at("volume").get<std::uint64_t>() <= UINT32_MAX
            && record.at("volume").get<std::uint32_t>() == info.dwVolumeSerialNumber
            && record.at("fileId").is_number_unsigned() && record.at("size").is_number_unsigned()
            && record.at("fileId").get<std::uint64_t>() == FileId(info)
            && record.at("size").get<std::uint64_t>() == FileSize(info)
            && record.at("sha256").is_string() && HexDigest(record.at("sha256").get<std::string>())
            && record.at("createdUtc").is_number_unsigned();
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

inline bool DirectoryHandle(HANDLE directory, BY_HANDLE_FILE_INFORMATION& info) {
    return GetFileType(directory) == FILE_TYPE_DISK && GetFileInformationByHandle(directory, &info)
        && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

inline std::optional<std::string> InspectionReceiptKey(const std::wstring& name) {
    constexpr std::wstring_view prefix = L"forge-map-";
    if (!name.starts_with(prefix) || !name.ends_with(L".json") || name.size() <= prefix.size() + 64 + 1 + 5
        || name[prefix.size() + 64] != L'-') return std::nullopt;
    const auto backup = name.substr(0, name.size() - 5);
    if (std::any_of(backup.begin(), backup.end(), [](wchar_t c) {
        return c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/'
            || c == L'\\' || c == L'|' || c == L'?' || c == L'*';
    })) return std::nullopt;
    auto extension = std::filesystem::path(backup).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (extension != L".w3x" && extension != L".w3m") return std::nullopt;
    const auto key = Utf8(std::filesystem::path(name.substr(prefix.size(), 64)));
    return HexDigest(key) ? std::optional(key) : std::nullopt;
}

inline std::vector<std::filesystem::path> PruneBackups(const std::filesystem::path& directory,
    const std::filesystem::path& indexDirectory, const std::string& sourceKey,
    const std::filesystem::path& newest, std::size_t keep, std::vector<Diagnostic>& diagnostics,
    const std::function<bool(std::uint64_t, std::size_t)>& allowance = {},
    const ProjectSaveControl* control = nullptr) {
    std::vector<std::filesystem::path> removed;
    if (keep == 0) return removed;
    struct BackupRecord {
        std::filesystem::path backup, receipt;
        std::uint64_t createdUtc;
    };
    std::vector<BackupRecord> records;
    const std::wstring prefix = L"forge-map-" + std::wstring(sourceKey.begin(), sourceKey.end()) + L"-";
    std::error_code error;
    std::filesystem::directory_iterator iterator(WindowsApiPath(indexDirectory), error), end;
    std::size_t examined = 0;
    bool unconfirmed = false;
    while (!error && iterator != end) {
        if (++examined > kMaxBackupRecords || (allowance && !allowance(0, 1))) {
            Warn(diagnostics, "BACKUP_PRUNE_LIMIT", "备份索引超过扫描预算，本次未清理旧备份。");
            return removed;
        }
        const auto receipt = indexDirectory / iterator->path().filename();
        const auto name = receipt.filename().wstring();
        if (name.starts_with(prefix) && name.ends_with(L".json")) {
            // 迁移的受限清理只处理已支持的地图后缀；未知载荷不打开。
            if (allowance && !InspectionReceiptKey(name)) {
                unconfirmed = true; iterator.increment(error); continue;
            }
            if (allowance && !allowance(kMaxRecordBytes, 0)) return removed;
            const auto backup = directory / name.substr(0, name.size() - 5);
            FileHandle metadata(CreateFileW(WindowsApiPath(receipt).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            const auto record = metadata.Valid() ? ReadRecord(metadata.Get()) : std::nullopt;
            FileHandle file(CreateFileW(WindowsApiPath(backup).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if (record && file.Valid() && RegularFile(file.Get(), info) && MatchesRecord(*record, sourceKey, backup, info)) {
                records.push_back({backup, receipt, record->at("createdUtc").get<std::uint64_t>()});
            } else {
                unconfirmed = true;
            }
        }
        iterator.increment(error);
    }
    if (error) {
        Warn(diagnostics, "BACKUP_PRUNE_FAILED", "无法完整读取备份索引，本次未清理旧备份。");
        return removed;
    }
    if (unconfirmed) Warn(diagnostics, "BACKUP_PRUNE_SKIPPED", "部分旧备份归属或文件身份无法确认，已保留这些文件。");
    std::sort(records.begin(), records.end(), [&newest](const auto& left, const auto& right) {
        if (left.backup == newest || right.backup == newest) return left.backup == newest && right.backup != newest;
        if (left.createdUtc != right.createdUtc) return left.createdUtc > right.createdUtc;
        return left.backup.filename() > right.backup.filename();
    });
    for (std::size_t position = keep; position < records.size(); ++position) {
        if (allowance && !allowance(kMaxRecordBytes, 0)) return removed;
        const auto& entry = records[position];
        // 打开时拒绝共享写入/删除，核验和删除始终操作同一对象；不跟随文件重解析点。
        FileHandle metadata(CreateFileW(WindowsApiPath(entry.receipt).c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        const auto record = metadata.Valid() ? ReadRecord(metadata.Get()) : std::nullopt;
        FileHandle file(CreateFileW(WindowsApiPath(entry.backup).c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        const bool owned = record && file.Valid() && RegularFile(file.Get(), info)
            && MatchesRecord(*record, sourceKey, entry.backup, info);
        if (owned && allowance && !allowance(FileSize(info), 0)) return removed;
        const auto hash = owned ? Sha256Handle(file.Get(), control) : Result<std::string>{};
        if (!owned || !hash.Succeeded()
            || !hash.value || record->at("sha256") != *hash.value) {
            Warn(diagnostics, "BACKUP_PRUNE_SKIPPED", "旧备份身份、内容或占用状态无法确认，已保留：" + Utf8(entry.backup));
            continue;
        }
        if (RemoveBackupPair(file, metadata) != BackupPairRemoval::Removed) {
            Warn(diagnostics, "BACKUP_PRUNE_SKIPPED", "旧备份或归属记录无法一起删除，已尝试保留原份。");
            continue;
        }
        removed.push_back(entry.backup);
    }
    return removed;
}


} // namespace War3Map::BackupStorage
