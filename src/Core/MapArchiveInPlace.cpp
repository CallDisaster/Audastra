#include "MapBackupStorage.h"

#include <exception>

namespace War3Map {
using namespace BackupStorage;

Result<MapBackupInspection> InspectMapBackupDirectory(const std::filesystem::path& directory,
    const MapBackupInspectionOptions& options) {
    if (directory.empty() || !directory.is_absolute() || directory.native().size() > 4096
        || std::any_of(directory.native().begin(), directory.native().end(), [](wchar_t c) { return c < 32; })
        || directory.native().starts_with(L"\\\\.\\") || directory.native().starts_with(L"\\\\?\\GLOBALROOT")
        || options.maxEntries > kMaxBackupRecords || options.maxReceipts > kMaxBackupRecords
        || options.maxReceiptBytes > kMaxRecordBytes || options.maxReadBytes > kMaxInspectionBytes
        || options.deadline == std::chrono::steady_clock::time_point::max()) {
        return Fail<MapBackupInspection>("BACKUP_INSPECTION_OPTIONS_INVALID",
            "旧备份检查需要受限的绝对目录、条目 / 收据 / 字节预算和有限期限。");
    }
    auto result = Result<MapBackupInspection>::Success(MapBackupInspection{});
    auto& inspection = *result.value;
    const auto active = [&options]() {
        return !options.stop.stop_requested() && std::chrono::steady_clock::now() < options.deadline;
    };
    const auto unknown = [&result](std::string code, std::string message) {
        result.value->state = MapBackupInspectionState::Unknown;
        result.value->confirmedBackup.reset();
        if (result.diagnostics.size() < 8) Warn(result.diagnostics, std::move(code), std::move(message));
        return std::move(result);
    };
    const auto interrupted = [&]() {
        return unknown(options.stop.stop_requested() ? "BACKUP_INSPECTION_CANCELLED" : "BACKUP_INSPECTION_DEADLINE",
            options.stop.stop_requested() ? "旧备份检查已取消；未修改任何备份。" : "旧备份检查达到期限，未能确认不存在备份。");
    };
    try {
        if (!active()) return interrupted();
        if (options.maxEntries == 0) return unknown("BACKUP_INSPECTION_ENTRY_LIMIT", "旧备份检查没有可用的目录项预算。");
        // 不申请写入或 DELETE；同时拒绝本根和 owner index 的重解析点及目录改名。
        FileHandle root(CreateFileW(WindowsApiPath(directory).c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        const auto rootError = root.Valid() ? ERROR_SUCCESS : GetLastError();
        if (!active()) return interrupted();
        if (!root.Valid()) {
            if (rootError == ERROR_FILE_NOT_FOUND || rootError == ERROR_PATH_NOT_FOUND) {
                inspection.state = MapBackupInspectionState::Absent;
                return result;
            }
            return unknown("BACKUP_INSPECTION_DIRECTORY_UNAVAILABLE", "无法只读确认旧备份目录。"
                + FailureDetails("inspection-root-open", rootError, directory));
        }
        BY_HANDLE_FILE_INFORMATION rootInfo{};
        if (!DirectoryHandle(root.Get(), rootInfo)) return unknown("BACKUP_INSPECTION_DIRECTORY_UNCONFIRMED",
            "旧备份根不是可确认的普通目录。");
        const auto actual = FinalDirectory(root.Get());
        if (!actual.Succeeded() || !actual.value) return unknown("BACKUP_INSPECTION_DIRECTORY_UNCONFIRMED",
            "无法确认旧备份目录的实际路径。");
        const auto indexDirectory = *actual.value / L".forge-backup-owners";
        const auto completeRoot = [&](bool indexMissing) {
            // 无候选收据时仍完整检查 root 的有界范围；孤立的服务命名文件只表示证据不足。
            std::error_code error;
            std::filesystem::directory_iterator iterator(WindowsApiPath(*actual.value), error), end;
            while (!error && iterator != end) {
                if (!active()) return interrupted();
                if (inspection.examinedEntries == options.maxEntries) return unknown("BACKUP_INSPECTION_ENTRY_LIMIT",
                    "旧备份目录超过条目预算，未能确认不存在备份。");
                ++inspection.examinedEntries;
                const auto name = iterator->path().filename().wstring();
                if (indexMissing && _wcsicmp(name.c_str(), L".forge-backup-owners") == 0) {
                    return unknown("BACKUP_INSPECTION_INDEX_UNCONFIRMED", "owner 索引身份在检查期间无法确认。");
                }
                if (name.starts_with(L"forge-map-")) return unknown("BACKUP_INSPECTION_OWNER_UNCONFIRMED",
                    "旧目录存在缺少可核对 owner 收据的候选文件，不能按名字确认归属。");
                iterator.increment(error);
            }
            if (!active()) return interrupted();
            if (error) return unknown("BACKUP_INSPECTION_ENUMERATION_FAILED", "未能完整读取旧备份目录。");
            inspection.state = MapBackupInspectionState::Absent;
            return std::move(result);
        };
        if (!active()) return interrupted();
        FileHandle indexRoot(CreateFileW(WindowsApiPath(indexDirectory).c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        const auto indexError = indexRoot.Valid() ? ERROR_SUCCESS : GetLastError();
        if (!active()) return interrupted();
        if (!indexRoot.Valid()) {
            if (indexError != ERROR_FILE_NOT_FOUND && indexError != ERROR_PATH_NOT_FOUND) {
                return unknown("BACKUP_INSPECTION_INDEX_UNAVAILABLE", "无法只读确认旧备份 owner 索引。"
                    + FailureDetails("inspection-index-open", indexError, indexDirectory));
            }
            return completeRoot(true);
        }
        BY_HANDLE_FILE_INFORMATION indexInfo{};
        if (!DirectoryHandle(indexRoot.Get(), indexInfo)) return unknown("BACKUP_INSPECTION_INDEX_UNCONFIRMED",
            "旧备份 owner 索引不是可确认的普通目录。");
        const auto actualIndex = FinalDirectory(indexRoot.Get());
        if (!actualIndex.Succeeded() || !actualIndex.value
            || *actualIndex.value != indexDirectory) return unknown("BACKUP_INSPECTION_INDEX_UNCONFIRMED",
                "旧备份 owner 索引实际路径不属于本次锁定目录。");
        std::error_code error;
        std::filesystem::directory_iterator iterator(WindowsApiPath(indexDirectory), error), end;
        bool unconfirmed = false;
        while (!error && iterator != end) {
            if (!active()) return interrupted();
            if (inspection.examinedEntries == options.maxEntries) return unknown("BACKUP_INSPECTION_ENTRY_LIMIT",
                "旧备份 owner 索引超过条目预算。");
            ++inspection.examinedEntries;
            const auto name = iterator->path().filename().wstring();
            if (name.starts_with(L"forge-map-")) {
                const auto key = InspectionReceiptKey(name);
                if (!key) {
                    unconfirmed = true;
                } else {
                    if (inspection.readReceipts == options.maxReceipts) return unknown("BACKUP_INSPECTION_RECEIPT_LIMIT",
                        "旧备份检查达到收据预算。");
                    const auto receipt = indexDirectory / name;
                    const auto backup = *actual.value / name.substr(0, name.size() - 5);
                    FileHandle metadata(CreateFileW(WindowsApiPath(receipt).c_str(), GENERIC_READ, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                    if (!active()) return interrupted();
                    BY_HANDLE_FILE_INFORMATION recordInfo{};
                    if (!metadata.Valid() || GetFileType(metadata.Get()) != FILE_TYPE_DISK
                        || !RegularFile(metadata.Get(), recordInfo) || FileSize(recordInfo) == 0) {
                        unconfirmed = true;
                    } else {
                        const auto recordSize = FileSize(recordInfo);
                        if (recordSize > options.maxReceiptBytes || recordSize > options.maxReadBytes - inspection.bytesRead) {
                            return unknown("BACKUP_INSPECTION_BYTE_LIMIT", "旧备份收据超过读取预算。");
                        }
                        ++inspection.readReceipts;
                        const auto record = ReadRecord(metadata.Get(), static_cast<DWORD>(options.maxReceiptBytes), &inspection.bytesRead);
                        if (!active()) return interrupted();
                        FileHandle file(CreateFileW(WindowsApiPath(backup).c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
                        if (!active()) return interrupted();
                        BY_HANDLE_FILE_INFORMATION before{};
                        const bool owned = record && file.Valid() && GetFileType(file.Get()) == FILE_TYPE_DISK
                            && RegularFile(file.Get(), before) && MatchesRecord(*record, *key, backup, before);
                        if (!owned) {
                            unconfirmed = true;
                        } else {
                            if (FileSize(before) > options.maxReadBytes - inspection.bytesRead) return unknown(
                                "BACKUP_INSPECTION_BYTE_LIMIT", "旧备份完整内容核对超过读取预算。");
                            if (!active()) return interrupted();
                            // 失败或取消仍保留预留成本，多根检查不能因不完整 progress 低估总 I/O 上界。
                            inspection.bytesRead += FileSize(before);
                            ProjectSaveControl* current = nullptr;
                            ProjectSaveControl control([&](ProjectSaveStage, std::uint64_t) {
                                if (!active()) (void)current->RequestCancellation();
                            });
                            current = &control;
                            const auto hash = Sha256Handle(file.Get(), &control);
                            if (!active()) return interrupted();
                            BY_HANDLE_FILE_INFORMATION after{}, recordAfter{};
                            if (hash.Succeeded() && hash.value && record->at("sha256") == *hash.value
                                && RegularFile(file.Get(), after) && SameFileState(before, after)
                                && RegularFile(metadata.Get(), recordAfter) && SameFileState(recordInfo, recordAfter)) {
                                if (!active()) return interrupted();
                                inspection.state = MapBackupInspectionState::ConfirmedAtLeastOne;
                                inspection.confirmedBackup = backup;
                                return result;
                            }
                            unconfirmed = true;
                        }
                    }
                }
            }
            iterator.increment(error);
        }
        if (!active()) return interrupted();
        if (error) return unknown("BACKUP_INSPECTION_ENUMERATION_FAILED", "未能完整读取旧备份 owner 索引。");
        if (unconfirmed) return unknown("BACKUP_INSPECTION_OWNER_UNCONFIRMED", "部分旧备份收据、原生身份或内容无法确认；所有文件保持不变。");
        return completeRoot(false);
    } catch (const std::exception&) {
        return unknown("BACKUP_INSPECTION_FAILED", "旧备份只读检查失败，未能确认不存在备份；没有写入或清理文件。");
    }
}

Result<InPlaceSaveResult> MapArchiveSession::SaveInPlace(std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals, const InPlaceSaveOptions& options) const {
    if (scriptAuditOnly_) return Result<InPlaceSaveResult>::Failure("MapArchiveSession", "SCRIPT_AUDIT_READ_ONLY", "只读脚本审计快照不能写回地图。");
    const auto directory = sourcePath_.parent_path();
    const std::wstring stem = sourcePath_.stem().wstring();
    const std::wstring extension = sourcePath_.extension().wstring();
    auto backupDirectory = options.backupDirectory.empty() ? directory / L"War3MapEditor Backups" : options.backupDirectory;
    std::error_code error;
    backupDirectory = std::filesystem::absolute(backupDirectory, error).lexically_normal();
    if (error) return Fail<InPlaceSaveResult>("BACKUP_DIRECTORY_FAILED", "无法解析备份目录的绝对路径，地图未保存。"
        + FailureDetails("backup-directory-absolute", static_cast<DWORD>(error.value()), options.backupDirectory));
    std::filesystem::create_directories(WindowsApiPath(backupDirectory), error);
    if (error) return Fail<InPlaceSaveResult>("BACKUP_DIRECTORY_FAILED", "无法创建备份目录，地图未保存。"
        + FailureDetails("backup-directory-create", static_cast<DWORD>(error.value()), backupDirectory));
    // 固定实际目录并阻止改名，避免符号链接/目录别名改变写入或清理目标。
    FileHandle backupRoot(CreateFileW(WindowsApiPath(backupDirectory).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!backupRoot.Valid()) return Fail<InPlaceSaveResult>("BACKUP_DIRECTORY_FAILED", "无法锁定备份目录，地图未保存。"
        + FailureDetails("backup-directory-open", GetLastError(), backupDirectory));
    const auto finalDirectory = FinalDirectory(backupRoot.Get());
    if (!finalDirectory.Succeeded() || !finalDirectory.value) return Forward<InPlaceSaveResult>(finalDirectory.diagnostics);
    backupDirectory = *finalDirectory.value;
    const auto indexDirectory = backupDirectory / L".forge-backup-owners";
    std::filesystem::create_directories(WindowsApiPath(indexDirectory), error);
    if (error) return Fail<InPlaceSaveResult>("BACKUP_DIRECTORY_FAILED", "无法创建备份归属索引，地图未保存。"
        + FailureDetails("backup-index-create", static_cast<DWORD>(error.value()), indexDirectory));
    FileHandle indexRoot(CreateFileW(WindowsApiPath(indexDirectory).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION indexInfo{};
    if (!indexRoot.Valid()) return Fail<InPlaceSaveResult>("BACKUP_DIRECTORY_FAILED", "无法锁定备份归属索引，地图未保存。"
        + FailureDetails("backup-index-open", GetLastError(), indexDirectory));
    if (!GetFileInformationByHandle(indexRoot.Get(), &indexInfo)) return Fail<InPlaceSaveResult>("BACKUP_DIRECTORY_FAILED", "无法核对备份归属索引身份，地图未保存。"
        + FailureDetails("backup-index-identity", GetLastError(), indexDirectory));
    if (!(indexInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (indexInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return Fail<InPlaceSaveResult>("BACKUP_DIRECTORY_FAILED", "备份归属索引不是可确认的普通目录，地图未保存。"
            + FailureDetails("backup-index-identity", ERROR_INVALID_DATA, indexDirectory));
    }

    FileHandle lease(CreateFileW(WindowsApiPath(sourcePath_).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!lease.Valid()) return Fail<InPlaceSaveResult>("SOURCE_LEASE_FAILED", "无法取得地图文件读取租约，地图未保存。"
        + FailureDetails("source-read-lease", GetLastError(), sourcePath_));
    const auto unchanged = VerifySourceUnchanged();
    if (!unchanged.Succeeded()) return Forward<InPlaceSaveResult>(unchanged.diagnostics);
    // 源路径已由 Open 解析为句柄的最终路径；卷 + 规范路径在多次替换后保持稳定。
    // 不把路径转成小写：启用目录大小写敏感时，同大小写折叠名仍可能是两张不同源图。
    const auto key = Sha256Text(std::to_string(volumeSerial_) + ":" + Utf8(sourcePath_));
    if (!key.Succeeded() || !key.value) return Forward<InPlaceSaveResult>(key.diagnostics);
    const auto suffix = UniqueSuffix();
    const auto temporary = directory / (L"." + stem + L".saving-" + suffix + extension);
    auto written = WriteCandidate(temporary, replacements, removals, false);
    if (!written.Succeeded() || !written.value) return Forward<InPlaceSaveResult>(std::move(written.diagnostics));
    const auto discardTemporary = [&temporary] { DeleteFileW(WindowsApiPath(temporary).c_str()); };

    const auto backup = backupDirectory / (L"forge-map-" + std::wstring(key.value->begin(), key.value->end()) + L"-" + suffix + extension);
    const auto receipt = indexDirectory / (backup.filename().wstring() + L".json");
    FileHandle backupFile(CreateFileW(WindowsApiPath(backup).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!backupFile.Valid()) {
        const DWORD code = GetLastError();
        discardTemporary();
        return Fail<InPlaceSaveResult>("BACKUP_WRITE_FAILED", "无法创建原图备份，地图未保存。" + FailureDetails("backup-file-create", code, backup));
    }
    FileHandle metadata(CreateFileW(WindowsApiPath(receipt).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    const DWORD metadataError = metadata.Valid() ? ERROR_SUCCESS : GetLastError();
    const auto discardBackup = [&] {
        if (backupFile.Valid()) (void)backupFile.Delete();
        if (metadata.Valid()) (void)metadata.Delete();
    };
    if (!metadata.Valid()) {
        discardBackup();
        discardTemporary();
        return Fail<InPlaceSaveResult>("BACKUP_WRITE_FAILED", "无法创建备份归属记录，地图未保存。" + FailureDetails("backup-record-create", metadataError, receipt));
    }
    if (const auto failure = CopySource(lease.Get(), backupFile.Get())) {
        discardBackup();
        discardTemporary();
        return Fail<InPlaceSaveResult>("BACKUP_WRITE_FAILED", "无法复制并刷盘原图备份，地图未保存。" + FailureDetails(failure->stage, failure->error, backup));
    }
    BY_HANDLE_FILE_INFORMATION backupInfo{};
    if (!RegularFile(backupFile.Get(), backupInfo)) {
        const DWORD code = GetLastError();
        discardBackup();
        discardTemporary();
        return Fail<InPlaceSaveResult>("BACKUP_WRITE_FAILED", "无法核对原图备份身份，地图未保存。" + FailureDetails("backup-file-identity", code, backup));
    }
    const auto copiedHash = Sha256Handle(backupFile.Get());
    if (!copiedHash.Succeeded() || !copiedHash.value || *copiedHash.value != sourceSha256_) {
        discardBackup();
        discardTemporary();
        return Fail<InPlaceSaveResult>("BACKUP_VERIFY_FAILED", "原图备份未通过内容核验，地图未保存。");
    }
    const nlohmann::json record{{"schema", kBackupSchema}, {"sourceKey", *key.value},
        {"backup", Utf8(backup.filename())}, {"sourceName", Utf8(sourcePath_.filename())},
        {"sha256", sourceSha256_}, {"size", FileSize(backupInfo)},
        {"volume", backupInfo.dwVolumeSerialNumber}, {"fileId", FileId(backupInfo)}, {"createdUtc", UtcTicks()}};
    if (const auto failure = WriteRecord(metadata.Get(), record)) {
        discardBackup();
        discardTemporary();
        return Fail<InPlaceSaveResult>("BACKUP_WRITE_FAILED", "无法写入备份归属记录，地图未保存。" + FailureDetails(failure->stage, failure->error, receipt));
    }

    // 独占创建恢复目录；ReplaceFileW 的三个路径均在源卷，永不把跨卷备份传给它。
    const auto recoveryDirectory = directory / (L"." + stem + L".saving-recovery-" + suffix);
    if (!std::filesystem::create_directory(WindowsApiPath(recoveryDirectory), error) || error) {
        const DWORD code = error ? static_cast<DWORD>(error.value()) : ERROR_ALREADY_EXISTS;
        discardBackup();
        discardTemporary();
        return Fail<InPlaceSaveResult>("REPLACE_FAILED", "无法创建源卷恢复位置，地图未保存。" + FailureDetails("recovery-directory-create", code, recoveryDirectory));
    }
    const auto recovery = recoveryDirectory / (L"original" + extension);
    // 只读属性句柄：不共享删除，目录不能被他人改名或删除。不能带 DELETE 访问：
    // Windows 10 上目录存在带 DELETE 的句柄时，ReplaceFileW 无法把原图移入（1175）。
    FileHandle recoveryRoot(CreateFileW(WindowsApiPath(recoveryDirectory).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION recoveryInfo{};
    DWORD recoveryError = recoveryRoot.Valid() ? ERROR_SUCCESS : GetLastError();
    const bool recoveryKnown = recoveryRoot.Valid() && GetFileInformationByHandle(recoveryRoot.Get(), &recoveryInfo);
    if (recoveryRoot.Valid() && !recoveryKnown) recoveryError = GetLastError();
    const bool recoveryOwned = recoveryKnown
        && (recoveryInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        && !(recoveryInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        && recoveryInfo.dwVolumeSerialNumber == volumeSerial_;
    if (recoveryKnown && !recoveryOwned) recoveryError = ERROR_INVALID_DATA;
    std::vector<Diagnostic> cleanupDiagnostics;
    bool recoveryDispositionAccepted = false;
    // 关闭锁定句柄后按卷号与文件号重开同一目录再删除；只删除空目录，仍有恢复文件时保留。
    const auto deleteRecoveryDirectory = [&] {
        recoveryRoot.Close();
        FileHandle reopened(CreateFileW(WindowsApiPath(recoveryDirectory).c_str(), FILE_READ_ATTRIBUTES | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION current{};
        if (!reopened.Valid() || !GetFileInformationByHandle(reopened.Get(), &current)) return false;
        if (current.dwVolumeSerialNumber != recoveryInfo.dwVolumeSerialNumber || FileId(current) != FileId(recoveryInfo)
            || !(current.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (current.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            SetLastError(ERROR_INVALID_DATA);
            return false;
        }
        return reopened.Delete();
    };
    const auto discardRecoveryDirectory = [&] {
        if (recoveryOwned && !deleteRecoveryDirectory()) {
            const DWORD code = GetLastError();
            Warn(cleanupDiagnostics, "RECOVERY_DIRECTORY_CLEANUP_FAILED", "源卷恢复目录未能清理。"
                + FailureDetails("recovery-directory-disposition", code, recoveryDirectory)
                + "；originalDisposition=" + (recoveryDispositionAccepted ? "accepted" : "not-accepted")
                + DirectoryContents(recoveryDirectory));
        }
        recoveryRoot.Close();
    };
    const auto withCleanupDiagnostics = [&cleanupDiagnostics](Result<InPlaceSaveResult> result) {
        result.diagnostics.insert(result.diagnostics.end(), cleanupDiagnostics.begin(), cleanupDiagnostics.end());
        return result;
    };
    const auto identity = VerifySourceIdentity();
    lease.Close();
    if (!identity.Succeeded() || !recoveryOwned) {
        discardBackup();
        discardTemporary();
        discardRecoveryDirectory();
        if (!identity.Succeeded()) return withCleanupDiagnostics(Forward<InPlaceSaveResult>(identity.diagnostics));
        return withCleanupDiagnostics(Fail<InPlaceSaveResult>("REPLACE_FAILED", "无法锁定源卷恢复位置，地图未保存。"
            + FailureDetails("recovery-directory-identity", recoveryError, recoveryDirectory)));
    }
    if (!ReplaceFileW(WindowsApiPath(sourcePath_).c_str(), WindowsApiPath(temporary).c_str(), WindowsApiPath(recovery).c_str(),
            REPLACEFILE_WRITE_THROUGH | REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr)) {
        const DWORD code = GetLastError();
        if (code == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
            const bool restored = MoveFileExW(WindowsApiPath(recovery).c_str(), WindowsApiPath(sourcePath_).c_str(), MOVEFILE_WRITE_THROUGH) != 0;
            const DWORD restoreError = restored ? ERROR_SUCCESS : GetLastError();
            discardTemporary();
            discardRecoveryDirectory();
            if (restored) discardBackup();
            return withCleanupDiagnostics(Fail<InPlaceSaveResult>("REPLACE_FAILED", restored
                ? "替换地图文件失败，原图已恢复，地图未保存。" + FailureDetails("source-replace", code, sourcePath_)
                : "替换地图文件失败，原图保留在恢复位置：" + Utf8(recovery) + "；已验证备份：" + Utf8(backup)
                    + FailureDetails("source-replace", code, sourcePath_) + FailureDetails("source-restore", restoreError, recovery)));
        }
        discardBackup();
        discardTemporary();
        discardRecoveryDirectory();
        return withCleanupDiagnostics(Fail<InPlaceSaveResult>("REPLACE_FAILED", code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED
                || code == ERROR_UNABLE_TO_REMOVE_REPLACED
            ? "地图文件正被其他程序占用（例如 World Editor），地图未保存。" + FailureDetails("source-replace", code, sourcePath_)
            : "替换地图文件失败，原图保持不变。" + FailureDetails("source-replace", code, sourcePath_)));
    }

    InPlaceSaveResult result;
    result.written = std::move(*written.value);
    result.written.path = sourcePath_;
    result.backup = backup;
    backupFile.Close();
    metadata.Close();
    auto diagnostics = std::move(written.diagnostics);
    FileHandle recoveryFile(CreateFileW(WindowsApiPath(recovery).c_str(), FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION originalInfo{};
    DWORD cleanupError = recoveryFile.Valid() ? ERROR_SUCCESS : GetLastError();
    const bool originalKnown = recoveryFile.Valid() && RegularFile(recoveryFile.Get(), originalInfo);
    if (recoveryFile.Valid() && !originalKnown) cleanupError = GetLastError();
    const bool originalOwned = originalKnown
        && originalInfo.dwVolumeSerialNumber == volumeSerial_ && FileId(originalInfo) == fileId_
        && FileSize(originalInfo) == FileSize(backupInfo);
    if (originalKnown && !originalOwned) cleanupError = ERROR_INVALID_DATA;
    if (originalOwned) {
        // 仅源卷恢复临时文件：身份已核实且备份已落盘。POSIX unlink在本删除句柄
        // 关闭时移除目录项，既有图标/其它reader仍可读取原对象，不依赖它们先关闭。
        // FileDispositionInfoEx的ABI类值为21；避免为旧目标宏扩大公共头/平台定义。
        struct RecoveryDispositionInfo { DWORD flags; } disposition{0x00000001u | 0x00000002u}; // DELETE | POSIX_SEMANTICS
        constexpr auto dispositionClass = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
        recoveryDispositionAccepted = SetFileInformationByHandle(recoveryFile.Get(), dispositionClass,
            &disposition, sizeof(disposition)) != 0;
        if (!recoveryDispositionAccepted) {
            cleanupError = GetLastError();
            if (cleanupError == ERROR_INVALID_FUNCTION || cleanupError == ERROR_INVALID_PARAMETER
                || cleanupError == ERROR_NOT_SUPPORTED || cleanupError == ERROR_INVALID_LEVEL) {
                Warn(diagnostics, "RECOVERY_POSIX_UNSUPPORTED", "系统或文件系统不支持恢复副本的POSIX清理，已保留恢复副本与目录。"
                    + FailureDetails("recovery-file-posix-disposition", cleanupError, recovery));
            }
        }
    }
    if (!originalOwned || !recoveryDispositionAccepted) Warn(diagnostics, "RECOVERY_CLEANUP_FAILED", "地图已保存，源卷恢复副本未能清理。"
        + FailureDetails(!recoveryFile.Valid() ? "recovery-file-open" : !originalOwned ? "recovery-file-identity" : "recovery-file-posix-disposition", cleanupError, recovery)
        + "；expectedVolume=" + std::to_string(volumeSerial_) + ", actualVolume=" + std::to_string(originalInfo.dwVolumeSerialNumber)
        + ", expectedFileId=" + std::to_string(fileId_) + ", actualFileId=" + std::to_string(FileId(originalInfo))
        + ", expectedSize=" + std::to_string(FileSize(backupInfo)) + ", actualSize=" + std::to_string(FileSize(originalInfo)));
    recoveryFile.Close();
    discardRecoveryDirectory();
    diagnostics.insert(diagnostics.end(), cleanupDiagnostics.begin(), cleanupDiagnostics.end());
    result.prunedBackups = PruneBackups(backupDirectory, indexDirectory, *key.value, backup, options.keepBackups, diagnostics);
    auto success = Result<InPlaceSaveResult>::Success(std::move(result));
    success.diagnostics = std::move(diagnostics);
    return success;
}

} // namespace War3Map
