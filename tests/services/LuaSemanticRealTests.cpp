#include "War3MapCore/Runtime/RuntimeLuaApiInventory.h"
#include "War3MapCore/Services/CodeCheckProject.h"

#include <Windows.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Has(const War3Map::Services::CodeCheckResult& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& warning) {
        return warning.code == code && warning.severity == War3Map::DiagnosticSeverity::Warning;
    });
}
}
int main() {
    using namespace War3Map;
    const auto* root = _wgetenv(L"FORGE_YDWE_ROOT");
    if (!root || !*root) {
        std::cout << "SKIP: set FORGE_YDWE_ROOT to read-only local runtime installation\n"; return 77;
    }
    try {
        const auto inventory = Runtime::CaptureRuntimeLuaApiInventory(std::filesystem::path(root));
        Require(inventory && inventory->trusted && !inventory->binding.empty(), "Configured actual runtime did not form trusted static registration inventory");
        const auto common = std::find_if(inventory->modules.begin(), inventory->modules.end(), [](const auto& module) {
            return module.name == "jass.common" && module.commonLookup;
        });
        Require(common != inventory->modules.end(), "Actual common dynamic lookup not statically validated");
        const auto native = std::find_if(common->fields.begin(), common->fields.end(), [](const auto& field) {
            return field.native && field.argumentCount && *field.argumentCount > 0;
        });
        Require(native != common->fields.end(), "No actual common.j fixed native signature available");
        auto snapshot = std::make_shared<Services::CodeCheckProjectSnapshot>();
        snapshot->binding = "original-unit-draft+actual-runtime-" + inventory->binding;
        // F9的运行库来源与JASS保存工具/UI层独立；无效toolRoot不能干扰所选Tree。
        snapshot->toolRoot = std::filesystem::path(root) / "bin/lua53.dll/not-a-directory";
        snapshot->luaRuntimeYdweRoot = std::filesystem::path(root) / "ui";
        Services::CodeCheckProjectCache cache;
        const auto result = Services::CheckProjectCode(snapshot, cache,
            {Trigger::CodeBlockKind::GlobalHeader, 0}, "local common = require('jass.common')\ncommon."
                + native->name + "()\nlocal unused = 1\n", Services::CodeLanguage::Lua53, 7);
        Require(result.Succeeded() && result.syntaxChecked && result.semanticChecked && !result.contextComplete,
            "Actual runtime project check lost warning-only gate or overstated coverage");
        Require(Has(result, "LUA_COMMON_ARGUMENT_COUNT") && Has(result, "LUA_UNUSED_LOCAL"),
            "Actual configured runtime did not produce common arity and unused-local warnings");
        Require(result.contextBinding == snapshot->binding && result.generation == 7, "Actual runtime warnings lost frozen draft identity");
        Require(result.reason.find("F9配置") != std::string::npos && result.reason.find("静态声明契约") != std::string::npos
            && result.reason.find("未确认实际游戏加载") != std::string::npos, "Static source scope disclaimer or selected configuration was lost");
        Require(result.reason.find(inventory->binding) != std::string::npos, "Project result lost this capture's complete seven-source inventory binding");
        Services::CodeCheckRequest text;
        text.language = Services::CodeLanguage::Lua53;
        text.source = {"original-independent.lua", "local common=require('jass.common');common." + native->name + "()\nlocal unused=1\n"};
        // 独立文本宿主不能通过伪造context事实代替本机读取。
        text.context.luaSemantic.runtime.verified = true;
        text.context.luaSemantic.runtime.binding = "forged-caller-inventory";
        const auto raw = Services::CheckProjectLuaSource(snapshot, std::move(text));
        Require(raw.Succeeded() && raw.semanticChecked && Has(raw, "LUA_COMMON_ARGUMENT_COUNT") && Has(raw, "LUA_UNUSED_LOCAL"),
            "Raw-text MCP shared entry did not use the same real runtime warnings");
        Require(raw.reason.find(inventory->binding) != std::string::npos, "Independent source result lost this capture's actual inventory identity");
        Services::CodeCheckRequest syntax;
        syntax.source = {"original-precedence.lua", "local common=require('jass.common');common." + native->name + "()\n"};
        auto flat = std::make_shared<Services::CodeCheckProjectSnapshot>(*snapshot);
        flat->luaRuntimeRoot = root; // 即使此目录恰有Tree，显式配置仍按F9的Flat语义，不自动猜布局。
        const auto flatResult = Services::CheckProjectLuaSource(flat, syntax);
        Require(flatResult.Succeeded() && !flatResult.semanticChecked && flatResult.reason.find("Flat") != std::string::npos,
            "Explicit F9 Flat configuration was silently replaced by valid Tree fallback");
        auto wrongTree = std::make_shared<Services::CodeCheckProjectSnapshot>(*snapshot);
        wrongTree->toolRoot = root;
        wrongTree->luaRuntimeYdweRoot = std::filesystem::path(root) / "bin/lua53.dll/not-a-directory";
        const auto wrongTreeResult = Services::CheckProjectLuaSource(wrongTree, syntax);
        Require(wrongTreeResult.Succeeded() && !wrongTreeResult.semanticChecked,
            "Invalid F9 Tree source fell back to a different valid JASS toolRoot");
        auto relativeTree = std::make_shared<Services::CodeCheckProjectSnapshot>(*snapshot);
        relativeTree->luaRuntimeYdweRoot = "relative-runtime/ui";
        const auto relativeResult = Services::CheckProjectLuaSource(relativeTree, syntax);
        Require(relativeResult.Succeeded() && !relativeResult.semanticChecked
            && relativeResult.reason.find("不是绝对路径") != std::string::npos,
            "Relative runtime configuration acquired identity from the process working directory");
        std::cout << "F9 source precedence: normalized Tree positive; explicit Flat and mismatched JASS root remain syntax-only\n";
        std::cout << "Actual read-only static inventory binding: " << inventory->binding
            << "\nActual common native arity checked: " << native->name << " / " << *native->argumentCount
            << "\nSources read: " << inventory->sources.size() << "\nNo DLL loaded, no map executed, no graphical process started\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
