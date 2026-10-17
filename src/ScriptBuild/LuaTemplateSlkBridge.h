#pragma once

#include "War3MapCore/Script/LuaTemplateSandbox.h"

#include "lua.h"

#include <string>
#include <vector>

namespace War3Map::Script::detail {

// 读取 w3x2lni 默认值/metadata 与最终地图物编，在栈顶压入 slk 库的数据表。
// 只在 Lua 外层做 C++ 工作；失败时不压栈，返回错误文本由调用者以 Lua 错误报告。
// diagnostics 非空时，把“目标 slk 库不读取、已跳过”的来源记为警告。
[[nodiscard]] std::string PushSlkData(lua_State* lua, const LuaTemplateSlkSources& sources,
    std::vector<Diagnostic>* diagnostics = nullptr);

// 读取 finish() 结果（栈上 index 处的表），按其中的移除/新建改写最终物编成员。
// 只改动带 W2lo 标记的生成对象；没有变化的类别不产生成员。失败返回错误文本。
[[nodiscard]] std::string ApplySlkResult(lua_State* lua, int index, const LuaTemplateSlkSources& sources,
    TemplateTransactionOutput& output);

} // namespace War3Map::Script::detail
