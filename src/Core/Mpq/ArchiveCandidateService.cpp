#include "War3MapCore/Mpq/ArchiveCandidateService.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "MpqWriterInternal.h"

#include <Windows.h>
#include <winternl.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace War3Map::Mpq {
namespace {

constexpr std::string_view kWtgName = "WAR3MAP.WTG";
constexpr std::string_view kWctName = "WAR3MAP.WCT";
constexpr std::string_view kListfileName = "(LISTFILE)";
constexpr std::uint32_t kMaxCandidateHashTableEntries = 65536;

MpqOpenLimits CandidateOpenLimits() {
    MpqOpenLimits limits;
    limits.maxArchivePrefixBytes = kArchiveCandidateMaxPrefixBytes;
    limits.maxDeclaredArchiveBytes = kArchiveCandidateMaxArchiveBytes;
    limits.maxHashTableEntries = kMaxCandidateHashTableEntries;
    limits.maxBlockTableEntries =
        static_cast<std::uint32_t>(kArchiveCandidateMaxMemberCount);
    limits.maxStrictMemberCount = kArchiveCandidateMaxMemberCount;
    return limits;
}

std::string UpperCanonical(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (const unsigned char raw : name) {
        const char c = static_cast<char>(raw);
        result.push_back(c == '/' ? '\\' : static_cast<char>(std::toupper(raw)));
    }
    return result;
}

bool IsSha256(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool ShaEquals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

Result<std::filesystem::path> ResolveTargetPath(const std::filesystem::path& path) {
    const std::wstring filename = path.filename().wstring();
    const auto unsafeFilename = [&] {
        if (filename.empty() || filename == L"." || filename == L".." ||
            filename.back() == L'.' || filename.back() == L' ') {
            return true;
        }
        return std::any_of(filename.begin(), filename.end(), [](wchar_t c) {
            return c < 0x20 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
                c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*';
        });
    }();
    if (path.empty() || !path.is_absolute() || unsafeFilename) {
        return Result<std::filesystem::path>::Failure(
            "ArchiveCandidate", "TARGET_PATH_INVALID",
            "candidate paths must be absolute file paths with safe simple filenames");
    }
    std::error_code ec;
    const auto parent = std::filesystem::weakly_canonical(path.parent_path(), ec);
    if (ec || !std::filesystem::is_directory(parent, ec) || ec) {
        return Result<std::filesystem::path>::Failure(
            "ArchiveCandidate", "TARGET_PARENT_INVALID",
            "candidate parent directory must already exist");
    }
    return Result<std::filesystem::path>::Success((parent / path.filename()).lexically_normal());
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool IsWithin(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    auto candidatePart = candidate.begin();
    auto rootPart = root.begin();
    for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() ||
            _wcsicmp(candidatePart->c_str(), rootPart->c_str()) != 0) {
            return false;
        }
    }
    return true;
}

Result<std::filesystem::path> FinalPathForHandle(HANDLE handle) {
    constexpr DWORD kFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, kFlags);
    if (required == 0) {
        return Result<std::filesystem::path>::Failure(
            "ArchiveCandidate", "FINAL_PATH_FAILED",
            "failed to resolve the handle-bound filesystem path");
    }
    std::wstring buffer(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()), kFlags);
    if (written == 0 || written >= buffer.size()) {
        return Result<std::filesystem::path>::Failure(
            "ArchiveCandidate", "FINAL_PATH_FAILED",
            "failed to resolve the complete handle-bound filesystem path");
    }
    buffer.resize(written);
    constexpr std::wstring_view kExtendedUnc = L"\\\\?\\UNC\\";
    constexpr std::wstring_view kExtended = L"\\\\?\\";
    if (buffer.starts_with(kExtendedUnc)) {
        buffer = L"\\\\" + buffer.substr(kExtendedUnc.size());
    } else if (buffer.starts_with(kExtended)) {
        buffer.erase(0, kExtended.size());
    }
    return Result<std::filesystem::path>::Success(
        std::filesystem::path(std::move(buffer)).lexically_normal());
}

Result<std::uint64_t> FileSizeForHandle(HANDLE handle) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) {
        return Result<std::uint64_t>::Failure(
            "ArchiveCandidate", "FILE_SIZE_FAILED",
            "failed to query the handle-bound file size");
    }
    return Result<std::uint64_t>::Success(static_cast<std::uint64_t>(size.QuadPart));
}

class UniqueWinHandle {
public:
    UniqueWinHandle() = default;
    explicit UniqueWinHandle(HANDLE handle) : handle_(handle) {}
    UniqueWinHandle(const UniqueWinHandle&) = delete;
    UniqueWinHandle& operator=(const UniqueWinHandle&) = delete;
    UniqueWinHandle(UniqueWinHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    UniqueWinHandle& operator=(UniqueWinHandle&& other) noexcept {
        if (this != &other) {
            Reset(std::exchange(other.handle_, INVALID_HANDLE_VALUE));
        }
        return *this;
    }
    ~UniqueWinHandle() { Reset(); }

    [[nodiscard]] bool Valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    void Reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (Valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::string JsonEscape(std::string_view value) {
    std::ostringstream output;
    for (const unsigned char c : value) {
        switch (c) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (c < 0x20) {
                constexpr char kHex[] = "0123456789abcdef";
                output << "\\u00" << kHex[(c >> 4) & 0x0F] << kHex[c & 0x0F];
            } else {
                output << static_cast<char>(c);
            }
        }
    }
    return output.str();
}

std::string BytesHex(std::string_view value) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char c : value) {
        result.push_back(kHex[(c >> 4) & 0x0F]);
        result.push_back(kHex[c & 0x0F]);
    }
    return result;
}

