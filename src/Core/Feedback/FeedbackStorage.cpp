#include "FeedbackInternal.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

namespace War3Map::Feedback::Detail {
namespace {

template<class T> Result<T> Failure(std::string code, std::string message) {
    return Result<T>::Failure("Feedback.Storage", std::move(code), std::move(message));
}
Status Fail(std::string code, std::string message) { return Status::Failure("Feedback.Storage", std::move(code), std::move(message)); }

class Handle {
public:
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~Handle() { if (Valid()) CloseHandle(handle_); }
    Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { if (Valid()) CloseHandle(handle_); handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE); }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] bool Valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE Get() const { return handle_; }
private:
    HANDLE handle_;
};

std::uint64_t FileSize(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}
bool Regular(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &info) && info.nNumberOfLinks == 1
        && !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE));
}

// 从根开始逐段持有禁止删除/重命名的目录句柄，不能经过 junction/symlink。
Result<std::filesystem::path> PinDirectories(const std::filesystem::path& requested, bool create, std::vector<Handle>& leases) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(requested, error).lexically_normal();
    if (error || requested.empty() || !absolute.is_absolute() || absolute.native().size() > 30000)
        return Failure<std::filesystem::path>("FEEDBACK_DIRECTORY_INVALID", "反馈本地目录无效。");
    auto pin = [&](const std::filesystem::path& path) -> bool {
        auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES && create) {
            if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
            attributes = GetFileAttributesW(path.c_str());
        }
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        Handle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!handle.Valid() || !GetFileInformationByHandle(handle.Get(), &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        leases.push_back(std::move(handle));
        return true;
    };
    auto path = absolute.root_path();
    if (!pin(path)) return Failure<std::filesystem::path>("FEEDBACK_DIRECTORY_UNAVAILABLE", "反馈目录不可锁定或包含链接。");
    for (const auto& part : absolute.relative_path()) {
        const auto component = part.native();
        if (component.empty() || component == L"." || component == L".." || component.find(L':') != std::wstring::npos
            || component.back() == L'.' || component.back() == L' ')
            return Failure<std::filesystem::path>("FEEDBACK_DIRECTORY_INVALID", "反馈目录包含不支持的路径组件。");
        path /= part;
        if (!pin(path)) return Failure<std::filesystem::path>("FEEDBACK_DIRECTORY_UNAVAILABLE", "反馈目录不可锁定或包含链接。");
    }
    return Result<std::filesystem::path>::Success(std::move(absolute));
}

struct FileSnapshot {
    Handle handle;
    std::vector<std::byte> bytes;
    std::string sha256;
};
Result<std::optional<FileSnapshot>> ReadSnapshot(const std::filesystem::path& file, std::size_t maxBytes) {
    Handle input(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!input.Valid()) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return Result<std::optional<FileSnapshot>>::Success(std::nullopt);
        return Failure<std::optional<FileSnapshot>>("FEEDBACK_QUEUE_READ_FAILED", "反馈队列不能只读锁定，原文件保持原样。");
    }
    if (!Regular(input.Get())) return Failure<std::optional<FileSnapshot>>("FEEDBACK_QUEUE_NOT_REGULAR", "反馈队列只接受单链接普通文件。");
    BY_HANDLE_FILE_INFORMATION before{};
    if (!GetFileInformationByHandle(input.Get(), &before) || FileSize(before) > maxBytes)
        return Failure<std::optional<FileSnapshot>>("FEEDBACK_QUEUE_BUDGET", "反馈队列长度超过容量或无法确认。");
    FileSnapshot snapshot{std::move(input), std::vector<std::byte>(static_cast<std::size_t>(FileSize(before))), {}};
    std::size_t offset = 0;
    while (offset < snapshot.bytes.size()) {
        const auto request = static_cast<DWORD>((std::min)(snapshot.bytes.size() - offset, std::size_t{1024 * 1024}));
        DWORD read = 0;
        if (!ReadFile(snapshot.handle.Get(), snapshot.bytes.data() + offset, request, &read, nullptr) || read == 0 || read > request)
            return Failure<std::optional<FileSnapshot>>("FEEDBACK_QUEUE_READ_FAILED", "反馈队列未完整读取。");
        offset += read;
    }
    std::byte extra{};
    DWORD tail = 0;
    BY_HANDLE_FILE_INFORMATION after{};
    if (!ReadFile(snapshot.handle.Get(), &extra, 1, &tail, nullptr) || tail != 0 || !GetFileInformationByHandle(snapshot.handle.Get(), &after)
        || FileSize(after) != FileSize(before) || CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0
        || before.nFileIndexHigh != after.nFileIndexHigh || before.nFileIndexLow != after.nFileIndexLow)
        return Failure<std::optional<FileSnapshot>>("FEEDBACK_QUEUE_CHANGED", "反馈队列在读取时变化，未使用部分内容。");
    const auto hash = Sha256(snapshot.bytes);
    if (!hash.Succeeded()) { Result<std::optional<FileSnapshot>> failure; failure.diagnostics = hash.diagnostics; return failure; }
    snapshot.sha256 = *hash.value;
    return Result<std::optional<FileSnapshot>>::Success(std::optional<FileSnapshot>{std::move(snapshot)});
}

