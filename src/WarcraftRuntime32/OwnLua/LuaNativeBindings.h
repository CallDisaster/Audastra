#pragma once

#include "NativeProvider.h"

struct lua_State;

namespace Audastra::Runtime32::OwnLua {

class LuaCallbacks;
class LuaNativeBindings final {
public:
    LuaNativeBindings(std::shared_ptr<NativeProvider> provider, LuaCallbacks& callbacks);
    // 必须在 lua_pcall 内调用；宿主拥有本实例直到 lua_close 完成。
    void Install(lua_State* lua);
    // 初始化base库时安装，提前保存rawset的脚本也不能绕过后来绑定的宿主表保护。
    static void InstallTableWriteGuard(lua_State* lua);
private:
    static int Lookup(lua_State* lua);
    static int LookupGlobal(lua_State* lua);
    static int RejectGlobalWrite(lua_State* lua);
    static int RawSet(lua_State* lua);
    static int Call(lua_State* lua);
    static int CollectHandle(lua_State* lua);
    static int EqualHandle(lua_State* lua);
    static int DescribeHandle(lua_State* lua);
    void PushHandle(lua_State* lua, const NativeHandle& handle);
    int PushValue(lua_State* lua, const NativeType& type, const NativeValue& value, const char* name);
    int ReadGlobal(lua_State* lua, std::string_view name);
    [[nodiscard]] bool ValidHandle(const NativeHandle& handle, std::string_view expected) const noexcept;
    [[nodiscard]] std::optional<NativeSignature> Signature(std::string_view name, std::string& error) const;
    [[nodiscard]] NativeCallResult CallProvider(std::string_view name, std::span<const NativeValue> arguments);
    std::shared_ptr<NativeProvider> provider_;
    LuaCallbacks& callbacks_;
    std::uint64_t generation_ = 0;
};

} // namespace Audastra::Runtime32::OwnLua
