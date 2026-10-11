#include "LuaBehaviorModules.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"

#include <lua.h>
#include <lauxlib.h>
#include <algorithm>

namespace Audastra::Runtime32::OwnLua {
namespace {
constexpr std::size_t kErrorBytes = 16 * 1024;
std::string_view Key(lua_State* lua) {
    if (lua_type(lua, 2) != LUA_TSTRING) luaL_error(lua, "运行时配置需要字段名文本。");
    std::size_t size = 0;
    const char* text = lua_tolstring(lua, 2, &size);
    if (!size || size > 128 || !LuaMailboxUtf8({text, size})) luaL_error(lua, "运行时配置字段名无效。");
    return {text, size};
}
int ErrorTrace(lua_State* lua) {
    const char* text = lua_tostring(lua, 1);
    luaL_traceback(lua, lua, text ? text : "错误回调抛出了非文本错误。", 1);
    return 1;
}
}

LuaBehaviorModules& LuaBehaviorModules::Self(lua_State* lua) {
    return *static_cast<LuaBehaviorModules*>(lua_touserdata(lua, lua_upvalueindex(1)));
}
int LuaBehaviorModules::ReadRuntime(lua_State* lua) {
    auto& self = Self(lua);
    const auto key = Key(lua);
    if (key == "error_handle") {
        if (self.errorHandler_ == LUA_NOREF) lua_pushnil(lua);
        else lua_rawgeti(lua, LUA_REGISTRYINDEX, self.errorHandler_);
    } else if (key == "handle_level") lua_pushinteger(lua, 2);
    else if (key == "catch_crash") lua_pushboolean(lua, true);
    else if (key == "sleep") lua_pushboolean(lua, false);
    else return luaL_error(lua, "自有 Lua 运行时尚未实现 jass.runtime.%s。", std::string(key).c_str());
    return 1;
}
int LuaBehaviorModules::WriteRuntime(lua_State* lua) {
    auto& self = Self(lua);
    const auto key = Key(lua);
    if (key == "error_handle") {
        if (!lua_isfunction(lua, 3) && !lua_isnil(lua, 3)) return luaL_error(lua, "jass.runtime.error_handle 需要函数或 nil。");
        lua_pushvalue(lua, 3);
        const int next = lua_isnil(lua, 3) ? (lua_pop(lua, 1), LUA_NOREF) : luaL_ref(lua, LUA_REGISTRYINDEX);
        if (self.errorHandler_ != LUA_NOREF) luaL_unref(lua, LUA_REGISTRYINDEX, self.errorHandler_);
        self.errorHandler_ = next;
    } else if (key == "handle_level") {
        if (!lua_isinteger(lua, 3) || lua_tointeger(lua, 3) != 2)
            return luaL_error(lua, "jass.runtime.handle_level 当前仅支持 2（持有引用的 userdata）。");
    } else if (key == "catch_crash") {
        if (!lua_isboolean(lua, 3) || !lua_toboolean(lua, 3))
            return luaL_error(lua, "jass.runtime.catch_crash 当前必须为 true；VM 失效时会停止本次测试。");
    } else if (key == "sleep") {
        if (!lua_isboolean(lua, 3) || lua_toboolean(lua, 3))
            return luaL_error(lua, "jass.runtime.sleep 尚不支持 true；等待类 native 会明确拒绝调用。");
    } else return luaL_error(lua, "自有 Lua 运行时尚未实现 jass.runtime.%s。", std::string(key).c_str());
    return 0;
}
int LuaBehaviorModules::ReadConsole(lua_State* lua) {
    if (Key(lua) != "enable") return luaL_error(lua, "自有 Lua 运行时尚未实现 jass.console.%s。", std::string(Key(lua)).c_str());
    lua_pushboolean(lua, Self(lua).consoleEnabled_);
    return 1;
}
int LuaBehaviorModules::WriteConsole(lua_State* lua) {
    if (Key(lua) != "enable") return luaL_error(lua, "自有 Lua 运行时尚未实现 jass.console.%s。", std::string(Key(lua)).c_str());
    if (!lua_isboolean(lua, 3)) return luaL_error(lua, "jass.console.enable 需要 boolean。");
    Self(lua).consoleEnabled_ = lua_toboolean(lua, 3) != 0;
    return 0;
}
int LuaBehaviorModules::Traceback(lua_State* lua) {
    // 只开放当前栈；不接受 thread、registry、upvalue 或任意内存访问。
    const int count = lua_gettop(lua);
    if (count > 2 || (count && !lua_isnil(lua, 1) && lua_type(lua, 1) != LUA_TSTRING))
        return luaL_error(lua, "debug.traceback 只接受可选的文本和栈层数。");
    const auto level = count > 1 ? luaL_checkinteger(lua, 2) : 1;
    if (level < 0 || level > 64) return luaL_error(lua, "debug.traceback 栈层数必须在 0 到 64 之间。");
    luaL_traceback(lua, lua, count ? lua_tostring(lua, 1) : nullptr, static_cast<int>(level));
    return 1;
}
void LuaBehaviorModules::Install(lua_State* lua) {
    luaL_getsubtable(lua, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    const auto module = [this, lua](const char* name, lua_CFunction read, lua_CFunction write) {
        lua_newtable(lua); lua_newtable(lua);
        lua_pushlightuserdata(lua, this); lua_pushcclosure(lua, read, 1); lua_setfield(lua, -2, "__index");
        lua_pushlightuserdata(lua, this); lua_pushcclosure(lua, write, 1); lua_setfield(lua, -2, "__newindex");
        lua_pushstring(lua, name); lua_setfield(lua, -2, "__metatable");
        lua_setmetatable(lua, -2); lua_setfield(lua, -2, name);
    };
    module("jass.runtime", ReadRuntime, WriteRuntime);
    module("jass.console", ReadConsole, WriteConsole);
    lua_newtable(lua);
    lua_pushcfunction(lua, Traceback); lua_setfield(lua, -2, "traceback");
    lua_pushvalue(lua, -1); lua_setglobal(lua, "debug");
    lua_setfield(lua, -2, "debug");
    lua_pop(lua, 1);
}
int LuaBehaviorModules::Notify(lua_State* lua) {
    auto& self = *static_cast<LuaBehaviorModules*>(lua_touserdata(lua, 1));
    lua_rawgeti(lua, LUA_REGISTRYINDEX, self.errorHandler_);
    lua_pushlstring(lua, self.error_.data(), self.error_.size());
    lua_call(lua, 1, 0);
    return 0;
}
std::string LuaBehaviorModules::NotifyError(lua_State* lua, std::string_view message, bool budgetAvailable) {
    if (errorHandler_ == LUA_NOREF || notifying_) return {};
    if (!budgetAvailable || !lua_checkstack(lua, 4)) return "jass.runtime.error_handle 未执行：原执行预算已耗尽。";
    const int top = lua_gettop(lua);
    notifying_ = true; error_ = message;
    struct Restore {
        LuaBehaviorModules& self; lua_State* lua; int top;
        ~Restore() { lua_settop(lua, top); self.notifying_ = false; self.error_ = {}; }
    } restore{*this, lua, top};
    lua_pushcfunction(lua, ErrorTrace);
    lua_pushcfunction(lua, Notify);
    lua_pushlightuserdata(lua, this);
    const int result = lua_pcall(lua, 1, 0, top + 1);
    if (result == LUA_OK) return {};
    std::size_t size = 0;
    const char* text = lua_tolstring(lua, -1, &size);
    if (!text) return "jass.runtime.error_handle 自身失败，无法取得错误文本。";
    const std::string_view failure(text, size);
    if (!LuaMailboxUtf8(failure)) return "jass.runtime.error_handle 自身失败，错误文本不是 UTF-8。";
    return "jass.runtime.error_handle 自身失败：" + std::string(failure.substr(0,
        LuaMailboxUtf8Prefix(failure, std::min(kErrorBytes, size))));
}

} // namespace Audastra::Runtime32::OwnLua
