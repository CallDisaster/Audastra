#include "War3MapCore/Script/ClassicLuaCompatibility.h"
#include "War3MapCore/Lua/LuaSyntax.h"

extern "C" {
#include <lua.h>
}
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace War3Map::Script;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
LuaTargetProfile Profile() {
    LuaTargetProfile result;
    result.identity = "original-classic-init-fixture";
    result.apis = {{"TriggerEvaluate", "boolean", {"trigger"}}, {"TriggerExecute", "nothing", {"trigger"}},
        {"SetPlayerAlliance", "nothing", {"player", "player", "alliancetype", "boolean"}}};
    for (const auto name : {"ALLIANCE_PASSIVE", "ALLIANCE_HELP_REQUEST", "ALLIANCE_HELP_RESPONSE", "ALLIANCE_SHARED_XP",
            "ALLIANCE_SHARED_SPELLS", "ALLIANCE_SHARED_VISION", "ALLIANCE_SHARED_CONTROL", "ALLIANCE_SHARED_ADVANCED_CONTROL"})
        result.constants.push_back({name, "alliancetype"});
    return result;
}
void* Allocate(void*, void* memory, std::size_t, std::size_t size) {
    if (!size) { std::free(memory); return nullptr; }
    return std::realloc(memory, size);
}
const char* Read(lua_State*, void* opaque, std::size_t* size) {
    auto& input = *static_cast<std::string_view*>(opaque);
    *size = input.size(); const auto* text = input.data(); input = {}; return text;
}
int Assert(lua_State* lua) {
    if (lua_toboolean(lua, 1)) return 0;
    lua_pushvalue(lua, 2); return lua_error(lua);
}
int Error(lua_State* lua) { lua_settop(lua, 1); return lua_error(lua); }
int Type(lua_State* lua) { lua_pushstring(lua, lua_typename(lua, lua_type(lua, 1))); return 1; }
int Byte(lua_State* lua) {
    std::size_t size = 0;
    const auto* bytes = lua_tolstring(lua, 1, &size);
    const auto index = lua_tointeger(lua, 2);
    if (!bytes || index < 1 || static_cast<std::uint64_t>(index) > size) {
        lua_pushliteral(lua, "unexpected string.byte fixture call"); return lua_error(lua);
    }
    lua_pushinteger(lua, static_cast<unsigned char>(bytes[index - 1])); return 1;
}
void Execute(std::string_view text, bool success = true) {
    std::unique_ptr<lua_State, decltype(&lua_close)> state(lua_newstate(Allocate, nullptr), lua_close);
    Check(static_cast<bool>(state), "official Lua state");
    auto* lua = state.get();
    lua_pushcfunction(lua, Assert); lua_setglobal(lua, "check");
    lua_pushcfunction(lua, Error); lua_setglobal(lua, "error");
    lua_pushcfunction(lua, Type); lua_setglobal(lua, "type");
    lua_newtable(lua); lua_pushcfunction(lua, Byte); lua_setfield(lua, -2, "byte"); lua_setglobal(lua, "string");
    Check(lua_load(lua, Read, &text, "@classic-compat-original.lua", "t") == LUA_OK, "compile original behavior fixture");
    const int code = lua_pcall(lua, 0, 0, 0);
    if (success && code != LUA_OK) throw std::runtime_error(lua_tostring(lua, -1));
    Check(success ? code == LUA_OK : code == LUA_ERRRUN, "expected compatibility execution result");
}
constexpr std::string_view kSetup = R"lua(
local calls, condition = {}, true
local common = {
    ALLIANCE_PASSIVE=0, ALLIANCE_HELP_REQUEST=1, ALLIANCE_HELP_RESPONSE=2,
    ALLIANCE_SHARED_XP=3, ALLIANCE_SHARED_SPELLS=4, ALLIANCE_SHARED_VISION=5,
    ALLIANCE_SHARED_CONTROL=6, ALLIANCE_SHARED_ADVANCED_CONTROL=7,
    TriggerEvaluate=function(t) calls[#calls+1]={'evaluate',t}; return condition end,
    TriggerExecute=function(t) calls[#calls+1]={'execute',t} end,
    SetPlayerAlliance=function(a,b,f,v) calls[#calls+1]={'alliance',a,b,f,v} end
}
local api=factory(common)
check(api.InitBlizzard==nil and api.SetPlayerSlotAvailable==nil and api.InitGenericPlayerSlots==nil
    and common.SetPlayerAllianceStateAllyBJ==nil and common.FourCC==nil, 'no native mutation or substitute JASS state')
)lua";
void Behavior(const ClassicLuaCompatibilityResult& built) {
    const auto prefix = "local factory=(function()\n" + built.moduleText + "\nend)()\n" + std::string(kSetup);
    Execute(prefix + R"lua(
for pass=1,2 do
    local enabled = pass==1
    calls={}; api.SetPlayerAllianceStateAllyBJ(3,7,enabled)
    check(#calls==5, 'ally flag count')
    for i=1,5 do
        check(calls[i][2]==3 and calls[i][3]==7 and calls[i][4]==i-1 and calls[i][5]==enabled, 'directed ally flags')
    end
    calls={}; api.SetPlayerAllianceStateVisionBJ(3,3,enabled)
    check(#calls==1 and calls[1][4]==5 and calls[1][5]==enabled, 'vision only; no extra self-player rule')
    calls={}; api.SetPlayerAllianceStateControlBJ(3,7,enabled)
    check(#calls==1 and calls[1][4]==6 and calls[1][5]==enabled, 'shared control only')
    calls={}; api.SetPlayerAllianceStateFullControlBJ(3,7,enabled)
    check(#calls==1 and calls[1][4]==7 and calls[1][5]==enabled, 'advanced control does not silently enable ordinary control')
end
)lua");
    Execute(prefix + R"lua(
check(api.FourCC('ABCD')==1094861636, 'network byte order rawcode')
check(api.FourCC('\000\000\000\000')==0, 'zero bytes retained')
check(api.FourCC('\127\255\255\255')==2147483647, 'positive int32 boundary')
check(api.FourCC('\128\000\000\000')==-2147483648, 'negative int32 boundary')
check(api.FourCC('\255\255\255\255')==-1, 'signed int32 rawcode')
calls={}; condition=false; api.ConditionalTriggerExecute(17)
check(#calls==1 and calls[1][1]=='evaluate' and calls[1][2]==17, 'false condition skips action')
calls={}; condition=true; api.ConditionalTriggerExecute(18)
check(#calls==2 and calls[1][1]=='evaluate' and calls[1][2]==18
    and calls[2][1]=='execute' and calls[2][2]==18, 'true condition executes once in order')
)lua");
    for (const auto argument : {"nil", "1234", "{}", "''", "'ABC'", "'ABCDE'"})
        Execute(prefix + "api.FourCC(" + argument + ")", false);
    Execute(prefix + "common.TriggerEvaluate=function() error('condition failure') end; api.ConditionalTriggerExecute(19)", false);
    Execute(prefix + "common.TriggerExecute=function() error('action failure') end; api.ConditionalTriggerExecute(20)", false);
}
}
int main() {
    try {
        const auto target = Profile();
        const auto built = BuildClassicLuaCompatibility(target);
        Check(built.Succeeded() && built.providedApis.size() == 6 && built.requiredNativeApis.size() == 3
            && built.requiredConstants.size() == 8, "explicit dependency and provided API lists");
        Check(War3Map::Lua::CheckSyntax(built.moduleText, "@classic-init.lua").Succeeded(), "module Lua 5.3 syntax");
        auto changed = target; changed.apis.pop_back();
        Check(!BuildClassicLuaCompatibility(changed).Succeeded(), "missing native refuses generated module");
        changed = target; changed.apis.front().returns = "real";
        Check(!BuildClassicLuaCompatibility(changed).Succeeded(), "wrong native signature refused");
        changed = target; changed.constants.front().type = "integer";
        Check(!BuildClassicLuaCompatibility(changed).Succeeded(), "wrong constant type refused");
        changed = target; changed.constants.push_back(changed.constants.front());
        Check(!BuildClassicLuaCompatibility(changed).Succeeded(), "ambiguous constants refused");
        changed = target; changed.apis.push_back({"FourCC", "real", {"string"}});
        Check(!BuildClassicLuaCompatibility(changed).Succeeded(), "conflicting compatibility API refused");
        changed = target; changed.apis.insert(changed.apis.end(), built.providedApis.begin(), built.providedApis.end());
        Check(BuildClassicLuaCompatibility(changed).moduleText == built.moduleText, "proven identical compatibility signatures accepted");
        Behavior(built);
        std::cout << "Classic Lua original initialization behavior passed; game_evidence=0\n";
        return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
