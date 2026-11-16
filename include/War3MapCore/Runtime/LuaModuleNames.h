#pragma once
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"

#include <string>
#include <string_view>

namespace Audastra::Runtime32 {
// require 与经典 Cheat 入口共用的地图内相对成员规则；失败返回空文本。
// 编辑器测试预检与 x86 运行时共用此实现，避免两边对同一模块名得出不同结论。
[[nodiscard]] inline std::string LuaModuleMember(std::string_view name) {
    if (name.empty() || name.size() > 240 || !LuaMailboxUtf8(name)) return {};
    std::string result;
    bool separator = true;
    for (const auto ch : name) {
        if (ch == '.' || ch == '/' || ch == '\\') {
            if (separator) return {};
            result += '\\'; separator = true;
        } else {
            if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f || ch == ':' || ch == '*' || ch == '?'
                || ch == '"' || ch == '<' || ch == '>' || ch == '|') return {};
            result += ch; separator = false;
        }
    }
    if (separator) return {};
    result += ".lua";
    return result;
}

struct LuaEntryCommand {
    bool recognized = false;
    std::string module;
    std::string error;
};

// 已核对的 YDWE 地图约定：Cheat("exec-lua: \"module\"")。与 YDWE 的 Lua 引擎相同，去掉两端空白后
// 两头都是双引号才剥掉，不加引号也可以（KKWE 演示图写成 Cheat("exec-lua:init")）。
// 此入口只接受一个模块文本，绝不把冒号后的任意内容作为 Lua 代码求值；不加引号时不许含空白和引号。
[[nodiscard]] inline LuaEntryCommand ParseLuaEntryCommand(std::string_view command) {
    constexpr std::string_view prefix = "exec-lua:";
    if (!command.starts_with(prefix)) return {};
    LuaEntryCommand result; result.recognized = true;
    auto value = command.substr(prefix.size());
    const auto space = [](char ch) { return ch == ' ' || ch == '\t'; };
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    while (!value.empty() && space(value.back())) value.remove_suffix(1);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    } else if (value.find_first_of(" \t\"'") != std::string_view::npos) {
        value = {};
    }
    if (value.empty() || value.size() > 240) {
        result.error = "exec-lua 入口需要一个地图模块名，例如 exec-lua: \"main\"。"; return result;
    }
    if (LuaModuleMember(value).empty()) { result.error = "exec-lua 模块名不是有效的地图内相对成员。"; return result; }
    result.module = value;
    return result;
}
} // namespace Audastra::Runtime32
