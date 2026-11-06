#include "War3MapCore/Services/LuaFolder.h"
#include "War3MapCore/Services/ImportPayloadPolicy.h"
#include "War3MapCore/Services/ClassicLuaMapSources.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Lua/LuaSyntax.h"
#include "War3MapCore/Runtime/LuaModuleNames.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <Windows.h>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace War3Map::Services {
namespace {
std::string Utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::string Key(std::string value) {
    for (auto& c : value) {
        if (c == '/') c = '\\';
        else if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return value;
}
struct Failure : std::runtime_error {
    std::string code;
    Failure(std::string c, std::string message) : std::runtime_error(std::move(message)), code(std::move(c)) {}
};
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
};
using Handles = std::vector<std::shared_ptr<Handle>>;
[[noreturn]] void Fail(const char* code, const std::filesystem::path& path, const char* reason) {
    throw Failure(code, Utf8(path) + "：" + reason);
}
std::shared_ptr<Handle> Open(const std::filesystem::path& path, DWORD access, DWORD creation, bool directory) {
    auto handle = std::make_shared<Handle>(CreateFileW(path.c_str(), access,
        FILE_SHARE_READ | (directory ? FILE_SHARE_WRITE : 0), nullptr, creation,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL), nullptr));
    if (handle->value == INVALID_HANDLE_VALUE) Fail("LUA_FOLDER_IO", path, "文件被占用、无权限或已改变。");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle->value, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        || bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
        Fail("LUA_FOLDER_UNSAFE_PATH", path, "来源不是普通文件或目录，未跟随链接。");
    return handle;
}
void PinDirectories(const std::filesystem::path& directory, Handles& handles, bool create = false) {
    const auto absolute = std::filesystem::absolute(directory).lexically_normal();
    auto part = absolute.root_path();
    if (absolute.root_name().wstring().size() != 2 || absolute.root_name().wstring()[1] != L':')
        Fail("LUA_FOLDER_UNSAFE_PATH", absolute, "仅支持普通本机盘符路径。");
    handles.push_back(Open(part, FILE_LIST_DIRECTORY, OPEN_EXISTING, true));
    for (const auto& name : absolute.relative_path()) {
        part /= name;
        if (create) {
            std::error_code ec;
            std::filesystem::create_directory(part, ec);
            if (ec && ec != std::errc::file_exists) Fail("LUA_FOLDER_IO", part, "无法创建目录。");
        }
        handles.push_back(Open(part, FILE_LIST_DIRECTORY, OPEN_EXISTING, true));
    }
}
std::vector<std::byte> Read(HANDLE handle, const std::filesystem::path& path) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0
        || static_cast<std::uint64_t>(size.QuadPart) > ImportPayloadPolicy::luaDraftBytes)
        Fail("LUA_FOLDER_FILE_BUDGET", path, "Lua 单文件超过2 MiB或长度不可读。");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(handle, start, nullptr, FILE_BEGIN)) Fail("LUA_FOLDER_IO", path, "无法读取文件。");
    DWORD read = 0;
    if (!bytes.empty() && (!ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
        || read != bytes.size())) Fail("LUA_FOLDER_IO", path, "文件读取不完整。");
    return bytes;
}
std::string Hash(std::span<const std::byte> bytes) {
    auto sha = Sha256(bytes);
    if (!sha.Succeeded()) throw Failure("LUA_FOLDER_HASH", "无法计算Lua来源SHA。");
    return *sha.value;
}
std::vector<std::filesystem::path> Enumerate(const std::filesystem::path& root,
    std::vector<std::filesystem::path>& directories, std::size_t& skipped, Handles& leases) {
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> pending{root};
    std::size_t scanned = 0;
    std::size_t pathBytes = 0;
    while (!pending.empty()) {
        auto directory = std::move(pending.back()); pending.pop_back();
        leases.push_back(Open(directory, FILE_LIST_DIRECTORY, OPEN_EXISTING, true));
        directories.push_back(directory);
        for (const auto& item : std::filesystem::directory_iterator(directory)) {
            if (++scanned > 262144) Fail("LUA_FOLDER_SCAN_BUDGET", directory, "目录条目超过262144项。");
            const auto path = item.path();
            pathBytes += path.native().size() * sizeof(std::filesystem::path::value_type);
            if (pathBytes > 16u * 1024u * 1024u || directories.size() + pending.size() > 8192)
                Fail("LUA_FOLDER_SCAN_BUDGET", path, "目录路径超过16 MiB或8192目录预算。");
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) Fail("LUA_FOLDER_IO", path, "无法查询条目。");
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { ++skipped; continue; }
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!path.filename().wstring().starts_with(L'.')) pending.push_back(path);
                continue;
            }
            if (Key(Utf8(path.extension())) != ".lua") continue;
            if (files.size() == kLuaFolderFiles) Fail("LUA_FOLDER_FILE_BUDGET", path, "Lua文件超过4096个。");
            files.push_back(path.lexically_relative(root));
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return Key(Utf8(a)) < Key(Utf8(b)); });
    return files;
}
Status Error(const Failure& error) { return Status::Failure("LuaFolder", error.code, error.what()); }
void Check(const Status& status) {
    if (!status.Succeeded()) throw Failure(status.diagnostics.front().code, status.diagnostics.front().message);
}
void Write(HANDLE handle, std::span<const std::byte> bytes, const std::filesystem::path& path) {
    LARGE_INTEGER start{};
    DWORD written = 0;
    if (!SetFilePointerEx(handle, start, nullptr, FILE_BEGIN)
        || (!bytes.empty() && (!WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
            || written != bytes.size())) || !SetEndOfFile(handle) || !FlushFileBuffers(handle))
        Fail("LUA_FOLDER_WRITE_FAILED", path, "写盘未完成。");
}
}

