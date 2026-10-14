#pragma once

#include "LuaLexer.h"
#include "War3MapCore/Lua/LuaCodeModel.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace War3Map::Lua::Detail {

struct Shape;
struct FunctionInfo;

enum class TypeKind : std::uint8_t {
    Unknown, Nil, Boolean, Integer, Float, Number, String, Function, Table, Module, Library, Jass
};

// 推断出的值类型。Module 为 jass.* 运行库模块，Library 为 string/math 等标准库表，Jass 为 JASS 类型。
struct Type {
    TypeKind kind = TypeKind::Unknown;
    std::string name;
    std::shared_ptr<Shape> shape;                  // Table 的已知字段
    std::shared_ptr<const FunctionInfo> function;  // 本地写的 Lua 函数
    const ApiMember* api = nullptr;                // 运行库或标准库函数
    [[nodiscard]] bool Known() const noexcept { return kind != TypeKind::Unknown; }
};

struct Field {
    Type type;
    std::string documentation;
    std::uint32_t offset = 0;
};
struct Shape {
    std::map<std::string, Field, std::less<>> fields;
};
struct FunctionInfo {
    std::string name;
    std::vector<std::string> parameters;
    bool variadic = false;
    bool method = false; // 用冒号定义，隐含 self
    std::string documentation;
    Type returns;
};
struct LocalSymbol {
    std::string name;
    Type type;
    std::uint32_t declared = 0; // 声明完成后的位置（之后才可见）
    std::uint32_t scopeEnd = 0;
    bool parameter = false;
    std::string documentation;
    std::uint32_t nameEnd = 0; // 声明处名字的结束位置
};

// 一个 Lua 文件的解析结果。
struct Model {
    std::vector<Token> tokens; // 不含注释
    std::vector<LocalSymbol> locals;
    std::map<std::string, Field, std::less<>> globals;
    Type exports;
    std::vector<CodeWarning> warnings;
    std::vector<std::uint32_t> lineStarts;
};

// require 地图模块时取得该模块导出的类型（带缓存与循环保护）。
using ModuleResolver = std::function<Type(std::string_view name)>;

struct ParseOptions {
    const ApiCatalog* catalog = nullptr;
    ModuleResolver resolve;
    std::function<bool(std::string_view)> moduleExists; // 为空表示无法判断
    bool warnings = false;
};

[[nodiscard]] std::shared_ptr<Model> ParseModel(std::string_view source, const ParseOptions& options);

// 类型与签名的显示文本。
[[nodiscard]] std::string TypeName(const Type& type);
[[nodiscard]] Type TypeFromName(std::string_view name, const ApiCatalog* catalog);
[[nodiscard]] Type MemberOf(const Type& base, std::string_view name, const ApiCatalog* catalog, bool method = false);

} // namespace War3Map::Lua::Detail
