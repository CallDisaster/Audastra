#include "LuaEntryCommand.h"
#include "LuaModulePath.h"

namespace Audastra::Runtime32::OwnLua {
LuaEntryCommand ParseLuaEntryCommand(std::string_view command) {
    constexpr std::string_view prefix = "exec-lua:";
    if (!command.starts_with(prefix)) return {};
    LuaEntryCommand result; result.recognized = true;
    auto value = command.substr(prefix.size());
    const auto space = [](char ch) { return ch == ' ' || ch == '\t'; };
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    while (!value.empty() && space(value.back())) value.remove_suffix(1);
    if (value.size() < 3 || value.size() > 242 || value.front() != '"' || value.back() != '"') {
        result.error = "exec-lua 入口需要一个双引号包裹的地图模块名。"; return result;
    }
    value = value.substr(1, value.size() - 2);
    if (LuaModuleMember(value).empty()) { result.error = "exec-lua 模块名不是有效的地图内相对成员。"; return result; }
    result.module = value;
    return result;
}
} // namespace Audastra::Runtime32::OwnLua
