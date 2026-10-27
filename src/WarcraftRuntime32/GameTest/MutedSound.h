#pragma once

#include <string>

namespace Audastra::Runtime32::GameTest {

// 测试会话静音。经典 1.27a 不认 -nosound：Storm 解析命令行遇到未知参数就停下，
// 排在后面的 -loadfile 跟着丢失，游戏停在主菜单。所以改在进程内让 Miles 打不开数字声卡：
// 游戏初始化不检查声卡是否打开成功，之后照常无声运行。
// 只由测试启动注入在 Game.dll 入口执行前安装一次（此时 Game.dll 导入的 Mss32.dll 已装载）；
// 找不到 Miles 或装不上时返回 false，由调用方结束这次测试，不让测试游戏出声。
[[nodiscard]] bool InstallMutedSound(std::string& error);

} // namespace Audastra::Runtime32::GameTest