class LuaFolderReadLease { public: Handles handles; };

bool IsAuthorLuaMember(std::string_view path) {
    const auto key = Key(std::string(path));
    if (!key.ends_with(".lua") || Resources::IsEditorManagedMemberPath(key)) return false;
    return std::none_of(kClassicLuaGeneratedMembers.begin(), kClassicLuaGeneratedMembers.end(),
        [&](auto generated) { return key == Key(std::string(generated)); });
}

Status ValidateLuaFolderFile(std::string_view path, std::span<const std::byte> bytes, bool syntax) {
    const auto canonical = Resources::CanonicalImportPath(path);
    auto module = std::string(path.substr(0, path.size() >= 4 ? path.size() - 4 : 0));
    for (auto& c : module) if (c == '/' || c == '\\') c = '.';
    if (!IsAuthorLuaMember(path) || !canonical.Succeeded()
        || Key(Audastra::Runtime32::LuaModuleMember(module)) != Key(std::string(path)))
        return Status::Failure("LuaFolder", "LUA_FOLDER_MODULE_PATH", std::string(path) + "：不是可用的手写Lua模块路径，不能覆盖编辑器生成脚本。");
    if (bytes.size() > ImportPayloadPolicy::luaDraftBytes)
        return Status::Failure("LuaFolder", "LUA_FOLDER_FILE_BUDGET", std::string(path) + "：Lua单文件超过2 MiB。");
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.find('\0') != text.npos || !Audastra::Runtime32::LuaMailboxUtf8(text))
        return Status::Failure("LuaFolder", "LUA_FOLDER_ENCODING", std::string(path) + "：Lua必须是无NUL的有效UTF-8。");
    if (syntax) {
        const auto checked = Lua::CheckSyntax(text, "@" + std::string(path));
        if (!checked.Succeeded()) {
            const auto& d = checked.diagnostics.front();
            return Status::Failure("LuaFolder", "LUA_FOLDER_SYNTAX", std::string(path) + " 第"
                + std::to_string(d.line) + "行：" + d.message);
        }
    }
    return Status::Ok();
}

