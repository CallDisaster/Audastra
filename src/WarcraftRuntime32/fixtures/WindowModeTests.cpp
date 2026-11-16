// 无边框窗口化：后台线程找到本进程的主窗口并子类化，在窗口自己的线程里去掉标题栏、铺满所在显示器；
// 之后窗口被改回带标题栏或改回小尺寸时再改回来。原创窗口，不需要游戏。
#include "../GameTest/WindowMode.h"

#include <windows.h>

#include <iostream>
#include <string>

namespace {

using Audastra::Runtime32::GameTest::IsBorderless;
using Audastra::Runtime32::GameTest::StartWindowMode;
using Audastra::Runtime32::GameTest::WindowModeOptions;

// 一边处理消息一边等条件成立。
template<class Predicate> bool PumpUntil(Predicate predicate, DWORD timeoutMs) {
    const auto start = GetTickCount64();
    while (!predicate()) {
        if (GetTickCount64() - start > timeoutMs) return false;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(10);
    }
    return true;
}

} // namespace

int main() {
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"AudastraWindowModeFixture";
    if (!RegisterClassW(&type)) { std::cerr << "register class\n"; return 1; }
    const HWND window = CreateWindowExW(0, type.lpszClassName, L"fixture", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80, 60, 640, 480, nullptr, nullptr, type.hInstance, nullptr);
    if (!window) { std::cerr << "create window\n"; return 2; }
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) { std::cerr << "monitor\n"; return 3; }
    if (IsBorderless(window)) { std::cerr << "starts with a frame\n"; return 4; }

    LONG applied = 0;
    std::string error;
    WindowModeOptions options;
    options.borderless = true;
    options.applied = &applied;
    if (!StartWindowMode(options, error)) { std::cerr << error << '\n'; return 5; }
    if (StartWindowMode(options, error)) { std::cerr << "started twice\n"; return 6; }
    if (!PumpUntil([&] { return IsBorderless(window); }, 5000) || applied < 1) { std::cerr << "not borderless\n"; return 7; }
    RECT rect{};
    GetWindowRect(window, &rect);
    if (!EqualRect(&rect, &monitor.rcMonitor)) { std::cerr << "does not fill the monitor\n"; return 8; }

    // 程序自己按分辨率改回小窗口：仍铺满显示器。
    SetWindowPos(window, nullptr, 100, 100, 800, 600, SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(window, &rect);
    if (!EqualRect(&rect, &monitor.rcMonitor)) { std::cerr << "resize not held\n"; return 9; }
    // 程序加回标题栏：去掉。
    SetWindowLongPtrW(window, GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) | WS_CAPTION | WS_THICKFRAME);
    if (GetWindowLongPtrW(window, GWL_STYLE) & (WS_CAPTION | WS_THICKFRAME)) { std::cerr << "caption came back\n"; return 10; }
    // 最小化不被拉回显示器。
    ShowWindow(window, SW_MINIMIZE);
    if (!PumpUntil([&] { return IsIconic(window) != FALSE; }, 2000)) { std::cerr << "minimize blocked\n"; return 11; }
    ShowWindow(window, SW_RESTORE);
    if (!PumpUntil([&] { return IsBorderless(window); }, 5000)) { std::cerr << "not borderless after restore\n"; return 12; }
    DestroyWindow(window);
    std::cout << "window mode: borderless applied " << applied << " time(s)\n";
    return 0;
}
