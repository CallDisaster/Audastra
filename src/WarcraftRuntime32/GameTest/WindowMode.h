#pragma once

#include <windows.h>

#include <string>

namespace Audastra::Runtime32::GameTest {

// 作者看着的测试的窗口接管，不改 Game.dll：
// - 无边框窗口化：游戏主窗口去掉标题栏与边框，铺满所在显示器；游戏自己改回窗口样式或大小时再改回来。
// - 限制鼠标：游戏窗口在前台时把鼠标限制在它的画面里；切到别的窗口、最小化或窗口关闭时放开。
// 主窗口是本进程面积最大的可见顶层窗口（不含工具窗口与有所有者的窗口）。后台线程找到它后子类化窗口过程，
// 改样式、改位置都在窗口自己的线程里做。applied、clips 计数实际生效的次数，供启动回执核对。
struct WindowModeOptions {
    bool borderless = false;
    bool cursorLock = false;
    volatile LONG* applied = nullptr;
    volatile LONG* clips = nullptr;
};
[[nodiscard]] bool StartWindowMode(const WindowModeOptions& options, std::string& error);

// 把窗口改成铺满所在显示器的无边框窗口；已经是时不动并返回假。必须在窗口所属线程调用。
bool MakeBorderless(HWND window);
// 窗口是否已是铺满所在显示器的无边框窗口。
[[nodiscard]] bool IsBorderless(HWND window);

} // namespace Audastra::Runtime32::GameTest
