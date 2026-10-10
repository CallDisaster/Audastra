#include "War3MapCore/Lua/LuaSemantic.h"
#include "War3MapCore/Services/CodeCheck.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Lua;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Has(const SemanticResult& result, std::string_view code) {
    return std::any_of(result.warnings.begin(), result.warnings.end(), [&](const auto& item) { return item.code == code; });
}
SemanticContext Fixture() {
    SemanticContext context;
    // 原创单元事实；不将此测试库存描述为本机运行库证明。
    context.runtime.verified = true; context.runtime.binding = "synthetic-unit-fixture";
    context.runtime.presentModules = {"jass.common", "jass.runtime"};
    context.runtime.commonLookupVerified = true;
    context.runtime.commonArguments = {{"UnitFixture", 2}, {"NoArgsFixture", 0}};
    context.mapGlobalsComplete = true; context.mapGlobals = {"MapValue"};
    return context;
}
void GlobalsAndUnused() {
    auto context = Fixture();
    const auto result = CheckSemantics("Defined = 1\nlocal used = MapValue + Defined\n"
        "local unused = 9\nprint(used, MissingGlobal)\n", "globals.lua", context);
    Require(result.syntax.Succeeded() && result.checked && !result.complete, "Static checker coverage or syntax result invalid");
    Require(result.reason.find("静态声明契约") != std::string::npos && result.reason.find("未确认实际游戏加载") != std::string::npos,
        "Static declarations were presented as actual loaded game runtime");
    Require(Has(result, "LUA_UNDEFINED_GLOBAL") && Has(result, "LUA_UNUSED_LOCAL"), "Required global/local warnings missing");
    Require(std::count_if(result.warnings.begin(), result.warnings.end(), [](const auto& warning) {
        return warning.code == "LUA_UNDEFINED_GLOBAL" && warning.line == 4 && warning.message.ends_with("MissingGlobal");
    }) == 1, "Real opcode line or global whitelist lost");
    const auto none = CheckSemantics("local captured = 1\nlocal function f() return captured end\n"
        "local x = f()\nfor k,v in pairs({}) do print(k,v,x) end\n", "capture.lua", context);
    Require(none.syntax.Succeeded() && none.warnings.empty(), "Closure capture, local function or loop registers were falsely unused");
    const auto shadow = CheckSemantics("local x = 1\ndo local x = 2; print(x) end\n", "shadow.lua", context);
    Require(std::count_if(shadow.warnings.begin(), shadow.warnings.end(), [](const auto& warning) {
        return warning.code == "LUA_UNUSED_LOCAL";
    }) == 1, "Shadowed local identity was merged");
    const auto strings = CheckSemantics("-- MissingComment()\nlocal x = [[MissingString()]]\nprint(x)\n", "string.lua", context);
    Require(strings.warnings.empty(), "Comment/string identifiers were inspected as code");
    context.mapGlobalsComplete = false;
    Require(!Has(CheckSemantics("print(MissingGlobal)\n", "unknown.lua", context), "LUA_UNDEFINED_GLOBAL"), "Incomplete map context asserted undefined global");
    const auto overridden = CheckSemantics("local _ENV = {print = print}\nprint(MissingGlobal)\n", "environment.lua", Fixture());
    Require(!Has(overridden, "LUA_UNDEFINED_GLOBAL"), "Custom local environment was treated as ordinary globals");
}
void CommonArityAndUnknown() {
    const auto context = Fixture();
    auto result = CheckSemantics("local common = require('jass.common')\ncommon.UnitFixture(1)\n", "calls.lua", context);
    Require(result.syntax.Succeeded() && Has(result, "LUA_COMMON_ARGUMENT_COUNT"), "Actual fixed call arguments not checked");
    Require(std::any_of(result.warnings.begin(), result.warnings.end(), [](const auto& value) {
        return value.code == "LUA_COMMON_ARGUMENT_COUNT" && value.line == 2;
    }), "Call opcode line lost");
    Require(!Has(CheckSemantics("local common = require('jass.common')\nlocal alias = common\n"
        "alias.UnitFixture(1,2)\ncommon.NoArgsFixture()\n", "ok.lua", context), "LUA_COMMON_ARGUMENT_COUNT"), "Correct signature rejected");
    Require(!Has(CheckSemantics("jass.common.UnitFixture(1)\n", "direct.lua", context), "LUA_COMMON_ARGUMENT_COUNT"), "Preload registration was falsely treated as global jass table proof");
    Require(!Has(CheckSemantics("local common = require('jass.common')\ncommon.UnitFixture(table.unpack({1,2}))\n", "open.lua", context),
        "LUA_COMMON_ARGUMENT_COUNT"), "Open argument count was guessed");
    Require(!Has(CheckSemantics("local common = require('jass.common')\nif MapValue then common = {} end\ncommon.UnitFixture(1)\n", "branch.lua", context),
        "LUA_COMMON_ARGUMENT_COUNT"), "Branch join retained a possibly replaced module alias");
    Require(!Has(CheckSemantics("local common = require('jass.common')\ncommon.UnitFixture = function() end\ncommon.UnitFixture()\n", "changed.lua", context),
        "LUA_COMMON_ARGUMENT_COUNT"), "Author-overridden field retained native signature");
    Require(!Has(CheckSemantics("require = function(name) return {} end\nlocal common = require('jass.common')\ncommon.UnitFixture(1)\n", "require.lua", context),
        "LUA_COMMON_ARGUMENT_COUNT"), "Author-overridden require was trusted as standard require");
    auto otherBlock = context; otherBlock.mapGlobals.insert("require");
    Require(!Has(CheckSemantics("local common=require('jass.common');common.UnitFixture(1)\n", "other-block.lua", otherBlock),
        "LUA_COMMON_ARGUMENT_COUNT"), "Frozen other block override of require was ignored");
    for (const auto* mutation : {"_G.require=function() return {} end", "package.loaded['jass.common']={UnitFixture=function() end}", "print('arbitrary callback')"}) {
        Require(!Has(CheckSemantics(std::string(mutation) + "\nlocal common=require('jass.common');common.UnitFixture(1)\n", "mutated.lua", context),
            "LUA_COMMON_ARGUMENT_COUNT"), "Standard require provenance recovered after a mutation or arbitrary callback");
    }
    auto incomplete = context; incomplete.runtime.commonLookupVerified = false;
    Require(!Has(CheckSemantics("local common=require('jass.common');common.UnitFixture(1)\n", "unverified.lua", incomplete),
        "LUA_COMMON_ARGUMENT_COUNT"), "Unverified common dynamic lookup emitted native signature warning");
}
void MissingModulesAndGates() {
    auto context = Fixture();
    Require(!Has(CheckSemantics("local x = require('jass.not_observed')\nreturn x\n", "unknown.lua", context),
        "LUA_RUNTIME_MODULE_MISSING"), "Partial registry asserted a missing module");
    context.runtime.moduleRegistryComplete = true;
    Require(Has(CheckSemantics("local x = require('jass.not_observed')\nreturn x\n", "missing.lua", context),
        "LUA_RUNTIME_MODULE_MISSING"), "Explicit complete registry absence not checked");
    Require(!Has(CheckSemantics("local x = require(MapValue)\nreturn x\n", "dynamic.lua", context),
        "LUA_RUNTIME_MODULE_MISSING"), "Dynamic require name guessed");
    context.runtime.verified = false;
    const auto syntaxOnly = CheckSemantics("local unused=1\nprint(MissingGlobal)\n", "syntax-only.lua", context);
    Require(syntaxOnly.syntax.Succeeded() && !syntaxOnly.checked && syntaxOnly.warnings.empty()
        && syntaxOnly.reason.starts_with("目前只检查语法"), "Missing trusted inventory did not remain syntax-only");
    auto longReason = Fixture(); longReason.runtime.reason = "所选来源：" + std::string(3500, 'x');
    const auto boundedReason = CheckSemantics("return 1\n", "reason.lua", longReason);
    Require(boundedReason.checked && boundedReason.reason.size() < 2048
        && boundedReason.reason.find("所选来源") != std::string::npos
        && boundedReason.reason.find("未确认实际游戏加载") != std::string::npos
        && boundedReason.reason.find(longReason.runtime.binding) != std::string::npos,
        "Reason budget discarded the source disclaimer/inventory binding or remained unbounded");
    Services::CodeCheckRequest request;
    request.language = Services::CodeLanguage::Lua53; request.source = {"body.lua", "print(MissingGlobal)\n"};
    request.context.luaSemantic = Fixture();
    const auto checked = Services::CheckCode(request);
    Require(checked.Succeeded() && checked.semanticChecked && !checked.contextComplete
        && std::any_of(checked.diagnostics.begin(), checked.diagnostics.end(), [](const auto& value) {
            return value.severity == DiagnosticSeverity::Warning && value.code == "LUA_UNDEFINED_GLOBAL";
        }), "Semantic warnings blocked save/check success or lost service severity");
    request.source.text = "local = 1\n";
    Require(!Services::CheckCode(request).Succeeded(), "Lua syntax failure no longer blocks save/check");
}
void CompileOnlyAndBudgets() {
    const auto marker = std::filesystem::temp_directory_path()
        / ("Audastra-Lua-K4-DoNotExecute-" + std::to_string(GetCurrentProcessId()) + ".txt");
    Require(!std::filesystem::exists(marker), "Refuse to overwrite existing marker");
    const auto dangerous = "os.execute([[cmd /c echo executed > \"" + marker.string()
        + "\"]])\nerror('do not run');while true do end\n";
    Require(CheckSemantics(dangerous, "never-execute.lua", Fixture()).syntax.Succeeded()
        && !std::filesystem::exists(marker), "Semantic check executed author code");
    auto quota = Fixture(); quota.maxInstructions = 1;
    const auto bounded = CheckSemantics("print(1,2,3)\n", "budget.lua", quota);
    Require(bounded.syntax.Succeeded() && !bounded.checked && bounded.warnings.empty(), "Semantic budget changed syntax/save gate");
    SyntaxOptions options; std::stop_source stopped; stopped.request_stop(); options.cancel = stopped.get_token();
    const auto cancelled = CheckSemantics("print(1)\n", "cancel.lua", Fixture(), options);
    Require(!cancelled.syntax.completed && !cancelled.checked, "Pre-cancel ignored");
    const auto definitions = CollectGlobalDefinitions("function MapFn() end\nMapConstant = 42\n", "map.lua");
    Require(definitions.syntax.Succeeded() && definitions.globalDefinitionsComplete
        && definitions.declaredGlobals.contains("MapFn") && definitions.declaredGlobals.contains("MapConstant"), "Real compiled global writes not collected");
    std::string nested;
    for (int i = 0; i < 64; ++i) nested += "return function() ";
    nested += "return 1 ";
    for (int i = 0; i < 64; ++i) nested += "end ";
    Require(CheckSemantics(nested, "nested.lua", Fixture()).syntax.Succeeded(), "Legal nested prototypes exhausted semantic worker stack");
    std::string large = "--[[" + std::string(8 * 1024 * 1024 - 64, 'x') + "]]\nlocal used=1;return used\n";
    const auto begin = std::chrono::steady_clock::now();
    const auto parsed = CheckSemantics(large, "large.lua", Fixture());
    Require(parsed.syntax.Succeeded() && parsed.checked && parsed.warnings.empty(), "8MiB text did not compile/analyze");
    std::cout << "8 MiB compile/analyze milliseconds: " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count() << '\n';
}
} // namespace
int main() {
    try { GlobalsAndUnused(); CommonArityAndUnknown(); MissingModulesAndGates(); CompileOnlyAndBudgets();
        std::cout << "Lua K4 static prototype warnings, coverage and compile-only gates passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