std::wstring UniqueName() {
    static std::atomic<std::uint64_t> sequence{0};
    return L".feedback-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())
        + L"-" + std::to_wstring(sequence.fetch_add(1)) + L".tmp";
}

Status AtomicWrite(const std::filesystem::path& target, std::span<const std::byte> bytes, bool replace) {
    const auto staging = target.parent_path() / UniqueName();
    Handle output(CreateFileW(staging.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!output.Valid() || !Regular(output.Get())) return Fail("FEEDBACK_QUEUE_WRITE_FAILED", "反馈临时文件创建失败，目标保持原样。");
    // 清理只按自有文件句柄执行；不按可变路径删除文件。
    auto removeOwned = [&] {
        FILE_DISPOSITION_INFO disposition{TRUE};
        (void)SetFileInformationByHandle(output.Get(), FileDispositionInfo, &disposition, sizeof(disposition));
    };
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto request = static_cast<DWORD>((std::min)(bytes.size() - offset, std::size_t{1024 * 1024}));
        DWORD written = 0;
        if (!WriteFile(output.Get(), bytes.data() + offset, request, &written, nullptr) || written == 0 || written > request) {
            removeOwned();
            return Fail("FEEDBACK_QUEUE_WRITE_FAILED", "反馈临时文件未写完，目标保持原样。");
        }
        offset += written;
    }
    if (!FlushFileBuffers(output.Get())) { removeOwned(); return Fail("FEEDBACK_QUEUE_FLUSH_FAILED", "反馈临时文件未完成刷新，目标保持原样。"); }
    const auto name = target.wstring();
    const auto requestBytes = sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t);
    if (requestBytes > (std::numeric_limits<DWORD>::max)()) { removeOwned(); return Fail("FEEDBACK_DIRECTORY_INVALID", "反馈目标路径过长。"); }
    std::vector<std::byte> buffer(requestBytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    // legacy FileRenameInfo 即使目标分享 DELETE，也不能替换尚有快照句柄的文件。
    // Windows 10 RS1+ 的 POSIX 替换保留旧快照句柄，使租约覆盖最后一次原子切换。
    // 不支持 Ex 的系统明确失败；不能通过先释放快照削弱检查后假报原子保证。
    if (replace) rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    else rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
    // 同目录按句柄原子改名，不删除旧目标，不通过两次 rename 宣称原子性。
    if (!SetFileInformationByHandle(output.Get(), replace ? FileRenameInfoEx : FileRenameInfo,
        buffer.data(), static_cast<DWORD>(buffer.size()))) {
        const auto renameError = GetLastError();
        removeOwned();
        return Fail("FEEDBACK_QUEUE_RENAME_FAILED", "反馈原子写出失败；已有目标未主动删除或覆盖。Win32=" + std::to_string(renameError));
    }
    return Status::Ok();
}

} // namespace

struct QueueStorage::Impl {
    std::filesystem::path directory;
    std::filesystem::path target;
    std::size_t maxBytes = 0;
    std::vector<Handle> directories;
    Handle processLease;
    bool loaded = false;
    std::optional<std::string> expectedSha256;
};

