#pragma once

#include "War3MapCore/Types.h"

#include <charconv>
#include <string_view>

namespace War3Map::MapData {

// JASS/Lua/参数资格共用；编号至少四位，不截断。只构造能安全作为标识符的真实ID。
[[nodiscard]] inline Result<std::string> GeneratedObjectSymbol(
    std::string_view kind, std::string_view rawcode, std::int32_t creationId) {
    if ((kind != "unit" && kind != "item" && kind != "dest") || rawcode.size() != 4 || creationId < 0)
        return Result<std::string>::Failure("MapObjectReference", "MAP_OBJECT_SYMBOL_INVALID", "对象类别、四字节ID或创建编号无效。");
    for (const unsigned char c : rawcode)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return Result<std::string>::Failure("MapObjectReference", "MAP_OBJECT_SYMBOL_INVALID", "对象ID无法生成安全的脚本标识符。");
    char digits[12];
    const auto converted = std::to_chars(digits, digits + sizeof(digits), creationId);
    const auto count = static_cast<std::size_t>(converted.ptr - digits);
    std::string symbol = "gg_" + std::string(kind) + "_" + std::string(rawcode) + "_";
    if (count < 4) symbol.append(4 - count, '0');
    symbol.append(digits, converted.ptr);
    return Result<std::string>::Success(std::move(symbol));
}

} // namespace War3Map::MapData
