#include "UpdatePath.h"
#include "War3MapCore/Hash.h"

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
Result<bool> EnumerateOriginStream(const std::filesystem::path& path) {
    using R = Result<bool>;
    WIN32_FIND_STREAM_DATA data{};
    const HANDLE stream = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &data, 0);
    if (stream == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_HANDLE_EOF) return R::Success(false);
        return Failure<bool>(path, "UPDATE_STREAM_INSPECTION_FAILED", "无法完整确认更新路径的NTFS数据流，拒绝继续。");
    }
    bool extra = false, zone = false, invalid = false;
    do {
        if (Equal(data.cStreamName, L"::$DATA")) continue;
        if (!Equal(data.cStreamName, kZoneIdentifierSuffix)) { extra = true; break; }
        if (zone || data.StreamSize.QuadPart <= 0 || std::uint64_t(data.StreamSize.QuadPart) > kMaxZoneIdentifierBytes) {
            invalid = true; break;
        }
        zone = true;
    } while (FindNextStreamW(stream, &data));
    const DWORD error = GetLastError();
    FindClose(stream);
    if (extra) return Failure<bool>(path, "UPDATE_ADS_REJECTED", "更新路径包含未知数据流(ADS)，拒绝继续。");
    if (invalid) return Failure<bool>(path, "UPDATE_ORIGIN_METADATA_INVALID", "下载来源元数据重复、为空或超过有界容量。");
    if (error != ERROR_HANDLE_EOF) return Failure<bool>(path, "UPDATE_STREAM_INSPECTION_FAILED", "更新路径的数据流未能完整枚举。");
    return R::Success(zone);
}

