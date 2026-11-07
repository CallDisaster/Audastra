#pragma once

#include "NativeProvider.h"
#include <functional>

struct lua_State;

namespace Audastra::Runtime32::OwnLua {

class LuaCallbacks;
class LuaNativeBindings final {
public:
    LuaNativeBindings(std::shared_ptr<NativeProvider> provider, LuaCallbacks& callbacks, std::function<bool()> expired = {});
    // 必须在 lua_pcall 内调用；宿主拥有本实例直到 lua_close 完成。
    void Install(lua_State* lua);
    // 初始化base库时安装，提前保存rawset的脚本也不能绕过后来绑定的宿主表保护。
    static void InstallTableWriteGuard(lua_State* lua);
    static void ProtectHostTable(lua_State* lua, int index);
private:
    static int Lookup(lua_State* lua);
    static int LookupGlobal(lua_State* lua);
    static int RejectGlobalWrite(lua_State* lua);
    static int WriteGlobal(lua_State* lua);
    static int LookupDebug(lua_State* lua);
    static int HandleMaximum(lua_State* lua);
    static int HandleDefinition(lua_State* lua);
    static int RawSet(lua_State* lua);
    static int Call(lua_State* lua);
    static int CollectHandle(lua_State* lua);
    static int EqualHandle(lua_State* lua);
    static int DescribeHandle(lua_State* lua);
    void PushHandle(lua_State* lua, const NativeHandle& handle);
    int PushValue(lua_State* lua, const NativeType& type, const NativeValue& value, const char* name);
    int ReadGlobal(lua_State* lua, std::string_view name);
    NativeValue ReadValue(lua_State* lua, int stack, const NativeType& type, const char* name);
    [[nodiscard]] bool ValidHandle(const NativeHandle& handle, std::string_view expected) const noexcept;
    [[nodiscard]] std::optional<NativeSignature> Signature(std::string_view name, std::string& error) const;
    [[nodiscard]] NativeCallResult CallProvider(std::string_view name, std::span<const NativeValue> arguments);
    std::shared_ptr<NativeProvider> provider_;
    LuaCallbacks& callbacks_;
    std::uint64_t generation_ = 0;
    std::function<bool()> expired_;
};

} // namespace Audastra::Runtime32::OwnLua
