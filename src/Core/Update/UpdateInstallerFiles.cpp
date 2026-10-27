#include "UpdateInstallerInternal.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <string>

namespace War3Map::Update::Detail {
namespace {

Result<RootIdentity> RootFromHandle(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return Result<RootIdentity>::Failure("Update", "UPDATE_ROOT_IDENTITY_FAILED", "无法读取目录文件身份。");
    return Result<RootIdentity>::Success({info.dwVolumeSerialNumber,
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow});
}

Status Failure(std::string code, std::string message) {
    return Status::Failure("Update", std::move(code), std::move(message));
}
class FindHandle {
public:
    explicit FindHandle(HANDLE value) : value_(value) {}
    ~FindHandle() { if (value_ != INVALID_HANDLE_VALUE) FindClose(value_); }
    [[nodiscard]] HANDLE Get() const { return value_; }
private:
    HANDLE value_;
};
std::filesystem::path Utf8Path(std::string_view text) {
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
Status EnsureDirectory(const std::filesystem::path& root, const std::filesystem::path& relative) {
    auto current = root;
    for (const auto& part : relative) {
        if (!SafeWindowsComponent(part.native())) return Failure("UPDATE_STAGE_PATH_INVALID", "镜像目录路径包含别名或越界。");
        const auto parent = LockSafeDirectory(current);
        if (!parent.Succeeded()) return {parent.diagnostics};
        current /= part;
        if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return Failure("UPDATE_STAGE_DIRECTORY_FAILED", "无法创建同卷镜像目录；请检查目录写入权限和空间。");
        const auto checked = LockSafeDirectory(current);
        if (!checked.Succeeded()) return {checked.diagnostics};
    }
    return Status::Ok();
}

bool SameObject(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

Status RemoveOwnedOrigin(NativeHandle& base, const std::filesystem::path& path, bool directory, OriginStreamReadLease& current) {
    BY_HANDLE_FILE_INFORMATION before{}, pinned{}, named{};
    if (!GetFileInformationByHandle(base.Get(), &before)) return Failure("UPDATE_COPY_METADATA_FAILED", "无法冻结自身镜像的原文件身份。");
    std::optional<std::string> mainHash;
    if (!directory) {
        const auto hash = Sha256Handle(base.Get());
        if (!hash.Succeeded()) return {hash.diagnostics};
        mainHash = *hash.value;
    }
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
    // 属性pin仅保留原对象身份，不宣称阻止rename；父目录anchors和源租约始终保留。
    NativeHandle pin(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, flags, nullptr));
    if (!pin.Valid() || !GetFileInformationByHandle(pin.Get(), &pinned) || !SameObject(before, pinned))
        return Failure("UPDATE_COPY_METADATA_FAILED", "自身镜像的来源标记删除身份pin不符。");
    current.stream = NativeHandle{};
    base = NativeHandle{}; // DELETE会跨stream检查共享；只释放自身镜像的目标tail。
    const auto streamPath = std::filesystem::path(path.native() + kZoneIdentifierSuffix);
    NativeHandle stream(CreateFileW(streamPath.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!stream.Valid() || !GetFileInformationByHandle(stream.Get(), &named) || !SameObject(before, named)
        || !CheckOpenedPath(pin.Get(), path, directory).Succeeded())
        return Failure("UPDATE_COPY_METADATA_FAILED", "自身镜像的待删除来源流身份或最终路径不符。");
    auto checked = LockUpdateOriginStreams(path, pin.Get(), true);
    if (!checked.Succeeded()) return {std::move(checked.diagnostics)};
    if (!OriginMatches(*checked.value, &*current.zoneIdentifier))
        return Failure("UPDATE_COPY_METADATA_FAILED", "自身镜像来源流在换挡后变化，未删除。");
    checked.value->stream = NativeHandle{};
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(stream.Get(), FileDispositionInfo, &disposition, sizeof(disposition)))
        return Failure("UPDATE_COPY_METADATA_FAILED", "自身镜像的已验证来源流未能接受删除。");
    stream = NativeHandle{};
    base = NativeHandle(CreateFileW(path.c_str(), directory ? FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY : GENERIC_READ | GENERIC_WRITE,
        directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : 0, nullptr, OPEN_EXISTING, flags, nullptr));
    BY_HANDLE_FILE_INFORMATION after{};
    if (!base.Valid() || !GetFileInformationByHandle(base.Get(), &after) || !SameObject(before, after)
        || !CheckOpenedPath(base.Get(), path, directory).Succeeded())
        return Failure("UPDATE_COPY_METADATA_FAILED", "删除自身标记后无法恢复相同身份的严格租约。");
    if (mainHash) {
        const auto hash = Sha256Handle(base.Get());
        if (!hash.Succeeded() || *hash.value != *mainHash || before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow)
            return Failure("UPDATE_COPY_METADATA_FAILED", "自身镜像默认流在标记删除换挡期间变化。");
    }
    return Status::Ok();
}

// 仅供CopyInventory写入自身next/rollback镜像；绝不清除源安装或备份上的标记。
Status CopyOwnedOrigin(NativeHandle& base, const std::filesystem::path& path, const OriginMetadata* expected, bool directory) {
    auto current = LockUpdateOriginStreams(path, base.Get());
    if (!current.Succeeded()) return {std::move(current.diagnostics)};
    if (OriginMatches(*current.value, expected)) return Status::Ok();
    const bool existed = current.value->zoneIdentifier.has_value();
    const auto streamPath = std::filesystem::path(path.native() + kZoneIdentifierSuffix);
    if (!expected) {
        const auto removed = RemoveOwnedOrigin(base, path, directory, *current.value);
        if (!removed.Succeeded()) return removed;
    } else {
        current.value->stream = NativeHandle{};
        NativeHandle output(CreateFileW(streamPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            existed ? OPEN_EXISTING : CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION original{}, actual{};
        if (!output.Valid() || !GetFileInformationByHandle(base.Get(), &original) || !GetFileInformationByHandle(output.Get(), &actual)
            || original.dwVolumeSerialNumber != actual.dwVolumeSerialNumber || original.nFileIndexHigh != actual.nFileIndexHigh
            || original.nFileIndexLow != actual.nFileIndexLow || !SetEndOfFile(output.Get()))
            return Failure("UPDATE_COPY_METADATA_FAILED", "无法独占写入自身镜像同一文件身份的下载标记。");
        DWORD written = 0;
        if (!WriteFile(output.Get(), expected->bytes.data(), static_cast<DWORD>(expected->bytes.size()), &written, nullptr)
            || written != expected->bytes.size() || !FlushFileBuffers(output.Get()))
            return Failure("UPDATE_COPY_METADATA_FAILED", "自身镜像的下载标记未完整写入或持久化。");
        const auto hash = Sha256Handle(output.Get());
        if (!hash.Succeeded() || *hash.value != expected->sha256)
            return Failure("UPDATE_COPY_METADATA_FAILED", "自身镜像的下载标记原字节校验失败。");
    }
    const auto checked = LockUpdateOriginStreams(path, base.Get());
    if (!checked.Succeeded()) return {checked.diagnostics};
    if (!OriginMatches(*checked.value, expected)) return Failure("UPDATE_COPY_METADATA_FAILED", "自身镜像下载标记的存在状态或原字节不符。");
    return Status::Ok();
}

Status CopyOne(const std::filesystem::path& source, const std::filesystem::path& destination,
    const UpdateFileEntry& expected, const OriginMetadata* origin, bool replace) {
    auto input = OpenSafeReadFile(source, 64ULL * 1024 * 1024 * 1024);
    if (!input.Succeeded()) return {std::move(input.diagnostics)};
    if (!OriginMatches(input.value->origin, origin)) return Failure("UPDATE_INVENTORY_CHANGED", "源用户文件下载标记与盘点不符，未复制元数据。");
    BY_HANDLE_FILE_INFORMATION original{};
    if (!GetFileInformationByHandle(input.value->file.Get(), &original)
        || (original.dwFileAttributes & (FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_OFFLINE)))
        return Failure("UPDATE_COPY_METADATA_UNSUPPORTED", "无法安全复制加密、离线或未知用户文件元数据；原树保持。");
    auto parent = LockSafeDirectory(destination.parent_path());
    if (!parent.Succeeded()) return {std::move(parent.diagnostics)};
    DWORD disposition = CREATE_NEW;
    const DWORD attributes = GetFileAttributesW(destination.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (!replace || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)))
            return Failure("UPDATE_STAGE_USER_CONFLICT", "恢复镜像中文件/目录碰撞，拒绝覆盖不确定内容。");
        // 只在自身新建镜像清除readonly；源目录的属性不变。
        if ((attributes & FILE_ATTRIBUTE_READONLY) && !SetFileAttributesW(destination.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY))
            return Failure("UPDATE_STAGE_WRITE_FAILED", "无法准备自身镜像中的只读文件。");
        disposition = OPEN_EXISTING;
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        return Failure("UPDATE_STAGE_WRITE_FAILED", "无法确认镜像目标文件是否存在。");
    }
    NativeHandle output(CreateFileW(destination.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!output.Valid()) return Failure("UPDATE_STAGE_WRITE_FAILED", "无法独占写入镜像文件；请检查目录权限和空间。");
    auto safe = CheckOpenedPath(output.Get(), destination, false);
    if (!safe.Succeeded()) return safe;
    if (disposition == OPEN_EXISTING && !SetEndOfFile(output.Get())) return Failure("UPDATE_STAGE_WRITE_FAILED", "无法截断自身恢复镜像文件。");
    std::array<std::byte, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    while (total < input.value->bytes) {
        const auto wanted = static_cast<DWORD>((std::min)(input.value->bytes - total, std::uint64_t{buffer.size()}));
        DWORD read = 0, written = 0;
        if (!ReadFile(input.value->file.Get(), buffer.data(), wanted, &read, nullptr) || read != wanted
            || !WriteFile(output.Get(), buffer.data(), read, &written, nullptr) || written != read)
            return Failure("UPDATE_COPY_FAILED", "复制用户内容中断，未授权目录切换。");
        total += read;
    }
    const auto digest = Sha256Handle(output.Get());
    if (total != expected.bytes || !digest.Succeeded() || *digest.value != expected.sha256)
        return Failure("UPDATE_INVENTORY_CHANGED", "镜像复制内容与原盘点不符，拒绝丢失并发用户改动。");
    const auto copiedOrigin = CopyOwnedOrigin(output, destination, origin, false);
    if (!copiedOrigin.Succeeded()) return copiedOrigin;
    if (!SetFileTime(output.Get(), &original.ftCreationTime, &original.ftLastAccessTime, &original.ftLastWriteTime)
        || !FlushFileBuffers(output.Get())) return Failure("UPDATE_COPY_FLUSH_FAILED", "镜像内容或时间戳未能持久写入。");
    output = NativeHandle{};
    constexpr DWORD preserved = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM
        | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    const auto finalAttributes = original.dwFileAttributes & preserved;
    if (!SetFileAttributesW(destination.c_str(), finalAttributes ? finalAttributes : FILE_ATTRIBUTE_NORMAL))
        return Failure("UPDATE_COPY_ATTRIBUTES_FAILED", "镜像文件属性未能保持，拒绝继续。");
    return Status::Ok();
}

} // namespace

Result<TreeInventory> InspectUpdateTree(const std::filesystem::path& root) {
    using R = Result<TreeInventory>;
    TreeInventory inventory;
    std::uint64_t originBytes = 0;
    const auto remember = [&](const std::wstring& key, const OriginMetadata* origin) {
        if (!origin) return true;
        if (origin->bytes.size() > kMaxOriginMetadataBytes - originBytes) return false;
        originBytes += origin->bytes.size(); inventory.origins.emplace(key, *origin);
        return true;
    };
    std::vector<std::pair<std::filesystem::path, unsigned>> pending{{root, 0}};
    while (!pending.empty()) {
        auto [directory, depth] = std::move(pending.back()); pending.pop_back();
        if (depth > 64) return R::Failure("Update", "UPDATE_INVENTORY_DEPTH_BUDGET", "安装目录超过64层，未返回部分盘点。");
        const auto directoryLease = LockSharedSafeDirectory(directory);
        if (!directoryLease.Succeeded()) return {std::nullopt, directoryLease.diagnostics};
        const auto directoryKey = directory == root ? std::wstring{} : directory.lexically_relative(root).generic_wstring();
        if (!remember(directoryKey, directoryLease.value->Origin()))
            return R::Failure("Update", "UPDATE_ORIGIN_METADATA_BUDGET", "安装树下载来源元数据超过累计有界容量。");
        WIN32_FIND_DATAW data{};
        FindHandle find(FindFirstFileW((directory / L"*").c_str(), &data));
        if (find.Get() == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;
            return R::Failure("Update", "UPDATE_INVENTORY_READ_FAILED", "无法完整枚举安装树。");
        }
        do {
            const std::wstring name(data.cFileName);
            if (name == L"." || name == L"..") continue;
            if (!SafeWindowsComponent(name) || (data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)))
                return R::Failure("Update", "UPDATE_INVENTORY_PATH_REJECTED", "安装树包含reparse、设备或不支持的文件名。");
            const auto path = directory / name;
            const auto relative = path.lexically_relative(root);
            auto key = relative.generic_wstring();
            if (key.empty() || relative.is_absolute() || inventory.files.contains(key) || inventory.directories.contains(key))
                return R::Failure("Update", "UPDATE_INVENTORY_PATH_COLLISION", "安装树包含大小写碰撞或重复身份。");
            if (inventory.files.size() + inventory.directories.size() >= 100000)
                return R::Failure("Update", "UPDATE_INVENTORY_COUNT_BUDGET", "安装树超过100000个文件/目录。");
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                inventory.directories.insert(std::move(key)); pending.emplace_back(path, depth + 1);
            } else {
                auto lease = OpenSafeReadFile(path, 64ULL * 1024 * 1024 * 1024, *directoryLease.value);
                if (!lease.Succeeded()) return {std::nullopt, std::move(lease.diagnostics)};
                if (!remember(key, lease.value->origin.zoneIdentifier ? &*lease.value->origin.zoneIdentifier : nullptr))
                    return R::Failure("Update", "UPDATE_ORIGIN_METADATA_BUDGET", "安装树下载来源元数据超过累计有界容量。");
                if (lease.value->bytes > 64ULL * 1024 * 1024 * 1024 - inventory.bytes)
                    return R::Failure("Update", "UPDATE_INVENTORY_SIZE_BUDGET", "安装树累计超过64GiB。");
                auto hash = Sha256Handle(lease.value->file.Get());
                if (!hash.Succeeded()) return {std::nullopt, std::move(hash.diagnostics)};
                inventory.bytes += lease.value->bytes;
                inventory.files.emplace(std::move(key), UpdateFileEntry{IO::GenericPathUtf8(relative), lease.value->bytes, std::move(*hash.value)});
            }
        } while (FindNextFileW(find.Get(), &data));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            return R::Failure("Update", "UPDATE_INVENTORY_READ_FAILED", "安装树枚举中断，拒绝使用部分结果。");
    }
    nlohmann::json snapshot{{"directories", nlohmann::json::array()}, {"files", nlohmann::json::array()}};
    for (const auto& directory : inventory.directories) snapshot["directories"].push_back(IO::GenericPathUtf8(directory));
    for (const auto& [key, file] : inventory.files) {
        (void)key;
        snapshot["files"].push_back({{"path", file.path}, {"bytes", file.bytes}, {"sha256", file.sha256}});
    }
    // 无流时保持原canonical JSON及hash，兼容既有无ADS事务/备份。
    if (!inventory.origins.empty()) {
        snapshot["origin_streams"] = nlohmann::json::array();
        for (const auto& [key, origin] : inventory.origins) snapshot["origin_streams"].push_back({
            {"path", IO::GenericPathUtf8(key)}, {"bytes", origin.bytes.size()}, {"sha256", origin.sha256}});
    }
    auto hash = Sha256Text(snapshot.dump());
    if (!hash.Succeeded()) return {std::nullopt, std::move(hash.diagnostics)};
    inventory.sha256 = std::move(*hash.value);
    return R::Success(std::move(inventory));
}

Result<RootIdentity> ReadRootIdentity(const std::filesystem::path& root) {
    auto lease = LockSafeDirectory(root);
    if (!lease.Succeeded()) return {std::nullopt, std::move(lease.diagnostics)};
    return RootFromHandle(lease.value->anchors.back().Get());
}

bool Within(const std::filesystem::path& child, const std::filesystem::path& root) {
    auto a = child.lexically_normal(), b = root.lexically_normal();
    auto part = a.begin();
    WindowsPathLess less;
    for (const auto& expected : b) {
        if (part == a.end() || less(part->native(), expected.native()) || less(expected.native(), part->native())) return false;
        ++part;
    }
    return true;
}

Status EnsureOwnedDirectories(const std::filesystem::path& root, const std::vector<std::string>& directories) {
    for (const auto& directory : directories) {
        const auto result = EnsureDirectory(root, Utf8Path(directory));
        if (!result.Succeeded()) return result;
    }
    return Status::Ok();
}

Status CopyInventory(const std::filesystem::path& source, const std::filesystem::path& destination,
    const TreeInventory& inventory, bool replace, const std::set<std::wstring, WindowsPathLess>& excluded) {
    for (const auto& directory : inventory.directories) {
        if (excluded.contains(directory)) return Failure("UPDATE_STAGE_USER_CONFLICT", "用户目录占用旧程序文件路径，无法安全回滚。");
        const auto created = EnsureDirectory(destination, std::filesystem::path(directory));
        if (!created.Succeeded()) return created;
    }
    for (const auto& [key, file] : inventory.files) {
        if (excluded.contains(key)) continue;
        const auto relative = Utf8Path(file.path);
        const auto created = EnsureDirectory(destination, relative.parent_path());
        if (!created.Succeeded()) return created;
        const auto copied = CopyOne(source / relative, destination / relative, file, InventoryOrigin(inventory, key), replace);
        if (!copied.Succeeded()) return copied;
    }
    // 文件/子目录创建完成后再同步目录标记；root与空目录的absence也必须覆盖旧镜像状态。
    std::vector<std::wstring> directoryKeys{L""};
    directoryKeys.insert(directoryKeys.end(), inventory.directories.begin(), inventory.directories.end());
    for (const auto& key : directoryKeys) {
        auto input = LockSafeDirectory(key.empty() ? source : source / key);
        auto output = LockSafeDirectory(key.empty() ? destination : destination / key);
        if (!input.Succeeded()) return {std::move(input.diagnostics)};
        if (!output.Succeeded()) return {std::move(output.diagnostics)};
        const auto expected = InventoryOrigin(inventory, key);
        if (!OriginMatches(input.value->origin, expected)) return Failure("UPDATE_INVENTORY_CHANGED", "源目录下载标记在复制期间变化。");
        output.value->origin.stream = NativeHandle{};
        const auto copied = CopyOwnedOrigin(output.value->anchors.back(), output.value->path, expected, true);
        if (!copied.Succeeded()) return copied;
    }
    const auto copied = InspectUpdateTree(destination);
    if (!copied.Succeeded()) return {copied.diagnostics};
    if (!PreservedOriginsMatch(inventory, *copied.value, excluded))
        return Failure("UPDATE_COPY_METADATA_FAILED", "镜像未完整保留用户文件或目录的下载标记。");
    return Status::Ok();
}

Result<std::vector<std::byte>> ReadLeaseBytes(FileReadLease& lease) {
    using R = Result<std::vector<std::byte>>;
    if (lease.bytes > kMaxUpdateManifestBytes) return R::Failure("Update", "UPDATE_INPUT_BUDGET", "签名输入超过容量。");
    std::vector<std::byte> bytes(static_cast<std::size_t>(lease.bytes));
    LARGE_INTEGER zero{};
    DWORD read = 0;
    if (!SetFilePointerEx(lease.file.Get(), zero, nullptr, FILE_BEGIN)
        || (!bytes.empty() && (!ReadFile(lease.file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
            || read != bytes.size()))) return R::Failure("Update", "UPDATE_INPUT_READ_FAILED", "无法完整读取已锁定的签名输入。");
    return R::Success(std::move(bytes));
}

Result<SignedInput> ReadSignedInput(const std::filesystem::path& manifest, const std::filesystem::path& signature,
    const TrustedUpdateKey& key) {
    auto rawLease = OpenSafeReadFile(manifest, kMaxUpdateManifestBytes);
    if (!rawLease.Succeeded()) return {std::nullopt, std::move(rawLease.diagnostics)};
    auto sigLease = OpenSafeReadFile(signature, 64);
    if (!sigLease.Succeeded()) return {std::nullopt, std::move(sigLease.diagnostics)};
    auto raw = ReadLeaseBytes(*rawLease.value), sig = ReadLeaseBytes(*sigLease.value);
    if (!raw.Succeeded()) return {std::nullopt, std::move(raw.diagnostics)};
    if (!sig.Succeeded()) return {std::nullopt, std::move(sig.diagnostics)};
    auto verified = VerifyAndParseUpdateManifest(*raw.value, *sig.value, key);
    if (!verified.Succeeded()) return {std::nullopt, std::move(verified.diagnostics)};
    return Result<SignedInput>::Success({std::move(*verified.value), std::move(*raw.value), std::move(*sig.value),
        std::move(*rawLease.value), std::move(*sigLease.value)});
}

Status WriteNewFile(const std::filesystem::path& file, std::span<const std::byte> bytes) {
    const auto parent = LockSafeDirectory(file.parent_path());
    if (!parent.Succeeded()) return {parent.diagnostics};
    NativeHandle handle(CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.Valid()) return Failure("UPDATE_METADATA_WRITE_FAILED", "无法独占新建更新元数据，请检查权限。");
    const auto checked = CheckOpenedPath(handle.Get(), file, false);
    if (!checked.Succeeded()) return checked;
    DWORD written = 0;
    if (bytes.size() > MAXDWORD || (!bytes.empty() && (!WriteFile(handle.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        || written != bytes.size())) || !FlushFileBuffers(handle.Get()))
        return Failure("UPDATE_METADATA_WRITE_FAILED", "更新元数据未能完整持久写入。");
    return Status::Ok();
}

Status RenameOwnedRoot(const std::filesystem::path& from, const std::filesystem::path& to, const RootIdentity& expected) {
    auto source = NormalizeLocalPath(from, true), destination = NormalizeLocalPath(to, true);
    if (!source.Succeeded()) return {std::move(source.diagnostics)};
    if (!destination.Succeeded()) return {std::move(destination.diagnostics)};
    if (source.value->parent_path() != destination.value->parent_path())
        return Failure("UPDATE_RENAME_SCOPE_INVALID", "更新只允许同一已锁定父目录中的同卷目录改名。");
    auto parent = LockSafeDirectory(source.value->parent_path());
    if (!parent.Succeeded()) return {std::move(parent.diagnostics)};
    if (GetFileAttributesW(destination.value->c_str()) != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND)
        return Failure("UPDATE_RENAME_DESTINATION_EXISTS", "更新目录切换目标已存在或身份未知，拒绝覆盖。");
    NativeHandle handle(CreateFileW(source.value->c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.Valid()) {
        const auto error = GetLastError();
        return Failure("UPDATE_DIRECTORY_RENAME_FAILED", "目录被占用或权限不足，无法改名（Windows 错误 "
            + std::to_string(error) + "）；未结束任何进程。");
    }
    const auto checked = CheckOpenedPath(handle.Get(), *source.value, true);
    if (!checked.Succeeded()) return checked;
    const auto identity = RootFromHandle(handle.Get());
    if (!identity.Succeeded() || *identity.value != expected)
        return Failure("UPDATE_ROOT_IDENTITY_CHANGED", "待切换目录文件身份已变化，拒绝改名。");
    const auto name = destination.value->native();
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists = FALSE; rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::copy(name.begin(), name.end(), rename->FileName);
    // 刚写入的程序文件可能被杀毒软件等短暂打开，目录改名会得到拒绝访问或共享冲突；等一小会儿再试（总计约 2 秒），
    // 仍失败才报错。只等待，不结束任何进程。
    for (DWORD attempt = 1, wait = 25;; ++attempt, wait = std::min<DWORD>(wait * 2, 400)) {
        if (SetFileInformationByHandle(handle.Get(), FileRenameInfo, rename, static_cast<DWORD>(buffer.size()))) break;
        const auto error = GetLastError();
        if (attempt >= 8 || (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION))
            return Failure("UPDATE_DIRECTORY_RENAME_FAILED", "同卷目录改名失败（Windows 错误 "
                + std::to_string(error) + "）；保留已有目录以便恢复。");
        Sleep(wait);
    }
    return CheckOpenedPath(handle.Get(), *destination.value, true);
}

Status VerifyProgramTree(const std::filesystem::path& root, const VerifiedUpdateManifest& target) {
    const auto inventory = InspectUpdateTree(root);
    if (!inventory.Succeeded()) return {inventory.diagnostics};
    for (const auto& file : target.Manifest().files) {
        const auto found = inventory.value->files.find(std::wstring(file.path.begin(), file.path.end()));
        if (found == inventory.value->files.end() || found->second.bytes != file.bytes || found->second.sha256 != file.sha256)
            return Failure("UPDATE_STAGE_PROGRAM_MISMATCH", "激活镜像中的程序文件与签名清单不符。");
    }
    return Status::Ok();
}

namespace {

struct CleanupFileLease {
    std::filesystem::path path;
    NativeHandle handle;
    FILE_BASIC_INFO basic{};
    OriginStreamReadLease origin;
};
struct CleanupDirectoryLease {
    std::wstring relative;
    std::filesystem::path path;
    NativeHandle handle;
    RootIdentity identity;
    DWORD attributes = 0;
    OriginStreamReadLease origin;
};

Status CheckCleanupStreams(const std::filesystem::path& path, HANDLE base, const OriginMetadata* expected) {
    const auto streams = LockUpdateOriginStreams(path, base, true);
    if (!streams.Succeeded()) return {streams.diagnostics};
    if (!OriginMatches(*streams.value, expected))
        return Failure("UPDATE_CLEANUP_CHANGED", "备份下载来源数据流与已冻结元数据不符，拒绝删除。");
    return Status::Ok();
}

Status CheckCleanupNames(const std::filesystem::path& root, const TreeInventory& expected,
    const std::vector<CleanupDirectoryLease>& directories) {
    std::size_t filesSeen = 0, directoriesSeen = 0;
    const auto inspect = [&](const std::filesystem::path& directory) -> Status {
        WIN32_FIND_DATAW data{};
        FindHandle find(FindFirstFileW((directory / L"*").c_str(), &data));
        if (find.Get() == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) return Status::Ok();
            return Failure("UPDATE_CLEANUP_CHANGED", "无法完整复核备份名称，未开始删除。");
        }
        do {
            const std::wstring name(data.cFileName);
            if (name == L"." || name == L"..") continue;
            if (!SafeWindowsComponent(name) || (data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)))
                return Failure("UPDATE_CLEANUP_CHANGED", "备份名称或路径类型在预检后变化，未开始删除。");
            const auto relative = (directory / name).lexically_relative(root).generic_wstring();
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!expected.directories.contains(relative))
                    return Failure("UPDATE_CLEANUP_CHANGED", "备份新增了未知目录，未开始删除。");
                ++directoriesSeen;
            } else {
                if (!expected.files.contains(relative))
                    return Failure("UPDATE_CLEANUP_CHANGED", "备份新增了未知文件，未开始删除。");
                ++filesSeen;
            }
        } while (FindNextFileW(find.Get(), &data));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            return Failure("UPDATE_CLEANUP_CHANGED", "备份名称枚举中断，未开始删除。");
        return Status::Ok();
    };
    auto status = inspect(root);
    if (!status.Succeeded()) return status;
    for (const auto& directory : directories) {
        status = inspect(directory.path);
        if (!status.Succeeded()) return status;
    }
    if (filesSeen != expected.files.size() || directoriesSeen != expected.directories.size())
        return Failure("UPDATE_CLEANUP_CHANGED", "备份文件或目录集合在预检后变化，未开始删除。");
    return Status::Ok();
}

bool CleanupPathMissing(const std::filesystem::path& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

} // namespace

TreeDeletionReceipt DeleteOwnedTree(const std::filesystem::path& root, const TreeInventory& expected,
    const RootIdentity& expectedId, const BackupCleanupTestHooks& hooks) {
    TreeDeletionReceipt receipt;
    const auto reject = [&](Status status) {
        receipt.status = std::move(status);
        return std::move(receipt);
    };
    try {
        auto rootLease = LockSafeDirectory(root);
        if (!rootLease.Succeeded()) return reject({std::move(rootLease.diagnostics)});
        const auto actualRoot = rootLease.value->path;
        const auto identity = RootFromHandle(rootLease.value->anchors.back().Get());
        if (!identity.Succeeded() || *identity.value != expectedId)
            return reject(Failure("UPDATE_CLEANUP_IDENTITY_CHANGED", "备份根身份已变化，保留整个未知目录。"));
        const auto checked = InspectUpdateTree(actualRoot);
        if (!checked.Succeeded()) return reject({checked.diagnostics});
        if (checked.value->sha256 != expected.sha256)
            return reject(Failure("UPDATE_CLEANUP_CHANGED", "备份内容已变化，保留全部未知内容。"));

        // 第一阶段不写属性、不标记删除。每个子目录只保留最后一枚租约，祖先由已持有租约保护；
        // 避免每个文件持有整条祖先链，并避免后续删目录时被本工具自己的重复租约阻塞。
        std::vector<CleanupDirectoryLease> directories;
        directories.reserve(expected.directories.size());
        FILE_BASIC_INFO rootBasic{};
        if (!GetFileInformationByHandleEx(rootLease.value->anchors.back().Get(), FileBasicInfo, &rootBasic, sizeof(rootBasic)))
            return reject(Failure("UPDATE_CLEANUP_BUSY", "无法预检备份根属性，未开始删除。"));
        for (const auto& relative : expected.directories) {
            const auto path = actualRoot / relative;
            auto lease = LockSafeDirectory(path);
            if (!lease.Succeeded()) return reject({std::move(lease.diagnostics)});
            const auto directoryId = RootFromHandle(lease.value->anchors.back().Get());
            FILE_BASIC_INFO basic{};
            if (!directoryId.Succeeded() || !GetFileInformationByHandleEx(lease.value->anchors.back().Get(), FileBasicInfo, &basic, sizeof(basic)))
                return reject(Failure("UPDATE_CLEANUP_BUSY", "无法预检备份子目录身份或属性，未开始删除。"));
            // 清理阶段的命名流必须允许本工具后续DELETE，但仍禁止其它写者。
            lease.value->origin.stream = NativeHandle{};
            auto origin = LockUpdateOriginStreams(path, lease.value->anchors.back().Get(), true);
            if (!origin.Succeeded()) return reject({std::move(origin.diagnostics)});
            if (!OriginMatches(*origin.value, InventoryOrigin(expected, relative)))
                return reject(Failure("UPDATE_CLEANUP_CHANGED", "备份目录下载标记已变化，未开始删除。"));
            directories.push_back({relative, path, std::move(lease.value->anchors.back()), *directoryId.value,
                basic.FileAttributes, std::move(*origin.value)});
        }
        std::vector<CleanupFileLease> files;
        files.reserve(expected.files.size());
        for (const auto& [key, file] : expected.files) {
            (void)key;
            const auto path = actualRoot / Utf8Path(file.path);
            NativeHandle handle(CreateFileW(path.c_str(), GENERIC_READ | DELETE | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!handle.Valid())
                return reject(Failure("UPDATE_CLEANUP_BUSY", "备份文件仍被占用，整个备份未开始删除。"));
            const auto safe = CheckOpenedPath(handle.Get(), path, false);
            const auto hash = Sha256Handle(handle.Get());
            LARGE_INTEGER size{};
            if (!safe.Succeeded() || !hash.Succeeded() || *hash.value != file.sha256 || !GetFileSizeEx(handle.Get(), &size)
                || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != file.bytes)
                return reject(Failure("UPDATE_CLEANUP_CHANGED", "备份文件身份或内容变化，整个备份未开始删除。"));
            FILE_BASIC_INFO basic{};
            if (!GetFileInformationByHandleEx(handle.Get(), FileBasicInfo, &basic, sizeof(basic)))
                return reject(Failure("UPDATE_CLEANUP_BUSY", "无法预检备份文件属性，整个备份未开始删除。"));
            auto origin = LockUpdateOriginStreams(path, handle.Get(), true);
            if (!origin.Succeeded()) return reject({std::move(origin.diagnostics)});
            if (!OriginMatches(*origin.value, InventoryOrigin(expected, key)))
                return reject(Failure("UPDATE_CLEANUP_CHANGED", "备份文件下载标记已变化，未开始删除。"));
            files.push_back({path, std::move(handle), basic, std::move(*origin.value)});
        }
        if (hooks.checkpoint) {
            const auto checkpoint = hooks.checkpoint("preflight-complete", actualRoot, actualRoot);
            if (!checkpoint.Succeeded()) return reject(checkpoint);
        }
        // 全部DELETE句柄已持有，不能再调用只分享READ的InspectUpdateTree来与自己冲突。
        // 名称另行枚举；身份、属性和SHA均复用已经锁定的原句柄，且不修改任何内容。
        const auto names = CheckCleanupNames(actualRoot, expected, directories);
        if (!names.Succeeded()) return reject(names);
        auto streams = CheckCleanupStreams(actualRoot, rootLease.value->anchors.back().Get(), InventoryOrigin(expected, L""));
        if (!streams.Succeeded()) return reject(streams);
        FILE_BASIC_INFO currentBasic{};
        if (!GetFileInformationByHandleEx(rootLease.value->anchors.back().Get(), FileBasicInfo, &currentBasic, sizeof(currentBasic))
            || currentBasic.FileAttributes != rootBasic.FileAttributes)
            return reject(Failure("UPDATE_CLEANUP_CHANGED", "备份根属性在预检后变化，未开始删除。"));
        for (const auto& directory : directories) {
            streams = CheckCleanupStreams(directory.path, directory.handle.Get(), InventoryOrigin(expected, directory.relative));
            const auto safe = CheckOpenedPath(directory.handle.Get(), directory.path, true);
            if (!streams.Succeeded()) return reject(streams);
            if (!safe.Succeeded() || !GetFileInformationByHandleEx(directory.handle.Get(), FileBasicInfo, &currentBasic, sizeof(currentBasic))
                || currentBasic.FileAttributes != directory.attributes)
                return reject(Failure("UPDATE_CLEANUP_CHANGED", "备份子目录身份或属性在预检后变化，未开始删除。"));
        }
        auto expectedFile = expected.files.begin();
        for (const auto& file : files) {
            streams = CheckCleanupStreams(file.path, file.handle.Get(), InventoryOrigin(expected, expectedFile->first));
            if (!streams.Succeeded()) return reject(streams);
            const auto safe = CheckOpenedPath(file.handle.Get(), file.path, false);
            const auto hash = Sha256Handle(file.handle.Get());
            if (!safe.Succeeded() || !hash.Succeeded() || *hash.value != expectedFile->second.sha256
                || !GetFileInformationByHandleEx(file.handle.Get(), FileBasicInfo, &currentBasic, sizeof(currentBasic))
                || currentBasic.FileAttributes != file.basic.FileAttributes)
                return reject(Failure("UPDATE_CLEANUP_CHANGED", "备份文件身份、内容或属性在预检后变化，未开始删除。"));
            ++expectedFile;
        }

        // 第二阶段才允许修改。删除本身无法跨整个目录树原子化，任何已接受操作都进入回执。
        for (auto& file : files) {
            const auto relative = file.path.lexically_relative(actualRoot).generic_wstring();
            streams = CheckCleanupStreams(file.path, file.handle.Get(), InventoryOrigin(expected, relative));
            if (!streams.Succeeded()) return reject(streams);
            const auto safe = CheckOpenedPath(file.handle.Get(), file.path, false);
            if (!safe.Succeeded() || !GetFileInformationByHandleEx(file.handle.Get(), FileBasicInfo, &currentBasic, sizeof(currentBasic))
                || currentBasic.FileAttributes != file.basic.FileAttributes)
                return reject(Failure("UPDATE_CLEANUP_CHANGED", "删除阶段遇到并发属性变化，停止清理并保留剩余内容。"));
            const bool readOnly = (file.basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
            if (readOnly) {
                FILE_BASIC_INFO writable{}; // 时间戳填0，保持原有时间；只去掉READONLY。
                writable.FileAttributes = file.basic.FileAttributes & ~FILE_ATTRIBUTE_READONLY;
                if (!writable.FileAttributes) writable.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                if (!SetFileInformationByHandle(file.handle.Get(), FileBasicInfo, &writable, sizeof(writable)))
                    return reject(Failure("UPDATE_CLEANUP_BUSY", "只读备份属性未能修改，停止清理并保留剩余内容。"));
                ++receipt.fileAttributeChanges;
            }
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(file.handle.Get(), FileDispositionInfo, &disposition, sizeof(disposition))) {
                if (readOnly) {
                    FILE_BASIC_INFO restored{};
                    restored.FileAttributes = file.basic.FileAttributes;
                    if (SetFileInformationByHandle(file.handle.Get(), FileBasicInfo, &restored, sizeof(restored)))
                        ++receipt.fileAttributeChanges;
                }
                return reject(Failure("UPDATE_CLEANUP_BUSY", "已验证备份文件未能接受删除，停止清理并保留剩余内容。"));
            }
            ++receipt.filesMarkedForDeletion;
            file.origin.stream = NativeHandle{};
            file.handle = NativeHandle{};
            if (hooks.checkpoint) {
                const auto checkpoint = hooks.checkpoint("file-delete-accepted", actualRoot, file.path);
                if (!checkpoint.Succeeded()) return reject(checkpoint);
            }
            if (!CleanupPathMissing(file.path))
                return reject(Failure("UPDATE_CLEANUP_BUSY", "已接受删除的备份文件路径尚未消失或出现新内容，停止清理。"));
        }

        std::sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) {
            return a.relative.size() != b.relative.size() ? a.relative.size() > b.relative.size() : WindowsPathLess{}(a.relative, b.relative);
        });
        const auto deleteDirectory = [&](const std::filesystem::path& path, const RootIdentity& expectedDirectory,
            DWORD expectedAttributes, NativeHandle& readLease, OriginStreamReadLease& origin) -> Status {
            origin.stream = NativeHandle{};
            readLease = NativeHandle{}; // 仅释放该目录，父目录身份仍由祖先租约保护。
            if (hooks.checkpoint) {
                const auto checkpoint = hooks.checkpoint("directory-lease-released", actualRoot, path);
                if (!checkpoint.Succeeded()) return checkpoint;
            }
            NativeHandle handle(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!handle.Valid()) return Failure("UPDATE_CLEANUP_BUSY", "备份目录有删除竞争或仍被占用，停止清理。");
            const auto safe = CheckOpenedPath(handle.Get(), path, true);
            const auto directoryId = RootFromHandle(handle.Get());
            FILE_BASIC_INFO basic{};
            if (!safe.Succeeded() || !directoryId.Succeeded() || *directoryId.value != expectedDirectory
                || !GetFileInformationByHandleEx(handle.Get(), FileBasicInfo, &basic, sizeof(basic))
                || basic.FileAttributes != expectedAttributes)
                return Failure("UPDATE_CLEANUP_IDENTITY_CHANGED", "备份目录最终身份或属性不符，不删除任何替换目录。");
            auto finalOrigin = LockUpdateOriginStreams(path, handle.Get(), true);
            if (!finalOrigin.Succeeded()) return {std::move(finalOrigin.diagnostics)};
            const auto relative = path == actualRoot ? std::wstring{} : path.lexically_relative(actualRoot).generic_wstring();
            if (!OriginMatches(*finalOrigin.value, InventoryOrigin(expected, relative)))
                return Failure("UPDATE_CLEANUP_CHANGED", "备份目录最终下载标记身份变化，拒绝删除。");
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(handle.Get(), FileDispositionInfo, &disposition, sizeof(disposition)))
                return Failure("UPDATE_CLEANUP_BUSY", "备份目录有新内容或未能接受删除，停止清理。");
            ++receipt.directoriesMarkedForDeletion;
            finalOrigin.value->stream = NativeHandle{};
            handle = NativeHandle{};
            if (!CleanupPathMissing(path)) return Failure("UPDATE_CLEANUP_BUSY", "已接受删除的目录路径尚未消失，停止清理。");
            return Status::Ok();
        };
        for (auto& directory : directories) {
            const auto deleted = deleteDirectory(directory.path, directory.identity, directory.attributes, directory.handle, directory.origin);
            if (!deleted.Succeeded()) return reject(deleted);
        }
        const auto deleted = deleteDirectory(actualRoot, expectedId, rootBasic.FileAttributes,
            rootLease.value->anchors.back(), rootLease.value->origin);
        if (!deleted.Succeeded()) return reject(deleted);
        receipt.rootRemoved = true;
        return receipt;
    } catch (const std::exception&) {
        return reject(Failure("UPDATE_CLEANUP_FAILED", "清理未完整执行，操作计数保留，未继续修改剩余备份。"));
    }
}

} // namespace War3Map::Update::Detail
