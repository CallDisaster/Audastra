#pragma once

#include "War3MapCore/Types.h"

#include <Windows.h>

namespace War3Map::Update::Detail {

class NativeHandle {
public:
    explicit NativeHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~NativeHandle() { if (Valid()) CloseHandle(value_); }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    NativeHandle(NativeHandle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if (this != &other) {
            if (Valid()) CloseHandle(value_);
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    [[nodiscard]] bool Valid() const noexcept { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
private:
    HANDLE value_;
};

struct DirectoryReadLease {
    std::filesystem::path path;
    // 所有祖先目录都拒绝删除/rename共享，避免检查后被junction替换。
    std::vector<NativeHandle> anchors;
};

struct FileReadLease {
    std::filesystem::path path;
    std::uint64_t bytes = 0;
    DirectoryReadLease parent;
    NativeHandle file;
};

[[nodiscard]] Result<std::filesystem::path> NormalizeLocalPath(
    const std::filesystem::path& input, bool requireAbsolute);
[[nodiscard]] Result<DirectoryReadLease> LockSafeDirectory(const std::filesystem::path& path);
[[nodiscard]] Result<FileReadLease> OpenSafeReadFile(const std::filesystem::path& path, std::uint64_t maxBytes);
[[nodiscard]] bool SafeWindowsComponent(std::wstring_view component);
// 创建/rename的目标仍通过同一普通磁盘、单链接、final-path门禁；调用方持有父租约。
[[nodiscard]] Status CheckOpenedPath(HANDLE handle, const std::filesystem::path& path, bool directory);

struct WindowsPathLess {
    bool operator()(std::wstring_view left, std::wstring_view right) const noexcept;
};

} // namespace War3Map::Update::Detail
