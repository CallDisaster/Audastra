#pragma once

#include "War3MapCore/Services/ObjectCommands.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Services {

// 物编紧凑文本：一行一个字段，写成“显示名 (字段[:等级[:变体]]) = 值”。
// 值的写法同紧凑 T：数字、"文本"（\" \\ \n \r \t 转义）、'A000'、true/false、列表 ['A000', 'A001']。
// 括号里的字段键才是身份；前面的显示名、行尾 // 注释和 # 开头的行只供阅读，写回时忽略。
// 只有 key = 值、不带显示名的行也接受。
struct ObjectFieldTextOptions {
    bool all = false;            // false：只列与父对象不同（已覆盖）的字段
    std::size_t maxLines = 4096; // 超出时报错，不返回截断的文本
};
// 给值找可读名（如引用对象的名称），用于行尾注释；返回空串表示不注释。
using ObjectFieldValueNamer = std::function<std::string(const ObjectFieldRow& row, const std::string& rawValue)>;

[[nodiscard]] Result<std::string> RenderObjectFieldText(std::span<const ObjectFieldRow> rows,
    const ObjectFieldTextOptions& options = {}, const ObjectFieldValueNamer& namer = {});

struct ObjectFieldTextChange {
    ObjectFieldChange change;
    std::size_t line = 0;
};
// 错误一次全部列出，各带行号；有任何错误时不返回部分结果。
[[nodiscard]] Result<std::vector<ObjectFieldTextChange>> ParseObjectFieldText(std::string_view text);

} // namespace War3Map::Services
