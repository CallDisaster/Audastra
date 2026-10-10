// 原创 real-Lua 5.3 模块观察门禁回归；只执行本仓库模板和本文件种子的自有代码。
#include <windows.h>
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace {
using Json = nlohmann::json;
int apiCalls = 0, nestedLoaderCalls = 0;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int NeverInvokeApi(lua_State* state) { ++apiCalls; return luaL_error(state, "Metadata must never invoke the game API fixture"); }
int CommonLookup(lua_State* state) {
    if (std::string_view(luaL_checkstring(state, 2)) == "ActualApi") lua_pushcfunction(state, NeverInvokeApi);
    else lua_pushnil(state);
    return 1;
}
int CommonPairs(lua_State* state) {
    lua_getglobal(state, "next"); lua_newtable(state);
    lua_pushliteral(state, "actual registration metadata, not the callable value"); lua_setfield(state, -2, "ActualApi");
    lua_pushnil(state); return 3;
}
int CommonLoader(lua_State* state) {
    lua_newtable(state); lua_newtable(state);
    lua_pushcfunction(state, CommonLookup); lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, CommonPairs); lua_setfield(state, -2, "__pairs");
    lua_setmetatable(state, -2); return 1;
}
int NestedLoader(lua_State* state) {
    ++nestedLoaderCalls;
    lua_getglobal(state, "require"); lua_pushliteral(state, "unregistered.authored.nested");
    lua_call(state, 1, 1); return 1;
}
void Register(lua_State* state, const std::string& name, lua_CFunction function) {
    lua_getglobal(state, "package"); lua_getfield(state, -1, "preload");
    lua_pushcfunction(state, function); lua_setfield(state, -2, name.c_str()); lua_pop(state, 2);
}
std::string Execute(lua_State* state, std::string_view source, bool result) {
    if (luaL_loadbufferx(state, source.data(), source.size(), "@AudastraOwnedPolicyTest", "t") != LUA_OK
        || lua_pcall(state, 0, result ? 1 : 0, 0) != LUA_OK) {
        const auto* error = lua_tostring(state, -1);
        throw std::runtime_error(error ? error : "Owned Lua test failed without an error string");
    }
    if (!result) return {};
    std::size_t bytes = 0; const auto* value = lua_tolstring(state, -1, &bytes);
    Require(value != nullptr, "Probe returned no string");
    std::string text(value, bytes); lua_pop(state, 1); return text;
}
void ReplaceOnce(std::string& source, std::string_view marker, const std::string& value) {
    const auto at = source.find(marker);
    Require(at != std::string::npos && source.find(marker, at + marker.size()) == std::string::npos, "Template marker is not unique");
    source.replace(at, marker.size(), value);
}
const Json& Module(const Json& observed, const char* name) {
    for (const auto& module : observed.at("modules")) if (module.at("name") == name) return module;
    throw std::runtime_error("Expected module observation missing");
}
void Case(const std::string& original, const char* name, bool budget = false) {
    apiCalls = 0; nestedLoaderCalls = 0;
    const auto close = [](lua_State* state) { if (state) lua_close(state); };
    std::unique_ptr<lua_State, decltype(close)> state(luaL_newstate(), close);
    Require(state != nullptr, "Own Lua state creation");
    constexpr std::array<std::pair<const char*, lua_CFunction>, 5> libraries{{
        {"_G", luaopen_base}, {LUA_LOADLIBNAME, luaopen_package}, {LUA_DBLIBNAME, luaopen_debug},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_TABLIBNAME, luaopen_table}
    }};
    for (const auto& [library, open] : libraries) {
        luaL_requiref(state.get(), library, open, 1); lua_pop(state.get(), 1);
    }
    Register(state.get(), "jass.common", CommonLoader);
    Register(state.get(), "jass.nested", NestedLoader);
    Execute(state.get(), R"lua(
__filesystemVisits, __foreignChunkExecuted, __luaPreloadExecuted = 0, false, false
__originalSearchers = package.searchers
local function filesystemSearcher(name)
    __filesystemVisits = __filesystemVisits + 1
    return function() __foreignChunkExecuted = true; return {InventedApi = function() end} end
end
for i = 2, #package.searchers do package.searchers[i] = filesystemSearcher end
package.preload['jass.luaPreload'] = function() __luaPreloadExecuted = true; return {} end
package.loaded['jass.already'] = {LoadedField = function() end}
)lua", false);
    if (budget) for (unsigned index = 0; index < 66; ++index) Register(state.get(), "jass.budget" + std::to_string(index), CommonLoader);
    const std::string nonce = "11111111111111111111111111111111";
    auto source = original;
    ReplaceOnce(source, "__AUDASTRA_PROBE_NONCE__", nonce);
    ReplaceOnce(source, "__AUDASTRA_COMMON_CANDIDATES__", "{\"ActualApi\"}");
    ReplaceOnce(source, "__AUDASTRA_MODULE_CANDIDATES__", "{\"jass.common\",\"jass.binaryOnly\",\"jass.nested\",\"jass.luaPreload\",\"jass.already\"}");
    const auto text = Execute(state.get(), source, true);
    const auto prefix = "AUDASTRA_LUA_API_V1:" + nonce + ':';
    Require(text.starts_with(prefix), "Own nonce response binding");
    const auto observed = Json::parse(text.substr(prefix.size()));
    Execute(state.get(), R"lua(
assert(package.searchers == __originalSearchers, 'original searchers not restored')
assert(__filesystemVisits == 0 and not __foreignChunkExecuted, 'filesystem fallback executed')
assert(not __luaPreloadExecuted, 'unbound Lua preload chunk executed')
)lua", false);
    Require(apiCalls == 0, "A game API fixture was invoked during metadata lookup");
    if (budget) {
        Require(observed.at("completed") == false, "Metadata budget failure was hidden");
        Require(nestedLoaderCalls == 0, "Budget failure executed a module loader");
    } else {
        Require(observed.at("completed") == true && observed.at("runtimePreloadOnly") == true, "Native preload policy proof missing");
        if (std::string_view(name) == "runtime-positive") {
            const auto& fields = Module(observed, "jass.common").at("fields");
            Require(fields.size() == 1 && fields[0].at("name") == "ActualApi" && fields[0].at("kind") == "function"
                && fields[0].at("origin") == "common-declaration-runtime-lookup", "Actual lookup did not override pairs registration metadata");
            Require(Module(observed, "jass.already").at("available") == true, "Actual loaded module was lost");
        } else if (std::string_view(name) == "binary-fallback-denied") {
            Require(Module(observed, "jass.binaryOnly").at("available") == false
                && Module(observed, "jass.binaryOnly").at("fields").empty(), "An unregistered binary candidate became an observed module");
        } else if (std::string_view(name) == "nested-fallback-denied") {
            Require(nestedLoaderCalls == 1 && Module(observed, "jass.nested").at("available") == false,
                "Nested require bypassed the native preload restriction");
        } else {
            Require(Module(observed, "jass.luaPreload").at("available") == false, "A Lua preload chunk was accepted as a runtime C loader");
        }
    }
    std::cout << "PASS original real-Lua policy case " << name << '\n';
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 2) return 64;
    try {
        const std::filesystem::path path(argv[1]);
        Require(path.is_absolute() && std::filesystem::file_size(path) < 64 * 1024, "Explicit own template path/budget required");
        std::ifstream input(path, std::ios::binary); Require(input.good(), "Own template cannot be read");
        const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        for (const auto* name : {"runtime-positive", "binary-fallback-denied", "nested-fallback-denied", "lua-preload-denied"}) Case(source, name);
        Case(source, "failure-restores-searchers", true);
        std::cout << "5/5 original real-Lua policy cases passed; no user DLL/Warcraft/YDWE/GUI/GPU executed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
