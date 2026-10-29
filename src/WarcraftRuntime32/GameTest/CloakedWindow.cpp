#include "CloakedWindow.h"
#include "WindowInput.h"

#include <detours.h>
#include <dwmapi.h>

#include <array>
#include <atomic>

namespace Audastra::Runtime32::GameTest {
namespace {

using CreateWindowExAFunction = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using CreateWindowExWFunction = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using ShowWindowFunction = BOOL(WINAPI*)(HWND, int);
using SetForegroundWindowFunction = BOOL(WINAPI*)(HWND);
using SetActiveWindowFunction = HWND(WINAPI*)(HWND);
using SetWindowPosFunction = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using QueryWindowFunction = HWND(WINAPI*)();
using ClipCursorFunction = BOOL(WINAPI*)(const RECT*);
using SetCursorPosFunction = BOOL(WINAPI*)(int, int);
using GetCursorPosFunction = BOOL(WINAPI*)(LPPOINT);
using GetAsyncKeyStateFunction = SHORT(WINAPI*)(int);
using GetKeyboardStateFunction = BOOL(WINAPI*)(PBYTE);

CreateWindowExAFunction originalCreateA = CreateWindowExA;
CreateWindowExWFunction originalCreateW = CreateWindowExW;
ShowWindowFunction originalShow = ShowWindow;
SetForegroundWindowFunction originalForeground = SetForegroundWindow;
SetActiveWindowFunction originalActive = SetActiveWindow;
SetWindowPosFunction originalPosition = SetWindowPos;
QueryWindowFunction originalQueryForeground = GetForegroundWindow;
QueryWindowFunction originalQueryActive = GetActiveWindow;
QueryWindowFunction originalQueryFocus = GetFocus;
ClipCursorFunction originalClip = ClipCursor;
SetCursorPosFunction originalSetCursor = SetCursorPos;
GetCursorPosFunction originalGetCursor = GetCursorPos;
GetAsyncKeyStateFunction originalKeyState = GetAsyncKeyState;
GetAsyncKeyStateFunction originalSynchronousKeyState = GetKeyState;
GetKeyboardStateFunction originalKeyboard = GetKeyboardState;
decltype(&TranslateMessage) originalTranslate = TranslateMessage;

volatile LONG* counter = nullptr;
std::atomic<bool> installed{false};
// 游戏只有一两个顶层窗口；满了以后新窗口一律当失败处理，不会漏成可见。
std::array<std::atomic<HWND>, 32> windows{};

bool Cloaked(HWND window) {
    if (!window) return false;
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(window, &pid) || pid != GetCurrentProcessId()) return false;
    const auto root = GetAncestor(window, GA_ROOT);
    for (const auto& entry : windows) {
        const auto value = entry.load();
        if (value && (value == window || value == root)) return true;
    }
    return false;
}

// 第一个仍存在的隐身窗口，即游戏主窗口。
HWND MainWindow() {
    if (const auto input = WindowInputTarget(); input && Cloaked(input)) return input;
    for (const auto& entry : windows) {
        const auto value = entry.load();
        if (value && Cloaked(value)) return value;
    }
    return nullptr;
}

// 游戏只有自认为在前台才推进菜单流程（例如 -loadfile 进图）。隐身窗口从不真正激活，
// 只在进程里补发激活消息，系统的前台与焦点不变。
void PretendActive(HWND window) {
    PostMessageW(window, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(window, WM_NCACTIVATE, TRUE, 0);
    PostMessageW(window, WM_ACTIVATE, WA_ACTIVE, 0);
    PostMessageW(window, WM_SETFOCUS, 0, 0);
}

bool TopLevel(DWORD style, HWND parent) {
    return !(style & WS_CHILD) && parent != HWND_MESSAGE;
}

constexpr DWORD kAddedExStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

// 隐身失败就结束进程：宁可这次测试失败，也不在用户桌面上露出窗口。
void Cloak(HWND window, bool show) {
    BOOL on = TRUE;
    bool recorded = false;
    for (auto& entry : windows) {
        auto previous = entry.load();
        if (previous == window) { recorded = true; break; }
        if ((!previous || !IsWindow(previous)) && entry.compare_exchange_strong(previous, window)) { recorded = true; break; }
    }
    if (!recorded || FAILED(DwmSetWindowAttribute(window, DWMWA_CLOAK, &on, sizeof(on))) || !InstallWindowInput(window)) {
        TerminateProcess(GetCurrentProcess(), 0xA0D5C10A);
        return;
    }
    if (counter) InterlockedIncrement(counter);
    if (show) originalShow(window, SW_SHOWNA);
    PretendActive(window);
}

HWND WINAPI CreateA(DWORD ex, LPCSTR type, LPCSTR name, DWORD style, int x, int y, int width, int height,
    HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
    if (!TopLevel(style, parent)) return originalCreateA(ex, type, name, style, x, y, width, height, parent, menu, instance, parameter);
    const auto window = originalCreateA((ex | kAddedExStyle) & ~WS_EX_APPWINDOW, type, name, style & ~WS_VISIBLE,
        x, y, width, height, parent, menu, instance, parameter);
    if (window) Cloak(window, style & WS_VISIBLE);
    return window;
}

HWND WINAPI CreateW(DWORD ex, LPCWSTR type, LPCWSTR name, DWORD style, int x, int y, int width, int height,
    HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
    if (!TopLevel(style, parent)) return originalCreateW(ex, type, name, style, x, y, width, height, parent, menu, instance, parameter);
    const auto window = originalCreateW((ex | kAddedExStyle) & ~WS_EX_APPWINDOW, type, name, style & ~WS_VISIBLE,
        x, y, width, height, parent, menu, instance, parameter);
    if (window) Cloak(window, style & WS_VISIBLE);
    return window;
}

// 显示类命令都改成“显示但不激活”；最小化也改掉，最小化后游戏不再绘制。隐藏照常。
BOOL WINAPI Show(HWND window, int command) {
    if (!Cloaked(window) || command == SW_HIDE) return originalShow(window, command);
    const auto result = originalShow(window, SW_SHOWNA);
    PretendActive(window);
    return result;
}

BOOL WINAPI Foreground(HWND window) {
    return Cloaked(window) ? TRUE : originalForeground(window);
}

HWND WINAPI Active(HWND window) {
    return Cloaked(window) ? GetActiveWindow() : originalActive(window);
}

BOOL WINAPI Position(HWND window, HWND after, int x, int y, int width, int height, UINT flags) {
    if (Cloaked(window)) {
        flags |= SWP_NOACTIVATE;
        if (after == HWND_TOPMOST || after == HWND_TOP) flags |= SWP_NOZORDER;
    }
    return originalPosition(window, after, x, y, width, height, flags);
}

// 游戏查询前台、激活与焦点时看到的是自己；系统实际的前台不变。
HWND WINAPI QueryForeground() { const auto main = MainWindow(); return main ? main : originalQueryForeground(); }
HWND WINAPI QueryActive() { const auto main = MainWindow(); return main ? main : originalQueryActive(); }
HWND WINAPI QueryFocus() { const auto main = MainWindow(); return main ? main : originalQueryFocus(); }

// 自认为在前台的游戏可能锁鼠标、移鼠标、读真实鼠标和按键：隐身期间一律不碰用户的输入。
BOOL WINAPI Clip(const RECT*) { return TRUE; }
BOOL WINAPI MoveCursor(int, int) { return TRUE; }
BOOL WINAPI ReadCursor(LPPOINT point) {
    const auto main = MainWindow();
    if (!point) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!main) { *point = {}; return TRUE; }
    if (WindowInputCursor(main, *point)) return TRUE;
    RECT client{};
    GetClientRect(main, &client);
    POINT center{(client.left + client.right) / 2, (client.top + client.bottom) / 2};
    ClientToScreen(main, &center);
    *point = center;
    return TRUE;
}
SHORT WINAPI ReadKey(int key) { return WindowInputKey(key, true); }
SHORT WINAPI ReadSynchronousKey(int key) { return WindowInputKey(key, false); }
BOOL WINAPI ReadKeyboard(PBYTE output) { return WindowInputKeyboard(output); }
BOOL WINAPI Translate(const MSG* message) { return WindowInputAlreadyTranslated(message) ? TRUE : originalTranslate(message); }

} // namespace

