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
    std::cout << "x86_user32_hooks_installed=true\nno_window_input_isolated=true\ngame_started=false\n";
    return 0;
}
