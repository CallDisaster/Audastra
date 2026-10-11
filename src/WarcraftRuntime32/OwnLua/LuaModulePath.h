#pragma once
#include <string>
#include <string_view>

namespace Audastra::Runtime32::OwnLua {
// require 与经典 Cheat 入口共用的地图内相对成员规则；失败返回空文本。
[[nodiscard]] std::string LuaModuleMember(std::string_view name);
}
