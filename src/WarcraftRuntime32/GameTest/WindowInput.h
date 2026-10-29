#pragma once
#include <windows.h>

namespace Audastra::Runtime32::GameTest {
// 只在本进程创建的测试窗口线程安装；接收器不操作其他窗口或系统输入状态。
[[nodiscard]] bool InstallWindowInput(HWND window);
[[nodiscard]] HWND WindowInputTarget();
[[nodiscard]] bool WindowInputCursor(HWND window, POINT& point);
[[nodiscard]] SHORT WindowInputKey(int key, bool consumeEdge);
[[nodiscard]] BOOL WindowInputKeyboard(PBYTE keys);
// 合成按键自行提供字符消息；消息循环再次翻译时只抑制该次动作，避免重复字符。
[[nodiscard]] bool WindowInputAlreadyTranslated(const MSG* message);
}
