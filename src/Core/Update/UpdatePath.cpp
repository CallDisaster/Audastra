#include "UpdatePath.h"

#include <algorithm>
#include <array>
#include <limits>

namespace War3Map::Update::Detail {
namespace {

template<typename T>
Result<T> Failure(const std::filesystem::path& path, std::string code, std::string message) {
    auto result = Result<T>::Failure("Update", std::move(code), std::move(message));
    result.diagnostics.back().origin = OriginSpan{path};
    return result;
}
bool Equal(std::wstring_view a, std::wstring_view b) {
    if (a.size() > INT_MAX || b.size() > INT_MAX) return false;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
Status NoAlternateStreams(const std::filesystem::path& path) {
    WIN32_FIND_STREAM_DATA data{};
    const HANDLE stream = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &data, 0);
    if (stream == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_HANDLE_EOF) return Status::Ok();
        return Status::Failure("Update", "UPDATE_STREAM_INSPECTION_FAILED", "无法完整确认更新路径的NTFS数据流，拒绝继续。");
    }
    bool extra = false;
    do {
        if (!Equal(data.cStreamName, L"::$DATA")) { extra = true; break; }
    } while (FindNextStreamW(stream, &data));
    const DWORD error = GetLastError();
    FindClose(stream);
    if (extra) return Status::Failure("Update", "UPDATE_ADS_REJECTED", "更新路径包含额外数据流(ADS)，拒绝继续。");
    if (error != ERROR_HANDLE_EOF) return Status::Failure("Update", "UPDATE_STREAM_INSPECTION_FAILED", "更新路径的数据流未能完整枚举。");
    return Status::Ok();
}
bool FinalMatches(HANDLE handle, const std::filesystem::path& expected) {
    const DWORD size = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (size == 0 || size > 32768) return false;
    std::wstring value(size, L'\0');
    const auto count = GetFinalPathNameByHandleW(handle, value.data(), size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!count || count >= size) return false;
    value.resize(count);
    if (!value.starts_with(L"\\\\?\\") || value.starts_with(L"\\\\?\\UNC\\")) return false;
    value.erase(0, 4);
    const auto actual = std::filesystem::path(value).lexically_normal().wstring();
    return Equal(actual, expected.lexically_normal().wstring());
}
bool RegularInfo(HANDLE handle, bool directory, const std::filesystem::path& path,
    BY_HANDLE_FILE_INFORMATION& info) {
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))
        || !!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory
        || (!directory && info.nNumberOfLinks != 1) || !FinalMatches(handle, path)) return false;
    return true;
}

} // namespace

Status CheckOpenedPath(HANDLE handle, const std::filesystem::path& path, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!RegularInfo(handle, directory, path, info)) return Status::Failure("Update",
        "UPDATE_REPARSE_OR_PATH_REJECTED", "已打开的更新路径不是可信普通磁盘身份，拒绝写入或改名。");
    return Status::Ok();
}

bool WindowsPathLess::operator()(std::wstring_view left, std::wstring_view right) const noexcept {
    if (left.size() > INT_MAX || right.size() > INT_MAX) return left < right;
    const auto result = CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE);
    return result == CSTR_LESS_THAN;
}

bool SafeWindowsComponent(std::wstring_view component) {
    if (component.empty() || component.size() > 255 || component == L"." || component == L".."
        || component.back() == L'.' || component.back() == L' ') return false;
    for (wchar_t c : component) {
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\'
            || c == L'|' || c == L'?' || c == L'*') return false;
    }
    auto stem = component.substr(0, component.find('.'));
    while (!stem.empty() && stem.back() == L' ') stem.remove_suffix(1);
    static constexpr std::array names{L"CON", L"PRN", L"AUX", L"NUL", L"CLOCK$", L"CONIN$", L"CONOUT$"};
    for (const auto* name : names) if (Equal(stem, name)) return false;
    if (stem.size() == 4 && (Equal(stem.substr(0, 3), L"COM") || Equal(stem.substr(0, 3), L"LPT"))
        && ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2'
            || stem[3] == L'\u00b3')) return false;
    return true;
}

