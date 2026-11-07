#pragma once
#include <charconv>
#include <cstdint>
#include <span>

namespace War3Map::Lua {
// 构建与游戏使用相同的、无区域设置的最短往返表示；整数不附加 .0。
inline std::to_chars_result NumberText(std::span<char> destination,double value) {
    return std::to_chars(destination.data(),destination.data()+destination.size(),value,std::chars_format::general);
}
inline std::to_chars_result NumberText(std::span<char> destination,std::int64_t value) {
    return std::to_chars(destination.data(),destination.data()+destination.size(),value);
}
}
