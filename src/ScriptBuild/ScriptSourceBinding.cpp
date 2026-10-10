#include "ScriptSourceBinding.h"
#include "War3MapCore/Script/ReadOnlyScriptSource.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/VerifiedFile.h"

#include <Windows.h>
#include <algorithm>
#include <exception>
#include <utility>

namespace War3Map::Script::Detail {
namespace {
using Kind = ScriptToolchainBindingKind;
using SnapshotResult = Result<SourceBindingSnapshot>;
struct Lease {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit Lease(HANDLE value) : handle(value) {}
    Lease(Lease&& other) noexcept : handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)) {}
    Lease(const Lease&) = delete;
    ~Lease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
SnapshotResult Fail(const std::filesystem::path& path, std::string code, std::string message) {
    auto result = SnapshotResult::Failure("Script.SourceBinding", std::move(code), std::move(message));
    result.diagnostics.back().origin = OriginSpan{path};
    return result;
}
std::uint64_t Ticks(const FILETIME& value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
std::uint64_t Size(const BY_HANDLE_FILE_INFORMATION& value) {
    return (static_cast<std::uint64_t>(value.nFileSizeHigh) << 32) | value.nFileSizeLow;
}
} // namespace

bool NormalizeSourcePath(const std::filesystem::path& input, std::filesystem::path& output) {
    if (!input.is_absolute() || input.native().size() > 4096 || input.native().find(L'\0') != std::wstring::npos ||
        input.native().starts_with(L"\\\\?\\") || input.native().starts_with(L"\\\\.\\")) return false;
    std::size_t depth = 0;
    for (const auto& part : input.relative_path()) {
        if (++depth > 64 || part == ".." || part.native().find_first_of(L":*?\"<>|") != std::wstring::npos ||
            (part != "." && !part.empty() && (part.native().back() == L'.' || part.native().back() == L' '))) return false;
    }
    output = input.lexically_normal();
    if (output.has_relative_path() && output.filename().empty()) output = output.parent_path();
    return true;
}

Result<SourceBindingSnapshot> CaptureSourceBinding(const std::filesystem::path& input,
    ScriptToolchainBindingKind expectedKind, bool required, std::uint64_t maxBytes) {
    try {
        std::filesystem::path path;
        if (!NormalizeSourcePath(input, path)) return Fail(input,
            "SCRIPT_USER_SOURCE_PATH_INVALID", "来源必须是有界绝对普通路径。");
        std::vector<std::filesystem::path> chain{path.root_path()};
        for (const auto& part : path.relative_path()) chain.push_back(chain.back() / part);
        std::vector<Lease> leases;
        SourceBindingSnapshot snapshot{{path, Kind::Missing, {}, 0, maxBytes}, {}, 0};
        for (std::size_t i = 0; i < chain.size(); ++i) {
            const bool last = i + 1 == chain.size();
            Lease lease(CreateFileW(chain[i].c_str(), last ? GENERIC_READ : FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (lease.handle == INVALID_HANDLE_VALUE) {
                const auto error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) break;
                return Fail(chain[i], "SCRIPT_USER_SOURCE_BINDING_FAILED", "无法只读锁定来源路径。");
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if (GetFileType(lease.handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(lease.handle, &info) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE)) return Fail(chain[i],
                    "SCRIPT_USER_SOURCE_BINDING_FAILED", "来源不是可读取的普通磁盘入口。");
            if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return Fail(chain[i],
                "SCRIPT_USER_SOURCE_REPARSE_REJECTED", "来源路径链包含 reparse 跳转。");
            const bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!last && !directory) return Fail(chain[i],
                "SCRIPT_USER_SOURCE_BINDING_FAILED", "来源祖先不是目录。");
            leases.push_back(std::move(lease));
            if (!last) continue;
            snapshot.binding.kind = directory ? Kind::Directory : Kind::RegularFile;
            if (snapshot.binding.kind != expectedKind || directory) break;
            snapshot.binding.bytes = Size(info);
            if (snapshot.binding.bytes > maxBytes) return Fail(path,
                "SCRIPT_USER_SOURCE_BINDING_BUDGET", "来源超过本次内容读取预算。");
            auto hash = Sha256Handle(leases.back().handle);
            if (!hash.Succeeded()) {
                SnapshotResult result;
                result.diagnostics = std::move(hash.diagnostics);
                return result;
            }
            auto content = ReadVerifiedFile(path, *hash.value, maxBytes);
            if (!content.Succeeded() || content.value->size() != snapshot.binding.bytes) {
                auto result = Fail(path, "SCRIPT_USER_SOURCE_BINDING_FAILED", "来源无法取得完整稳定快照。");
                result.diagnostics.insert(result.diagnostics.begin(), content.diagnostics.begin(), content.diagnostics.end());
                return result;
            }
            BY_HANDLE_FILE_INFORMATION after{};
            if (!GetFileInformationByHandle(leases.back().handle, &after) || Size(after) != Size(info) ||
                after.dwVolumeSerialNumber != info.dwVolumeSerialNumber || after.nFileIndexHigh != info.nFileIndexHigh ||
                after.nFileIndexLow != info.nFileIndexLow || Ticks(after.ftLastWriteTime) != Ticks(info.ftLastWriteTime)) {
                return Fail(path, "SCRIPT_USER_SOURCE_BINDING_FAILED", "来源身份或修改时间在捕获期间发生变化。");
            }
            snapshot.binding.sha256 = std::move(*hash.value);
            snapshot.bytes = std::move(*content.value);
            snapshot.lastWriteTicks = Ticks(info.ftLastWriteTime);
        }
        if (snapshot.binding.kind != expectedKind && (required || snapshot.binding.kind != Kind::Missing)) {
            return Fail(path, "SCRIPT_USER_SOURCE_BINDING_FAILED", "来源缺失或类型不符合入口约定。");
        }
        return SnapshotResult::Success(std::move(snapshot));
    } catch (const std::exception&) {
        return Fail(input, "SCRIPT_USER_SOURCE_BINDING_FAILED", "来源捕获失败，未返回部分内容。");
    }
}

} // namespace War3Map::Script::Detail

namespace War3Map::Script {
Result<ReadOnlyScriptSource> CaptureReadOnlyScriptSource(
    const std::filesystem::path& path, bool required, std::uint64_t maxBytes) {
    auto captured = Detail::CaptureSourceBinding(path, ScriptToolchainBindingKind::RegularFile,
        required, (std::min)(maxBytes, std::uint64_t{16 * 1024 * 1024}));
    if (!captured.Succeeded() || !captured.value) return {{}, std::move(captured.diagnostics)};
    return Result<ReadOnlyScriptSource>::Success({std::move(captured.value->binding),
        std::move(captured.value->bytes), captured.value->lastWriteTicks});
}
} // namespace War3Map::Script
