#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"

#include <span>
#include <memory>

namespace War3Map::Script {

class PreparedLuaAuthorCode;

// 由目标运行时适配器提供；不能仅因 UI 中存在同名定义而推断 Lua native 可用。
struct LuaApiSignature {
    std::string name;
    std::string returns;
    std::vector<std::string> parameters;
};

// 仅绑定目标运行时已提供的具名值，不接受待执行表达式或从UI名称猜测。
struct LuaEngineConstant { std::string name, type; };
// 宿主已验证的生成对象符号；与用户变量、函数和helper名称分别校验冲突。
struct LuaGlobalBinding { std::string name, type; };

struct LuaTargetProfile {
    std::string identity;
    std::vector<LuaApiSignature> apis;
    // 名称 -> 父类型；handle 是内建根，拒绝循环/未知父类型。
    std::vector<std::pair<std::string, std::string>> handleTypes;
    std::vector<LuaEngineConstant> constants;
    // 默认仍是经典 GUI→Lua 迁移。只有实际原生 Lua 源捕获的 seal 可放行 WCT 作者代码。
    std::shared_ptr<const PreparedLuaAuthorCode> nativeAuthorCode;
};

// 将明确的全局回调名绑定到一个 GUI 触发器的动作；不解析任意代码文本。
struct LuaCallbackBinding {
    std::string symbol;
    StableId triggerId = 0;
};

struct LuaTriggerSymbol {
    StableId triggerId = 0;
    std::string variable;
    std::string conditions;
    std::string actions;
    std::string initializer;
};

struct LuaSourceDiagnostic {
    Diagnostic diagnostic;
    StableId triggerId = 0;
    StableId nodeId = 0;
};

enum class LuaSourceSpanKind : std::uint8_t { Variable, Trigger, Event, Statement, Callback, AuthorHeader, AuthorCustom };

struct LuaSourceSpan {
    // 实际生成文本中的字节半开区间；不是 UTF-8 字符列。嵌套范围允许重叠。
    std::size_t byteOffset = 0, byteLength = 0;
    StableId triggerId = 0, nodeId = 0;
    LuaSourceSpanKind kind = LuaSourceSpanKind::Trigger;
    [[nodiscard]] bool operator==(const LuaSourceSpan&) const noexcept = default;
};

// 只用于编辑器自己启动的 Lua 单人测试；正式生成的默认选项不改变任何文本。
struct LuaTriggerBuildOptions { bool testHotReload = false; };

struct LuaTestTriggerChunk {
    LuaTriggerSymbol symbol;
    std::string functions, initializer, events;
    bool initialization = false;
    bool customCode = false;
    std::size_t functionsOffset = 0, initializerOffset = 0;
    // 测试转发前的作者原文；重载时重新分析并核对实际 functions，不能单独充当可信资格。
    std::string authorSource;
};

struct LuaTestGlobalChunk {
    StableId variableId = 0;
    std::string name, declaration, initialization;
    std::string shape; // 类型、数组范围与默认值；既有变量不能借热重载改变结构。
    std::size_t declarationOffset = 0, initializationOffset = 0;
};

struct LuaTriggerUnit {
    // 编译失败时为空；不是完整 war3map.lua（不含 config/main 或预放对象初始化）。
    std::string text;
    std::string targetIdentity;
    std::string targetSha256;
    std::string scriptSha256;
    std::string sourceMapSha256;
    std::string uiCatalogSha256;
    // 来源 seal 的基线指纹，不冒称包含本次尚未保存的 GUI 修改。
    std::string sourceWtgSha256;
    std::string sourceWctSha256;
    std::vector<LuaTriggerSymbol> symbols;
    std::vector<std::string> requiredApis;
    std::vector<std::string> requiredGlobals;
    std::vector<std::string> requiredConstants;
    std::vector<LuaSourceDiagnostic> diagnostics;
    // 按起点、外层到内层稳定排序；仅 AuthorHeader 使用全局代码块的零身份，helper 没有映射。
    // 表达式及匿名回调内部定位到外部语句/触发器，不声称是逐字符反编译映射。失败时为空。
    std::vector<LuaSourceSpan> sourceSpans;
    bool testHotReload = false;
    std::string testSharedSource;
    std::vector<LuaTestTriggerChunk> testTriggers;
    std::vector<LuaTestGlobalChunk> testGlobals;
    [[nodiscard]] bool Succeeded() const noexcept;
};

// 纯函数：不改文档、不执行代码、不改地图语言/保存状态。经典整数除法向零截断；
// 不支持的语义阻断整个编译单元，并保留源身份，禁止返回部分脚本当成成功。
[[nodiscard]] LuaTriggerUnit GenerateLuaTriggerUnit(const Trigger::TriggerDocument& document,
    const Trigger::UiCatalog& catalog, const LuaTargetProfile& target,
    std::span<const LuaCallbackBinding> callbacks = {},
    std::span<const LuaGlobalBinding> globals = {}, LuaTriggerBuildOptions options = {});

} // namespace War3Map::Script
