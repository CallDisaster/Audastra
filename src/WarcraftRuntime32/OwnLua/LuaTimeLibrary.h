#pragma once
struct lua_State;
namespace Audastra::Runtime32::OwnLua {
// 只安装 date/time/clock；不打开完整 os 库、进程、环境或文件接口。
void InstallLuaTimeLibrary(lua_State* lua);
}