Result<LuaFolderSnapshot> CaptureLuaFolder(const std::filesystem::path& folder, bool syntax, bool holdReadLease) {
    try {
        LuaFolderSnapshot snapshot;
        snapshot.root = std::filesystem::absolute(folder).lexically_normal();
        std::error_code ec;
        const bool exists = std::filesystem::exists(snapshot.root, ec);
        if ((!exists && !ec) || ec == std::errc::no_such_file_or_directory) {
            snapshot.missing = true;
            snapshot.sha256 = Hash({});
            return Result<LuaFolderSnapshot>::Success(std::move(snapshot));
        }
        if (ec || !std::filesystem::is_directory(snapshot.root)) Fail("LUA_FOLDER_DIRECTORY", folder, "无法读取关联目录。");
        auto lease = std::make_shared<LuaFolderReadLease>();
        PinDirectories(snapshot.root, lease->handles);
        const auto paths = Enumerate(snapshot.root, snapshot.directories, snapshot.skippedLinks, lease->handles);
        std::set<std::string> names;
        std::size_t total = 0;
        std::string identity = "lua-folder-v1\n" + Utf8(snapshot.root) + '\n';
        for (const auto& relative : paths) {
            const auto name = Utf8(relative);
            if (!names.insert(Key(name)).second) Fail("LUA_FOLDER_DUPLICATE", relative, "模块路径大小写冲突。");
            auto handle = Open(snapshot.root / relative, GENERIC_READ, OPEN_EXISTING, false);
            auto bytes = Read(handle->value, relative);
            Check(ValidateLuaFolderFile(name, bytes, syntax));
            total += bytes.size();
            if (total > kLuaFolderTotalBytes) Fail("LUA_FOLDER_TOTAL_BUDGET", relative, "Lua合计超过32 MiB。");
            const auto sha = Hash(bytes);
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle->value, &info)) Fail("LUA_FOLDER_IO", relative, "无法核对文件身份。");
            identity += Key(name) + ':' + sha + ':' + std::to_string(info.dwVolumeSerialNumber) + ':'
                + std::to_string(info.nFileIndexHigh) + ':' + std::to_string(info.nFileIndexLow) + '\n';
            snapshot.files.push_back({name, sha, std::make_shared<const std::vector<std::byte>>(std::move(bytes))});
            lease->handles.push_back(std::move(handle));
        }
        std::vector<std::filesystem::path> checkDirectories;
        std::size_t skipped = 0;
        Handles checkLeases;
        if (Enumerate(snapshot.root, checkDirectories, skipped, checkLeases) != paths)
            Fail("LUA_FOLDER_CHANGED", folder, "读取期间目录文件列表改变，请重试。");
        snapshot.sha256 = Hash(std::as_bytes(std::span(identity)));
        if (holdReadLease) snapshot.lease = std::move(lease);
        return Result<LuaFolderSnapshot>::Success(std::move(snapshot));
    } catch (const Failure& error) { return {std::nullopt, Error(error).diagnostics}; }
    catch (const std::exception&) { return Result<LuaFolderSnapshot>::Failure("LuaFolder", "LUA_FOLDER_IO", "无法完整捕获Lua文件夹，请检查权限、路径和预算。"); }
}

Status VerifyLuaFolder(const LuaFolderSnapshot& snapshot) {
    const auto current = CaptureLuaFolder(snapshot.root);
    if (!current.Succeeded()) return {current.diagnostics};
    if (current.value->missing != snapshot.missing || current.value->sha256 != snapshot.sha256)
        return Status::Failure("LuaFolder", "LUA_FOLDER_CHANGED", "Lua文件夹在快照后改变，未发布地图，请重试。");
    return Status::Ok();
}

LuaFolderDifference CompareLuaFolder(std::span<const LuaFolderFile> map, const LuaFolderSnapshot& folder) {
    LuaFolderDifference diff;
    std::map<std::string, const LuaFolderFile*> disk;
    for (const auto& file : folder.files) disk.emplace(Key(file.path), &file);
    for (const auto& file : map) {
        const auto found = disk.find(Key(file.path));
        if (found == disk.end()) diff.mapOnly.push_back(file.path);
        else {
            if (found->second->sha256 != file.sha256) diff.different.push_back(file.path);
            disk.erase(found);
        }
    }
    for (const auto& [key, file] : disk) diff.folderOnly.push_back(file->path);
    return diff;
}

Status WriteLuaFolderFile(const std::filesystem::path& root, std::string_view name,
    std::span<const std::byte> bytes, std::string_view expectedSha256) {
    try {
        Check(ValidateLuaFolderFile(name, bytes, false));
        const auto relative = std::filesystem::u8path(name);
        const auto path = root / relative;
        Handles directories;
        PinDirectories(path.parent_path(), directories, true);
        auto handle = Open(path, GENERIC_READ | GENERIC_WRITE, expectedSha256.empty() ? CREATE_NEW : OPEN_EXISTING, false);
        auto previous = Read(handle->value, path);
        if (!expectedSha256.empty() && Hash(previous) != expectedSha256) Fail("LUA_FOLDER_WRITE_CONFLICT", path, "外部文件已改变，草稿保留，未覆盖。");
        try { Write(handle->value, bytes, path); }
        catch (...) {
            try { Write(handle->value, previous, path); }
            catch (...) { Fail("LUA_FOLDER_WRITE_INCOMPLETE", path, "写入及恢复均失败，请核对磁盘文件；地图未改变。"); }
            throw;
        }
        return Status::Ok();
    } catch (const Failure& error) { return Error(error); }
    catch (...) { return Status::Failure("LuaFolder", "LUA_FOLDER_IO", std::string(name) + "：无法写入Lua文件。"); }
}

