#include "LuaNativeBindings.h"
#include "LuaCallbacks.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"

#include <lua.h>
#include <lauxlib.h>
#include <cmath>
#include <exception>
#include <limits>
#include <new>

namespace Audastra::Runtime32::OwnLua {
namespace {
constexpr char kHandleMetatable[] = "Audastra.OwnLua.NativeHandle";
char handleCacheKey;
struct LuaHandle {
    alignas(NativeHandle) std::byte storage[sizeof(NativeHandle)];
    LuaNativeBindings* bindings = nullptr;
    bool alive = false;
    std::byte reserved[3]{};
    NativeHandle& Value() { return *std::launder(reinterpret_cast<NativeHandle*>(storage)); }
};
LuaHandle* GetHandle(lua_State* lua, int index) {
    auto* handle = static_cast<LuaHandle*>(luaL_testudata(lua, index, kHandleMetatable));
    return handle && handle->alive ? handle : nullptr;
}
LuaNativeBindings& Bindings(lua_State* lua) {
    return *static_cast<LuaNativeBindings*>(lua_touserdata(lua, lua_upvalueindex(1)));
}
class GenerationBoundCallback final : public NativeCallback {
public:
    GenerationBoundCallback(std::weak_ptr<NativeProvider> provider, std::uint64_t generation,
        std::shared_ptr<NativeCallback> callback)
        : provider_(std::move(provider)), generation_(generation), callback_(std::move(callback)) {}
    NativeCallbackReturn Returns() const noexcept override { return callback_->Returns(); }
    NativeCallbackResult Invoke() override {
        const auto provider = provider_.lock();
        const auto callback = callback_;
        if (!provider || provider->Generation() != generation_)
            return {false, false, "Lua 回调所属地图代际已经结束。"};
        return callback->Invoke();
    }
private:
    std::weak_ptr<NativeProvider> provider_;
    std::uint64_t generation_;
    std::shared_ptr<NativeCallback> callback_;
};
}

LuaNativeBindings::LuaNativeBindings(std::shared_ptr<NativeProvider> provider, LuaCallbacks& callbacks)
    : provider_(std::move(provider)), callbacks_(callbacks), generation_(provider_ ? provider_->Generation() : 0) {}

bool LuaNativeBindings::ValidHandle(const NativeHandle& handle, std::string_view expected) const noexcept {
    return handle.value && handle.generation == generation_ && provider_->Generation() == generation_
        && handle.owner && handle.owner->Alive() && NativeIdentifier(handle.type)
        && provider_->IsHandleType(handle.type, expected);
}
std::optional<NativeSignature> LuaNativeBindings::Signature(std::string_view name, std::string& error) const {
    try { return provider_->Lookup(name); }
    catch (const std::exception& exception) { error = std::string("native 签名查询异常：") + exception.what(); }
    catch (...) { error = "native 签名查询发生未知异常。"; }
    return {};
}
NativeCallResult LuaNativeBindings::CallProvider(std::string_view name, std::span<const NativeValue> arguments) {
    try { return provider_->Invoke(name, arguments); }
    catch (const std::exception& error) { return {false, {}, std::string("native 提供者异常：") + error.what()}; }
    catch (...) { return {false, {}, "native 提供者发生未知异常。"}; }
}

int LuaNativeBindings::CollectHandle(lua_State* lua) {
    if (auto* handle = GetHandle(lua, 1)) {
        handle->alive = false;
        handle->Value().~NativeHandle();
    }
    return 0;
}
int LuaNativeBindings::EqualHandle(lua_State* lua) {
    auto* first = GetHandle(lua, 1); auto* second = GetHandle(lua, 2);
    lua_pushboolean(lua, first && second && first->bindings == second->bindings
        && first->bindings->ValidHandle(first->Value(), first->Value().type)
        && second->bindings->ValidHandle(second->Value(), second->Value().type)
        && first->Value().generation == second->Value().generation && first->Value().value == second->Value().value);
    return 1;
}
int LuaNativeBindings::DescribeHandle(lua_State* lua) {
    auto* handle = GetHandle(lua, 1);
    if (!handle) return luaL_error(lua, "native handle 已失效。");
    const auto& value = handle->Value();
    lua_pushfstring(lua, "%s:%I", value.type.c_str(), static_cast<lua_Integer>(value.value));
    return 1;
}
void LuaNativeBindings::PushHandle(lua_State* lua, const NativeHandle& value) {
    lua_rawgetp(lua, LUA_REGISTRYINDEX, &handleCacheKey);
    lua_rawgeti(lua, -1, static_cast<lua_Integer>(value.value));
    if (auto* cached = GetHandle(lua, -1); cached && cached->bindings == this
        && cached->Value().generation == value.generation && cached->Value().value == value.value
        && ValidHandle(cached->Value(), cached->Value().type)) {
        if (ValidHandle(cached->Value(), value.type)) { lua_remove(lua, -2); return; }
        if (provider_->IsHandleType(value.type, cached->Value().type)) {
            cached->Value() = value; // 提供者确认了更具体的类型，沿用原 userdata，保留 Lua 表键身份。
            lua_remove(lua, -2); return;
        }
    }
    lua_pop(lua, 1);
    auto* handle = static_cast<LuaHandle*>(lua_newuserdata(lua, sizeof(LuaHandle)));
    handle->bindings = this; handle->alive = false;
    // 在可能分配失败的元表设置前注册 __gc；随后构造失败时 alive=false，不析构未构造对象。
    luaL_setmetatable(lua, kHandleMetatable);
    ::new (handle->storage) NativeHandle(value); handle->alive = true;
    lua_pushvalue(lua, -1); lua_rawseti(lua, -3, static_cast<lua_Integer>(value.value));
    lua_remove(lua, -2);
}

int LuaNativeBindings::Lookup(lua_State* lua) {
    auto& self = Bindings(lua);
    if (self.provider_->Generation() != self.generation_) return luaL_error(lua, "jass.common 所属地图代际已经结束。");
    if (lua_type(lua, 2) != LUA_TSTRING) return luaL_error(lua, "jass.common 需要 native 名称文本。");
    std::size_t size = 0; const auto* value = lua_tolstring(lua, 2, &size);
    const std::string name(value, size);
    if (!NativeIdentifier(name)) return luaL_error(lua, "jass.common 的 native 名称无效。");
    std::string error;
    if (!self.Signature(name, error)) return luaL_error(lua, "jass.common 无法查询 native %s：%s", name.c_str(),
        error.empty() ? "尚未注册。" : error.c_str());
    lua_pushlightuserdata(lua, &self); lua_pushlstring(lua, name.data(), name.size());
    lua_pushcclosure(lua, Call, 2);
    return 1;
}

int LuaNativeBindings::Call(lua_State* lua) {
    auto& self = Bindings(lua);
    const auto* name = lua_tostring(lua, lua_upvalueindex(2));
    if (self.provider_->Generation() != self.generation_) return luaL_error(lua, "native %s 的地图代际已经结束。", name);
    std::string lookupError;
    const auto signature = self.Signature(name, lookupError);
    if (!signature || signature->arguments.size() > 32) return luaL_error(lua, "native %s 的可信签名不可用：%s", name,
        lookupError.empty() ? "尚未注册。" : lookupError.c_str());
    if (signature->returns.kind == NativeKind::Code)
        return luaL_error(lua, "native %s 返回 code 的转换尚未实现，未调用 native。", name);
    if (lua_gettop(lua) != static_cast<int>(signature->arguments.size()))
        return luaL_error(lua, "native %s 参数数量不符：需要 %I 个。", name, static_cast<lua_Integer>(signature->arguments.size()));
    std::vector<NativeValue> arguments;
    arguments.reserve(signature->arguments.size());
    for (std::size_t index = 0; index < signature->arguments.size(); ++index) {
        const auto& type = signature->arguments[index];
        const int stack = static_cast<int>(index + 1);
        switch (type.kind) {
        case NativeKind::Integer: {
            int integral = 0; const auto value = lua_tointegerx(lua, stack, &integral);
            if (lua_type(lua, stack) != LUA_TNUMBER || !integral || value < std::numeric_limits<std::int32_t>::min()
                || value > std::numeric_limits<std::int32_t>::max())
                return luaL_error(lua, "native %s 第%d参数需要 int32 整数。", name, stack);
            arguments.emplace_back(static_cast<std::int32_t>(value)); break;
        }
        case NativeKind::Real: {
            const auto number = lua_tonumber(lua, stack); const auto value = static_cast<float>(number);
            if (lua_type(lua, stack) != LUA_TNUMBER || !std::isfinite(number) || !std::isfinite(value))
                return luaL_error(lua, "native %s 第%d参数需要有限 float32。", name, stack);
            arguments.emplace_back(value); break;
        }
        case NativeKind::Boolean:
            if (lua_type(lua, stack) != LUA_TBOOLEAN) return luaL_error(lua, "native %s 第%d参数需要 boolean。", name, stack);
            arguments.emplace_back(lua_toboolean(lua, stack) != 0); break;
        case NativeKind::String: {
            if (lua_isnil(lua, stack)) { arguments.emplace_back(std::monostate{}); break; }
            if (lua_type(lua, stack) != LUA_TSTRING) return luaL_error(lua, "native %s 第%d参数需要 string 或 nil。", name, stack);
            std::size_t size = 0; const auto* value = lua_tolstring(lua, stack, &size);
            if (size > 1024 * 1024 || !LuaMailboxUtf8({value, size}))
                return luaL_error(lua, "native %s 第%d参数不是有效的有界 UTF-8 字符串。", name, stack);
            arguments.emplace_back(std::string(value, size)); break;
        }
        case NativeKind::Handle: {
            if (lua_isnil(lua, stack)) { arguments.emplace_back(std::monostate{}); break; }
            auto* handle = GetHandle(lua, stack);
            if (!handle || handle->bindings != &self || !self.ValidHandle(handle->Value(), type.handleType))
                return luaL_error(lua, "native %s 第%d参数需要本局有效的 %s handle。", name, stack, type.handleType.c_str());
            arguments.emplace_back(handle->Value()); break;
        }
        case NativeKind::Code: {
            std::optional<NativeCallbackReturn> returns;
            std::string callbackError;
            try { returns = self.provider_->CallbackReturn(name, index); }
            catch (const std::exception& error) { callbackError = error.what(); }
            catch (...) { callbackError = "回调能力查询发生未知异常。"; }
            if (!returns) return luaL_error(lua, "native %s 第%d参数尚未建立经过验证的回调桥：%s", name, stack,
                callbackError.empty() ? "提供者未声明回调语义。" : callbackError.c_str());
            if (lua_isnil(lua, stack)) { arguments.emplace_back(std::monostate{}); break; }
            if (!lua_isfunction(lua, stack)) return luaL_error(lua, "native %s 第%d参数需要 Lua 函数或 nil。", name, stack);
            try {
                arguments.emplace_back(std::make_shared<GenerationBoundCallback>(self.provider_, self.generation_,
                    self.callbacks_.Capture(lua, stack, *returns)));
            }
            catch (const std::exception& error) { return luaL_error(lua, "native %s 无法捕获回调：%s", name, error.what()); }
            break;
        }
        case NativeKind::Nothing:
            return luaL_error(lua, "native %s 含无效参数签名。", name);
        }
    }
    const auto result = self.CallProvider(name, arguments);
    if (!result.succeeded) return luaL_error(lua, "native %s 失败：%s", name, result.error.empty() ? "提供者未返回诊断。" : result.error.c_str());
    const auto& returned = signature->returns;
    switch (returned.kind) {
    case NativeKind::Nothing:
        if (std::holds_alternative<std::monostate>(result.value)) return 0;
        break;
    case NativeKind::Integer:
        if (const auto* value = std::get_if<std::int32_t>(&result.value)) { lua_pushinteger(lua, *value); return 1; }
        break;
    case NativeKind::Real:
        if (const auto* value = std::get_if<float>(&result.value); value && std::isfinite(*value)) { lua_pushnumber(lua, *value); return 1; }
        break;
    case NativeKind::Boolean:
        if (const auto* value = std::get_if<bool>(&result.value)) { lua_pushboolean(lua, *value); return 1; }
        break;
    case NativeKind::String:
        if (std::holds_alternative<std::monostate>(result.value)) { lua_pushnil(lua); return 1; }
        if (const auto* value = std::get_if<std::string>(&result.value);
            value && value->size() <= 1024 * 1024 && LuaMailboxUtf8(*value)) { lua_pushlstring(lua, value->data(), value->size()); return 1; }
        break;
    case NativeKind::Handle:
        if (std::holds_alternative<std::monostate>(result.value)) { lua_pushnil(lua); return 1; }
        if (const auto* value = std::get_if<NativeHandle>(&result.value); value && self.ValidHandle(*value, returned.handleType)) {
            self.PushHandle(lua, *value); return 1;
        }
        break;
    case NativeKind::Code:
        return luaL_error(lua, "native %s 返回 code 的转换尚未实现。", name);
    }
    return luaL_error(lua, "native %s 返回值与可信签名不符，结果已拒绝。", name);
}

void LuaNativeBindings::Install(lua_State* lua) {
    if (!provider_ || !generation_) luaL_error(lua, "native 提供者没有有效的地图代际。");
    luaL_newmetatable(lua, kHandleMetatable);
    lua_pushcfunction(lua, CollectHandle); lua_setfield(lua, -2, "__gc");
    lua_pushcfunction(lua, EqualHandle); lua_setfield(lua, -2, "__eq");
    lua_pushcfunction(lua, DescribeHandle); lua_setfield(lua, -2, "__tostring");
    lua_pushliteral(lua, "native handle"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    lua_newtable(lua); lua_newtable(lua);
    lua_pushliteral(lua, "v"); lua_setfield(lua, -2, "__mode"); lua_setmetatable(lua, -2);
    lua_rawsetp(lua, LUA_REGISTRYINDEX, &handleCacheKey);
    luaL_getsubtable(lua, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_newtable(lua); lua_newtable(lua);
    lua_pushlightuserdata(lua, this); lua_pushcclosure(lua, Lookup, 1); lua_setfield(lua, -2, "__index");
    lua_pushliteral(lua, "jass.common"); lua_setfield(lua, -2, "__metatable"); lua_setmetatable(lua, -2);
    lua_setfield(lua, -2, "jass.common"); lua_pop(lua, 1);
}

} // namespace Audastra::Runtime32::OwnLua