bool ValidZoneIdentifier(std::span<const std::byte> bytes) {
    // 来源URL只是不可执行的元数据；只辨识ZoneTransfer和唯一ZoneId，原字节不重编码。
    // 部分解压工具在末尾写入NUL结束符（如 "...zip\0"），只容许出现在结尾，中间的NUL仍拒绝。
    std::string text;
    std::size_t at = 0;
    const auto byte = [&](std::size_t i) { return std::to_integer<unsigned char>(bytes[i]); };
    const auto allowed = [](unsigned value) {
        return value && value != 127 && (value >= 32 || value == '\r' || value == '\n' || value == '\t');
    };
    if (bytes.size() >= 2 && byte(0) == 0xff && byte(1) == 0xfe) {
        if (bytes.size() % 2) return false;
        auto size = bytes.size();
        while (size > 2 && byte(size - 2) == 0 && byte(size - 1) == 0) size -= 2;
        std::wstring wide;
        for (at = 2; at < size; at += 2) {
            const auto value = byte(at) | (unsigned(byte(at + 1)) << 8);
            if (!allowed(value)) return false;
            wide.push_back(static_cast<wchar_t>(value));
        }
        if (wide.empty()) return false;
        const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
            nullptr, 0, nullptr, nullptr);
        if (!length) return false;
        text.resize(static_cast<std::size_t>(length));
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
            text.data(), length, nullptr, nullptr) != length) return false;
    } else {
        auto size = bytes.size();
        while (size > 0 && byte(size - 1) == 0) --size;
        if (size >= 3 && byte(0) == 0xef && byte(1) == 0xbb && byte(2) == 0xbf) at = 3;
        for (; at < size; ++at) {
            const auto value = byte(at);
            if (!allowed(value)) return false;
            text.push_back(static_cast<char>(value));
        }
    }
    const auto trim = [](std::string_view value) {
        const auto first = value.find_first_not_of(" \t\r");
        if (first == std::string_view::npos) return std::string_view{};
        return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    };
    const auto equal = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
        for (std::size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
        return true;
    };
    bool section = false, zone = false;
    for (std::size_t begin = 0; begin <= text.size();) {
        const auto end = text.find('\n', begin);
        auto line = trim(std::string_view(text).substr(begin, end == std::string::npos ? end : end - begin));
        if (!line.empty()) {
            if (!section) {
                if (!equal(line, "[ZoneTransfer]")) return false;
                section = true;
            } else {
                const auto separator = line.find('=');
                if (separator == std::string_view::npos || separator == 0 || line.front() == '[') return false;
                const auto key = trim(line.substr(0, separator)), value = trim(line.substr(separator + 1));
                if (key.empty()) return false;
                if (equal(key, "ZoneId")) {
                    if (zone || value.size() != 1 || value.front() < '0' || value.front() > '4') return false;
                    zone = true;
                } else {
                    for (const auto c : key) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
                }
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return section && zone;
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

bool HeldDirectoryMatches(const DirectoryReadLease& lease) {
    if (!lease.path.is_absolute() || lease.anchors.empty() || lease.anchors.size() > 66
        || lease.origin.zoneIdentifier.has_value() != lease.origin.stream.Valid()) return false;
    // 目录句柄允许WRITE共享；拒绝rename不代表能跳过后续reparse及final-path检查。
    auto prefix = lease.path.root_path();
    std::size_t index = 0;
    const auto checked = [&] {
        if (index >= lease.anchors.size() || !lease.anchors[index].Valid()) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        return RegularInfo(lease.anchors[index++].Get(), true, prefix, info);
    };
    if (!checked()) return false;
    for (const auto& component : lease.path.relative_path()) {
        prefix /= component;
        if (!checked()) return false;
    }
    return index == lease.anchors.size();
}

Result<FileReadLease> OpenValidatedReadFile(const std::filesystem::path& input,
    std::filesystem::path normalized, std::uint64_t maxBytes, DirectoryReadLease parent,
    SharedDirectoryReadLease sharedParent) {
    NativeHandle handle(CreateFileW(normalized.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.Valid()) return Failure<FileReadLease>(input, "UPDATE_FILE_OPEN_FAILED",
        "无法只读锁定更新文件；文件可能不存在、不可访问或正在写入/删除。");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!RegularInfo(handle.Get(), false, normalized, info)) return Failure<FileReadLease>(input,
        "UPDATE_REPARSE_OR_PATH_REJECTED", "更新文件不是单链接普通文件、含reparse或实际路径不符。");
    const auto length = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (length > maxBytes) return Failure<FileReadLease>(input, "UPDATE_FILE_BUDGET", "更新文件超过读取容量限制。");
    auto streams = LockUpdateOriginStreams(normalized, handle.Get());
    if (!streams.Succeeded()) return {std::nullopt, streams.diagnostics};
    return Result<FileReadLease>::Success(FileReadLease{std::move(normalized), length,
        std::move(parent), std::move(sharedParent), std::move(handle), std::move(*streams.value)});
}

} // namespace

Result<OriginStreamReadLease> LockUpdateOriginStreams(const std::filesystem::path& path, HANDLE base, bool shareDelete) {
    using R = Result<OriginStreamReadLease>;
    const auto listed = EnumerateOriginStream(path);
    if (!listed.Succeeded()) return {std::nullopt, listed.diagnostics};
    OriginStreamReadLease lease;
    if (*listed.value) {
        const auto named = std::filesystem::path(path.native() + kZoneIdentifierSuffix);
        lease.stream = NativeHandle(CreateFileW(named.c_str(), GENERIC_READ,
            FILE_SHARE_READ | (shareDelete ? FILE_SHARE_DELETE : 0), nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION baseInfo{}, streamInfo{};
        LARGE_INTEGER size{};
        if (!lease.stream.Valid() || GetFileType(lease.stream.Get()) != FILE_TYPE_DISK
            || !GetFileInformationByHandle(base, &baseInfo) || !GetFileInformationByHandle(lease.stream.Get(), &streamInfo)
            || baseInfo.dwVolumeSerialNumber != streamInfo.dwVolumeSerialNumber
            || baseInfo.nFileIndexHigh != streamInfo.nFileIndexHigh || baseInfo.nFileIndexLow != streamInfo.nFileIndexLow
            || !GetFileSizeEx(lease.stream.Get(), &size) || size.QuadPart <= 0
            || std::uint64_t(size.QuadPart) > kMaxZoneIdentifierBytes)
            return Failure<OriginStreamReadLease>(path, "UPDATE_STREAM_INSPECTION_FAILED", "无法锁定同一文件身份的有界下载来源数据流。");
        OriginMetadata metadata;
        metadata.bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(lease.stream.Get(), metadata.bytes.data(), static_cast<DWORD>(metadata.bytes.size()), &read, nullptr)
            || read != metadata.bytes.size() || !ValidZoneIdentifier(metadata.bytes))
            return Failure<OriginStreamReadLease>(path, "UPDATE_ORIGIN_METADATA_INVALID", "下载来源数据流无法完整读取或不是受支持的ZoneTransfer元数据。");
        const auto digest = Sha256(metadata.bytes);
        if (!digest.Succeeded()) return {std::nullopt, digest.diagnostics};
        metadata.sha256 = *digest.value; lease.zoneIdentifier = std::move(metadata);
    }
    // 命名流共享不锁定其它stream名称；完整枚举必须再次成立。
    const auto after = EnumerateOriginStream(path);
    if (!after.Succeeded()) return {std::nullopt, after.diagnostics};
    if (*after.value != lease.zoneIdentifier.has_value())
        return Failure<OriginStreamReadLease>(path, "UPDATE_INVENTORY_CHANGED", "下载来源数据流在读取期间变化。");
    return R::Success(std::move(lease));
}

bool OriginMatches(const OriginStreamReadLease& actual, const OriginMetadata* expected) {
    return actual.zoneIdentifier.has_value() == (expected != nullptr)
        && (!expected || *actual.zoneIdentifier == *expected);
}

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
    auto streams = LockUpdateOriginStreams(lease.path, lease.anchors.back().Get());
    if (!streams.Succeeded()) return {std::nullopt, streams.diagnostics};
    lease.origin = std::move(*streams.value);
    return Result<DirectoryReadLease>::Success(std::move(lease));
}

Result<SharedDirectoryReadLease> LockSharedSafeDirectory(const std::filesystem::path& input) {
    auto locked = LockSafeDirectory(input);
    if (!locked.Succeeded()) return {std::nullopt, std::move(locked.diagnostics)};
    return Result<SharedDirectoryReadLease>::Success(
        SharedDirectoryReadLease(std::make_shared<const DirectoryReadLease>(std::move(*locked.value))));
}

Result<FileReadLease> OpenSafeReadFile(const std::filesystem::path& input, std::uint64_t maxBytes) {
    auto normalized = NormalizeLocalPath(input, false);
    if (!normalized.Succeeded()) return {std::nullopt, std::move(normalized.diagnostics)};
    auto parent = LockSafeDirectory(normalized.value->parent_path());
    if (!parent.Succeeded()) return {std::nullopt, std::move(parent.diagnostics)};
    return OpenValidatedReadFile(input, std::move(*normalized.value), maxBytes, std::move(*parent.value), {});
}

Result<FileReadLease> OpenSafeReadFile(const std::filesystem::path& input, std::uint64_t maxBytes,
    const SharedDirectoryReadLease& parent) {
    auto normalized = NormalizeLocalPath(input, false);
    if (!normalized.Succeeded()) return {std::nullopt, std::move(normalized.diagnostics)};
    if (!parent.lease_ || !Equal(normalized.value->parent_path().native(), parent.lease_->path.native()))
        return Failure<FileReadLease>(input, "UPDATE_PARENT_LEASE_MISMATCH", "共享目录租约为空或不属于文件的直接父目录。");
    if (!HeldDirectoryMatches(*parent.lease_)) return Failure<FileReadLease>(input,
        "UPDATE_REPARSE_OR_PATH_REJECTED", "共享目录租约的祖先句柄、普通目录或实际路径身份不再成立。");
    // 已存在的Zone句柄持续拒绝WRITE/DELETE，因此不重读/重哈希原字节；
    // 其它stream名称仍可新增，每文件复核枚举及presence，不能将工厂时的absent永久缓存。
    const auto listed = EnumerateOriginStream(parent.lease_->path);
    if (!listed.Succeeded()) return {std::nullopt, listed.diagnostics};
    if (*listed.value != parent.lease_->origin.zoneIdentifier.has_value()) return Failure<FileReadLease>(input,
        "UPDATE_INVENTORY_CHANGED", "共享目录的下载来源数据流在租约期间变化。");
    DirectoryReadLease emptyParent;
    return OpenValidatedReadFile(input, std::move(*normalized.value), maxBytes, std::move(emptyParent), parent);
}

} // namespace War3Map::Update::Detail
