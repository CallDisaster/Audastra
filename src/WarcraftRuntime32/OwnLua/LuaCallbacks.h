#pragma once

#include "NativeProvider.h"
#include <functional>
#include <memory>

struct lua_State;

namespace Audastra::Runtime32::OwnLua {

// 所有 Lua API 均由宿主线程执行。跨线程析构只把待释放引用交回宿主，不能操作 Lua。
class LuaCallbacks final {
public:
    using Runner = std::function<NativeCallbackResult(int reference, NativeCallbackReturn returns)>;
    LuaCallbacks();
    ~LuaCallbacks();
    LuaCallbacks(const LuaCallbacks&) = delete;
    LuaCallbacks& operator=(const LuaCallbacks&) = delete;
    void Initialize(lua_State* state, Runner runner);
    [[nodiscard]] std::shared_ptr<NativeCallback> Capture(lua_State* state, int index, NativeCallbackReturn returns);
    void Drain();
    // 必须在 lua_close 之前调用；仍由 provider 持有的闭包从此明确过期。
    void Close() noexcept;
    [[nodiscard]] std::size_t Count() const noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace Audastra::Runtime32::OwnLua
