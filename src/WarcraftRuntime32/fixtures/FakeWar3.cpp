#include <windows.h>
#include <dwmapi.h>
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
    if (argc > 1 && wcsstr(argv[argc - 1], L"visible-no-activate.w3x")) {
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); GetStartupInfoW(&startup);
        if (!(startup.dwFlags & STARTF_USESHOWWINDOW) || startup.wShowWindow != SW_SHOWNOACTIVATE) return 59;
    }
    if (argc > 1 && wcsstr(argv[argc - 1], L"default-display.w3x")) {
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); GetStartupInfoW(&startup);
        if (startup.dwFlags & STARTF_USESHOWWINDOW) return 60;
    }
    if (argc > 1 && wcsstr(argv[argc - 1], L"nogame.w3x")) return 0;
    if (argc > 1 && wcsstr(argv[argc - 1], L"hang.w3x")) { Sleep(INFINITE); return 45; }
    if (argc > 1 && wcsstr(argv[argc - 1], L"concurrent.w3x")) return ConcurrentGameLoad();
    // 游戏目录里有 Miles 替身时先装载它：原版 Game.dll 导入 Mss32.dll，装载 Game.dll 时它已在进程里。
    HMODULE miles = nullptr;
    {
        wchar_t path[4096]{};
        const auto length = GetModuleFileNameW(nullptr, path, 4096);
        auto* basename = length ? wcsrchr(path, L'\\') : nullptr;
        if (basename) {
            wcscpy_s(basename + 1, static_cast<std::size_t>(path + 4096 - basename - 1), L"Mss32.dll");
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && !(miles = LoadLibraryW(path))) return 74;
        }
    }
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
    // 要求静音时打开数字声卡必须失败；没要求时必须照常成功。
    if (miles) {
        using OpenDigitalDriver = void*(__stdcall*)(unsigned, int, int, unsigned);
        const auto open = reinterpret_cast<OpenDigitalDriver>(GetProcAddress(miles, "_AIL_open_digital_driver@16"));
        auto* shared = Audastra::Runtime32::OpenChannel();
        const bool muted = shared && shared->muteSound;
        if (shared) UnmapViewOfFile(shared);
        if (!open || (open(44100, 16, 2, 0) == nullptr) != muted) return 75;
    }
    // 隐身启动：像游戏那样建一个可见顶层窗口，核对它确实被 DWM 隐身、不进任务栏、不激活。
    if (argc > 1 && wcsstr(argv[argc - 1], L"cloak.w3x")) {
        WNDCLASSW type{}; type.lpfnWndProc = DefWindowProcW; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"AudastraCloakFixture";
        if (!RegisterClassW(&type)) return 70;
        const HWND window = CreateWindowExW(WS_EX_APPWINDOW, type.lpszClassName, L"fixture", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            0, 0, 320, 240, nullptr, nullptr, type.hInstance, nullptr);
        if (!window) return 71;
        BOOL cloaked = FALSE;
        const auto style = GetWindowLongW(window, GWL_EXSTYLE);
        const bool ok = SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked
            && IsWindowVisible(window) && (style & WS_EX_TOOLWINDOW) && (style & WS_EX_NOACTIVATE) && !(style & WS_EX_APPWINDOW)
            && SetForegroundWindow(window) && GetForegroundWindow() == window && GetActiveWindow() == window;
        // 游戏自己查询时以为在前台；系统真实的前台（GetGUIThreadInfo 不经钩子）不能是它。
        GUITHREADINFO foreground{}; foreground.cbSize = sizeof(foreground);
        const bool reallyForeground = GetGUIThreadInfo(0, &foreground) && foreground.hwndActive == window;
        POINT cursor{};
        RECT client{}; GetClientRect(window, &client);
        POINT center{(client.left + client.right) / 2, (client.top + client.bottom) / 2}; ClientToScreen(window, &center);
        const bool isolated = GetCursorPos(&cursor) && cursor.x == center.x && cursor.y == center.y && GetAsyncKeyState(VK_SPACE) == 0;
        if (reallyForeground || !isolated) { DestroyWindow(window); return 73; }
        DestroyWindow(window);
        if (!ok) return 72;
    }
    const auto start = reinterpret_cast<int (*)()>(GetProcAddress(game, "StartFakeGame"));
    if (!start) return 42;
    const auto result = start();
    FreeLibrary(game);
    return result;
}
