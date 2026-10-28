#pragma once
#include <windows.h>

namespace Audastra::Runtime32::GameTest {
// 只在本进程创建的测试窗口线程安装；接收器不操作其他窗口或系统输入状态。
[[nodiscard]] bool InstallWindowInput(HWND window);
[[nodiscard]] HWND WindowInputTarget();
[[nodiscard]] bool WindowInputCursor(HWND window, POINT& point);
[[nodiscard]] SHORT WindowInputKey(int key, bool consumeEdge);
[[nodiscard]] BOOL WindowInputKeyboard(PBYTE keys);
}
