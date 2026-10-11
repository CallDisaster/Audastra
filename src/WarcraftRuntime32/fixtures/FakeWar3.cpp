#include <windows.h>
#include <cwchar>
#include <cstring>
#include "../RuntimeProtocol.h"

namespace {
DWORD WINAPI ConcurrentEnter(LPVOID extended) {
    const auto game = extended ? LoadLibraryExW(L"Game.dll", nullptr, 0) : LoadLibraryA("Game.dll");
    if (!game) return 51;
    const auto start = reinterpret_cast<int (*)()>(GetProcAddress(game, "StartFakeGame"));
    const auto result = start ? start() : 52;
    auto* shared = Audastra::Runtime32::OpenChannel();
    if (shared) {
        if (result == 0) InterlockedIncrement(&shared->fixtureConcurrentPass);
        UnmapViewOfFile(shared);
    }
    FreeLibrary(game);
    return static_cast<DWORD>(result);
}
int ConcurrentGameLoad() {
    auto* shared = Audastra::Runtime32::OpenChannel();
    if (!shared) return 53;
    InterlockedExchange(&shared->fixtureInitializeDelayMs, 400);
    HANDLE first = CreateThread(nullptr, 0, ConcurrentEnter, nullptr, 0, nullptr);
    if (!first) { UnmapViewOfFile(shared); return 54; }
    const auto start = GetTickCount64();
    while (!InterlockedCompareExchange(&shared->fixtureConcurrentEntered, 0, 0) && GetTickCount64() - start < 5000) Sleep(1);
    if (!shared->fixtureConcurrentEntered) { CloseHandle(first); UnmapViewOfFile(shared); return 55; }
    HANDLE second = CreateThread(nullptr, 0, ConcurrentEnter, reinterpret_cast<LPVOID>(1), 0, nullptr);
    if (!second) { CloseHandle(first); UnmapViewOfFile(shared); return 56; }
    HANDLE threads[] = {first, second};
    const bool completed = WaitForMultipleObjects(2, threads, TRUE, 10000) == WAIT_OBJECT_0;
    DWORD firstExit = STILL_ACTIVE, secondExit = STILL_ACTIVE;
    GetExitCodeThread(first, &firstExit); GetExitCodeThread(second, &secondExit);
    CloseHandle(first); CloseHandle(second);
    const auto passed = shared->fixtureConcurrentPass;
    UnmapViewOfFile(shared);
    return completed && firstExit == 0 && secondExit == 0 && passed == 2 ? 0 : 57;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc > 1 && wcsstr(argv[argc - 1], L"minimized.w3x")) {
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); GetStartupInfoW(&startup);
        if (!(startup.dwFlags & STARTF_USESHOWWINDOW) || startup.wShowWindow != SW_SHOWMINNOACTIVE) return 58;
    }
    if (argc > 1 && wcsstr(argv[argc - 1], L"nogame.w3x")) return 0;
    if (argc > 1 && wcsstr(argv[argc - 1], L"hang.w3x")) { Sleep(INFINITE); return 45; }
    if (argc > 1 && wcsstr(argv[argc - 1], L"concurrent.w3x")) return ConcurrentGameLoad();
    // 正常LoadLibraryA导入，与经典War3在主入口之后装载Game.dll的顺序相同。
    HMODULE game = nullptr;
    if (argc > 1 && wcsstr(argv[argc - 1], L"absolute-a.w3x")) {
        char path[4096]{};
        if (!GetModuleFileNameA(nullptr, path, 4096)) return 46;
        auto* basename = strrchr(path, '\\');
        if (!basename) return 46;
        strcpy_s(basename + 1, static_cast<std::size_t>(path + 4096 - basename - 1), "Game.dll");
        game = LoadLibraryA(path);
    } else if (argc > 1 && wcsstr(argv[argc - 1], L"absolute-w.w3x")) {
        wchar_t path[4096]{};
        if (!GetModuleFileNameW(nullptr, path, 4096)) return 47;
        auto* basename = wcsrchr(path, L'\\');
        if (!basename) return 47;
        wcscpy_s(basename + 1, static_cast<std::size_t>(path + 4096 - basename - 1), L"Game.dll");
        game = LoadLibraryW(path);
    } else if (argc > 1 && wcsstr(argv[argc - 1], L"relative-w.w3x")) game = LoadLibraryW(L".\\Game.dll");
    else if (argc > 1 && wcsstr(argv[argc - 1], L"extended-a.w3x")) game = LoadLibraryExA(".\\Game.dll", nullptr, 0);
    else game = LoadLibraryA("Game.dll");
    if (!game) return 41;
    const auto start = reinterpret_cast<int (*)()>(GetProcAddress(game, "StartFakeGame"));
    if (!start) return 42;
    const auto result = start();
    FreeLibrary(game);
    return result;
}
