#include "../RuntimeProtocol.h"
#include <cstring>
#include <string>

namespace {
struct State { std::string marker, result; int top = 1; bool probeLoaded = false; };
}
extern "C" void* CreateFakeLuaProbeState(const char* marker) {
    auto* state = new State; state->marker = marker; return state;
}
extern "C" void DeleteFakeLuaProbeState(void* pointer) { delete static_cast<State*>(pointer); }
extern "C" int lua_pcallk(void*, int, int, int, std::intptr_t, void*) { return 0; }
extern "C" int lua_gettop(void* pointer) { return static_cast<State*>(pointer)->top; }
extern "C" void lua_settop(void* pointer, int top) { auto& state = *static_cast<State*>(pointer); state.top = top; if (top == 1) state.probeLoaded = false; }
extern "C" int lua_type(void*, int) { return 4; }
extern "C" const char* lua_tolstring(void* pointer, int, std::size_t* size) {
    const auto& state = *static_cast<State*>(pointer); const auto& text = state.probeLoaded ? state.result : state.marker;
    *size = text.size(); return text.c_str();
}
extern "C" int luaL_loadbufferx(void* pointer, const char*, std::size_t, const char*, const char* mode) {
    if (!mode || std::strcmp(mode, "t") != 0) return 2;
    auto* shared = Audastra::Runtime32::OpenChannel(); if (!shared) return 2;
    auto& state = *static_cast<State*>(pointer); state.probeLoaded = true; state.top = 2;
    state.result = "AUDASTRA_LUA_API_V1:" + std::string(shared->probeNonce)
        + ":{\"schema\":\"audastra-lua-api-observation-v1\",\"nonce\":\"" + shared->probeNonce
        + "\",\"completed\":true,\"fixture\":true,\"modules\":[{\"name\":\"jass.fixture\",\"available\":true,\"enumerationComplete\":false,\"fields\":[{\"name\":\"OriginalFixtureFunction\",\"kind\":\"function\",\"parameterCount\":null}]}]}";
    UnmapViewOfFile(shared); return 0;
}
