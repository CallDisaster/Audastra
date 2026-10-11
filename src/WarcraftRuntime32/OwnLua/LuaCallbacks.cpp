#include "LuaCallbacks.h"

#include <lua.h>
#include <lauxlib.h>
#include <windows.h>
#include <atomic>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace Audastra::Runtime32::OwnLua {

struct LuaCallbacks::State {
    static constexpr std::size_t kMaximum = 4096;
    struct Callback final : NativeCallback {
        std::shared_ptr<State> state;
        int reference = LUA_NOREF;
        NativeCallbackReturn returns = NativeCallbackReturn::Nothing;
        Callback(std::shared_ptr<State> owner, NativeCallbackReturn result) : state(std::move(owner)), returns(result) {}
        ~Callback() override { if (reference != LUA_NOREF) state->Release(reference); }
        NativeCallbackReturn Returns() const noexcept override { return returns; }
        NativeCallbackResult Invoke() override {
            const auto owner = state; // 回调内可能移除自身的最后一个原生持有者。
            const auto resultType = returns;
            const auto function = reference;
            if (GetCurrentThreadId() != owner->thread || !owner->open.load())
                return {false, false, "Lua 回调所属宿主已结束或调用线程不符。"};
            try { return owner->runner(function, resultType); }
            catch (const std::exception& error) { return {false, false, std::string("Lua 回调宿主异常：") + error.what()}; }
            catch (...) { return {false, false, "Lua 回调宿主发生未知异常。"}; }
        }
    };
    lua_State* lua = nullptr;
    DWORD thread = 0;
    Runner runner;
    std::atomic<bool> open{false};
    std::atomic<std::size_t> count{0};
    std::mutex pendingMutex;
    std::vector<int> pending;
    void Release(int reference) noexcept {
        if (thread == GetCurrentThreadId() && open.load()) {
            luaL_unref(lua, LUA_REGISTRYINDEX, reference);
            --count;
        } else {
            std::lock_guard lock(pendingMutex);
            if (open.load()) pending.push_back(reference); // 容量在 Initialize 一次性预留，上限由 Capture 保证。
        }
    }
};

LuaCallbacks::LuaCallbacks() : state_(std::make_shared<State>()) {}
LuaCallbacks::~LuaCallbacks() { Close(); }
void LuaCallbacks::Initialize(lua_State* lua, Runner runner) {
    if (state_->lua || !lua || !runner) throw std::logic_error("Lua 回调宿主不能重复初始化。");
    state_->pending.reserve(State::kMaximum);
    state_->lua = lua;
    state_->thread = GetCurrentThreadId();
    state_->runner = std::move(runner);
    state_->open = true;
}
std::shared_ptr<NativeCallback> LuaCallbacks::Capture(lua_State* lua, int index, NativeCallbackReturn returns) {
    Drain();
    if (!state_->open.load() || lua != state_->lua || GetCurrentThreadId() != state_->thread)
        throw std::logic_error("Lua 回调只能在当前宿主线程捕获。");
    if (state_->count >= State::kMaximum) throw std::runtime_error("Lua 回调超过 4096 个存活引用的预算。");
    if (!lua_isfunction(lua, index)) throw std::invalid_argument("Lua code 参数需要函数。");
    auto callback = std::make_shared<State::Callback>(state_, returns);
    lua_pushvalue(lua, index);
    callback->reference = luaL_ref(lua, LUA_REGISTRYINDEX);
    ++state_->count;
    return callback;
}
void LuaCallbacks::Drain() {
    if (GetCurrentThreadId() != state_->thread || !state_->open.load()) return;
    std::lock_guard lock(state_->pendingMutex);
    for (const auto reference : state_->pending) {
        luaL_unref(state_->lua, LUA_REGISTRYINDEX, reference);
        --state_->count;
    }
    state_->pending.clear();
}
void LuaCallbacks::Close() noexcept {
    std::lock_guard lock(state_->pendingMutex);
    state_->open = false;
    state_->runner = {};
    state_->lua = nullptr;
    state_->pending.clear();
    state_->count = 0; // registry 随后由 lua_close 一次释放。
}
std::size_t LuaCallbacks::Count() const noexcept { return state_->count.load(); }

} // namespace Audastra::Runtime32::OwnLua
