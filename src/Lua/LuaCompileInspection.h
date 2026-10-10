#pragma once

#include "War3MapCore/Lua/LuaSyntax.h"
#include <functional>

struct lua_State;
namespace War3Map::Lua::Detail {
// 仅本目标使用；编译闭包只供只读原型审查，调用方不能运行它。
using CompiledInspection = std::function<void(lua_State*)>;
SyntaxResult CheckSyntaxWithInspection(std::string_view source, std::string_view name,
    const SyntaxOptions& options, const CompiledInspection& inspection);
}
