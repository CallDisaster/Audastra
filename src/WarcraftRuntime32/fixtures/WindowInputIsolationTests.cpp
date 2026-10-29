#include "../GameTest/CloakedWindow.h"
#include <windows.h>
#include <iostream>
#include <string>
#include <algorithm>

int main() {
    LONG count = 0; std::string error;
    if (!Audastra::Runtime32::GameTest::InstallCloakedWindow(&count, error)) { std::cerr << error; return 1; }
    // 只有安装成功才调用输入API：它们现在仅作用于本原创进程的虚拟状态。
    POINT point{17,29}; BYTE keyboard[256]; std::fill_n(keyboard,256,0xff);
    if (!GetCursorPos(&point) || point.x || point.y || !SetCursorPos(123,456) || !ClipCursor(nullptr)
        || GetAsyncKeyState(VK_LBUTTON) || GetKeyState(VK_CONTROL) || GetKeyState(VK_LCONTROL)
        || !GetKeyboardState(keyboard) || !std::all_of(keyboard,keyboard+256,[](BYTE key) { return key == 0; })
        || GetKeyboardState(nullptr) || GetLastError() != ERROR_INVALID_PARAMETER || count != 0) return 2;
    if (GetMessagePos()) return 3;
    MSG message{};
    (void)PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    if (!PostThreadMessageW(GetCurrentThreadId(), WM_APP + 1, 41, 73)
        || GetMessageW(&message, nullptr, 0, 0) <= 0 || message.message != WM_APP + 1
        || message.wParam != 41 || message.lParam != 73 || message.hwnd) return 4;
    if (!PostThreadMessageA(GetCurrentThreadId(), WM_APP + 2, 43, 79)
        || !PeekMessageA(&message, nullptr, WM_APP + 2, WM_APP + 2, PM_NOREMOVE)
        || message.wParam != 43 || GetMessageA(&message, nullptr, WM_APP + 2, WM_APP + 2) <= 0
        || message.message != WM_APP + 2 || message.lParam != 79) return 5;
    if (!PostThreadMessageW(GetCurrentThreadId(), WM_APP + 3, 47, 83)
        || !PeekMessageW(&message, nullptr, WM_APP + 3, WM_APP + 3, PM_REMOVE)
        || message.wParam != 47 || message.lParam != 83
        || PeekMessageW(&message, nullptr, WM_APP + 3, WM_APP + 3, PM_REMOVE)) return 6;
    PostQuitMessage(17);
    if (GetMessageW(&message, nullptr, 0, 0) != 0 || message.message != WM_QUIT || message.wParam != 17) return 7;
    std::cout << "x86_user32_hooks_installed=true\nno_window_input_isolated=true\ngame_started=false\n";
    return 0;
}
