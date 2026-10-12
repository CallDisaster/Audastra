#pragma once
#include <string>
#include <string_view>

namespace Audastra::Runtime32::OwnLua {
struct LuaEntryCommand {
    bool recognized = false;
    std::string module;
    std::string error;
};
// 已核对的 YDWE 地图约定：Cheat("exec-lua: \"module\"")。
// 此入口只接受一个模块文本，绝不把冒号后的任意内容作为 Lua 代码求值。
[[nodiscard]] LuaEntryCommand ParseLuaEntryCommand(std::string_view command);
} // namespace Audastra::Runtime32::OwnLua
