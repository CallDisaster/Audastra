#include "RuntimeLuaProbe.h"
#include "RuntimeFiles.h"

#include <winternl.h>
#include <detours.h>
#include <cstring>
#include <cwctype>
#include <memory>
#include <string_view>
#include <utility>

namespace Audastra::Runtime32 {
namespace {
constexpr NTSTATUS kDenied = static_cast<NTSTATUS>(0xC0000022u);
constexpr ACCESS_MASK kWriteAccess = GENERIC_WRITE | GENERIC_ALL | DELETE | WRITE_DAC | WRITE_OWNER
    | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | MAXIMUM_ALLOWED;
using CreateFileFunction = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using OpenFileFunction = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
using PcallFunction = int (__cdecl*)(void*, int, int, int, std::intptr_t, void*);
using GetTopFunction = int (__cdecl*)(void*);
using TypeFunction = int (__cdecl*)(void*, int);
using ToStringFunction = const char* (__cdecl*)(void*, int, std::size_t*);
using SetTopFunction = void (__cdecl*)(void*, int);
using LoadBufferFunction = int (__cdecl*)(void*, const char*, std::size_t, const char*, const char*);
CreateFileFunction originalCreate = nullptr;
OpenFileFunction originalOpen = nullptr;
PcallFunction originalPcall = nullptr;
GetTopFunction getTop = nullptr;
TypeFunction luaType = nullptr;
ToStringFunction toString = nullptr;
SetTopFunction setTop = nullptr;
LoadBufferFunction loadBuffer = nullptr;
SharedEvidence* channel = nullptr;
std::wstring writeRoot, protectedLog, redirectedLog, protectedRoot;
std::vector<std::wstring> nonfatalBlockedPaths;
std::string payloadPrefix, beginMarker;
std::vector<std::pair<std::wstring, std::wstring>> deviceDrives;
thread_local bool evaluatingPath = false;

std::wstring Lower(std::wstring value) {
    for (auto& c : value) c = static_cast<wchar_t>(towlower(c));
    return value;
}
bool Starts(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() && _wcsnicmp(value.data(), prefix.data(), prefix.size()) == 0;
}
std::wstring FullPath(std::wstring_view raw) {
    if (raw.empty() || raw.size() >= kPathCapacity || raw.find(L'\0') != std::wstring_view::npos) return {};
    std::wstring path(raw);
    if (Starts(path, L"\\??\\") || Starts(path, L"\\\\?\\")) path.erase(0, 4);
    else if (Starts(path, L"\\DosDevices\\")) path.erase(0, 12);
    else if (Starts(path, L"\\Device\\")) {
        bool translated = false;
        for (const auto& [device, drive] : deviceDrives) {
            if (Starts(path, device) && (path.size() == device.size() || path[device.size()] == L'\\')) {
                path = drive + path.substr(device.size()); translated = true; break;
            }
        }
        if (!translated) return {};
    }
    // GLOBALROOT、Volume GUID 或其它 NT 别名不能退化成沙盒相对路径再获准。
    if (path.size() < 3 || !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))
        || path[1] != L':' || path[2] != L'\\') return {};
    std::array<wchar_t, kPathCapacity> full{};
    const auto count = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (!count || count >= full.size()) return {};
    // TEMP 等环境可以使用 8.3 名称。展开存在的最长父路径，避免把同一普通路径误当越界。
    auto existing = std::filesystem::path(std::wstring(full.data(), count));
    std::vector<std::filesystem::path> suffix;
    while (!existing.empty()) {
        std::array<wchar_t, kPathCapacity> longPath{};
        const auto longCount = GetLongPathNameW(existing.c_str(), longPath.data(), static_cast<DWORD>(longPath.size()));
        if (longCount && longCount < longPath.size()) {
            auto expanded = std::filesystem::path(std::wstring(longPath.data(), longCount));
            for (auto item = suffix.rbegin(); item != suffix.rend(); ++item) expanded /= *item;
            const auto value = expanded.wstring(); if (value.size() >= kPathCapacity) return {};
            return Lower(value);
        }
        const auto parent = existing.parent_path(); if (parent == existing) break;
        suffix.push_back(existing.filename()); existing = parent;
    }
    return {};
}
std::wstring ObjectPath(const OBJECT_ATTRIBUTES* object) {
    if (!object || !object->ObjectName || !object->ObjectName->Buffer || object->ObjectName->Length % sizeof(wchar_t)
        || object->ObjectName->Length > (kPathCapacity - 1) * sizeof(wchar_t)) return {};
    std::wstring name(object->ObjectName->Buffer, object->ObjectName->Length / sizeof(wchar_t));
    if (object->RootDirectory && !name.starts_with(L"\\")) {
        std::array<wchar_t, kPathCapacity> root{};
        const auto count = GetFinalPathNameByHandleW(object->RootDirectory, root.data(), static_cast<DWORD>(root.size()), FILE_NAME_NORMALIZED);
        if (!count || count >= root.size()) return {};
        name = std::wstring(root.data(), count) + L"\\" + name;
    }
    return FullPath(name);
}
bool SystemDevice(const OBJECT_ATTRIBUTES* object) {
    if (!object || object->RootDirectory || !object->ObjectName || !object->ObjectName->Buffer) return false;
    const std::wstring_view name(object->ObjectName->Buffer, object->ObjectName->Length / sizeof(wchar_t));
    // 非磁盘设备的 Windows IPC / 图形 I/O 不属于文件写出；未知磁盘路径仍拒绝。
    return Starts(name, L"\\Device\\NamedPipe\\") || Starts(name, L"\\Device\\Afd")
        || Starts(name, L"\\Device\\ConDrv") || Starts(name, L"\\Device\\Video")
        || Starts(name, L"\\Device\\DxgKrnl") || Starts(name, L"\\Device\\KsecDD");
}
void Denied(const std::wstring& path, bool mayContinue = false) noexcept {
    const bool underProtectedRoot = !path.empty() && (path == protectedRoot
        || (Starts(path, protectedRoot) && path.size() > protectedRoot.size() && path[protectedRoot.size()] == L'\\'));
    const bool nonfatal = mayContinue && !path.empty()
        && std::find(nonfatalBlockedPaths.begin(), nonfatalBlockedPaths.end(), path) != nonfatalBlockedPaths.end() && !underProtectedRoot;
    const auto index = InterlockedIncrement(&channel->probeBlockedWriteCount) - 1;
    if (InterlockedCompareExchange(&channel->probeWriteDenied, 1, 0) == 0) {
        wcsncpy_s(channel->probeDeniedPath, path.empty() ? L"<unresolved file path>" : path.c_str(), _TRUNCATE);
    }
    if (index >= 0 && static_cast<std::size_t>(index) < kBlockedWriteCapacity) {
        auto& record = channel->probeBlockedWrites[static_cast<std::size_t>(index)];
        wcsncpy_s(record.path, path.empty() ? L"<unresolved file path>" : path.c_str(), _TRUNCATE);
        record.status = static_cast<DWORD>(kDenied); record.nonfatal = nonfatal ? 1 : 0;
        MemoryBarrier(); InterlockedExchange(&record.ready, 1);
    } else InterlockedExchange(&channel->probeBlockedWriteOverflow, 1);
    // 非致命只改变观察期限；所有拒写仍由调用者在原 NT dispatch 前返回 ACCESS_DENIED。
    if (!nonfatal || channel->probeBlockedWriteOverflow) {
        InterlockedIncrement(&channel->probeFatalWriteCount);
        strcpy_s(channel->errorStage, "lua-probe-write-outside-sandbox");
        channel->errorWin32 = ERROR_ACCESS_DENIED;
        MemoryBarrier(); InterlockedExchange(&channel->failed, 1);
    }
}
struct PathDecision {
    bool allowed = false;
    std::wstring redirected;
    // share-write 允许本次已核验的写；不共享 delete，持有至原 NT 调用结束以固定名字/父目录。
    std::vector<std::unique_ptr<Handle>> leases;
};
bool ProtectDestination(const std::wstring& path, PathDecision& decision) {
    const auto retain = [&](const std::filesystem::path& target, bool directory) {
        auto handle = std::make_unique<Handle>(CreateFileW(target.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle->value, &info)
            || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory
            || (!directory && info.nNumberOfLinks != 1)) return false;
        std::array<wchar_t, kPathCapacity> actual{};
        const auto count = GetFinalPathNameByHandleW(handle->value, actual.data(), static_cast<DWORD>(actual.size()), FILE_NAME_NORMALIZED);
        if (!count || count >= actual.size() || FullPath(std::wstring_view(actual.data(), count)) != FullPath(target.wstring())) return false;
        decision.leases.push_back(std::move(handle));
        return true;
    };
    const std::filesystem::path target(path);
    for (auto parent = target.parent_path(); !parent.empty();) {
        if (!retain(parent, true)) return false;
        const auto above = parent.parent_path(); if (above == parent) break;
        parent = above;
    }
    const auto attributes = GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return retain(target, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    // 不存在的叶子才可新建；不可把无权限、未知路径或解析失败当成不存在。
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}
PathDecision CheckWrite(const OBJECT_ATTRIBUTES* object, ACCESS_MASK access, ULONG disposition, ULONG options) {
    if (!(access & kWriteAccess) && disposition == 1 && !(options & FILE_DELETE_ON_CLOSE)) return {true, {}, {}};
    if (evaluatingPath) { Denied({}); return {}; }
    evaluatingPath = true;
    struct Reset { ~Reset() { evaluatingPath = false; } } reset;
    if (SystemDevice(object)) return {true, {}, {}};
    const auto path = ObjectPath(object);
    if (!path.empty() && path == protectedLog) {
        PathDecision decision;
        if (!PlainPath(std::filesystem::path(redirectedLog)) || !ProtectDestination(redirectedLog, decision)) { Denied(redirectedLog); return {}; }
        decision.allowed = true; decision.redirected = L"\\??\\" + redirectedLog;
        InterlockedIncrement(&channel->probeLogRedirects); return decision;
    }
    if (!path.empty() && (path == writeRoot || (Starts(path, writeRoot) && path.size() > writeRoot.size()
        && path[writeRoot.size()] == L'\\')) && PlainPath(std::filesystem::path(path))) {
        PathDecision decision;
        if (ProtectDestination(path, decision)) { decision.allowed = true; return decision; }
        Denied(path); return {};
    }
    PathDecision deniedLeases;
    const bool mayContinue = !path.empty() && std::find(nonfatalBlockedPaths.begin(), nonfatalBlockedPaths.end(), path) != nonfatalBlockedPaths.end()
        && PlainPath(std::filesystem::path(path)) && ProtectDestination(path, deniedLeases);
    Denied(path, mayContinue); return {};
}
NTSTATUS NTAPI ObserveCreate(PHANDLE file, ACCESS_MASK access, POBJECT_ATTRIBUTES object, PIO_STATUS_BLOCK io,
    PLARGE_INTEGER size, ULONG attributes, ULONG share, ULONG disposition, ULONG options, PVOID ea, ULONG eaLength) {
    try {
        const auto decision = CheckWrite(object, access, disposition, options);
        if (!decision.allowed) return kDenied;
        if (decision.redirected.empty()) return originalCreate(file, access, object, io, size, attributes, share, disposition, options, ea, eaLength);
        auto replacement = *object;
        UNICODE_STRING name{}; name.Buffer = const_cast<wchar_t*>(decision.redirected.c_str());
        name.Length = static_cast<USHORT>(decision.redirected.size() * sizeof(wchar_t)); name.MaximumLength = name.Length;
        replacement.RootDirectory = nullptr; replacement.ObjectName = &name;
        return originalCreate(file, access, &replacement, io, size, attributes, share, disposition, options, ea, eaLength);
    } catch (...) { Denied({}); return kDenied; }
}
NTSTATUS NTAPI ObserveOpen(PHANDLE file, ACCESS_MASK access, POBJECT_ATTRIBUTES object, PIO_STATUS_BLOCK io, ULONG share, ULONG options) {
    try {
        const auto decision = CheckWrite(object, access, 1, options);
        if (!decision.allowed) return kDenied;
        if (decision.redirected.empty()) return originalOpen(file, access, object, io, share, options);
        auto replacement = *object;
        UNICODE_STRING name{}; name.Buffer = const_cast<wchar_t*>(decision.redirected.c_str());
        name.Length = static_cast<USHORT>(decision.redirected.size() * sizeof(wchar_t)); name.MaximumLength = name.Length;
        replacement.RootDirectory = nullptr; replacement.ObjectName = &name;
        return originalOpen(file, access, &replacement, io, share, options);
    } catch (...) { Denied({}); return kDenied; }
}
int __cdecl ObservePcall(void* state, int arguments, int results, int errorFunction, std::intptr_t context, void* continuation) {
    const auto status = originalPcall(state, arguments, results, errorFunction, context, continuation);
    if (status != 0 || results == 0 || channel->probeCaptured || channel->failed) return status;
    // 只从本次返回值识别 nonce；不重复执行地图作者 chunk。
    if (getTop(state) <= 0 || luaType(state, -1) != 4) return status;
    std::size_t size = 0;
    const auto* bytes = toString(state, -1, &size);
    if (!bytes || size != beginMarker.size() || std::memcmp(bytes, beginMarker.data(), size) != 0) return status;
    if (InterlockedCompareExchange(&channel->probeCaptureStarted, 1, 0) != 0) return status;
    const auto originalTop = getTop(state);
    // 唯一执行的是同次地图 nonce 触发的、已 SHA 捕获的自有元数据探针。
    // 使用原始 pcall trampoline，避免再次经过自己的观察点；保留原 EXExecuteScript 返回栈。
    auto probeStatus = loadBuffer(state, channel->probeSource, channel->probeSourceBytes, "@AudastraRuntimeProbe.lua", "t");
    if (probeStatus == 0) probeStatus = originalPcall(state, 0, 1, 0, 0, nullptr);
    if (probeStatus != 0) {
        setTop(state, originalTop); channel->errorWin32 = ERROR_INVALID_DATA;
        strcpy_s(channel->errorStage, "lua-probe-owned-chunk-failed");
        MemoryBarrier(); InterlockedExchange(&channel->failed, 1); return status;
    }
    size = 0; bytes = luaType(state, -1) == 4 ? toString(state, -1, &size) : nullptr;
    if (!bytes || size < payloadPrefix.size() || std::memcmp(bytes, payloadPrefix.data(), payloadPrefix.size()) != 0) {
        setTop(state, originalTop); channel->errorWin32 = ERROR_INVALID_DATA;
        strcpy_s(channel->errorStage, "lua-probe-owned-chunk-result-invalid");
        MemoryBarrier(); InterlockedExchange(&channel->failed, 1); return status;
    }
    const auto payloadSize = size - payloadPrefix.size();
    if (!payloadSize || payloadSize >= kLuaProbePayloadCapacity) {
        channel->errorWin32 = ERROR_BUFFER_OVERFLOW; strcpy_s(channel->errorStage, "lua-probe-payload-budget");
        setTop(state, originalTop); MemoryBarrier(); InterlockedExchange(&channel->failed, 1); return status;
    }
    std::memcpy(channel->probePayload, bytes + payloadPrefix.size(), payloadSize);
    channel->probePayload[payloadSize] = '\0'; channel->probePayloadBytes = static_cast<DWORD>(payloadSize);
    setTop(state, originalTop);
    MemoryBarrier(); InterlockedExchange(&channel->probeCaptured, 1);
    return status;
}
} // namespace