Result<std::filesystem::path> NormalizeLocalPath(const std::filesystem::path& input, bool requireAbsolute) {
    try {
        const auto& value = input.native();
        if (value.empty() || value.size() > 30000 || value.starts_with(L"\\\\") || value.starts_with(L"//")
            || (requireAbsolute && !input.is_absolute()) || (input.has_root_name() && !input.is_absolute())
            || (input.has_root_directory() && !input.has_root_name())) return Failure<std::filesystem::path>(input,
                "UPDATE_LOCAL_PATH_INVALID", "更新只接受明确的本机目录/文件路径，拒绝UNC、设备与相对安装根。");
        for (const auto& component : input.relative_path()) {
            if (!SafeWindowsComponent(component.native())) return Failure<std::filesystem::path>(input,
                "UPDATE_LOCAL_PATH_INVALID", "更新路径包含ADS、Windows别名、点路径或不支持的文件名。");
        }
        const auto size = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
        if (size == 0 || size > 32768) return Failure<std::filesystem::path>(input,
            "UPDATE_LOCAL_PATH_INVALID", "无法确定更新路径的完整本机身份。");
        std::wstring absolute(size, L'\0');
        const auto count = GetFullPathNameW(input.c_str(), size, absolute.data(), nullptr);
        if (!count || count >= size) return Failure<std::filesystem::path>(input,
            "UPDATE_LOCAL_PATH_INVALID", "无法完整读取更新路径身份。");
        absolute.resize(count);
        std::filesystem::path result(absolute);
        const auto root = result.root_path().native();
        const auto drive = GetDriveTypeW(root.c_str());
        if (root.size() != 3 || root[1] != L':' || (root[2] != L'\\' && root[2] != L'/')
            || (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE)) return Failure<std::filesystem::path>(input,
                "UPDATE_LOCAL_PATH_INVALID", "更新路径不是本机固定或可移动磁盘，拒绝网络映射与设备路径。");
        return Result<std::filesystem::path>::Success(result.lexically_normal());
    } catch (const std::exception&) {
        return Failure<std::filesystem::path>(input, "UPDATE_LOCAL_PATH_INVALID", "无法确认更新路径。");
    }
}

Result<DirectoryReadLease> LockSafeDirectory(const std::filesystem::path& input) {
    auto normalized = NormalizeLocalPath(input, true);
    if (!normalized.Succeeded()) return {std::nullopt, std::move(normalized.diagnostics)};
    DirectoryReadLease lease;
    lease.path = *normalized.value;
    auto prefix = lease.path.root_path();
    std::vector<std::filesystem::path> paths{prefix};
    for (const auto& component : lease.path.relative_path()) { prefix /= component; paths.push_back(prefix); }
    if (paths.size() > 66) return Failure<DirectoryReadLease>(input, "UPDATE_PATH_DEPTH_BUDGET", "更新路径目录层数超过64级。");
    for (const auto& path : paths) {
        // 属性访问不参与Windows共享约束；加入最小目录读取位，租约才真正拒绝DELETE/rename。
        NativeHandle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!handle.Valid()) return Failure<DirectoryReadLease>(path, "UPDATE_DIRECTORY_OPEN_FAILED",
            "无法锁定更新目录；目录可能不存在、不可访问或有删除/改名活动。");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!RegularInfo(handle.Get(), true, path, info)) return Failure<DirectoryReadLease>(path,
            "UPDATE_REPARSE_OR_PATH_REJECTED", "更新目录是reparse/设备、实际路径不符或无法确认普通磁盘身份。");
        lease.anchors.push_back(std::move(handle));
    }
    // 祖先仍逐级锁定并验证普通磁盘/reparse/实际路径身份，但不会被复制的
    // 祖先数据流不属于更新内容。只检查本次目标；inventory会逐目录调用本函数。
    const auto streams = NoAlternateStreams(lease.path);
    if (!streams.Succeeded()) return {std::nullopt, streams.diagnostics};
    return Result<DirectoryReadLease>::Success(std::move(lease));
}

Result<FileReadLease> OpenSafeReadFile(const std::filesystem::path& input, std::uint64_t maxBytes) {
    auto normalized = NormalizeLocalPath(input, false);
    if (!normalized.Succeeded()) return {std::nullopt, std::move(normalized.diagnostics)};
    auto parent = LockSafeDirectory(normalized.value->parent_path());
    if (!parent.Succeeded()) return {std::nullopt, std::move(parent.diagnostics)};
    NativeHandle handle(CreateFileW(normalized.value->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.Valid()) return Failure<FileReadLease>(input, "UPDATE_FILE_OPEN_FAILED",
        "无法只读锁定更新文件；文件可能不存在、不可访问或正在写入/删除。");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!RegularInfo(handle.Get(), false, *normalized.value, info)) return Failure<FileReadLease>(input,
        "UPDATE_REPARSE_OR_PATH_REJECTED", "更新文件不是单链接普通文件、含reparse或实际路径不符。");
    const auto length = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (length > maxBytes) return Failure<FileReadLease>(input, "UPDATE_FILE_BUDGET", "更新文件超过读取容量限制。");
    const auto streams = NoAlternateStreams(*normalized.value);
    if (!streams.Succeeded()) return {std::nullopt, streams.diagnostics};
    return Result<FileReadLease>::Success(FileReadLease{std::move(*normalized.value), length,
        std::move(*parent.value), std::move(handle)});
}

} // namespace War3Map::Update::Detail
