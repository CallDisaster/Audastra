#include "LuaHost.h"
#include "LuaNativeBindings.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"

// 此独立 Lua 库全体以 C++ 编译，使用上游支持的异常展开，不能包 extern "C"。
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <limits>
#include <unordered_set>

namespace Audastra::Runtime32::OwnLua {
namespace {
char loadedModulesKey;
constexpr int kHookInterval = 1000;
constexpr std::size_t kConsoleBytes = 16 * 1024;
LuaHostResult Failure(LuaReloadResult code, std::string message) { return {code, std::move(message)}; }

std::string ModuleMember(std::string_view name) {
    if (name.empty() || name.size() > 240 || !LuaMailboxUtf8(name)) return {};
    std::string result;
    bool separator = true;
    for (const auto ch : name) {
        if (ch == '.' || ch == '/' || ch == '\\') {
            if (separator) return {};
            result += '\\'; separator = true;
        } else {
            if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f || ch == ':' || ch == '*' || ch == '?'
                || ch == '"' || ch == '<' || ch == '>' || ch == '|') return {};
            result += ch; separator = false;
        }
    }
    if (separator) return {};
    result += ".lua";
    return result;
}
}

struct LuaHost::Impl {
    LuaModuleReader reader;
    LuaHostLimits limits;
    lua_State* state = nullptr;
    DWORD thread = 0;
    std::size_t memory = 0, totalRead = 0;
    std::uint64_t instructionsLeft = 0, started = 0;
    std::uint32_t random = 0x41d05327;
    bool executing = false, budgetExceeded = false;
    std::string console;
    std::unordered_set<std::string> loading;
    std::unordered_set<std::string> readModules;
    std::unique_ptr<LuaNativeBindings> natives;
    explicit Impl(LuaModuleReader value, LuaHostLimits configured) : reader(std::move(value)), limits(configured) {}
    ~Impl() { if (state) lua_close(state); }
    static Impl& Host(lua_State* lua) { return **static_cast<Impl**>(lua_getextraspace(lua)); }
    static void* Allocate(void* user, void* pointer, std::size_t oldSize, std::size_t size) noexcept {
        auto& self = *static_cast<Impl*>(user);
        if (!pointer) oldSize = 0; // 新分配时 Lua 将类型码放在 osize，不能计入内存差额。
        if (!size) { std::free(pointer); self.memory -= std::min(oldSize, self.memory); return nullptr; }
        if (oldSize > self.memory || size > self.limits.memoryBytes - (self.memory - oldSize)) {
            self.budgetExceeded = true; return nullptr;
        }
        auto* next = std::realloc(pointer, size);
        if (!next) { self.budgetExceeded = true; return nullptr; }
        self.memory = self.memory - oldSize + size;
        return next;
    }
    static int Traceback(lua_State* lua) {
        const char* text = lua_tostring(lua, 1);
        luaL_traceback(lua, lua, text ? text : "Lua 抛出了非文本错误。", 1);
        return 1;
    }
    bool Expired() noexcept {
        if (budgetExceeded || GetTickCount64() - started >= limits.milliseconds) {
            budgetExceeded = true; return true;
        }
        return false;
    }
    static void Hook(lua_State* lua, lua_Debug*) {
        auto& self = Host(lua);
        if (self.instructionsLeft <= kHookInterval || self.Expired()) {
            self.budgetExceeded = true;
            // 错误被作者 pcall 捕获后下一条 Lua 指令仍会拒绝，不能无限吞掉预算错误。
            lua_sethook(lua, Hook, LUA_MASKCOUNT, 1);
            luaL_error(lua, "Lua 执行超出指令或时间预算。");
        }
        self.instructionsLeft -= kHookInterval;
    }
    static int Disabled(lua_State* lua) {
        return luaL_error(lua, "自有 Lua 运行时不开放 %s。", lua_tostring(lua, lua_upvalueindex(1)));
    }
    static void Disable(lua_State* lua, const char* name) {
        lua_pushstring(lua, name); lua_pushcclosure(lua, Disabled, 1); lua_setglobal(lua, name);
    }
    static int SetMetatable(lua_State* lua) {
        // lua_close 的作者 __gc 不受普通 chunk 生命周期约束；首版不接受作者终结器。
        // 普通 class/metatable 仍可使用。debug/registry 不开放，宿主持有的 userdata 元表以后独立封装。
        luaL_checkany(lua, 2);
        if (lua_istable(lua, 2)) {
            lua_pushliteral(lua, "__gc"); lua_rawget(lua, 2);
            const bool finalizer = !lua_isnil(lua, -1); lua_pop(lua, 1);
            if (finalizer) return luaL_error(lua, "自有 Lua 运行时不开放作者定义的 __gc 终结器。");
        }
        lua_pushvalue(lua, lua_upvalueindex(1)); lua_pushvalue(lua, 1); lua_pushvalue(lua, 2);
        lua_call(lua, 2, 1); return 1;
    }
    static int Print(lua_State* lua) {
        auto& self = Host(lua);
        const int count = lua_gettop(lua);
        for (int index = 1; index <= count; ++index) {
            std::size_t size = 0;
            const auto* text = luaL_tolstring(lua, index, &size);
            if (self.console.size() < kConsoleBytes) {
                if (index > 1) self.console += '\t';
                const std::string_view value(text, size);
                if (LuaMailboxUtf8(value)) self.console.append(value.substr(0,
                    LuaMailboxUtf8Prefix(value, kConsoleBytes - self.console.size())));
                else {
                    constexpr std::string_view marker = "[非UTF-8文本]";
                    self.console.append(marker.substr(0, LuaMailboxUtf8Prefix(marker, kConsoleBytes - self.console.size())));
                }
            }
            lua_pop(lua, 1);
        }
        if (self.console.size() < kConsoleBytes) self.console += '\n';
        return 0;
    }
    std::uint32_t NextRandom() noexcept {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        return random;
    }
    static int Random(lua_State* lua) {
        auto& self = Host(lua);
        const int count = lua_gettop(lua);
        if (!count) { lua_pushnumber(lua, static_cast<double>(self.NextRandom()) / 4294967296.0); return 1; }
        if (count > 2) return luaL_error(lua, "math.random 参数数量无效。");
        const auto first = luaL_checkinteger(lua, 1);
        const auto low = count == 1 ? 1 : first;
        const auto high = count == 1 ? first : luaL_checkinteger(lua, 2);
        if (low > high || low < std::numeric_limits<std::int32_t>::min() || high > std::numeric_limits<std::int32_t>::max())
            return luaL_error(lua, "math.random 当前仅支持有效 int32 区间。");
        const auto range = static_cast<std::uint64_t>(high - low) + 1;
        const auto ceiling = (0x100000000ull / range) * range;
        std::uint64_t value = 0;
        do { value = self.NextRandom(); } while (value >= ceiling);
        lua_pushinteger(lua, low + static_cast<lua_Integer>(value % range)); return 1;
    }
    static int RandomSeed(lua_State* lua) {
        auto& self = Host(lua);
        self.random = static_cast<std::uint32_t>(luaL_checkinteger(lua, 1));
        if (!self.random) self.random = 0x41d05327;
        return 0;
    }
    LuaModuleRead ReadModule(std::string_view member) {
        try { return reader ? reader(member, limits.sourceBytes) : LuaModuleRead{false, {}, "未配置地图归档读取器。"}; }
        catch (const std::exception& error) { return {false, {}, error.what()}; }
        catch (...) { return {false, {}, "读取地图 Lua 模块时发生异常。"}; }
    }
    static int Require(lua_State* lua) {
        auto& self = Host(lua);
        if (lua_type(lua, 1) != LUA_TSTRING) return luaL_error(lua, "require 需要模块名文本。");
        std::size_t size = 0;
        const auto* raw = lua_tolstring(lua, 1, &size);
        const std::string name(raw, size);
        const auto member = ModuleMember(name);
        if (member.empty()) return luaL_error(lua, "require 模块名不属于地图内有效相对成员。");
        lua_rawgetp(lua, LUA_REGISTRYINDEX, &loadedModulesKey);
        lua_getfield(lua, -1, name.c_str());
        if (lua_toboolean(lua, -1)) return 1;
        lua_pop(lua, 1);
        if (name.starts_with("jass.")) return luaL_error(lua, "自有 Lua 运行时尚未实现模块 %s。", name.c_str());
        if (self.loading.contains(member)) return luaL_error(lua, "Lua 模块循环依赖：%s。", name.c_str());
        if (self.loading.size() >= self.limits.requireDepth || self.readModules.size() >= self.limits.moduleCount
            || self.totalRead >= self.limits.totalModuleBytes) {
            self.budgetExceeded = true;
            return luaL_error(lua, "Lua 模块读取超出会话预算。");
        }
        self.loading.insert(member);
        struct Loading { Impl& host; const std::string& member; ~Loading() { host.loading.erase(member); } } guard{self, member};
        const auto module = self.ReadModule(member);
        if (!module.error.empty()) return luaL_error(lua, "读取 %s 失败：%s", member.c_str(), module.error.c_str());
        if (!module.found) return luaL_error(lua, "地图内找不到 Lua 模块：%s。", member.c_str());
        if (module.source.size() > self.limits.sourceBytes || module.source.size() > self.limits.totalModuleBytes - self.totalRead) {
            self.budgetExceeded = true;
            return luaL_error(lua, "Lua 模块 %s 超出读取预算。", member.c_str());
        }
        self.totalRead += module.source.size();
        self.readModules.insert(member);
        if (!LuaMailboxUtf8(module.source)) return luaL_error(lua, "Lua 模块 %s 不是有效 UTF-8 文本。", member.c_str());
        const std::string chunk = "@" + member;
        if (luaL_loadbufferx(lua, module.source.data(), module.source.size(), chunk.c_str(), "t") != LUA_OK) return lua_error(lua);
        if (self.Expired()) return luaL_error(lua, "Lua 模块编译超出时间预算。");
        lua_pushlstring(lua, name.data(), name.size());
        lua_pushlstring(lua, member.data(), member.size());
        lua_call(lua, 2, 1);
        if (lua_isnil(lua, -1)) { lua_pop(lua, 1); lua_pushboolean(lua, 1); }
        lua_pushvalue(lua, -1); lua_setfield(lua, -3, name.c_str());
        return 1;
    }
    static int InitializeLibraries(lua_State* lua) {
        luaL_requiref(lua, "_G", luaopen_base, 1); lua_pop(lua, 1);
        luaL_requiref(lua, "table", luaopen_table, 1); lua_pop(lua, 1);
        luaL_requiref(lua, "string", luaopen_string, 1); lua_pop(lua, 1);
        luaL_requiref(lua, "math", luaopen_math, 1); lua_pop(lua, 1);
        luaL_requiref(lua, "utf8", luaopen_utf8, 1); lua_pop(lua, 1);
        for (const auto name : {"dofile", "loadfile", "load", "collectgarbage"}) Disable(lua, name);
        lua_getglobal(lua, "setmetatable"); lua_pushcclosure(lua, SetMetatable, 1); lua_setglobal(lua, "setmetatable");
        lua_getglobal(lua, "string"); lua_pushnil(lua); lua_setfield(lua, -2, "dump"); lua_pop(lua, 1);
        lua_getglobal(lua, "math");
        lua_pushcfunction(lua, Random); lua_setfield(lua, -2, "random");
        lua_pushcfunction(lua, RandomSeed); lua_setfield(lua, -2, "randomseed"); lua_pop(lua, 1);
        lua_pushcfunction(lua, Print); lua_setglobal(lua, "print");
        lua_pushcfunction(lua, Require); lua_setglobal(lua, "require");
        luaL_getsubtable(lua, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
        lua_rawsetp(lua, LUA_REGISTRYINDEX, &loadedModulesKey);
        return 0;
    }
    static int InitializeNatives(lua_State* lua) { Host(lua).natives->Install(lua); return 0; }
    LuaHostResult Error(int code, LuaReloadResult fallback) {
        const char* message = lua_tostring(state, -1);
        LuaHostResult result{budgetExceeded || code == LUA_ERRMEM ? LuaReloadResult::BudgetExceeded : fallback,
            message ? message : "Lua 宿主无法取得错误文本。"};
        lua_settop(state, 0);
        return result;
    }
};

LuaHost::LuaHost(LuaModuleReader reader, LuaHostLimits limits) : impl_(std::make_unique<Impl>(std::move(reader), limits)) {}
LuaHost::~LuaHost() = default;
LuaHostResult LuaHost::Initialize() {
    auto& self = *impl_;
    if (self.state || self.thread) return Failure(LuaReloadResult::HostFailure, "Lua 宿主不能重复初始化。");
    if (self.limits.memoryBytes < 128 * 1024 || self.limits.memoryBytes > 64 * 1024 * 1024
        || !self.limits.sourceBytes || self.limits.sourceBytes > 4 * 1024 * 1024
        || !self.limits.totalModuleBytes || self.limits.totalModuleBytes > 32 * 1024 * 1024
        || !self.limits.moduleCount || self.limits.moduleCount > 512 || !self.limits.requireDepth || self.limits.requireDepth > 64
        || !self.limits.instructions || self.limits.instructions > 100'000'000
        || !self.limits.milliseconds || self.limits.milliseconds > 5000)
        return Failure(LuaReloadResult::InvalidRequest, "Lua 宿主预算配置无效。");
    self.thread = GetCurrentThreadId();
    self.state = lua_newstate(Impl::Allocate, &self);
    if (!self.state) return Failure(LuaReloadResult::BudgetExceeded, "无法在预算内创建 Lua 宿主。");
    *static_cast<Impl**>(lua_getextraspace(self.state)) = &self;
    lua_pushcfunction(self.state, Impl::InitializeLibraries);
    const int code = lua_pcall(self.state, 0, 0, 0);
    if (code != LUA_OK) { auto result = self.Error(code, LuaReloadResult::HostFailure); lua_close(self.state); self.state = nullptr; return result; }
    return {LuaReloadResult::Success, {}};
}

LuaHostResult LuaHost::Execute(std::string_view source, std::string_view chunkName) {
    auto& self = *impl_;
    if (!self.state || !self.thread || GetCurrentThreadId() != self.thread || self.executing)
        return Failure(LuaReloadResult::NotReady, "Lua 宿主尚未就绪、线程不符或正处于执行中。");
    if (source.empty() || source.size() > self.limits.sourceBytes || !LuaMailboxUtf8(source) || !LuaMailboxChunkName(chunkName))
        return Failure(LuaReloadResult::InvalidRequest, "Lua 代码或名称不是有效的有界 UTF-8 文本。");
    self.executing = true; self.budgetExceeded = false;
    self.instructionsLeft = self.limits.instructions; self.started = GetTickCount64();
    struct Execution {
        Impl& host;
        ~Execution() { lua_sethook(host.state, nullptr, 0, 0); lua_settop(host.state, 0); host.executing = false; }
    } execution{self};
    const std::string name(chunkName);
    lua_pushcfunction(self.state, Impl::Traceback);
    int code = luaL_loadbufferx(self.state, source.data(), source.size(), name.c_str(), "t");
    if (code != LUA_OK) return self.Error(code, LuaReloadResult::SyntaxError);
    if (self.Expired()) return Failure(LuaReloadResult::BudgetExceeded, "Lua 编译超出时间预算，代码尚未执行。");
    lua_sethook(self.state, Impl::Hook, LUA_MASKCOUNT, kHookInterval);
    code = lua_pcall(self.state, 0, 0, 1);
    if (code != LUA_OK) return self.Error(code, LuaReloadResult::RuntimeError);
    if (self.Expired()) return Failure(LuaReloadResult::BudgetExceeded, "Lua 执行超出时间或内存预算；已发生的副作用没有回滚。");
    return {LuaReloadResult::Success, {}};
}
LuaHostResult LuaHost::BindNatives(std::shared_ptr<NativeProvider> provider) {
    auto& self = *impl_;
    if (!self.state || self.thread != GetCurrentThreadId() || self.executing || self.natives || !provider || !provider->Generation())
        return Failure(LuaReloadResult::NotReady, "Lua native 提供者未就绪、线程不符或已经绑定。");
    self.natives = std::make_unique<LuaNativeBindings>(std::move(provider));
    self.budgetExceeded = false;
    lua_pushcfunction(self.state, Impl::InitializeNatives);
    const int code = lua_pcall(self.state, 0, 0, 0);
    if (code != LUA_OK) return self.Error(code, LuaReloadResult::HostFailure);
    return {LuaReloadResult::Success, {}};
}
std::size_t LuaHost::MemoryBytes() const noexcept { return impl_->memory; }
std::string_view LuaHost::ConsoleText() const noexcept { return impl_->console; }

} // namespace Audastra::Runtime32::OwnLua
