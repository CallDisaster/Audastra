#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

// 测试游戏窗口的画面捕获：不依赖 Qt，单独成文件以免 C++/WinRT 头与 Qt 头混在一处。
namespace War3Map::QtApp {

// 窗口客户区的一帧，BGRA 自上而下逐行紧排。
struct WindowFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;
    // 这一帧距离本次捕获第一帧的毫秒数；单帧捕获恒为 0。
    int elapsedMilliseconds = 0;
};

// 用 Windows 图形捕获取窗口经系统合成后的一帧：窗口被遮挡也能取到，D3D/OpenGL/Vulkan 交换链同样可取。
// 在调用线程初始化多线程套间，界面线程请放到工作线程调用。取不到时返回空帧并把原因写进 error。
WindowFrame CaptureWindowFrame(HWND window, int timeoutMilliseconds, std::string& error);

// 同一次捕获里按间隔连取 count 帧，用来看动起来的画面（投射物、特效）。每帧都是到点之后系统新交来的画面；
// 中途取不到时交回已取到的帧并把原因写进 error。
std::vector<WindowFrame> CaptureWindowFrames(HWND window, int count, int intervalMilliseconds, int timeoutMilliseconds,
    std::string& error);

} // namespace War3Map::QtApp
