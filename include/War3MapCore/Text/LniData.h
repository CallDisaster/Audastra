#pragma once
#include "War3MapCore/Types.h"
#include <functional>
#include <map>
#include <span>
#include <string_view>

namespace War3Map::Text {
// 经典 LNI 的数据语法树；不执行 Lua 表达式，保留标量原文及字节位置。
struct LniDataValue {
    enum class Kind { Number, String, Table } kind = Kind::Table;
    std::string text;
    std::map<std::string, LniDataValue> fields;
    std::size_t offset = 0;
    bool used = false;
};
[[nodiscard]] Result<LniDataValue> ParseLniData(std::span<const std::byte> data,
    const std::function<std::string(std::string_view)>& normalizeKey = {});
} // namespace War3Map::Text