QueueStorage::QueueStorage(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
QueueStorage::~QueueStorage() = default;

Result<std::unique_ptr<QueueStorage>> QueueStorage::Open(const std::filesystem::path& directory, std::size_t maxBytes) {
    try {
        if (maxBytes == 0 || maxBytes > 64 * 1024 * 1024) return Failure<std::unique_ptr<QueueStorage>>("FEEDBACK_QUEUE_BUDGET", "反馈存储容量无效。");
        auto impl = std::make_unique<Impl>();
        auto pinned = PinDirectories(directory, true, impl->directories);
        if (!pinned.Succeeded()) { Result<std::unique_ptr<QueueStorage>> failure; failure.diagnostics = pinned.diagnostics; return failure; }
        impl->directory = *pinned.value;
        impl->target = impl->directory / L"queue.json";
        impl->maxBytes = maxBytes;
        impl->processLease = Handle(CreateFileW((impl->directory / L".feedback-queue.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!impl->processLease.Valid() || !Regular(impl->processLease.Get()))
            return Failure<std::unique_ptr<QueueStorage>>("FEEDBACK_QUEUE_BUSY", "反馈队列已被其他实例使用或锁文件无效。");
        return Result<std::unique_ptr<QueueStorage>>::Success(std::unique_ptr<QueueStorage>(new QueueStorage(std::move(impl))));
    } catch (const std::exception&) {
        return Failure<std::unique_ptr<QueueStorage>>("FEEDBACK_STORAGE_FAILED", "反馈本地存储初始化失败。");
    }
}

Result<std::optional<std::vector<std::byte>>> QueueStorage::Load() {
    try {
        auto read = ReadSnapshot(impl_->target, impl_->maxBytes);
        if (!read.Succeeded()) { Result<std::optional<std::vector<std::byte>>> failure; failure.diagnostics = read.diagnostics; return failure; }
        impl_->loaded = true;
        impl_->expectedSha256 = read.value->has_value() ? std::optional{read.value->value().sha256} : std::nullopt;
        return Result<std::optional<std::vector<std::byte>>>::Success(read.value->has_value()
            ? std::optional<std::vector<std::byte>>{std::move(read.value->value().bytes)} : std::nullopt);
    } catch (const std::exception&) {
        return Failure<std::optional<std::vector<std::byte>>>("FEEDBACK_STORAGE_FAILED", "反馈本地队列读取失败。");
    }
}

Status QueueStorage::Save(std::span<const std::byte> bytes) {
    try {
        if (!impl_->loaded || bytes.size() > impl_->maxBytes) return Fail("FEEDBACK_QUEUE_BUDGET", "反馈队列未初始化或超过容量。");
        auto current = ReadSnapshot(impl_->target, impl_->maxBytes);
        if (!current.Succeeded()) return Status{current.diagnostics};
        const auto actual = current.value->has_value() ? std::optional{current.value->value().sha256} : std::nullopt;
        if (actual != impl_->expectedSha256) return Fail("FEEDBACK_QUEUE_EXTERNAL_CHANGE", "反馈队列已被外部修改，未覆盖其内容。");
        const auto hash = Sha256(bytes);
        if (!hash.Succeeded()) return Status{hash.diagnostics};
        const auto written = AtomicWrite(impl_->target, bytes, actual.has_value());
        if (written.Succeeded()) impl_->expectedSha256 = *hash.value;
        return written;
    } catch (const std::exception&) {
        return Fail("FEEDBACK_STORAGE_FAILED", "反馈持久化失败，未宣称状态已保存。");
    }
}

Status WriteNewExport(const std::filesystem::path& requested, std::span<const std::byte> bytes) {
    try {
        if (requested.empty() || requested.filename().empty() || requested.filename().native().find(L':') != std::wstring::npos
            || requested.filename().native().back() == L'.' || requested.filename().native().back() == L' ')
            return Fail("FEEDBACK_EXPORT_PATH_INVALID", "报告包目标路径无效。");
        std::vector<Handle> directories;
        const auto parent = PinDirectories(requested.parent_path().empty() ? std::filesystem::path(L".") : requested.parent_path(), false, directories);
        if (!parent.Succeeded()) return Status{parent.diagnostics};
        const auto target = *parent.value / requested.filename();
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND)
            return Fail("FEEDBACK_EXPORT_TARGET_EXISTS", "报告包目标已存在或无法确认，不覆盖已有文件。");
        return AtomicWrite(target, bytes, false);
    } catch (const std::exception&) {
        return Fail("FEEDBACK_EXPORT_FAILED", "报告包写出失败，未覆盖已有目标。");
    }
}

} // namespace War3Map::Feedback::Detail
