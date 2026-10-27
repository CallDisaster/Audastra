#pragma once

#include "../OwnLua/GameImage127.h"

#include <string>

namespace Audastra::Runtime32::GameTest {

// 测试会话里让 1.27a 失焦后照常运行，Agent 在后台或看不见的桌面上测试时游戏不停。
// - 挡住“暂停游戏”请求：失焦、菜单、单机对话框都经过同一入口；继续游戏的请求照常放行。
// - 失焦后的后台空闲节流返回 0 毫秒，游戏按前台速度推进。
// 只由测试启动注入在 Game.dll 入口执行前安装一次，钩子随测试进程存活；入口字节与映像身份不符时拒绝安装。
// 入口事实取自同一作者的 WarVK（Game.dll 1.27a 生命周期钩子），代码按本项目重写。
[[nodiscard]] bool InstallBackgroundPlay127(const OwnLua::GameImage127& image, std::string& error);

} // namespace Audastra::Runtime32::GameTest
