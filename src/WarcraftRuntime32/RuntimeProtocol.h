#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <cwchar>
#include <cstdlib>
#include <string>

namespace Audastra::Runtime32 {
inline constexpr std::uint32_t kMagic = 0x41555254;
inline constexpr std::uint32_t kVersion = 7;
inline constexpr DWORD kRuntimeFailure = 0xA001;
inline constexpr std::size_t kPathCapacity = 4096;
inline constexpr std::size_t kLuaProbePayloadCapacity = 512 * 1024;
inline constexpr std::size_t kLuaProbeSourceCapacity = 64 * 1024;
inline constexpr std::size_t kBlockedWriteCapacity = 32;
inline constexpr std::size_t kNonfatalBlockedPathCapacity = 4;
inline constexpr std::array<const wchar_t*, 6> kModules = {
    L"ydbase.dll", L"lua53.dll", L"ydwar3.dll", L"yd_loader.dll", L"yd_jass_api.dll", L"yd_lua_engine.dll"};
enum class RuntimeLayout : std::uint32_t { Flat = 0, YdweTree = 1 };
// Bare：不加载扩展运行库，只装测试钩子（如后台运行）。
enum class RuntimeMode : std::uint32_t { ExternalYdwe = 0, OwnedLua = 1, Bare = 2 };
inline bool ValidMode(RuntimeMode mode) {
    return mode == RuntimeMode::ExternalYdwe || mode == RuntimeMode::OwnedLua || mode == RuntimeMode::Bare;
}
inline bool ValidLayout(RuntimeLayout layout) {
    return layout == RuntimeLayout::Flat || layout == RuntimeLayout::YdweTree;
}
// 同一清单固定先加载 bin 的三个依赖，再加载 plugin/warcraft3 的三个插件。
inline std::wstring ModuleRelativePath(RuntimeLayout layout, std::size_t index) {
    if (!ValidLayout(layout) || index >= kModules.size()) return {};
    if (layout == RuntimeLayout::YdweTree)
        return std::wstring(index < 3 ? L"bin\\" : L"plugin\\warcraft3\\") + kModules[index];
    return kModules[index];
}
// 仅新建游戏进程继承此匿名映射句柄；不使用具名IPC，不修改机器环境。
struct ModuleEvidence {
    wchar_t path[kPathCapacity]{};
    char expectedSha256[65]{};
    char actualSha256[65]{};
    LONG loaded = 0;
    LONG initializeCalled = 0;
};
struct BlockedWriteEvidence {
    wchar_t path[kPathCapacity]{};
    DWORD status = 0;
    LONG nonfatal = 0;
    LONG ready = 0;
};
struct OwnedLuaEvidence {
    wchar_t mapPath[kPathCapacity]{};
    char mapSha256[65]{};
    char nonce[33]{};
    // Win32游戏只接受显式 HANDLE_LIST 继承的匿名邮箱，不根据名字或PID重开。
    std::uint32_t mailboxHandle = 0;
    LONG hotReload = 0;
    LONG observerInstalled = 0;
    LONG ready = 0;
    DWORD mainThreadId = 0;
    DWORD nativeCount = 0;
    DWORD modules = 0;
    DWORD callbacks = 0;
    DWORD polls = 0;
    DWORD errors = 0;
    std::uint64_t nativeCalls = 0;
    char failureText[4096]{};
};
struct SharedEvidence {
    std::uint32_t magic = kMagic;
    std::uint32_t version = kVersion;
    wchar_t runtimeRoot[kPathCapacity]{};
    RuntimeLayout runtimeLayout = RuntimeLayout::Flat;
    RuntimeMode runtimeMode = RuntimeMode::ExternalYdwe;
    OwnedLuaEvidence ownedLua{};
    LONG hookInstalled = 0;
    LONG gameObserved = 0;
    LONG runtimeInitialized = 0;
    LONG failed = 0;
    // 测试会话要求游戏失焦后照常运行；bootstrap 核对 1.27a 后安装钩子并置 Installed。
    LONG backgroundPlay = 0;
    LONG backgroundPlayInstalled = 0;
    DWORD errorWin32 = 0;
    char errorStage[96]{};
    // 原创无窗口fixture只用于证明跨进程Initialize确实执行；生产插件不写此字段。
    LONG fixturePluginBits = 0;
    LONG fixtureInitializeDelayMs = 0;
    LONG fixtureConcurrentEntered = 0;
    LONG fixtureConcurrentPass = 0;
    LONG fixtureConcurrentWaitObserved = 0;
    ModuleEvidence gameModule{};
    std::array<ModuleEvidence, kModules.size()> modules{};
    // 自有元数据探针的匿名 IPC；默认关闭，普通 F9 不观察 Lua 或限制文件写出。
    LONG probeEnabled = 0;
    char probeNonce[33]{};
    DWORD probeSourceBytes = 0;
    char probeSource[kLuaProbeSourceCapacity]{};
    wchar_t probeWriteRoot[kPathCapacity]{};
    wchar_t probeLogPath[kPathCapacity]{};
    wchar_t probeDeniedPath[kPathCapacity]{};
    DWORD probeNonfatalBlockedPathCount = 0;
    std::array<std::array<wchar_t, kPathCapacity>, kNonfatalBlockedPathCapacity> probeNonfatalBlockedPaths{};
    LONG probeBlockedWriteCount = 0;
    LONG probeFatalWriteCount = 0;
    LONG probeBlockedWriteOverflow = 0;
    std::array<BlockedWriteEvidence, kBlockedWriteCapacity> probeBlockedWrites{};
    LONG probeWriteGuardInstalled = 0;
    LONG probeObserverInstalled = 0;
    LONG probeWriteDenied = 0;
    LONG probeLogRedirects = 0;
    LONG probeCaptureStarted = 0;
    LONG probeCaptured = 0;
    DWORD probePayloadBytes = 0;
    char probePayload[kLuaProbePayloadCapacity]{};
};
inline SharedEvidence* OpenChannel() {
    wchar_t text[32]{};
    const auto length = GetEnvironmentVariableW(L"AUDASTRA_TEST_CHANNEL", text, 32);
    if (length == 0 || length >= 32) return nullptr;
    wchar_t* end = nullptr;
    const auto value = wcstoul(text, &end, 10);
    if (!end || *end || value == 0) return nullptr;
    auto* shared = static_cast<SharedEvidence*>(MapViewOfFile(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(value)), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedEvidence)));
    if (shared && (shared->magic != kMagic || shared->version != kVersion || !ValidLayout(shared->runtimeLayout)
        || !ValidMode(shared->runtimeMode))) { UnmapViewOfFile(shared); return nullptr; }
    return shared;
}
} // namespace Audastra::Runtime32
