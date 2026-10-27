#pragma once

#include <windows.h>

#include <string>

namespace Audastra::Runtime32::GameTest {

// 测试会话里让游戏的顶层窗口留在用户桌面上照常绘制，但用户看不见、也碰不到：
// - 创建时去掉显示、加上工具窗口与不激活样式（不进任务栏和 Alt-Tab，点击不激活），再让 DWM 隐身后才显示；
// - 对这些窗口的显示、置前、激活与改位置请求一律改成不激活。
// 原版 D3D9/OpenGL 在不可见的独立桌面上建不起设备，这样游戏仍在输入桌面上，能照常出画面和截图。
// 只由测试启动注入在游戏建窗口前安装一次；隐身失败时结束测试进程，不让窗口露在用户桌面上。
// cloaked 计数本次隐身的窗口数，供启动回执核对。
[[nodiscard]] bool InstallCloakedWindow(volatile LONG* cloaked, std::string& error);

} // namespace Audastra::Runtime32::GameTest
