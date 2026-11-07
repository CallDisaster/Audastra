#include "HslkSandbox.h"
#include <lauxlib.h>
#include <lualib.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace War3Map::HLua::Detail {
HslkSandbox::HslkSandbox(const HslkLimits& limits, std::span<const std::string> existing)
    : ids(existing), limits_(limits), deadline_(std::chrono::steady_clock::now() + limits.timeout) {}
HslkSandbox::~HslkSandbox() { if (lua_) lua_close(lua_); }
HslkSandbox& HslkSandbox::Host(lua_State* lua) { return **static_cast<HslkSandbox**>(lua_getextraspace(lua)); }
void* HslkSandbox::Allocate(void* user, void* pointer, std::size_t old, std::size_t size) noexcept {
    auto& host = *static_cast<HslkSandbox*>(user);
    if (!pointer) old = 0;
    if (!size) { std::free(pointer); host.memory_ -= std::min(old, host.memory_); return nullptr; }
    if (old > host.memory_ || size > host.limits_.memoryBytes - (host.memory_ - old)) {
        host.exceeded_ = true; return nullptr;
    }
    auto next = std::realloc(pointer, size);
    if (!next) { host.exceeded_ = true; return nullptr; }
    host.memory_ = host.memory_ - old + size; host.peak = std::max(host.peak, host.memory_);
    return next;
}
void HslkSandbox::Hook(lua_State* lua, lua_Debug*) {
    auto& host = Host(lua); host.instructions += 1000;
    if (host.exceeded_ || host.instructions > host.limits_.instructions
        || std::chrono::steady_clock::now() >= host.deadline_) {
        host.exceeded_ = true; lua_sethook(lua, Hook, LUA_MASKCOUNT, 1);
        luaL_error(lua, "HLUA_SANDBOX_LIMIT: 构建超过指令、时间或内存预算。");
    }
}
int HslkSandbox::Print(lua_State* lua) {
    auto& host = Host(lua);
    for (int i = 1; i <= lua_gettop(lua); ++i) {
        std::size_t size = 0; const char* value = luaL_tolstring(lua, i, &size);
        if (host.console.size() < 16384) {
            if (i > 1) host.console += '\t';
            host.console.append(value, std::min(size, 16384 - host.console.size()));
        }
        lua_pop(lua, 1);
    }
    if (host.console.size() < 16384) host.console += '\n';
    return 0;
}
Status HslkSandbox::Error(const char* code) {
    std::size_t size = 0; const auto* text = lua_tolstring(lua_, -1, &size);
    auto result = Status::Failure("HLua", exceeded_ ? "HLUA_SANDBOX_LIMIT" : code,
        source + "：" + (text ? std::string(text, std::min<std::size_t>(size, 16384)) : "Lua 执行失败。"));
    lua_settop(lua_, 0); return result;
}
Status HslkSandbox::Initialize() {
    if (!limits_.memoryBytes || !limits_.objects || !limits_.fieldBytes || !limits_.instructions || limits_.timeout.count() <= 0)
        return Status::Failure("HLua", "HLUA_SANDBOX_ARGUMENT", "构建预算必须为正数。");
    lua_ = lua_newstate(Allocate, this);
    if (!lua_) return Status::Failure("HLua", "HLUA_SANDBOX_LIMIT", "无法创建受限 Lua 状态。");
    *static_cast<HslkSandbox**>(lua_getextraspace(lua_)) = this;
    lua_pushcfunction(lua_, [](lua_State* L) -> int {
        luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
        luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
        luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
        luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
        luaL_requiref(L, "utf8", luaopen_utf8, 1); lua_pop(L, 1);
        for (const char* name : {"dofile", "loadfile", "load", "collectgarbage"}) { lua_pushnil(L); lua_setglobal(L, name); }
        lua_getglobal(L, "setmetatable");
        lua_pushcclosure(L, [](lua_State* S) -> int {
            if (lua_istable(S, 2)) {
                lua_pushliteral(S, "__gc"); lua_rawget(S, 2);
                const bool finalizer = !lua_isnil(S, -1); lua_pop(S, 1);
                if (finalizer) return luaL_error(S, "构建沙箱不允许 __gc。");
            }
            lua_pushvalue(S, lua_upvalueindex(1)); lua_pushvalue(S, 1); lua_pushvalue(S, 2); lua_call(S, 2, 1); return 1;
        }, 1); lua_setglobal(L, "setmetatable");
        lua_getglobal(L, "tostring");
        lua_pushcclosure(L, [](lua_State* S) -> int {
            if (lua_type(S, 1) == LUA_TNUMBER) {
                char text[64]; const auto number = lua_tonumber(S, 1);
                if (!std::isfinite(number)) return luaL_error(S, "构建数字必须有限。");
                auto end = std::to_chars(text, text + sizeof(text), number, std::chars_format::general);
                if (end.ec != std::errc{}) return luaL_error(S, "无法格式化构建数字。");
                lua_pushlstring(S, text, end.ptr - text); return 1;
            }
            lua_pushvalue(S, lua_upvalueindex(1)); lua_pushvalue(S, 1); lua_call(S, 1, 1); return 1;
        }, 1); lua_setglobal(L, "tostring");
        lua_pushcfunction(L, Print); lua_setglobal(L, "print");
        return 0;
    });
    if (lua_pcall(lua_, 0, 0, 0) != LUA_OK) return Error("HLUA_SANDBOX_INIT");
    lua_sethook(lua_, Hook, LUA_MASKCOUNT, 1000);
    return Run("unpack = table.unpack; math.pow = function(a,b) return a ^ b end", "=numeric-compatibility");
}
Status HslkSandbox::Run(std::string_view text, std::string_view name) {
    source = name;
    const std::string chunk = "@" + std::string(name);
    if (luaL_loadbufferx(lua_, text.data(), text.size(), chunk.c_str(), "t") != LUA_OK) return Error("HLUA_SYNTAX");
    lua_pushcfunction(lua_, [](lua_State* L) -> int {
        luaL_traceback(L,L,lua_type(L,1)==LUA_TSTRING ? lua_tostring(L,1) : "构建抛出非文本错误。",1); return 1;
    });
    lua_insert(lua_,-2);
    if (lua_pcall(lua_, 0, 0, 1) != LUA_OK) return Error("HLUA_BUILD_EXECUTION");
    lua_pop(lua_,1);
    return exceeded_ ? Status::Failure("HLua", "HLUA_SANDBOX_LIMIT", "构建预算已耗尽。") : Status::Ok();
}
Status HslkSandbox::CallText(const char* function, std::string_view text) {
    // 函数查找和参数分配也属于保护范围。
    struct Call { const char* name; std::string_view text; } call{function, text};
    lua_pushcfunction(lua_, [](lua_State* L) -> int {
        const auto& call = *static_cast<Call*>(lua_touserdata(L, 1));
        lua_getglobal(L, call.name);
        if (!lua_isfunction(L, -1)) return luaL_error(L, "缺少构建接口 %s。", call.name);
        lua_pushlstring(L, call.text.data(), call.text.size()); lua_call(L, 1, 0); return 0;
    });
    lua_pushlightuserdata(lua_, &call);
    if (lua_pcall(lua_, 1, 0, 0) != LUA_OK) return Error("HLUA_BUILD_INTERFACE");
    return Status::Ok();
}
int HslkSandbox::AllocateId(lua_State* L) {
    auto& host = Host(L); luaL_checktype(L, 1, LUA_TTABLE);
    const auto field = [L](const char* key, bool optional = false) {
        lua_getfield(L, 1, key);
        if (optional && lua_isnil(L, -1)) { lua_pop(L, 1); return std::string{}; }
        if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "%s 需要文本。", key);
        std::size_t size = 0; const auto* value = lua_tolstring(L, -1, &size);
        std::string output(value, size); lua_pop(L, 1); return output;
    };
    auto result = host.ids.Allocate(field("_class"), field("_parent"), field("_id_force", true));
    if (!result.Succeeded()) return luaL_error(L, "%s: %s", result.diagnostics.front().code.c_str(), result.diagnostics.front().message.c_str());
    std::string origin=host.source;
    for (int level=1;level<32;++level) {
        lua_Debug frame{};
        if (!lua_getstack(L,level,&frame) || !lua_getinfo(L,"Sl",&frame)) break;
        const std::string_view name=frame.source ? frame.source : "";
        if (name.starts_with("@hslk/") || name.starts_with("@hslk\\")) {
            origin=std::string(name.substr(1))+':'+std::to_string(frame.currentline); break;
        }
    }
    host.objectSources.emplace(*result.value,std::move(origin));
    lua_pushlstring(L, result.value->data(), result.value->size()); return 1;
}
Status HslkSandbox::InstallAllocator() {
    lua_pushcfunction(lua_, [](lua_State* L) -> int {
        lua_pushcfunction(L, AllocateId); lua_setglobal(L, "SLK_ID"); return 0;
    });
    if (lua_pcall(lua_, 0, 0, 0) != LUA_OK) return Error("HLUA_BUILD_INTERFACE");
    return Status::Ok();
}