bool InstallLuaProbeWriteGuard(SharedEvidence* shared) {
    if (!shared || !shared->probeEnabled) return true;
    channel = shared;
    try {
        for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
            const std::wstring drive{letter, L':'};
            std::array<wchar_t, kPathCapacity> device{};
            if (QueryDosDeviceW(drive.c_str(), device.data(), static_cast<DWORD>(device.size()))) deviceDrives.emplace_back(device.data(), drive);
        }
        writeRoot = FullPath(shared->probeWriteRoot);
        protectedRoot = FullPath(shared->runtimeRoot);
        if (shared->probeNonfatalBlockedPathCount > kNonfatalBlockedPathCapacity) return false;
        for (DWORD index = 0; index < shared->probeNonfatalBlockedPathCount; ++index) {
            const auto nonfatalBlockedPath = FullPath(shared->probeNonfatalBlockedPaths[index].data());
            if (nonfatalBlockedPath.empty() || !PlainPath(std::filesystem::path(nonfatalBlockedPath))
                || nonfatalBlockedPath == writeRoot
                || (Starts(nonfatalBlockedPath, writeRoot) && nonfatalBlockedPath.size() > writeRoot.size()
                    && nonfatalBlockedPath[writeRoot.size()] == L'\\') || nonfatalBlockedPath == protectedRoot
                || (Starts(nonfatalBlockedPath, protectedRoot) && nonfatalBlockedPath.size() > protectedRoot.size()
                    && nonfatalBlockedPath[protectedRoot.size()] == L'\\')
                || std::find(nonfatalBlockedPaths.begin(), nonfatalBlockedPaths.end(), nonfatalBlockedPath) != nonfatalBlockedPaths.end()) return false;
            nonfatalBlockedPaths.push_back(nonfatalBlockedPath);
            wcscpy_s(shared->probeNonfatalBlockedPaths[index].data(), kPathCapacity, nonfatalBlockedPath.c_str());
        }
        protectedLog = FullPath((std::filesystem::path(shared->runtimeRoot) / L"logs" / L"war3.log").wstring());
        redirectedLog = FullPath(shared->probeLogPath);
        if (writeRoot.empty() || protectedRoot.empty() || protectedLog.empty() || redirectedLog.empty()
            || !PlainPath(std::filesystem::path(writeRoot)) || !PlainPath(std::filesystem::path(redirectedLog))) return false;
        const auto nt = GetModuleHandleW(L"ntdll.dll");
        originalCreate = reinterpret_cast<CreateFileFunction>(GetProcAddress(nt, "NtCreateFile"));
        originalOpen = reinterpret_cast<OpenFileFunction>(GetProcAddress(nt, "NtOpenFile"));
        if (!originalCreate || !originalOpen || DetourTransactionBegin() != NO_ERROR) return false;
        if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR
            || DetourAttach(reinterpret_cast<PVOID*>(&originalCreate), ObserveCreate) != NO_ERROR
            || DetourAttach(reinterpret_cast<PVOID*>(&originalOpen), ObserveOpen) != NO_ERROR) {
            DetourTransactionAbort(); return false;
        }
        if (DetourTransactionCommit() != NO_ERROR) return false;
        InterlockedExchange(&shared->probeWriteGuardInstalled, 1); return true;
    } catch (...) { return false; }
}
bool InstallLuaProbeObserver(HMODULE module) {
    if (!channel || !channel->probeEnabled) return true;
    payloadPrefix = "AUDASTRA_LUA_API_V1:" + std::string(channel->probeNonce) + ':';
    beginMarker = "AUDASTRA_LUA_API_BEGIN_V1:" + std::string(channel->probeNonce);
    originalPcall = reinterpret_cast<PcallFunction>(GetProcAddress(module, "lua_pcallk"));
    getTop = reinterpret_cast<GetTopFunction>(GetProcAddress(module, "lua_gettop"));
    luaType = reinterpret_cast<TypeFunction>(GetProcAddress(module, "lua_type"));
    toString = reinterpret_cast<ToStringFunction>(GetProcAddress(module, "lua_tolstring"));
    setTop = reinterpret_cast<SetTopFunction>(GetProcAddress(module, "lua_settop"));
    loadBuffer = reinterpret_cast<LoadBufferFunction>(GetProcAddress(module, "luaL_loadbufferx"));
    if (!originalPcall || !getTop || !luaType || !toString || !setTop || !loadBuffer || DetourTransactionBegin() != NO_ERROR) return false;
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR || DetourAttach(reinterpret_cast<PVOID*>(&originalPcall), ObservePcall) != NO_ERROR) {
        DetourTransactionAbort(); return false;
    }
    if (DetourTransactionCommit() != NO_ERROR) return false;
    InterlockedExchange(&channel->probeObserverInstalled, 1); return true;
}
} // namespace Audastra::Runtime32
