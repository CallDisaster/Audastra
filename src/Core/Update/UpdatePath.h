#pragma once

#include "War3MapCore/Types.h"

#include <Windows.h>

#include <memory>

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

inline constexpr std::uint64_t kMaxZoneIdentifierBytes = 16 * 1024;
inline constexpr std::uint64_t kMaxOriginMetadataBytes = 16 * 1024 * 1024;
inline constexpr wchar_t kZoneIdentifierSuffix[] = L":Zone.Identifier:$DATA";

struct OriginMetadata {
    std::vector<std::byte> bytes;
    std::string sha256;
    [[nodiscard]] bool operator==(const OriginMetadata&) const = default;
};
struct OriginStreamReadLease {
    std::optional<OriginMetadata> zoneIdentifier;
    // NTFS sharing按stream独立；默认流的只读句柄不能代替此租约。
    NativeHandle stream;
};
struct DirectoryReadLease {
    std::filesystem::path path;
    // 所有祖先目录都拒绝删除/rename共享，避免检查后被junction替换。
    std::vector<NativeHandle> anchors;
    OriginStreamReadLease origin;
};

struct FileReadLease;
class SharedDirectoryReadLease {
public:
    SharedDirectoryReadLease() = default;
    [[nodiscard]] bool Valid() const noexcept { return lease_ != nullptr; }
    // 只读元数据不暴露目录/命名流句柄；有效owner只能由真实锁定工厂创建。
    [[nodiscard]] const OriginMetadata* Origin() const noexcept {
        return lease_ && lease_->origin.zoneIdentifier ? &*lease_->origin.zoneIdentifier : nullptr;
    }
private:
    explicit SharedDirectoryReadLease(std::shared_ptr<const DirectoryReadLease> lease) : lease_(std::move(lease)) {}
    friend Result<SharedDirectoryReadLease> LockSharedSafeDirectory(const std::filesystem::path& path);
    friend Result<FileReadLease> OpenSafeReadFile(const std::filesystem::path& path, std::uint64_t maxBytes,
        const SharedDirectoryReadLease& parent);
    std::shared_ptr<const DirectoryReadLease> lease_;
};

struct FileReadLease {
    std::filesystem::path path;
    std::uint64_t bytes = 0;
    DirectoryReadLease parent;
    // 先声明父owner，逆序析构时保证文件及其ADS先关闭，再释放祖先目录锁。
    SharedDirectoryReadLease sharedParent;
    NativeHandle file;
    OriginStreamReadLease origin;
};

// base是已验证的普通磁盘文件/目录句柄。只接受固定Zone.Identifier元数据；
// cleanup仅为自身DELETE句柄增加share-delete，仍不给任何命名流写共享。
[[nodiscard]] Result<OriginStreamReadLease> LockUpdateOriginStreams(
    const std::filesystem::path& path, HANDLE base, bool shareDelete = false);
[[nodiscard]] bool OriginMatches(const OriginStreamReadLease& actual, const OriginMetadata* expected);

[[nodiscard]] Result<std::filesystem::path> NormalizeLocalPath(
    const std::filesystem::path& input, bool requireAbsolute);
[[nodiscard]] Result<DirectoryReadLease> LockSafeDirectory(const std::filesystem::path& path);
[[nodiscard]] Result<SharedDirectoryReadLease> LockSharedSafeDirectory(const std::filesystem::path& path);
[[nodiscard]] Result<FileReadLease> OpenSafeReadFile(const std::filesystem::path& path, std::uint64_t maxBytes);
[[nodiscard]] Result<FileReadLease> OpenSafeReadFile(const std::filesystem::path& path, std::uint64_t maxBytes,
    const SharedDirectoryReadLease& parent);
[[nodiscard]] bool SafeWindowsComponent(std::wstring_view component);
// 创建/rename的目标仍通过同一普通磁盘、单链接、final-path门禁；调用方持有父租约。
[[nodiscard]] Status CheckOpenedPath(HANDLE handle, const std::filesystem::path& path, bool directory);

struct WindowsPathLess {
    bool operator()(std::wstring_view left, std::wstring_view right) const noexcept;
};

} // namespace War3Map::Update::Detail
