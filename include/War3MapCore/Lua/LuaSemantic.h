#pragma once

#include "War3MapCore/Lua/LuaSyntax.h"
#include <map>
#include <set>

namespace War3Map::Lua {

// 由读取并核验本机运行库的服务构造；MCP/UI不接收用户提供的“已核验”标志。
struct SemanticRuntime {
    bool verified = false;
    std::string binding;
    std::string reason;
    std::set<std::string, std::less<>> presentModules;
    bool moduleRegistryComplete = false;
    std::map<std::string, std::size_t, std::less<>> commonArguments;
    // common.j只有与实际注册的动态native入口绑定后才能用于参数检查。
    bool commonLookupVerified = false;
};
struct SemanticContext {
    SemanticRuntime runtime;
    std::set<std::string, std::less<>> mapGlobals;
    bool mapGlobalsComplete = false;
    std::size_t maxInstructions = 2 * 1024 * 1024;
    std::size_t maxDiagnostics = 4096;
};
struct SemanticWarning {
    std::string code, message;
    std::uint32_t line = 0;
};
struct SemanticResult {
    SyntaxResult syntax;
    bool checked = false;
    bool complete = false;
    std::string reason;
    std::vector<SemanticWarning> warnings;
    std::set<std::string, std::less<>> declaredGlobals;
    bool globalDefinitionsComplete = true;
};

// 同次Lua 5.3文本编译产物的只读原型检查；不安装库、不执行代码。
// 分支合流、动态名称及变参调用保留unknown，不猜参数数目或模块不存在。
[[nodiscard]] SemanticResult CheckSemantics(std::string_view source, std::string_view name,
    const SemanticContext& context, const SyntaxOptions& options = {});

// 只发现编译产物中的常量全局定义，用于冻结项目全局白名单；不执行其他代码块。
[[nodiscard]] SemanticResult CollectGlobalDefinitions(std::string_view source,
    std::string_view name, const SyntaxOptions& options = {});
} // namespace War3Map::Lua
