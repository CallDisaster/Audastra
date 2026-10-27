#include <windows.h>

// 原创 Miles 替身：只导出 1.27a Game.dll 打开数字声卡用的同名函数，返回非空句柄表示打开成功。
// 不含任何 Miles 实现；只用来核对测试静音钩子确实让这个函数失败。
extern "C" __declspec(dllexport) void* __stdcall AIL_open_digital_driver(unsigned, int, int, unsigned) {
    static int driver = 0;
    return &driver;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
