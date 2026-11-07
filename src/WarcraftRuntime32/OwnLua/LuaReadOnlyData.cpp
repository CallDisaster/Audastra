#include "LuaReadOnlyData.h"
#include "LuaNativeBindings.h"
#include <lua.h>
#include <lauxlib.h>
#include <utility>

namespace Audastra::Runtime32::OwnLua {
namespace {
char cacheKey;
int Read(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1)); lua_pushvalue(L, 2); lua_rawget(L, -2);
    PushLuaReadOnlyData(L, -1); return 1;
}
int Next(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1)); lua_pushvalue(L, 2);
    if (!lua_next(L, -2)) return 0;
    const auto key = lua_absindex(L, -2), value = lua_absindex(L, -1);
    lua_pushvalue(L, key); PushLuaReadOnlyData(L, value); return 2;
}
int Pairs(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1)); lua_pushcclosure(L, Next, 1);
    lua_pushnil(L); lua_pushnil(L); return 3;
}
int Length(lua_State* L) { lua_pushinteger(L, lua_rawlen(L, lua_upvalueindex(1))); return 1; }
int Reject(lua_State* L) { return luaL_error(L, "jass.slk 是本次测试图的只读物编快照。"); }
}
void PushLuaReadOnlyData(lua_State* L, int index) {
    index = lua_absindex(L, index);
    if (!lua_istable(L, index)) { lua_pushvalue(L, index); return; }
    lua_rawgetp(L, LUA_REGISTRYINDEX, &cacheKey);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1); lua_newtable(L); lua_pushvalue(L, -1); lua_rawsetp(L, LUA_REGISTRYINDEX, &cacheKey);
    }
    const auto cache = lua_absindex(L, -1);
    lua_pushvalue(L, index); lua_rawget(L, cache);
    if (!lua_isnil(L, -1)) { lua_remove(L, cache); return; }
    lua_pop(L, 1); lua_newtable(L); lua_newtable(L);
    for (const auto& item : {std::pair{"__index", Read}, std::pair{"__pairs", Pairs}, std::pair{"__len", Length}}) {
        lua_pushvalue(L, index); lua_pushcclosure(L, item.second, 1); lua_setfield(L, -2, item.first);
    }
    lua_pushcfunction(L, Reject); lua_setfield(L, -2, "__newindex");
    lua_pushliteral(L, "jass.slk"); lua_setfield(L, -2, "__metatable"); lua_setmetatable(L, -2);
    LuaNativeBindings::ProtectHostTable(L, -1);
    lua_pushvalue(L, index); lua_pushvalue(L, -2); lua_rawset(L, cache);
    lua_remove(L, cache);
}
} // namespace Audastra::Runtime32::OwnLua