int HslkSandbox::Extract(lua_State* L) {
    auto& host = Host(L);
    auto& objects = *static_cast<std::vector<HslkObject>*>(lua_touserdata(L, 1));
    lua_getglobal(L, "SLK_GO"); luaL_checktype(L, -1, LUA_TTABLE);
    const auto count = lua_rawlen(L, -1);
    if (count > host.limits_.objects) return luaL_error(L, "HLUA_OBJECT_BUDGET: 生成物体超过预算。");
    const auto string = [L](int index, const char* key) {
        lua_getfield(L, index, key); std::size_t size = 0;
        if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "物体的 %s 缺失或不是文本。", key);
        const auto* text = lua_tolstring(L, -1, &size); std::string value(text, size); lua_pop(L, 1); return value;
    };
    const auto atom = [&](int index) -> FieldAtom {
        host.fieldBytes_ += sizeof(FieldAtom);
        if (host.fieldBytes_ > host.limits_.fieldBytes) luaL_error(L, "HLUA_FIELD_BUDGET: 生成字段超过预算。");
        if (lua_type(L, index) == LUA_TNUMBER) {
            const auto number = lua_tonumber(L, index);
            if (!std::isfinite(number)) luaL_error(L, "物编数字必须有限。");
            return number;
        }
        if (lua_type(L, index) != LUA_TSTRING) luaL_error(L, "HLUA_FIELD_VALUE: 物编只接受数字、字符串及其一维数组。");
        std::size_t size = 0; const auto* text = lua_tolstring(L, index, &size);
        host.fieldBytes_ += size;
        if (host.fieldBytes_ > host.limits_.fieldBytes) luaL_error(L, "HLUA_FIELD_BUDGET: 生成字段超过预算。");
        return std::string(text, size);
    };
    for (std::size_t i = 1; i <= count; ++i) {
        lua_rawgeti(L, -1, i); luaL_checktype(L, -1, LUA_TTABLE);
        HslkObject object; object.id = string(-1, "_id"); object.parent = string(-1, "_parent"); object.objectClass = string(-1, "_class");
        if (host.objectSources.contains(object.id)) object.source=host.objectSources.at(object.id);
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) != LUA_TSTRING) return luaL_error(L, "物编字段名必须是文本。");
            std::size_t size = 0; const auto* key = lua_tolstring(L, -2, &size);
            std::string name(key, size);
            if (!name.empty() && name.front() != '_') {
                HslkField field; field.name = std::move(name);
                host.fieldBytes_ += field.name.size() + sizeof(HslkField);
                if (host.fieldBytes_ > host.limits_.fieldBytes) return luaL_error(L, "生成字段超过预算。");
                if (lua_istable(L, -1)) {
                    field.array = true; const auto length = lua_rawlen(L, -1);
                    if (length > 65536) return luaL_error(L, "物编数组超过65536项。");
                    std::size_t entries = 0; lua_pushnil(L);
                    while (lua_next(L, -2)) {
                        const bool valid = lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1
                            && static_cast<std::uint64_t>(lua_tointeger(L, -2)) <= length;
                        lua_pop(L, 1); if (!valid) return luaL_error(L, "物编数组存在空洞、非整数键或嵌套表。");
                        ++entries;
                    }
                    if (entries != length) return luaL_error(L, "物编数组存在空洞。");
                    for (std::size_t a = 1; a <= length; ++a) { lua_rawgeti(L, -1, a); field.values.push_back(atom(-1)); lua_pop(L, 1); }
                } else field.values.push_back(atom(-1));
                if (field.name == "Name" && !field.values.empty() && std::holds_alternative<std::string>(field.values.front())) object.name = std::get<std::string>(field.values.front());
                object.fields.push_back(std::move(field));
            }
            lua_pop(L, 1);
        }
        std::sort(object.fields.begin(), object.fields.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        objects.push_back(std::move(object)); lua_pop(L, 1);
    }
    return 0;
}
Status HslkSandbox::ReadObjects(std::vector<HslkObject>& output) {
    lua_pushcfunction(lua_, Extract); lua_pushlightuserdata(lua_, &output);
    if (lua_pcall(lua_, 1, 0, 0) != LUA_OK) return Error("HLUA_FIELD_VALUE");
    return Status::Ok();
}
} // namespace War3Map::HLua::Detail