bool InstallCloakedWindow(volatile LONG* cloaked, std::string& error) {
    if (installed.load()) { error = "窗口隐身钩子已经安装。"; return false; }
    counter = cloaked;
    LONG code = DetourTransactionBegin();
    if (code == NO_ERROR) {
        code = DetourUpdateThread(GetCurrentThread());
        const auto attach = [&](auto& original, auto replacement) {
            if (code == NO_ERROR) code = DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(replacement));
        };
        attach(originalCreateA, CreateA);
        attach(originalCreateW, CreateW);
        attach(originalShow, Show);
        attach(originalForeground, Foreground);
        attach(originalActive, Active);
        attach(originalPosition, Position);
        attach(originalQueryForeground, QueryForeground);
        attach(originalQueryActive, QueryActive);
        attach(originalQueryFocus, QueryFocus);
        attach(originalClip, Clip);
        attach(originalSetCursor, MoveCursor);
        attach(originalGetCursor, ReadCursor);
        attach(originalKeyState, ReadKey);
        attach(originalSynchronousKeyState, ReadSynchronousKey);
        attach(originalKeyboard, ReadKeyboard);
        attach(originalTranslate, Translate);
        if (code == NO_ERROR) code = DetourTransactionCommit();
        else DetourTransactionAbort();
    }
    if (code != NO_ERROR) { error = "安装窗口隐身钩子失败：" + std::to_string(code); return false; }
    installed = true;
    return true;
}

} // namespace Audastra::Runtime32::GameTest