class DirectoryLease {
public:
    DirectoryLease() = default;
    DirectoryLease(const DirectoryLease&) = delete;
    DirectoryLease& operator=(const DirectoryLease&) = delete;
    DirectoryLease(DirectoryLease&&) noexcept = default;
    DirectoryLease& operator=(DirectoryLease&&) noexcept = default;
    ~DirectoryLease() = default;

    static Result<DirectoryLease> Open(const std::filesystem::path& path) {
        DirectoryLease result;
        result.handle_.Reset(CreateFileW(
            path.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!result.handle_.Valid()) {
            return Result<DirectoryLease>::Failure(
                "ArchiveCandidate", "TARGET_DIRECTORY_LEASE_FAILED",
                "candidate directory must remain locked against rename/delete");
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(result.handle_.Get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return Result<DirectoryLease>::Failure(
                "ArchiveCandidate", "TARGET_PARENT_INVALID",
                "candidate parent must resolve to a directory");
        }
        auto finalPath = FinalPathForHandle(result.handle_.Get());
        if (!finalPath.Succeeded() || !finalPath.value.has_value()) {
            Result<DirectoryLease> failure;
            failure.diagnostics = std::move(finalPath.diagnostics);
            return failure;
        }
        result.path_ = std::move(*finalPath.value);
        for (auto ancestor = result.path_.parent_path(); !ancestor.empty();) {
            const auto parent = ancestor.parent_path();
            if (parent.empty() || SamePath(ancestor, parent)) {
                break; // 卷/共享根不可重命名，不依赖 drive mapping 作为目录身份。
            }
            UniqueWinHandle ancestorHandle(CreateFileW(
                ancestor.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            if (!ancestorHandle.Valid()) {
                return Result<DirectoryLease>::Failure(
                    "ArchiveCandidate", "TARGET_ANCESTOR_LEASE_FAILED",
                    "every renamable candidate ancestor must remain locked through publication");
            }
            result.ancestorHandles_.push_back(std::move(ancestorHandle));
            ancestor = parent;
        }
        auto stableFinalPath = FinalPathForHandle(result.handle_.Get());
        if (!stableFinalPath.Succeeded() || !stableFinalPath.value.has_value() ||
            !SamePath(*stableFinalPath.value, result.path_)) {
            return Result<DirectoryLease>::Failure(
                "ArchiveCandidate", "TARGET_DIRECTORY_MOVED_DURING_LEASE",
                "candidate directory identity changed while its ancestor lease chain was acquired");
        }
        return Result<DirectoryLease>::Success(std::move(result));
    }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_.Get(); }
    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

private:
    UniqueWinHandle handle_;
    std::vector<UniqueWinHandle> ancestorHandles_;
    std::filesystem::path path_;
};

class HeldFile {
public:
    using NtSetInformationFileFn = NTSTATUS(NTAPI*)(
        HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);

    struct PreparedPublish {
        std::vector<std::byte> renameBuffer;
        NtSetInformationFileFn function = nullptr;
    };

    enum class CleanupStep {
        Succeeded,
        InvalidHandle,
        DispositionFailed,
        CloseFailed,
    };

    struct CleanupOutcome {
        CleanupStep step = CleanupStep::Succeeded;
        DWORD win32Error = ERROR_SUCCESS;
    };

    HeldFile() = default;
    HeldFile(const HeldFile&) = delete;
    HeldFile& operator=(const HeldFile&) = delete;
    HeldFile(HeldFile&& other) noexcept {
        *this = std::move(other);
    }
    HeldFile& operator=(HeldFile&& other) noexcept {
        if (this != &other) {
            Close();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
            parentPath_ = std::move(other.parentPath_);
            deleteOnClose_ = std::exchange(other.deleteOnClose_, false);
        }
        return *this;
    }
    ~HeldFile() { Close(); }

    static Result<HeldFile> Create(
        const std::filesystem::path& path,
        const std::filesystem::path& expectedParent) {
        HeldFile result;
        result.handle_ = CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (result.handle_ == INVALID_HANDLE_VALUE) {
            return Result<HeldFile>::Failure(
                "ArchiveCandidate", "TEMP_CREATE_EXCLUSIVE_FAILED",
                "temporary file must not exist and must be exclusively creatable");
        }
        result.deleteOnClose_ = true;
        auto finalPath = FinalPathForHandle(result.handle_);
        if (!finalPath.Succeeded() || !finalPath.value.has_value() ||
            !SamePath(finalPath.value->parent_path(), expectedParent)) {
            if (!finalPath.Succeeded()) {
                Result<HeldFile> failure;
                failure.diagnostics = std::move(finalPath.diagnostics);
                return failure;
            }
            return Result<HeldFile>::Failure(
                "ArchiveCandidate", "TEMP_PARENT_CHANGED",
                "temporary file did not resolve inside the leased candidate directory");
        }
        result.parentPath_ = expectedParent;
        return Result<HeldFile>::Success(std::move(result));
    }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }

    Result<PreparedPublish> PreparePublish(
        const DirectoryLease& targetDirectory,
        const std::filesystem::path& targetFileName) const {
        if (targetFileName.empty() || targetFileName != targetFileName.filename() ||
            !SamePath(parentPath_, targetDirectory.Path())) {
            return Result<PreparedPublish>::Failure(
                "ArchiveCandidate", "PUBLISH_NAME_INVALID",
                "publication requires a simple name inside the leased target directory");
        }
        const std::wstring targetName = targetFileName.wstring();
        constexpr std::size_t kMaxRenameBuffer =
            (std::numeric_limits<ULONG>::max)();
        if (targetName.size() >
            (kMaxRenameBuffer - sizeof(FILE_RENAME_INFO)) / sizeof(wchar_t)) {
            return Result<PreparedPublish>::Failure(
                "ArchiveCandidate", "PUBLISH_NAME_TOO_LONG",
                "publication filename exceeds the native rename buffer contract");
        }
        const ULONG bytes = static_cast<ULONG>(targetName.size() * sizeof(wchar_t));
        PreparedPublish prepared;
        prepared.renameBuffer.assign(
            sizeof(FILE_RENAME_INFO) + bytes, std::byte{0});
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(prepared.renameBuffer.data());
        rename->ReplaceIfExists = FALSE;
        rename->RootDirectory = targetDirectory.Get();
        rename->FileNameLength = bytes;
        std::memcpy(rename->FileName, targetName.data(), bytes);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        prepared.function = ntdll == nullptr
            ? nullptr
            : reinterpret_cast<NtSetInformationFileFn>(
                GetProcAddress(ntdll, "NtSetInformationFile"));
        if (prepared.function == nullptr) {
            return Result<PreparedPublish>::Failure(
                "ArchiveCandidate", "NATIVE_RENAME_UNAVAILABLE",
                "handle-relative rename entry point is unavailable");
        }
        return Result<PreparedPublish>::Success(std::move(prepared));
    }

    NTSTATUS PublishPrepared(PreparedPublish& prepared) noexcept {
        IO_STATUS_BLOCK ioStatus{};
        constexpr auto kFileRenameInformation = static_cast<FILE_INFORMATION_CLASS>(10);
        return prepared.function(
            handle_, &ioStatus, prepared.renameBuffer.data(),
            static_cast<ULONG>(prepared.renameBuffer.size()),
            kFileRenameInformation);
    }

    void Release() noexcept { deleteOnClose_ = false; }
    CleanupOutcome DeleteOwnedNowNoThrow() noexcept {
        if (handle_ == INVALID_HANDLE_VALUE || !deleteOnClose_) {
            return CleanupOutcome{CleanupStep::InvalidHandle, ERROR_INVALID_HANDLE};
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(
                handle_, FileDispositionInfo, &disposition, sizeof(disposition))) {
            return CleanupOutcome{CleanupStep::DispositionFailed, GetLastError()};
        }
        if (!CloseHandle(handle_)) {
            return CleanupOutcome{CleanupStep::CloseFailed, GetLastError()};
        }
        handle_ = INVALID_HANDLE_VALUE;
        deleteOnClose_ = false;
        return CleanupOutcome{};
    }
    void CloseReleased() noexcept {
        if (!deleteOnClose_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    void Close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            if (deleteOnClose_) {
                FILE_DISPOSITION_INFO disposition{TRUE};
                SetFileInformationByHandle(
                    handle_, FileDispositionInfo, &disposition, sizeof(disposition));
            }
            CloseHandle(handle_);
        }
        handle_ = INVALID_HANDLE_VALUE;
        deleteOnClose_ = false;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::filesystem::path parentPath_;
    bool deleteOnClose_ = false;
};

Status WriteTextHandle(HANDLE handle, std::string_view text) {
    bool succeeded = true;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const DWORD length = static_cast<DWORD>((std::min)(
            text.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(handle, text.data() + offset, length, &written, nullptr) ||
            written != length) {
            succeeded = false;
            break;
        }
        offset += written;
    }
    if (!FlushFileBuffers(handle)) {
        succeeded = false;
    }
    if (!succeeded) {
        return Status::Failure(
            "ArchiveCandidate", "MANIFEST_WRITE_FAILED", "failed to write candidate manifest");
    }
    return Status::Ok();
}

template <typename T>
Result<T> ForwardFailure(std::vector<Diagnostic> diagnostics) {
    Result<T> result;
    result.diagnostics = std::move(diagnostics);
    return result;
}

class SourceLease {
public:
    SourceLease() = default;
    SourceLease(const SourceLease&) = delete;
    SourceLease& operator=(const SourceLease&) = delete;
    SourceLease(SourceLease&&) noexcept = default;
    SourceLease& operator=(SourceLease&&) noexcept = default;
    ~SourceLease() = default;

    static Result<SourceLease> Open(const std::filesystem::path& path) {
        SourceLease result;
        result.handle_.Reset(CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!result.handle_.Valid()) {
            return Result<SourceLease>::Failure(
                "ArchiveCandidate", "SOURCE_LEASE_FAILED",
                "source archive must remain locked against write/delete for the whole transaction");
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(result.handle_.Get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return Result<SourceLease>::Failure(
                "ArchiveCandidate", "SOURCE_INVALID",
                "source archive must resolve to a regular file");
        }
        auto finalPath = FinalPathForHandle(result.handle_.Get());
        auto size = FileSizeForHandle(result.handle_.Get());
        if (!finalPath.Succeeded() || !finalPath.value.has_value()) {
            Result<SourceLease> failure;
            failure.diagnostics = std::move(finalPath.diagnostics);
            return failure;
        }
        if (!size.Succeeded() || !size.value.has_value()) {
            Result<SourceLease> failure;
            failure.diagnostics = std::move(size.diagnostics);
            return failure;
        }
        result.path_ = std::move(*finalPath.value);
        result.size_ = *size.value;
        return Result<SourceLease>::Success(std::move(result));
    }

    [[nodiscard]] bool Valid() const noexcept { return handle_.Valid(); }
    [[nodiscard]] HANDLE Get() const noexcept { return handle_.Get(); }
    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }
    [[nodiscard]] std::uint64_t Size() const noexcept { return size_; }

private:
    UniqueWinHandle handle_;
    std::filesystem::path path_;
    std::uint64_t size_ = 0;
};

} // namespace

std::filesystem::path ArchiveCandidateTempPath(const ArchiveCandidateRequest& request) {
    return request.outputArchivePath.parent_path() /
        (request.outputArchivePath.filename().wstring() + L".txn-" +
         std::to_wstring(request.transactionId) + L".tmp");
}

std::filesystem::path ArchiveCandidateManifestTempPath(const ArchiveCandidateRequest& request) {
    return request.manifestPath.parent_path() /
        (request.manifestPath.filename().wstring() + L".txn-" +
         std::to_wstring(request.transactionId) + L".tmp");
}

Result<ArchiveCandidateResult> ExportArchiveCandidateImpl(
    const ArchiveCandidateRequest& request) {
    if (request.transactionId == 0 || !IsSha256(request.expectedSourceSha256) ||
        !IsSha256(request.uiCatalogSha256) ||
        !IsSha256(request.wtg.expectedOriginalSha256) ||
        !IsSha256(request.wct.expectedOriginalSha256) || !request.finalGuard) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "REQUEST_BINDING_INVALID",
            "transaction and all SHA-256 bindings are required");
    }

