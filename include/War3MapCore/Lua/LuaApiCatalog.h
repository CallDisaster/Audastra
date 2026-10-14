#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace War3Map::Lua {

enum class ApiKind : std::uint8_t { Module, Function, Constant, Variable, Field, Keyword, Local, Parameter, Global };

// type 为 JASS 类型名（unit、integer…）或 Lua 类型（number、string、boolean、function、table、any）。
struct ApiParameter { std::string name, type; };

struct ApiMember {
    std::string name;
    ApiKind kind = ApiKind::Function;
    std::vector<ApiParameter> parameters;
    bool variadic = false;
    std::string returns;       // 函数返回类型；空表示没有返回值或未知
    std::string type;          // 常量、变量、字段的类型
    std::string value;         // 常量值原文（可选）
    std::string documentation;
    std::string module;        // 所属模块（jass.common、string…）；全局为空
    std::string table;         // 全局值本身是哪张标准库表（string、math…）
};

struct ApiModule {
    std::string name;
    std::string documentation;
    std::vector<ApiMember> members; // 按名称排序
    bool closed = true;     // 成员集合完整：访问未列出的成员在运行时报错
    bool requirable = true; // 可以用 require 取得（标准库表不行）
    [[nodiscard]] const ApiMember* Find(std::string_view name) const;
};

// 一份运行环境的静态接口说明；只描述声明，不执行代码。
struct ApiCatalog {
    std::string binding; // 来源身份（common.j 摘要等）
    std::string source;  // 来源说明
    bool nativesLoaded = false;
    std::map<std::string, ApiModule, std::less<>> modules;
    std::vector<ApiMember> globals; // 按名称排序
    std::map<std::string, std::string, std::less<>> typeParents;
    // 能经 jass.common 读到的全部 JASS 全局名（common.j、blizzard.j、地图变量）。
    std::set<std::string, std::less<>> jassGlobals;
    [[nodiscard]] const ApiModule* Module(std::string_view name) const;
    [[nodiscard]] const ApiMember* Global(std::string_view name) const;
    [[nodiscard]] bool IsJassType(std::string_view type) const;
    // type 是否为 base 或其子类型（JASS handle 继承）。
    [[nodiscard]] bool Extends(std::string_view type, std::string_view base) const;
};

struct JassNativeInput {
    std::string name;
    std::vector<ApiParameter> parameters;
    std::string returns;
    std::string documentation;
};
struct JassGlobalInput {
    std::string name, type, value, documentation;
    bool constant = false;
};
struct OwnedRuntimeInput {
    std::string binding, source;
    std::vector<JassNativeInput> natives;
    std::vector<JassGlobalInput> commonGlobals; // common.j 的全局（列入 jass.common 补全）
    std::vector<JassGlobalInput> otherGlobals;  // blizzard.j 与地图全局（列入 jass.globals）
    std::vector<std::pair<std::string, std::string>> types; // 类型、父类型
};

// 编辑器自带 Lua 运行环境的接口：运行时实际开放的 Lua 标准库子集，
// 以及 jass.common / jass.globals / jass.runtime / jass.console。
[[nodiscard]] std::shared_ptr<const ApiCatalog> BuildOwnedRuntimeCatalog(OwnedRuntimeInput input);

[[nodiscard]] const std::vector<std::string_view>& LuaKeywords();

} // namespace War3Map::Lua
