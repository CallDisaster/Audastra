#pragma once
struct lua_State;
namespace Audastra::Runtime32::OwnLua {
// 压入同一资料值的只读视图；迭代器也不泄露可修改的底表。
void PushLuaReadOnlyData(lua_State* lua, int index);
}
