#pragma once
#include "War3MapCore/HLua/Hslk.h"
#include "War3MapCore/HLua/IdAllocator.h"
#include <chrono>
#include <memory>
#include <lua.h>

namespace War3Map::HLua::Detail {

// 所有入口均在保护调用里，Lua 以 C++ 异常模式编译；失败可正常销毁宿主拥有者。
class HslkSandbox final {
public:
    HslkSandbox(const HslkLimits& limits, std::span<const std::string> existing);
    ~HslkSandbox();
    HslkSandbox(const HslkSandbox&) = delete;
    Status Initialize();
    Status Run(std::string_view text, std::string_view name);
    Status CallText(const char* function, std::string_view text);
    Status ReadObjects(std::vector<HslkObject>& output);
    void InstallAllocator();
    lua_State* State() const { return lua_; }
    std::string console;
    std::string source;
    IdAllocator ids;
    std::size_t peak = 0;
    std::uint64_t instructions = 0;
private:
    HslkLimits limits_;
    lua_State* lua_ = nullptr;
    std::size_t memory_ = 0, fieldBytes_ = 0;
    std::chrono::steady_clock::time_point deadline_;
    bool exceeded_ = false;
    static HslkSandbox& Host(lua_State* lua);
    static void* Allocate(void*, void*, std::size_t, std::size_t) noexcept;
    static void Hook(lua_State*, lua_Debug*);
    static int Print(lua_State*);
    static int AllocateId(lua_State*);
    static int Extract(lua_State*);
    Status Error(const char* code);
};
} // namespace War3Map::HLua::Detail
