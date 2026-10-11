#pragma once

#include <string>
#include <string_view>

struct lua_State;

namespace Audastra::Runtime32::OwnLua {

// 公开配置的受限实现；不创建控制台窗口，不开放 registry、文件或任意内存接口。
class LuaBehaviorModules final {
public:
    void Install(lua_State* lua);
    [[nodiscard]] bool ConsoleEnabled() const noexcept { return consoleEnabled_; }
    // 在宿主已建立预算和主线程身份的保护调用内通知。始终保留原始错误。
    [[nodiscard]] std::string NotifyError(lua_State* lua, std::string_view message, bool budgetAvailable);
private:
    int errorHandler_ = -2; // LUA_NOREF；引用随所属 Lua state 一起释放。
    bool consoleEnabled_ = true;
    bool notifying_ = false;
    std::string_view error_;
    static LuaBehaviorModules& Self(lua_State* lua);
    static int ReadRuntime(lua_State* lua);
    static int WriteRuntime(lua_State* lua);
    static int ReadConsole(lua_State* lua);
    static int WriteConsole(lua_State* lua);
    static int Traceback(lua_State* lua);
    static int Notify(lua_State* lua);
};

} // namespace Audastra::Runtime32::OwnLua