Status RemoveLuaFolderFile(const std::filesystem::path& root, const LuaFolderFile& expected) {
    try {
        Check(ValidateLuaFolderFile(expected.path, {}, false));
        const auto path = root / std::filesystem::u8path(expected.path);
        Handles directories;
        PinDirectories(path.parent_path(), directories);
        auto file = Open(path, GENERIC_READ | DELETE, OPEN_EXISTING, false);
        if (Hash(Read(file->value, path)) != expected.sha256) Fail("LUA_FOLDER_WRITE_CONFLICT", path, "删除前文件已改变，未删除。");
        FILE_DISPOSITION_INFO remove{TRUE};
        if (!SetFileInformationByHandle(file->value, FileDispositionInfo, &remove, sizeof(remove)))
            Fail("LUA_FOLDER_IO", path, "无法删除已确认的文件。");
        return Status::Ok();
    } catch (const Failure& error) { return Error(error); }
    catch (...) { return Status::Failure("LuaFolder", "LUA_FOLDER_IO", expected.path + "：无法删除Lua文件。"); }
}

Status ReconcileLuaFolder(const LuaFolderSnapshot& reviewed, std::span<const LuaFolderFile> map, LuaFolderChoice choice) {
    if (choice == LuaFolderChoice::Cancel) return Status::Failure("LuaFolder", "LUA_FOLDER_CANCELLED", "已取消关联，未改动文件。");
    if (reviewed.missing || reviewed.lease) return Status::Failure("LuaFolder", "LUA_FOLDER_REVIEW", "关联需要存在目录的只读审阅快照。");
    std::size_t total = 0;
    std::set<std::string> names;
    for (const auto& file : map) {
        if (!file.bytes) return Status::Failure("LuaFolder", "LUA_FOLDER_REVIEW", "地图Lua正文不完整。");
        const auto valid = ValidateLuaFolderFile(file.path, *file.bytes, false);
        if (!valid.Succeeded()) return valid;
        total += file.bytes->size();
        if (map.size() > kLuaFolderFiles || total > kLuaFolderTotalBytes || !names.insert(Key(file.path)).second)
            return Status::Failure("LuaFolder", "LUA_FOLDER_TOTAL_BUDGET", file.path + "：地图Lua超预算或路径重复。");
    }
    const auto verified = VerifyLuaFolder(reviewed);
    if (!verified.Succeeded()) return verified;
    if (choice == LuaFolderChoice::Folder) return Status::Ok();
    std::map<std::string, const LuaFolderFile*> remaining;
    for (const auto& file : reviewed.files) remaining.emplace(Key(file.path), &file);
    for (const auto& file : map) {
        const auto found = remaining.find(Key(file.path));
        const auto write = WriteLuaFolderFile(reviewed.root, file.path, *file.bytes,
            found == remaining.end() ? std::string_view{} : std::string_view(found->second->sha256));
        if (!write.Succeeded()) return write;
        if (found != remaining.end()) remaining.erase(found);
    }
    for (const auto& [key, file] : remaining) {
        const auto removed = RemoveLuaFolderFile(reviewed.root, *file);
        if (!removed.Succeeded()) return removed;
    }
    return Status::Ok();
}

Status RenameLuaFolderFile(const std::filesystem::path& root, const LuaFolderFile& expected, std::string_view newPath) {
    try {
        Check(ValidateLuaFolderFile(expected.path, {}, false));
        Check(ValidateLuaFolderFile(newPath, {}, false));
        const auto from = root / std::filesystem::u8path(expected.path);
        const auto to = std::filesystem::absolute(root / std::filesystem::u8path(newPath)).lexically_normal();
        Handles directories;
        PinDirectories(from.parent_path(), directories);
        PinDirectories(to.parent_path(), directories, true);
        auto file = Open(from, GENERIC_READ | DELETE, OPEN_EXISTING, false);
        if (Hash(Read(file->value, from)) != expected.sha256) Fail("LUA_FOLDER_WRITE_CONFLICT", from, "改名前文件已改变。");
        const auto name = to.wstring();
        std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
        auto& info = *reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        info.ReplaceIfExists = FALSE;
        info.RootDirectory = nullptr;
        info.FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        std::copy(name.begin(), name.end(), info.FileName);
        if (!SetFileInformationByHandle(file->value, FileRenameInfo, &info, static_cast<DWORD>(storage.size())))
            Fail("LUA_FOLDER_IO", to, "无法改名，目标可能已经存在。");
        return Status::Ok();
    } catch (const Failure& error) { return Error(error); }
    catch (...) { return Status::Failure("LuaFolder", "LUA_FOLDER_IO", "Lua文件改名失败。"); }
}
} // namespace War3Map::Services
