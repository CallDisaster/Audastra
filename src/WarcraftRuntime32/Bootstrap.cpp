#include "RuntimeProtocol.h"
#include "RuntimeFiles.h"
#include "RuntimeLuaProbe.h"
#include "OwnLua/OwnedLuaSession.h"
#include "GameUi/GameUiModule.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"
#include <detours.h>
#include <cstring>
#include <type_traits>

using namespace Audastra::Runtime32;
namespace {
SharedEvidence* evidence = nullptr;
using LoadLibraryAFunction = HMODULE (WINAPI*)(LPCSTR);
using LoadLibraryWFunction = HMODULE (WINAPI*)(LPCWSTR);
using LoadLibraryExAFunction = HMODULE (WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFunction = HMODULE (WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadLibraryAFunction originalLoadLibraryA = nullptr;
LoadLibraryWFunction originalLoadLibraryW = nullptr;
LoadLibraryExAFunction originalLoadLibraryExA = nullptr;
LoadLibraryExWFunction originalLoadLibraryExW = nullptr;
LONG initializeStarted = 0;
LONG initializeThreadId = 0;

[[noreturn]] void Fail(const char* stage, DWORD error = GetLastError()) {
    if (evidence) {
        evidence->errorWin32 = error;
        strncpy_s(evidence->errorStage, stage, _TRUNCATE);
        InterlockedExchange(&evidence->failed, 1);
        MemoryBarrier();
    }
    // 只终止当前新建游戏，绝不枚举进程或弹系统错误框。
    TerminateProcess(GetCurrentProcess(), kRuntimeFailure);
    ExitProcess(kRuntimeFailure);
}
bool CallInitialize(FARPROC function) {
    __try { reinterpret_cast<void (WINAPI*)()>(function)(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void InitializeOwnedRuntime(HMODULE game) {
    auto& owned = evidence->ownedLua;
    const auto record = [&owned](std::string_view message) {
        if (!LuaMailboxUtf8(message)) message = "自有 Lua 启动失败。";
        const auto count = LuaMailboxUtf8Prefix(message, sizeof(owned.failureText) - 1);
        std::memcpy(owned.failureText, message.data(), count); owned.failureText[count] = 0;
    };
    auto image = OwnLua::GameImage127::Open(game);
    if (!image.image) { record(image.error); Fail("owned-lua-game-identity", ERROR_BAD_EXE_FORMAT); }
    if (!owned.mailboxHandle || owned.hotReload < 0 || owned.hotReload > 1
        || owned.mapSha256[64] || owned.nonce[32]
        || !LuaMailboxHex(owned.mapSha256, 64) || !LuaMailboxHex(owned.nonce, 32))
        Fail("owned-lua-launch-identity", ERROR_INVALID_DATA);
    auto* mailbox = static_cast<LuaTestMailbox*>(MapViewOfFile(
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(owned.mailboxHandle)), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LuaTestMailbox)));
    if (!mailbox || !LuaMailboxHeader(*mailbox)) Fail("owned-lua-mailbox-view", ERROR_INVALID_DATA);
    OwnLua::OwnedLuaSessionConfig config;
    config.mapPath = owned.mapPath; config.mapSha256 = owned.mapSha256;
    config.nonce = owned.nonce; config.mailbox = mailbox; config.hotReload = owned.hotReload != 0;
    config.experimentalJapi = AUD_JAPI_BRIDGE_EXPERIMENTAL != 0;
    // 内置界面库：用同一份已核验的 Game.dll 映像解析入口，作为 audastraui 模块装进每个新 Host。
    config.japiModules = [](const OwnLua::GameImage127& image, audastra::japi::Host& host) {
        return GameUi::InstallGameUiModule127(host, image.Base(),
            [&image](GameUi::Address address, std::size_t bytes) { return image.Executable(address, bytes); });
    };
    OwnLua::OwnedLuaSessionEvents events;
    events.observe = [&owned](const OwnLua::OwnedLuaObservation& state) {
        owned.mainThreadId = state.threadId; owned.nativeCount = state.nativeCount;
        owned.modules = state.modules; owned.callbacks = state.callbacks; owned.polls = state.polls;
        owned.errors = state.errors;
        owned.nativeCalls = state.nativeCalls;
        InterlockedExchange(&owned.ready, state.ready ? 1 : 0);
    };
    events.fatal = [record](std::string_view message) { record(message); Fail("owned-lua-session-failed", ERROR_INVALID_DATA); };
    std::string error;
    if (!OwnLua::InstallOwnedLuaSession(std::move(image.image), std::move(config), std::move(events), error)) {
        record(error);
        UnmapViewOfFile(mailbox);
        Fail("owned-lua-session-install", ERROR_INVALID_DATA);
    }
    // 成功后映射与已安装会话由当前测试进程共同持有，不在 loader lock 下析构。
    InterlockedExchange(&owned.observerInstalled, 1);
    InterlockedExchange(&evidence->runtimeInitialized, 1);
}
void InitializeRuntime() {
    if (!InstallLuaProbeWriteGuard(evidence)) Fail("lua-probe-write-guard-install", ERROR_ACCESS_DENIED);
    for (std::size_t index = 0; index < kModules.size(); ++index) {
        auto& module = evidence->modules[index];
        const auto expected = std::filesystem::path(evidence->runtimeRoot) / ModuleRelativePath(evidence->runtimeLayout, index);
        Handle input(OpenLockedFile(expected));
        if (input.value == INVALID_HANDLE_VALUE || !IsX86Image(input.value)) Fail("runtime-file-lease", ERROR_BAD_EXE_FORMAT);
        const auto before = Sha256(input.value);
        if (before.empty() || before != module.expectedSha256) Fail("runtime-preload-sha", ERROR_CRC);
        const auto loaded = LoadLibraryExW(expected.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loaded) Fail("runtime-load-library");
        const auto length = GetModuleFileNameW(loaded, module.path, static_cast<DWORD>(kPathCapacity));
        if (!length || length >= kPathCapacity || !EqualPath(module.path, expected.wstring())) Fail("runtime-module-path", ERROR_INVALID_DATA);
        Handle actual(OpenLockedFile(module.path));
        if (actual.value == INVALID_HANDLE_VALUE) Fail("runtime-loaded-file-lease");
        const auto actualSha = Sha256(actual.value);
        if (actualSha.empty() || actualSha != module.expectedSha256) Fail("runtime-loaded-sha", ERROR_CRC);
        strcpy_s(module.actualSha256, actualSha.c_str());
        InterlockedExchange(&module.loaded, 1);
        if (index == 1 && !InstallLuaProbeObserver(loaded)) Fail("lua-probe-observer-install", ERROR_PROC_NOT_FOUND);
        if (index >= 4) {
            const auto initialize = GetProcAddress(loaded, "Initialize");
            if (!initialize) Fail("runtime-initialize-export", ERROR_PROC_NOT_FOUND);
            if (!CallInitialize(initialize)) Fail("runtime-initialize-exception", ERROR_EXCEPTION_IN_SERVICE);
            InterlockedExchange(&module.initializeCalled, 1);
        }
    }
    InterlockedExchange(&evidence->runtimeInitialized, 1);
}
template<class Character> bool RequestsGame(const Character* name) {
    if (!name) return false;
    const Character* basename = name;
    for (auto* current = name; *current; ++current) if (*current == static_cast<Character>('\\') || *current == static_cast<Character>('/')) basename = current + 1;
    if constexpr (std::is_same_v<Character, char>) return _stricmp(basename, "Game.dll") == 0;
    else return _wcsicmp(basename, L"Game.dll") == 0;
}
void ObserveModule(HMODULE loaded, bool requestedGame) {
    if (!loaded) { if (requestedGame) Fail("game-load-library"); return; }
    try {
        std::array<wchar_t, kPathCapacity> actualPath{};
        const auto length = GetModuleFileNameW(loaded, actualPath.data(), static_cast<DWORD>(actualPath.size()));
        if (!length || length >= actualPath.size()) { if (requestedGame) Fail("game-module-path", ERROR_INVALID_DATA); return; }
        const auto actual = std::filesystem::path(actualPath.data());
        // 以实际映射的模块识别；兼容Game.dll、相对路径、完整ANSI/Unicode路径。
        if (!EqualPath(actual.filename().wstring(), L"Game.dll")) return;
        auto& game = evidence->gameModule;
        if (!EqualPath(actualPath.data(), game.path)) Fail("game-module-path", ERROR_INVALID_DATA);
        if (InterlockedCompareExchange(&initializeStarted, 1, 0) != 0) {
            // 同线程插件初始化重入不能等待自己；其他线程必须等完整初始化。
            if (static_cast<DWORD>(InterlockedCompareExchange(&initializeThreadId, 0, 0)) == GetCurrentThreadId()) return;
            if (evidence->fixtureInitializeDelayMs > 0) InterlockedExchange(&evidence->fixtureConcurrentWaitObserved, 1);
            const auto start = GetTickCount64();
            while (!InterlockedCompareExchange(&evidence->runtimeInitialized, 0, 0)) {
                if (InterlockedCompareExchange(&evidence->failed, 0, 0)) Fail("runtime-concurrent-initialization-failed", evidence->errorWin32);
                if (GetTickCount64() - start >= 15000) Fail("runtime-concurrent-initialization-timeout", WAIT_TIMEOUT);
                Sleep(1);
            }
            return;
        }
        InterlockedExchange(&initializeThreadId, static_cast<LONG>(GetCurrentThreadId()));
        Handle file(OpenLockedFile(actual));
        if (file.value == INVALID_HANDLE_VALUE) Fail("game-loaded-file-lease");
        const auto sha = Sha256(file.value);
        if (sha.empty() || sha != game.expectedSha256) Fail("game-loaded-sha", ERROR_CRC);
        strcpy_s(game.actualSha256, sha.c_str());
        InterlockedExchange(&game.loaded, 1);
        InterlockedExchange(&evidence->gameObserved, 1);
        if (evidence->runtimeMode == RuntimeMode::OwnedLua) InitializeOwnedRuntime(loaded);
        else InitializeRuntime();
    } catch (...) { Fail("runtime-cpp-exception", ERROR_UNHANDLED_EXCEPTION); }
}
HMODULE WINAPI ObserveLoadLibraryA(LPCSTR name) {
    const auto loaded = originalLoadLibraryA(name);
    const auto error = GetLastError();
    ObserveModule(loaded, RequestsGame(name));
    SetLastError(error);
    return loaded;
}
HMODULE WINAPI ObserveLoadLibraryW(LPCWSTR name) {
    const auto loaded = originalLoadLibraryW(name);
    const auto error = GetLastError();
    ObserveModule(loaded, RequestsGame(name));
    SetLastError(error);
    return loaded;
}
HMODULE WINAPI ObserveLoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    const auto loaded = originalLoadLibraryExA(name, file, flags);
    const auto error = GetLastError();
    if (!(flags & (DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE)))
        ObserveModule(loaded, RequestsGame(name));
    SetLastError(error);
    return loaded;
}
HMODULE WINAPI ObserveLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    const auto loaded = originalLoadLibraryExW(name, file, flags);
    const auto error = GetLastError();
    if (!(flags & (DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE)))
        ObserveModule(loaded, RequestsGame(name));
    SetLastError(error);
    return loaded;
}
bool InstallImportHook() {
    auto* image = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    if (!image || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return false;
    const auto import = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!import.VirtualAddress || import.VirtualAddress >= nt->OptionalHeader.SizeOfImage) return false;
    bool found = false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image + import.VirtualAddress);
    for (unsigned libraries = 0; descriptor->Name && libraries < 256; ++descriptor, ++libraries) {
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(image + descriptor->OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA32*>(image + descriptor->FirstThunk);
        for (unsigned entries = 0; names->u1.AddressOfData && entries < 8192; ++names, ++addresses, ++entries) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            auto* item = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(image + names->u1.AddressOfData);
            const auto* name = reinterpret_cast<const char*>(item->Name);
            const bool ansi = std::strcmp(name, "LoadLibraryA") == 0;
            const bool wide = std::strcmp(name, "LoadLibraryW") == 0;
            const bool extendedAnsi = std::strcmp(name, "LoadLibraryExA") == 0;
            const bool extendedWide = std::strcmp(name, "LoadLibraryExW") == 0;
            if (!ansi && !wide && !extendedAnsi && !extendedWide) continue;
            DWORD protection = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(DWORD), PAGE_READWRITE, &protection)) return false;
            if (ansi) {
                originalLoadLibraryA = reinterpret_cast<LoadLibraryAFunction>(addresses->u1.Function);
                addresses->u1.Function = reinterpret_cast<DWORD>(&ObserveLoadLibraryA);
            } else if (wide) {
                originalLoadLibraryW = reinterpret_cast<LoadLibraryWFunction>(addresses->u1.Function);
                addresses->u1.Function = reinterpret_cast<DWORD>(&ObserveLoadLibraryW);
            } else if (extendedAnsi) {
                originalLoadLibraryExA = reinterpret_cast<LoadLibraryExAFunction>(addresses->u1.Function);
                addresses->u1.Function = reinterpret_cast<DWORD>(&ObserveLoadLibraryExA);
            } else {
                originalLoadLibraryExW = reinterpret_cast<LoadLibraryExWFunction>(addresses->u1.Function);
                addresses->u1.Function = reinterpret_cast<DWORD>(&ObserveLoadLibraryExW);
            }
            DWORD ignored = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(DWORD), protection, &ignored)) return false;
            found = true;
        }
    }
    if (found) InterlockedExchange(&evidence->hookInstalled, 1);
    else SetLastError(ERROR_PROC_NOT_FOUND);
    return found;
}
} // namespace

// Detours的导入表注入需要稳定ordinal 1；函数只作为装载锚点，不执行初始化。
extern "C" __declspec(dllexport) void AudastraBootstrapAnchor() {}

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        if (DetourIsHelperProcess()) return TRUE;
        DetourRestoreAfterWith();
        evidence = OpenChannel();
        if (!evidence) return FALSE;
        if (!InstallImportHook()) {
            evidence->errorWin32 = GetLastError();
            strcpy_s(evidence->errorStage, "bootstrap-import-hook");
            InterlockedExchange(&evidence->failed, 1);
            return FALSE;
        }
    }
    return TRUE;
}
