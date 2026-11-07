#include "LuaTimeLibrary.h"
#include <lua.h>
#include <lauxlib.h>
#include <ctime>
#include <limits>
#include <string>
#include <string_view>

namespace Audastra::Runtime32::OwnLua {
namespace {
int Clock(lua_State* L) { lua_pushnumber(L, static_cast<double>(std::clock()) / CLOCKS_PER_SEC); return 1; }
void Integer(lua_State* L, const char* key, int value) { lua_pushinteger(L, value); lua_setfield(L, -2, key); }
int Time(lua_State* L) {
    if (lua_isnoneornil(L, 1)) { lua_pushinteger(L, static_cast<lua_Integer>(std::time(nullptr))); return 1; }
    luaL_checktype(L, 1, LUA_TTABLE);
    const auto field = [L](const char* name, int fallback, bool required = false) {
        lua_getfield(L, 1, name);
        if (lua_isnil(L, -1) && !required) { lua_pop(L, 1); return fallback; }
        int valid = 0; const auto value = lua_tointegerx(L, -1, &valid); lua_pop(L, 1);
        if (!valid || value < -100000 || value > 100000) luaL_error(L, "os.time 的 %s 缺失或超出范围。", name);
        return static_cast<int>(value);
    };
    std::tm value{};
    const auto year = field("year", 0, true);
    if (year < 1970 || year > 3000) return luaL_error(L, "os.time 年份必须在1970..3000之间。");
    value.tm_year = year - 1900; value.tm_mon = field("month", 0, true) - 1; value.tm_mday = field("day", 0, true);
    value.tm_hour = field("hour", 12); value.tm_min = field("min", 0); value.tm_sec = field("sec", 0);
    lua_getfield(L, 1, "isdst"); value.tm_isdst = lua_isnil(L, -1) ? -1 : lua_toboolean(L, -1); lua_pop(L, 1);
    const auto time = std::mktime(&value);
    if (time == static_cast<std::time_t>(-1)) return luaL_error(L, "os.time 无法表示该时间。");
    lua_pushvalue(L, 1);
    Integer(L, "year", value.tm_year + 1900); Integer(L, "month", value.tm_mon + 1); Integer(L, "day", value.tm_mday);
    Integer(L, "hour", value.tm_hour); Integer(L, "min", value.tm_min); Integer(L, "sec", value.tm_sec);
    Integer(L, "wday", value.tm_wday + 1); Integer(L, "yday", value.tm_yday + 1);
    lua_pushboolean(L, value.tm_isdst > 0); lua_setfield(L, -2, "isdst"); lua_pop(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(time)); return 1;
}
int Date(lua_State* L) {
    std::size_t length = 0; const auto* raw = luaL_optlstring(L, 1, "%c", &length);
    if (length > 256 || std::string_view(raw, length).find('\0') != std::string_view::npos)
        return luaL_error(L, "os.date 格式超过256字节或含零字节。");
    std::string format(raw, length);
    const auto time = lua_isnoneornil(L, 2) ? std::time(nullptr) : static_cast<std::time_t>(luaL_checkinteger(L, 2));
    if (time < 0 || time > 32535215999LL) return luaL_error(L, "os.date 时间超出受支持范围。");
    std::tm value{};
    const bool utc = format.starts_with('!'); if (utc) format.erase(0, 1);
    if ((utc ? gmtime_s(&value, &time) : localtime_s(&value, &time)) != 0) return luaL_error(L, "os.date 时间不可表示。");
    if (format == "*t") {
        lua_newtable(L);
        Integer(L, "year", value.tm_year + 1900); Integer(L, "month", value.tm_mon + 1); Integer(L, "day", value.tm_mday);
        Integer(L, "hour", value.tm_hour); Integer(L, "min", value.tm_min); Integer(L, "sec", value.tm_sec);
        Integer(L, "wday", value.tm_wday + 1); Integer(L, "yday", value.tm_yday + 1);
        lua_pushboolean(L, value.tm_isdst > 0); lua_setfield(L, -2, "isdst"); return 1;
    }
    // CRT 对非法格式会触发无效参数处理；先限制到标准时间转换项。
    constexpr std::string_view conversions = "aAbBcdHIjmMpSUwWxXyYzZ%";
    for (std::size_t i = 0; i < format.size(); ++i) if (format[i] == '%') {
        if (++i == format.size() || conversions.find(format[i]) == conversions.npos)
            return luaL_error(L, "os.date 含不支持的格式项。");
    }
    char text[4096]{};
    if (format.empty()) { lua_pushliteral(L, ""); return 1; }
    const auto size = std::strftime(text, sizeof(text), format.c_str(), &value);
    if (!size) return luaL_error(L, "os.date 结果超过4096字节或格式不可用。");
    lua_pushlstring(L, text, size); return 1;
}
}
void InstallLuaTimeLibrary(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, Date); lua_setfield(L, -2, "date");
    lua_pushcfunction(L, Time); lua_setfield(L, -2, "time");
    lua_pushcfunction(L, Clock); lua_setfield(L, -2, "clock"); lua_setglobal(L, "os");
}
} // namespace Audastra::Runtime32::OwnLua
