#include "War3MapCore/Services/MapTestWorkspace.h"

#include "War3MapCore/Hash.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace War3Map::Services {
struct MapTestWorkspaceLease::Impl {
    std::filesystem::path root;
    std::mutex mutex;
    bool consumed = false;
    bool refreshEligible = false, refreshAttempted = false, refreshFailed = false;
#ifdef _WIN32
    struct Entry {
        std::filesystem::path path;
        HANDLE handle = INVALID_HANDLE_VALUE;
        BY_HANDLE_FILE_INFORMATION identity{};
        std::string sha;
    };
    std::vector<Entry> directories, files;
    Status Capture(bool openRoot);
    Status ValidateRootPath() const;
    void DiscardNewMembers() noexcept {
        for (const auto& file : files) if (file.handle != INVALID_HANDLE_VALUE) CloseHandle(file.handle);
        files.clear();
        for (std::size_t index = 1; index < directories.size(); ++index)
            if (directories[index].handle != INVALID_HANDLE_VALUE) CloseHandle(directories[index].handle);
        if (!directories.empty()) directories.resize(1);
    }
    ~Impl() {
        for (const auto& file : files) if (file.handle != INVALID_HANDLE_VALUE) CloseHandle(file.handle);
        for (const auto& directory : directories) if (directory.handle != INVALID_HANDLE_VALUE) CloseHandle(directory.handle);
    }
#endif
};
MapTestWorkspaceLease::MapTestWorkspaceLease(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MapTestWorkspaceLease::~MapTestWorkspaceLease() = default;
const std::filesystem::path& MapTestWorkspaceLease::Root() const noexcept { return impl_->root; }

#ifdef _WIN32
Status MapTestWorkspaceLease::Impl::ValidateRootPath() const {
    const auto fail = [] { return Status::Failure("Map.Test.Workspace", "MAP_TEST_CLEANUP_IDENTITY", "测试目录路径不再指向已持有的原root，保留目录。"); };
    if (directories.empty() || directories.front().handle == INVALID_HANDLE_VALUE) return fail();
    // 只读身份探针共享DELETE以兼容已持有的原目录DELETE句柄；探针本身不请求DELETE。
    HANDLE probe = CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (probe == INVALID_HANDLE_VALUE) return fail();
    BY_HANDLE_FILE_INFORMATION original{}, current{};
    const bool same = GetFileInformationByHandle(directories.front().handle, &original)
        && GetFileInformationByHandle(probe, &current)
        && !(original.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        && !(current.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        && (current.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        && current.dwVolumeSerialNumber == original.dwVolumeSerialNumber
        && current.nFileIndexHigh == original.nFileIndexHigh && current.nFileIndexLow == original.nFileIndexLow;
    CloseHandle(probe);
    return same ? Status::Ok() : fail();
}

Status MapTestWorkspaceLease::Impl::Capture(bool openRoot) {
    const auto fail = [](std::string code, std::string message) { return Status::Failure("Map.Test.Workspace", std::move(code), std::move(message)); };
    std::uint64_t totalBytes = 0;
    const auto open = [&](const std::filesystem::path& path, bool directory) -> Status {
        // 文件初期不请求DELETE，与原YDWE CopyFile及ReadVerifiedFile的只读share兼容。
        const auto access = directory ? (FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | DELETE) : GENERIC_READ;
        // 目录自身不共享写入/删除；子文件的创建与写入操作不同对象，不需要共享root写入。
        const auto sharing = FILE_SHARE_READ;
        HANDLE handle = CreateFileW(path.c_str(), access, sharing, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL), nullptr);
        if (handle == INVALID_HANDLE_VALUE) return Status::Failure("Map.Test.Workspace", "MAP_TEST_CLEANUP_LEASE", "测试目录或文件无法取得防替换租约，保留目录。");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory || (!directory && info.nNumberOfLinks != 1)) {
            CloseHandle(handle); return Status::Failure("Map.Test.Workspace", "MAP_TEST_CLEANUP_IDENTITY", "测试目录含重解析点、硬链接或不一致身份，保留目录。");
        }
        MapTestWorkspaceLease::Impl::Entry entry{path, handle, info, {}};
        if (directory) {
            if (directories.size() >= 16) { CloseHandle(handle); return Status::Failure("Map.Test.Workspace", "MAP_TEST_CLEANUP_BUDGET", "测试目录数超出清理预算，保留目录。"); }
            try { directories.push_back(std::move(entry)); } catch (...) { CloseHandle(handle); throw; }
        } else {
            const auto size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            if (files.size() >= 512 || size > 4ull * 1024 * 1024 * 1024 - totalBytes) {
                CloseHandle(handle); return Status::Failure("Map.Test.Workspace", "MAP_TEST_CLEANUP_BUDGET", "测试文件数或总大小超出清理预算，保留目录。");
            }
            const auto sha = Sha256Handle(handle);
            if (!sha.Succeeded()) { CloseHandle(handle); Status result; result.diagnostics = sha.diagnostics; return result; }
            entry.sha = *sha.value; totalBytes += size;
            try { files.push_back(std::move(entry)); } catch (...) { CloseHandle(handle); throw; }
        }
        return Status::Ok();
    };
    if (openRoot) {
        const auto status = open(root, true);
        if (!status.Succeeded()) return status;
    }
    auto status = ValidateRootPath();
    if (!status.Succeeded()) return status;
    // BFS：每次在枚举该目录前先持有其原目录句柄，父目录重命名被原租约拒绝。
    for (std::size_t index = 0; index < directories.size(); ++index) {
        const auto directory = directories[index].path;
        std::error_code ec;
        std::filesystem::directory_iterator iterator(directory, ec);
        if (ec) return fail("MAP_TEST_CLEANUP_INVENTORY", "测试目录无法完整枚举，保留目录。");
        for (const auto& entry : iterator) {
            const auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                return fail("MAP_TEST_CLEANUP_REPARSE", "测试目录出现无法读取或重解析成员，保留目录。");
            status = open(entry.path(), (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
            if (!status.Succeeded()) return status;
        }
    }
    return ValidateRootPath();
}
#endif

Result<std::shared_ptr<MapTestWorkspaceLease>> FreezeMapTestWorkspace(const std::filesystem::path& ownedRoot) {
    using Output = Result<std::shared_ptr<MapTestWorkspaceLease>>;
    const auto fail = [](std::string code, std::string message) { return Output::Failure("Map.Test.Workspace", std::move(code), std::move(message)); };
#ifdef _WIN32
    if (!ownedRoot.is_absolute() || ownedRoot == ownedRoot.root_path())
        return fail("MAP_TEST_CLEANUP_ROOT", "测试清理租约必须绑定本次独占的绝对子目录。");
    auto state = std::make_unique<MapTestWorkspaceLease::Impl>();
    state->root = ownedRoot.lexically_normal();
    // 拒绝所有祖先重解析点，避免从表面owned路径读写另一目录。
    for (auto current = state->root; !current.empty();) {
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return fail("MAP_TEST_CLEANUP_REPARSE", "测试目录或其祖先不存在或经重解析点访问，保留目录。");
        const auto parent = current.parent_path(); if (parent == current) break; current = parent;
    }
    const auto status = state->Capture(true);
    if (!status.Succeeded()) { Output result; result.diagnostics = status.diagnostics; return result; }
    state->refreshEligible = state->directories.size() == 1 && state->files.empty();
    return Output::Success(std::shared_ptr<MapTestWorkspaceLease>(new MapTestWorkspaceLease(std::move(state))));
#else
    (void)ownedRoot; return fail("MAP_TEST_PLATFORM", "测试目录原句柄清理租约仅支持Windows。");
#endif
}

Status MapTestWorkspaceLease::Refresh() {
    std::lock_guard lock(impl_->mutex);
    const auto fail = [](std::string code, std::string message) { return Status::Failure("Map.Test.Workspace", std::move(code), std::move(message)); };
    if (impl_->consumed || !impl_->refreshEligible || impl_->refreshAttempted)
        return fail("MAP_TEST_REFRESH_NOT_ALLOWED", "只有初次冻结为空root的租约允许刷新一次，目录已保留。");
    impl_->refreshAttempted = true;
    impl_->refreshFailed = true;
#ifdef _WIN32
    try {
        const auto status = impl_->Capture(false); // 原root句柄持续持有，绝不再申请root DELETE句柄。
        if (!status.Succeeded()) {
            impl_->DiscardNewMembers();
            return status;
        }
    } catch (...) {
        impl_->DiscardNewMembers();
        return fail("MAP_TEST_REFRESH_FAILED", "刷新测试成员清单未能完成，原root仍由租约持有且全部文件保留。");
    }
    impl_->refreshFailed = false;
    return Status::Ok();
#else
    return fail("MAP_TEST_PLATFORM", "测试目录原句柄清理租约仅支持Windows。");
#endif
}

Status MapTestWorkspaceLease::Cleanup(bool gameTerminalConfirmed) {
    std::lock_guard lock(impl_->mutex);
    const auto fail = [](std::string code, std::string message) { return Status::Failure("Map.Test.Workspace", std::move(code), std::move(message)); };
    if (!gameTerminalConfirmed) return fail("MAP_TEST_CLEANUP_GAME_ACTIVE", "游戏全树终态尚未确认，测试目录保留。");
    if (impl_->consumed) return fail("MAP_TEST_CLEANUP_CONSUMED", "本次清理租约已经执行，不能复用旧身份。");
    if (impl_->refreshFailed) return fail("MAP_TEST_CLEANUP_INCOMPLETE", "测试目录成员刷新未完整成功，禁止用部分清单清理，保留全部文件。");
#ifdef _WIN32
    const auto rootIdentity = impl_->ValidateRootPath();
    if (!rootIdentity.Succeeded()) return rootIdentity;
    // 清理前扫描所有冻结目录，未知新增项令整个清理停止，不扩大本次删除名单。
    std::set<std::filesystem::path> known;
    for (const auto& entry : impl_->directories) known.insert(entry.path);
    for (const auto& entry : impl_->files) known.insert(entry.path);
    for (const auto& directory : impl_->directories) {
        BY_HANDLE_FILE_INFORMATION actual{};
        if (!GetFileInformationByHandle(directory.handle, &actual) || (actual.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || !(actual.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            || actual.dwVolumeSerialNumber != directory.identity.dwVolumeSerialNumber
            || actual.nFileIndexHigh != directory.identity.nFileIndexHigh || actual.nFileIndexLow != directory.identity.nFileIndexLow)
            return fail("MAP_TEST_CLEANUP_IDENTITY", "测试目录原身份变化或出现重解析，保留全部文件。");
        std::error_code ec; std::filesystem::directory_iterator iterator(directory.path, ec);
        if (ec) return fail("MAP_TEST_CLEANUP_INVENTORY", "测试目录无法复核，清理停止并保留目录。");
        for (const auto& entry : iterator) if (!known.contains(entry.path()))
            return fail("MAP_TEST_CLEANUP_UNKNOWN_ENTRY", "测试目录出现未冻结的新成员，清理停止并保留全部文件。");
    }
    for (const auto& file : impl_->files) {
        BY_HANDLE_FILE_INFORMATION actual{};
        if (!GetFileInformationByHandle(file.handle, &actual)
            || actual.dwVolumeSerialNumber != file.identity.dwVolumeSerialNumber
            || actual.nFileIndexHigh != file.identity.nFileIndexHigh || actual.nFileIndexLow != file.identity.nFileIndexLow)
            return fail("MAP_TEST_CLEANUP_IDENTITY", "测试文件原句柄身份无法确认，保留目录。");
        const auto sha = Sha256Handle(file.handle);
        if (!sha.Succeeded() || *sha.value != file.sha)
            return fail("MAP_TEST_CLEANUP_CHANGED", "测试文件内容已变化或无法回读，保留目录。");
    }
    impl_->consumed = true;
    for (auto& file : impl_->files) {
        // 目录身份仍由原句柄固定。只读share必须先关闭才能请求DELETE；新句柄
        // 必须精确匹配冻结身份及SHA，替换/硬链接/重解析时不删除任何未知文件。
        CloseHandle(file.handle); file.handle = INVALID_HANDLE_VALUE;
        file.handle = CreateFileW(file.path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION actual{};
        if (file.handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.handle, &actual)
            || (actual.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || actual.nNumberOfLinks != 1
            || actual.dwVolumeSerialNumber != file.identity.dwVolumeSerialNumber
            || actual.nFileIndexHigh != file.identity.nFileIndexHigh || actual.nFileIndexLow != file.identity.nFileIndexLow)
            return fail("MAP_TEST_CLEANUP_PARTIAL", "取得删除句柄时文件身份变化或不可访问；已知先前项可能已清理，当前及剩余项保留。");
        const auto sha = Sha256Handle(file.handle);
        if (!sha.Succeeded() || *sha.value != file.sha)
            return fail("MAP_TEST_CLEANUP_PARTIAL", "取得删除句柄时文件内容变化或无法核对；当前及剩余项保留。");
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(file.handle, FileDispositionInfo, &disposition, sizeof(disposition)))
            return fail("MAP_TEST_CLEANUP_PARTIAL", "已清理部分冻结文件，仍有原句柄删除失败；剩余项保留，不能称全部清理成功。");
        CloseHandle(file.handle); file.handle = INVALID_HANDLE_VALUE;
    }
    for (auto iterator = impl_->directories.rbegin(); iterator != impl_->directories.rend(); ++iterator) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(iterator->handle, FileDispositionInfo, &disposition, sizeof(disposition)))
            return fail("MAP_TEST_CLEANUP_DIRECTORY_RETAINED", "冻结文件已清理，但目录仍有新增项或删除失败；目录保留。");
        CloseHandle(iterator->handle); iterator->handle = INVALID_HANDLE_VALUE;
    }
    return Status::Ok();
#else
    return fail("MAP_TEST_PLATFORM", "测试目录原句柄清理租约仅支持Windows。");
#endif
}
} // namespace War3Map::Services