    std::error_code ec;
    const auto requestedSource = std::filesystem::canonical(request.sourceArchivePath, ec);
    if (ec || !std::filesystem::is_regular_file(requestedSource, ec) || ec) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "SOURCE_INVALID", "source archive must be an existing regular file");
    }
    auto outputResult = ResolveTargetPath(request.outputArchivePath);
    auto manifestResult = ResolveTargetPath(request.manifestPath);
    if (!outputResult.Succeeded() || !outputResult.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(outputResult.diagnostics));
    }
    if (!manifestResult.Succeeded() || !manifestResult.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(manifestResult.diagnostics));
    }
    const auto requestedOutput = *outputResult.value;
    const auto requestedManifest = *manifestResult.value;
    const std::filesystem::path requiredManifest(
        requestedOutput.wstring() + L".manifest.json");
    if (!SamePath(requestedManifest, requiredManifest) ||
        !SamePath(requestedOutput.parent_path(), requestedManifest.parent_path())) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "CANDIDATE_ISOLATION_INVALID",
            "sidecar must be <output>.manifest.json and the candidate directory tree must be "
            "disjoint from the source directory tree");
    }

    auto sourceLeaseResult = SourceLease::Open(requestedSource);
    if (!sourceLeaseResult.Succeeded() || !sourceLeaseResult.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(sourceLeaseResult.diagnostics));
    }
    SourceLease sourceLease = std::move(*sourceLeaseResult.value);
    constexpr std::uint64_t kMaxPhysicalSourceBytes =
        kArchiveCandidateMaxPrefixBytes + kArchiveCandidateMaxArchiveBytes;
    if (sourceLease.Size() > kMaxPhysicalSourceBytes) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "SOURCE_PHYSICAL_BUDGET_EXCEEDED",
            "source archive exceeds the bounded prefix plus MPQ size contract");
    }

    auto directoryLeaseResult = DirectoryLease::Open(requestedOutput.parent_path());
    if (!directoryLeaseResult.Succeeded() || !directoryLeaseResult.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(directoryLeaseResult.diagnostics));
    }
    DirectoryLease directoryLease = std::move(*directoryLeaseResult.value);
    const auto output =
        (directoryLease.Path() / requestedOutput.filename()).lexically_normal();
    const auto manifest =
        (directoryLease.Path() / requestedManifest.filename()).lexically_normal();
    if (IsWithin(directoryLease.Path(), sourceLease.Path().parent_path()) ||
        IsWithin(sourceLease.Path().parent_path(), directoryLease.Path())) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "CANDIDATE_ISOLATION_INVALID",
            "handle-bound candidate and source directory trees must be disjoint");
    }

    ArchiveCandidateRequest normalized = request;
    normalized.outputArchivePath = output;
    normalized.manifestPath = manifest;
    const auto tempArchive = ArchiveCandidateTempPath(normalized);
    const auto tempManifest = ArchiveCandidateManifestTempPath(normalized);
    const std::vector<std::filesystem::path> paths{output, manifest, tempArchive, tempManifest};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        ec.clear();
        if (SamePath(paths[i], sourceLease.Path()) ||
            std::filesystem::exists(paths[i], ec) || ec) {
            return Result<ArchiveCandidateResult>::Failure(
                "ArchiveCandidate", "OUTPUT_PRECONDITION_FAILED",
                "output, sidecar, and temporary paths must not exist and must differ from source");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (SamePath(paths[i], paths[j])) {
                return Result<ArchiveCandidateResult>::Failure(
                    "ArchiveCandidate", "OUTPUT_PATH_COLLISION",
                    "output, sidecar, and temporary paths must be distinct");
            }
        }
    }

    auto sourceHash = Sha256Handle(sourceLease.Get());
    if (!sourceHash.Succeeded() || !sourceHash.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(sourceHash.diagnostics));
    }
    if (!ShaEquals(*sourceHash.value, request.expectedSourceSha256)) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "SOURCE_SHA_MISMATCH", "source archive SHA-256 changed");
    }

    const MpqOpenLimits openLimits = CandidateOpenLimits();
    auto opened = MpqArchive::OpenReadOnlyHandle(
        sourceLease.Get(), sourceLease.Path(), openLimits);
    if (!opened.Succeeded() || !opened.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(opened.diagnostics));
    }
    MpqArchive& archive = *opened.value;
    auto strictFiles = archive.ListFilesStrict();
    if (!strictFiles.Succeeded() || !strictFiles.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(strictFiles.diagnostics));
    }
    // 旁路写链要求源 (listfile) 与 writer 输出逐字节一致；元数据成员会在重建后过期，
    // 此处保持 fail-closed，而不是静默丢弃。
    if (std::any_of(strictFiles.value->begin(), strictFiles.value->end(),
            [](const MpqStrictFileInfo& info) { return IsMpqMetadataMemberName(info.name); })) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "ARCHIVE_METADATA_UNSUPPORTED",
            "side-write rejects sources carrying (attributes) or (signature)");
    }
    if (archive.ArchivePrefixSize() > kArchiveCandidateMaxPrefixBytes) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "ARCHIVE_PREFIX_BUDGET_EXCEEDED",
            "source prefix exceeds the 16 MiB side-write budget");
    }
    if (archive.DeclaredArchiveSize() > kArchiveCandidateMaxArchiveBytes) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "DECLARED_ARCHIVE_BUDGET_EXCEEDED",
            "declared MPQ exceeds the 256 MiB side-write budget");
    }
    if (strictFiles.value->size() > kArchiveCandidateMaxMemberCount) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "MEMBER_COUNT_BUDGET_EXCEEDED",
            "archive member count exceeds the bounded side-write inventory");
    }

    std::uint64_t sourceDecodedBytes = 0;
    std::uint64_t originalWtgBytes = 0;
    std::uint64_t originalWctBytes = 0;
    bool hasWtgMetadata = false;
    bool hasWctMetadata = false;
    for (const auto& info : *strictFiles.value) {
        if (info.fileSize > kArchiveCandidateMaxMemberBytes) {
            return Result<ArchiveCandidateResult>::Failure(
                "ArchiveCandidate", "MEMBER_DECODED_BUDGET_EXCEEDED",
                "an archive member exceeds the 64 MiB decoded-size budget: " + info.name);
        }
        if (info.fileSize > kArchiveCandidateMaxDecodedBytes - sourceDecodedBytes) {
            return Result<ArchiveCandidateResult>::Failure(
                "ArchiveCandidate", "SOURCE_DECODED_BUDGET_EXCEEDED",
                "source member inventory exceeds the 128 MiB decoded-size budget");
        }
        sourceDecodedBytes += info.fileSize;
        const std::string canonical = UpperCanonical(info.name);
        if (canonical == kWtgName) {
            originalWtgBytes = info.fileSize;
            hasWtgMetadata = true;
        } else if (canonical == kWctName) {
            originalWctBytes = info.fileSize;
            hasWctMetadata = true;
        }
    }
    if (!hasWtgMetadata || !hasWctMetadata) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "TRIGGER_PAIR_MISSING",
            "source must contain a complete war3map.wtg/war3map.wct pair");
    }
    if (request.wtg.data.size() > kArchiveCandidateMaxMemberBytes ||
        request.wct.data.size() > kArchiveCandidateMaxMemberBytes) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "REPLACEMENT_MEMBER_BUDGET_EXCEEDED",
            "WTG/WCT replacement exceeds the 64 MiB decoded-size budget");
    }
    std::uint64_t candidateDecodedBytes =
        sourceDecodedBytes - originalWtgBytes - originalWctBytes;
    for (const std::size_t replacementSize :
         {request.wtg.data.size(), request.wct.data.size()}) {
        if (replacementSize > kArchiveCandidateMaxDecodedBytes - candidateDecodedBytes) {
            return Result<ArchiveCandidateResult>::Failure(
                "ArchiveCandidate", "CANDIDATE_DECODED_BUDGET_EXCEEDED",
                "candidate member inventory exceeds the 128 MiB decoded-size budget");
        }
        candidateDecodedBytes += replacementSize;
    }

    auto prefix = archive.ReadArchivePrefix();
    if (!prefix.Succeeded() || !prefix.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(prefix.diagnostics));
    }

    std::vector<MpqBuildFile> buildFiles;
    buildFiles.reserve(strictFiles.value->size() - 1);
    std::map<std::string, std::string> originalMemberShas;
    std::map<std::string, std::string> originalNames;
    bool foundWtg = false;
    bool foundWct = false;
    bool foundListfile = false;
    std::vector<std::byte> canonicalListfileData;
    for (const auto& info : *strictFiles.value) {
        auto data = archive.ReadFile(info.name);
        if (!data.Succeeded() || !data.value.has_value()) {
            return ForwardFailure<ArchiveCandidateResult>(std::move(data.diagnostics));
        }
        const std::string canonical = UpperCanonical(info.name);
        originalNames.emplace(canonical, info.name);
        const auto originalSha = Sha256(*data.value);
        if (!originalSha.Succeeded() || !originalSha.value.has_value()) {
            return ForwardFailure<ArchiveCandidateResult>(std::move(originalSha.diagnostics));
        }
        originalMemberShas.emplace(canonical, *originalSha.value);
        if (canonical == kListfileName) {
            std::string generated;
            for (const auto& listed : buildFiles) {
                generated += listed.name;
                generated += "\r\n";
            }
            if (data.value->size() != generated.size() ||
                (!generated.empty() && std::memcmp(
                    data.value->data(), generated.data(), generated.size()) != 0)) {
                return Result<ArchiveCandidateResult>::Failure(
                    "ArchiveCandidate", "LISTFILE_NOT_CANONICAL",
                    "source (listfile) bytes must match the writer's canonical order and CRLF form");
            }
            canonicalListfileData = std::move(*data.value);
            foundListfile = true;
            continue;
        }

        MpqBuildFile build;
        build.name = info.name;
        build.compress = true;
        if (canonical == kWtgName) {
            if (!ShaEquals(*originalSha.value, request.wtg.expectedOriginalSha256)) {
                return Result<ArchiveCandidateResult>::Failure(
                    "ArchiveCandidate", "WTG_ORIGINAL_SHA_MISMATCH",
                    "war3map.wtg does not match the transaction baseline");
            }
            build.data = request.wtg.data;
            foundWtg = true;
        } else if (canonical == kWctName) {
            if (!ShaEquals(*originalSha.value, request.wct.expectedOriginalSha256)) {
                return Result<ArchiveCandidateResult>::Failure(
                    "ArchiveCandidate", "WCT_ORIGINAL_SHA_MISMATCH",
                    "war3map.wct does not match the transaction baseline");
            }
            build.data = request.wct.data;
            foundWct = true;
        } else {
            build.data = std::move(*data.value);
        }
        buildFiles.push_back(std::move(build));
    }
    if (!foundWtg || !foundWct || !foundListfile) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "TRIGGER_PAIR_MISSING",
            "source must contain a complete war3map.wtg/war3map.wct pair and canonical (listfile)");
    }
    std::map<std::string, const std::vector<std::byte>*> expectedMembers;
    for (const auto& build : buildFiles) {
        expectedMembers.emplace(UpperCanonical(build.name), &build.data);
    }
    expectedMembers.emplace(std::string(kListfileName), &canonicalListfileData);

    auto sourceAfterRead = Sha256Handle(sourceLease.Get());
    if (!sourceAfterRead.Succeeded() || !sourceAfterRead.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(sourceAfterRead.diagnostics));
    }
    if (!ShaEquals(*sourceAfterRead.value, request.expectedSourceSha256)) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "SOURCE_CHANGED_DURING_READ", "source changed while members were read");
    }

    auto tempArchiveCreated = HeldFile::Create(tempArchive, directoryLease.Path());
    if (!tempArchiveCreated.Succeeded() || !tempArchiveCreated.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(tempArchiveCreated.diagnostics));
    }
    HeldFile tempArchiveFile = std::move(*tempArchiveCreated.value);
    auto written = WriteMpqArchiveToHandle(
        tempArchiveFile.Get(), buildFiles, *prefix.value);
    if (!written.Succeeded()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(written.diagnostics));
    }
    auto generatedCandidateSha = Sha256Handle(tempArchiveFile.Get());
    if (!generatedCandidateSha.Succeeded() || !generatedCandidateSha.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(generatedCandidateSha.diagnostics));
    }
    auto candidateOpened = MpqArchive::OpenReadOnlyHandle(
        tempArchiveFile.Get(), tempArchive, openLimits);
    if (!candidateOpened.Succeeded() || !candidateOpened.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(candidateOpened.diagnostics));
    }
    auto candidatePrefix = candidateOpened.value->ReadArchivePrefix();
    auto candidateStrict = candidateOpened.value->ListFilesStrict();
    if (!candidatePrefix.Succeeded() || !candidatePrefix.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(candidatePrefix.diagnostics));
    }
    if (!candidateStrict.Succeeded() || !candidateStrict.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(candidateStrict.diagnostics));
    }
    if (*candidatePrefix.value != *prefix.value ||
        candidateStrict.value->size() != expectedMembers.size()) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "CANDIDATE_STRUCTURE_MISMATCH",
            "candidate prefix or member count differs from source");
    }
    std::set<std::string> verifiedNames;
    for (const auto& info : *candidateStrict.value) {
        const std::string canonical = UpperCanonical(info.name);
        const auto expected = expectedMembers.find(canonical);
        if (expected == expectedMembers.end() || !verifiedNames.insert(canonical).second) {
            return Result<ArchiveCandidateResult>::Failure(
                "ArchiveCandidate", "CANDIDATE_MEMBER_SET_MISMATCH",
                "candidate member set differs from source");
        }
        if (info.fileSize != expected->second->size()) {
            return Result<ArchiveCandidateResult>::Failure(
                "ArchiveCandidate", "CANDIDATE_MEMBER_SIZE_MISMATCH",
                "candidate member size differs from the bounded expected bytes: " + info.name);
        }
        auto actual = candidateOpened.value->ReadFile(info.name);
        if (!actual.Succeeded() || !actual.value.has_value()) {
            return ForwardFailure<ArchiveCandidateResult>(std::move(actual.diagnostics));
        }
        if (*actual.value != *expected->second) {
            return Result<ArchiveCandidateResult>::Failure(
                "ArchiveCandidate", "CANDIDATE_MEMBER_HASH_MISMATCH",
                "candidate member bytes failed full verification: " + info.name);
        }
    }
    if (verifiedNames.size() != expectedMembers.size()) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "CANDIDATE_MEMBER_SET_MISMATCH",
            "candidate does not contain every source member");
    }
    candidateOpened.value.reset();

    auto wtgSha = Sha256(request.wtg.data);
    auto wctSha = Sha256(request.wct.data);
    if (!wtgSha.Succeeded() || !wtgSha.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(wtgSha.diagnostics));
    }
    if (!wctSha.Succeeded() || !wctSha.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(wctSha.diagnostics));
    }
    auto lockedCandidateSha = Sha256Handle(tempArchiveFile.Get());
    if (!lockedCandidateSha.Succeeded() || !lockedCandidateSha.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(lockedCandidateSha.diagnostics));
    }
    if (!ShaEquals(*lockedCandidateSha.value, *generatedCandidateSha.value)) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "TEMP_ARCHIVE_DRIFT",
            "temporary archive changed after its generated-handle SHA-256 was bound");
    }

    std::ostringstream manifestJson;
    manifestJson
        << "{\n"
        << "  \"schemaVersion\": 1,\n"
        << "  \"candidateKind\": \"mpq-side-write\",\n"
        << "  \"distributionStatus\": \"blocked\",\n"
        << "  \"deploymentAuthorized\": false,\n"
        << "  \"mapArchiveIntegrationAuthorized\": false,\n"
        << "  \"sourceArchiveModified\": false,\n"
        << "  \"runnableMapClaimed\": false,\n"
        << "  \"editorTrueSave\": false,\n"
        << "  \"runtimeValidated\": false,\n"
        << "  \"warcraftExecuted\": false,\n"
        << "  \"transactionId\": \"" << request.transactionId << "\",\n"
        << "  \"sourceArchiveSha256\": \"" << JsonEscape(*sourceHash.value) << "\",\n"
        << "  \"candidateSha256\": \"" << JsonEscape(*generatedCandidateSha.value) << "\",\n"
        << "  \"uiCatalogSha256\": \"" << JsonEscape(request.uiCatalogSha256) << "\",\n"
        << "  \"memberCount\": " << expectedMembers.size() << ",\n"
        << "  \"members\": [\n";
    std::size_t memberIndex = 0;
    for (const auto& [canonical, candidateData] : expectedMembers) {
        auto originalSha = originalMemberShas.find(canonical);
        auto originalName = originalNames.find(canonical);
        if (originalSha == originalMemberShas.end() || originalName == originalNames.end()) {
            return Result<ArchiveCandidateResult>::Failure(
                "ArchiveCandidate", "MANIFEST_INVENTORY_FAILED",
                "member inventory lost its source identity");
        }
        auto candidateMemberSha = Sha256(*candidateData);
        if (!candidateMemberSha.Succeeded() || !candidateMemberSha.value.has_value()) {
            return Result<ArchiveCandidateResult>::Failure(
                "ArchiveCandidate", "MANIFEST_INVENTORY_HASH_FAILED",
                "failed to hash complete member inventory");
        }
        const bool replaced = canonical == kWtgName || canonical == kWctName;
        manifestJson
            << "    {\"canonicalName\": \"" << JsonEscape(canonical)
            << "\", \"nameBytesHex\": \"" << BytesHex(originalName->second)
            << "\", \"status\": \"" << (replaced ? "replaced" : "unchanged")
            << "\", \"sourceSha256\": \"" << originalSha->second
            << "\", \"candidateSha256\": \"" << *candidateMemberSha.value << "\"}"
            << (++memberIndex == expectedMembers.size() ? "\n" : ",\n");
    }
    manifestJson
        << "  ],\n"
        << "  \"replacements\": {\n"
        << "    \"war3map.wtg\": {\"originalSha256\": \""
        << JsonEscape(request.wtg.expectedOriginalSha256) << "\", \"candidateSha256\": \""
        << JsonEscape(*wtgSha.value) << "\"},\n"
        << "    \"war3map.wct\": {\"originalSha256\": \""
        << JsonEscape(request.wct.expectedOriginalSha256) << "\", \"candidateSha256\": \""
        << JsonEscape(*wctSha.value) << "\"}\n"
        << "  }\n"
        << "}\n";
    auto tempManifestCreated = HeldFile::Create(tempManifest, directoryLease.Path());
    if (!tempManifestCreated.Succeeded() || !tempManifestCreated.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(tempManifestCreated.diagnostics));
    }
    HeldFile tempManifestFile = std::move(*tempManifestCreated.value);
    auto manifestWritten = WriteTextHandle(tempManifestFile.Get(), manifestJson.str());
    if (!manifestWritten.Succeeded()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(manifestWritten.diagnostics));
    }
    const std::string manifestText = manifestJson.str();
    auto expectedManifestSha = Sha256Text(manifestText);
    if (!expectedManifestSha.Succeeded() || !expectedManifestSha.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(expectedManifestSha.diagnostics));
    }
    auto lockedManifestSha = Sha256Handle(tempManifestFile.Get());
    if (!lockedManifestSha.Succeeded() || !lockedManifestSha.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(lockedManifestSha.diagnostics));
    }
    if (!ShaEquals(*lockedManifestSha.value, *expectedManifestSha.value)) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "TEMP_MANIFEST_DRIFT",
            "temporary manifest differs from the bytes written through its exclusive handle");
    }
    auto manifestPublishPrepared = tempManifestFile.PreparePublish(
        directoryLease, manifest.filename());
    if (!manifestPublishPrepared.Succeeded() ||
        !manifestPublishPrepared.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(
            std::move(manifestPublishPrepared.diagnostics));
    }
    auto archivePublishPrepared = tempArchiveFile.PreparePublish(
        directoryLease, output.filename());
    if (!archivePublishPrepared.Succeeded() ||
        !archivePublishPrepared.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(
            std::move(archivePublishPrepared.diagnostics));
    }

    auto sourceBeforePublish = Sha256Handle(sourceLease.Get());
    if (!sourceBeforePublish.Succeeded() || !sourceBeforePublish.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(sourceBeforePublish.diagnostics));
    }
    if (!ShaEquals(*sourceBeforePublish.value, request.expectedSourceSha256)) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "SOURCE_CHANGED_BEFORE_PUBLISH",
            "source changed before candidate publication");
    }
    auto finalGuard = request.finalGuard();
    if (!finalGuard.Succeeded()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(finalGuard.diagnostics));
    }
    auto sourceAfterGuard = Sha256Handle(sourceLease.Get());
    if (!sourceAfterGuard.Succeeded() || !sourceAfterGuard.value.has_value()) {
        return ForwardFailure<ArchiveCandidateResult>(std::move(sourceAfterGuard.diagnostics));
    }
    if (!ShaEquals(*sourceAfterGuard.value, request.expectedSourceSha256)) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "SOURCE_CHANGED_AFTER_FINAL_GUARD",
            "source changed during the final external guard");
    }

    // 所有可能分配的结果字段都在提交点前完成，避免 archive rename 成功后再抛异常。
    ArchiveCandidateResult result;
    result.outputArchivePath = output;
    result.manifestPath = manifest;
    result.candidateSha256 = *generatedCandidateSha.value;
    result.wtgSha256 = *wtgSha.value;
    result.wctSha256 = *wctSha.value;

    // sidecar 先发布，archive rename 是提交点；提交失败时显式回滚 sidecar 并报告清理错误。
    const NTSTATUS manifestPublishStatus = tempManifestFile.PublishPrepared(
        *manifestPublishPrepared.value);
    if (manifestPublishStatus < 0) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "PUBLISH_NO_REPLACE_FAILED",
            "handle-relative sidecar publish failed without replacing the target (ntstatus=" +
                std::to_string(static_cast<std::uint32_t>(manifestPublishStatus)) + ")");
    }
    const NTSTATUS archivePublishStatus = tempArchiveFile.PublishPrepared(
        *archivePublishPrepared.value);
    if (archivePublishStatus < 0) {
        // 先执行完全无分配的回滚，再构造任何错误文本。
        const auto rollback = tempManifestFile.DeleteOwnedNowNoThrow();
        auto failure = Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "PUBLISH_NO_REPLACE_FAILED",
            "handle-relative archive publish failed without replacing the target (ntstatus=" +
                std::to_string(static_cast<std::uint32_t>(archivePublishStatus)) + ")");
        if (rollback.step != HeldFile::CleanupStep::Succeeded) {
            std::string code;
            switch (rollback.step) {
            case HeldFile::CleanupStep::InvalidHandle:
                code = "ROLLBACK_HANDLE_INVALID";
                break;
            case HeldFile::CleanupStep::DispositionFailed:
                code = "ROLLBACK_DELETE_FAILED";
                break;
            case HeldFile::CleanupStep::CloseFailed:
                code = "ROLLBACK_CLOSE_FAILED";
                break;
            case HeldFile::CleanupStep::Succeeded:
                break;
            }
            failure.diagnostics.push_back(Diagnostic{
                DiagnosticSeverity::Error,
                "ArchiveCandidate",
                std::move(code),
                "explicit sidecar rollback failed (win32=" +
                    std::to_string(rollback.win32Error) + ")",
                std::nullopt});
        }
        return failure;
    }

    tempManifestFile.Release();
    tempArchiveFile.Release();
    tempManifestFile.CloseReleased();
    tempArchiveFile.CloseReleased();
    return Result<ArchiveCandidateResult>::Success(std::move(result));
}

Result<ArchiveCandidateResult> ExportArchiveCandidate(
    const ArchiveCandidateRequest& request) {
    try {
        return ExportArchiveCandidateImpl(request);
    } catch (const std::exception& exception) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "UNEXPECTED_EXCEPTION", exception.what());
    } catch (...) {
        return Result<ArchiveCandidateResult>::Failure(
            "ArchiveCandidate", "UNEXPECTED_EXCEPTION", "unknown exception");
    }
}

} // namespace War3Map::Mpq
