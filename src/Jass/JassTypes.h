#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// JASS 类型表与转换规则（检查器与 vJass 翻译共用）。
namespace War3Map::Jass::detail {

using TypeId = std::int32_t;

// 内置类型的固定编号；type 声明的句柄类型从 kFirstUserType 开始。
inline constexpr TypeId kErrorType = 0; // 已报错的表达式：与任何类型兼容，避免连锁报错
inline constexpr TypeId kNothing = 1;
inline constexpr TypeId kNull = 2;
inline constexpr TypeId kInteger = 3;
inline constexpr TypeId kReal = 4;
inline constexpr TypeId kBoolean = 5;
inline constexpr TypeId kString = 6;
inline constexpr TypeId kCode = 7;
inline constexpr TypeId kHandle = 8;
inline constexpr TypeId kFirstUserType = 9;
inline constexpr TypeId kUnknownType = -1;

class TypeTable {
public:
    TypeTable();

    // 声明 type name extends parent；parent 须已存在。name 所指文本须在类型表使用期间有效。返回新类型，名称重复或父类型未知时返回 kUnknownType。
    TypeId Declare(std::string_view name, TypeId parent);
    [[nodiscard]] TypeId Find(std::string_view name) const;
    [[nodiscard]] const std::string& Name(TypeId type) const;
    [[nodiscard]] TypeId Parent(TypeId type) const;

    [[nodiscard]] bool IsHandle(TypeId type) const;
    [[nodiscard]] static bool IsNumeric(TypeId type) noexcept { return type == kInteger || type == kReal; }
    // from 是否为 to 本身或其子类型。
    [[nodiscard]] bool Extends(TypeId from, TypeId to) const;
    // 赋值 / 传参 / 返回时 from 能否转为 to。
    [[nodiscard]] bool CanConvert(TypeId from, TypeId to) const;
    // == / != 两侧能否比较：同类型、数字之间、任一侧为 null、两个句柄类型。
    [[nodiscard]] bool CanCompare(TypeId left, TypeId right) const;

private:
    struct Entry {
        std::string name;
        TypeId parent = kUnknownType;
    };
    std::vector<Entry> entries_;
    std::unordered_map<std::string_view, TypeId> byName_;
};

} // namespace War3Map::Jass::detail
