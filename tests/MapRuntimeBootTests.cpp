#include "War3MapCore/Script/MapRuntimeBoot.h"
#include <iostream>
#include <string>
#include <stdexcept>
using namespace War3Map::Script;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
constexpr auto original = "function preserved_boot takes nothing returns nothing\ncall StartCampaignAI(Player(PLAYER_NEUTRAL_PASSIVE), \"control.ai\")\nendfunction\n"
    "function main takes nothing returns nothing\n    call preserved_boot()\n    call SetCameraBounds(0,0,0,0,0,0,0,0)\nendfunction\n";
void Boot() {
    auto installed = ApplyMapRuntimeBoot(original, {});
    Require(installed.Succeeded(), "install early boot while preserving other early initialization");
    Require(VerifyMapRuntimeBoot(*installed.value, true).Succeeded(), "single first executable statement");
    Require(installed.value->find("call preserved_boot()") != std::string::npos, "other initialization retained");
    Require(!ApplyMapRuntimeBoot(*installed.value, {}).Succeeded(), "manual or unowned boot is not adopted");
    MapRuntimeBootPlan owned; owned.ownsExisting = true;
    auto repeated = ApplyMapRuntimeBoot(*installed.value, owned);
    Require(repeated.Succeeded() && repeated.value == installed.value, "owned repeated save is idempotent");
    owned.enabled = false;
    auto removed = ApplyMapRuntimeBoot(*installed.value, owned);
    Require(removed.Succeeded() && VerifyMapRuntimeBoot(*removed.value, false).Succeeded(), "owned boot removal");
    Require(removed.value->find("call preserved_boot()") != std::string::npos, "unrelated AI survives removal");
    for (const auto player : {"PLAYER_NEUTRAL_AGGRESSIVE", "12", "somePlayerId"}) {
        const auto conflict = "function other_boot takes nothing returns nothing\ncall StartCampaignAI(Player(" + std::string(player)
            + "), \"other.ai\")\nendfunction\nfunction main takes nothing returns nothing\ncall other_boot()\nendfunction\n";
        const auto rejected = ApplyMapRuntimeBoot(conflict, {});
        Require(!rejected.Succeeded() && !rejected.value && !rejected.diagnostics.empty()
            && rejected.diagnostics.front().code == "RUNTIME_COEXISTENCE_UNVERIFIED", "unverified same-player sequence is never silently accepted");
    }
    const auto melee = "function main takes nothing returns nothing\ncall StartMeleeAI(Player(12), \"other.ai\")\nendfunction\n";
    Require(!ApplyMapRuntimeBoot(melee, {}).Succeeded(), "melee AI competition also requires a validated sequence");
    Require(ApplyMapRuntimeBoot("function main takes nothing returns nothing\ncall StartMeleeAI(Player(0), \"other.ai\")\nendfunction\n", {}).Succeeded(),
        "known different-player AI remains intact");
    owned.enabled = true; owned.onlyIfUsed = true;
    auto automaticRemoval = ApplyMapRuntimeBoot(*installed.value, owned);
    Require(automaticRemoval.Succeeded() && VerifyMapRuntimeBoot(*automaticRemoval.value, false).Succeeded(),
        "automatic mode removes previously managed boot when dependency disappears");
    const auto dynamic = *installed.value + "function dynamic_call takes string target returns nothing\ncall ExecuteFunc(target)\nendfunction\n";
    Require(AnalyzeMapRuntimeUsage(dynamic).value->unresolvedDynamicCalls && !ApplyMapRuntimeBoot(dynamic, owned).Succeeded(),
        "dynamic coverage cannot be mistaken for evidence that an existing runtime can be removed");
    const auto comments = "// call AudastraUIAvailable()\nfunction main takes nothing returns nothing\nlocal string label\nset label = \"AjInitialize()\"\nendfunction\n";
    Require(UsesMapRuntime(comments).Succeeded() && !*UsesMapRuntime(comments).value, "comments and strings are not dependencies");
    Require(*UsesMapRuntime("call AudastraUIAvailable()\n").value, "real UI call is a dependency");
    Require(!*UsesMapRuntime("function AudastraUIAvailable takes nothing returns boolean\nreturn true\nendfunction\n").value,
        "function declaration alone is not a call or callback reference");
    for (const auto name : {"AjRequest", "AjModuleUnload", "AjPump", "AjOn", "AjOff", "AjBindEvents"})
        Require(*UsesMapRuntime("call " + std::string(name) + "()\n").value, "all transport-dependent public entry points detected");
    Require(*UsesMapRuntime("call ExecuteFunc(\"AjInitialize\")\n").value, "literal dynamic entry point detected");
    Require(*UsesMapRuntime("call TimerStart(t,1.0,false,function AjDispatch)\n").value, "real callback reference detected");
    MapRuntimeBootPlan automatic; automatic.onlyIfUsed = true;
    Require(ApplyMapRuntimeBoot(original, automatic).value == original, "unused map stays unchanged");
    Require(!ApplyMapRuntimeBoot("function main takes nothing returns nothing\nlocal integer x = GetRandomInt(1,2)\nendfunction\n", {}).Succeeded(), "local executable initialization not reordered");
    Require(!ApplyMapRuntimeBoot("// no main\n", {}).Succeeded(), "missing main rejected");
    auto doubled = *installed.value;
    doubled.insert(doubled.find("    call preserved_boot"), "    call StartCampaignAI(Player(PLAYER_NEUTRAL_AGGRESSIVE), \"warvk_loader_raw.ai\")\n");
    Require(!ApplyMapRuntimeBoot(doubled, owned).Succeeded(), "duplicate boot rejected");
    Require(!VerifyMapRuntimeBoot(original, true).Succeeded(), "missing final boot rejected");
}
void Identity() {
    const std::string source = "globals\nstring AjExpectedRuntimeIdentity = \"\"\nendglobals\n" + std::string(original);
    MapRuntimeBootPlan plan; plan.payloadSha256 = std::string(64, 'a');
    auto result = ApplyMapRuntimeBoot(source, plan);
    Require(result.Succeeded() && result.value->find(plan.payloadSha256) != std::string::npos, "bind verified runtime identity before type check");
    Require(VerifyMapRuntimeIdentity(*result.value, plan.payloadSha256).Succeeded(), "read-only identity final check");
    Require(!VerifyMapRuntimeIdentity(*result.value, std::string(64, 'b')).Succeeded(), "reuse cannot silently accept wrong payload identity");
    for (const std::string invalid : {
        std::string(original) + "function f takes nothing returns nothing\nlocal string AjExpectedRuntimeIdentity = \"\"\nendfunction\n",
        source + "function f takes nothing returns nothing\nset AjExpectedRuntimeIdentity = \"\"\nendfunction\n",
        source + "function f takes string AjExpectedRuntimeIdentity returns nothing\nendfunction\n",
        "globals\nstring AjExpectedRuntimeIdentity = \"\" + \"\"\nendglobals\n" + std::string(original),
        "globals\nstring AjExpectedRuntimeIdentity = \"\"\nstring AjExpectedRuntimeIdentity = \"\"\nendglobals\n" + std::string(original)}) {
        Require(!ApplyMapRuntimeBoot(invalid, plan).Succeeded(), "ambiguous or runtime-mutated identity rejected");
        Require(!VerifyMapRuntimeIdentity(invalid, plan.payloadSha256).Succeeded(), "reuse runs same strict binding check");
    }
    Require(!ApplyMapRuntimeBoot(original, plan).Succeeded(), "old SDK cannot bypass identity binding");
    unsigned checks = 0;
    plan.verifyScriptInputs = [&](std::span<const std::string> hashes) {
        ++checks;
        if (hashes.size() == 1 && hashes.front() == std::string(64, 'c')) return War3Map::Status::Ok();
        return War3Map::Status::Failure("fixture", "SCRIPT_IDENTITY_REJECTED", "synthetic source mismatch");
    };
    Require(!ApplyMapRuntimeBoot(source, plan).Succeeded() && checks == 1, "unproven injection is refused before boot publication");
    plan.injectedSourceSha256 = {std::string(64, 'c')};
    Require(ApplyMapRuntimeBoot(source, plan).Succeeded() && checks == 2, "matched consumed injection proceeds to identity binding");
    plan.payloadSha256 = "not-a-hash";
    Require(!ApplyMapRuntimeBoot(source, plan).Succeeded(), "invalid identity rejected");
}
void TestDiagnostics() {
    const std::string source = "function AjStartTestDiagnostics takes nothing returns nothing\nendfunction\n" + std::string(original);
    MapRuntimeBootPlan plan; plan.testRunDiagnostics = true;
    const auto installed = ApplyMapRuntimeBoot(source, plan);
    Require(installed.Succeeded() && VerifyMapRuntimeTestDiagnostics(*installed.value, true).Succeeded(), "test copy schedules a single diagnostic after the first AI boot");
    Require(!VerifyMapRuntimeTestDiagnostics(*installed.value, false).Succeeded(), "formal final check refuses a test call");
    Require(!ApplyMapRuntimeBoot(*installed.value, plan).Succeeded(), "manual diagnostic and boot not adopted");
    plan.ownsExisting = true;
    Require(ApplyMapRuntimeBoot(*installed.value, plan).value == installed.value, "test save is idempotent");
    plan.testRunDiagnostics = false;
    const auto formal = ApplyMapRuntimeBoot(*installed.value, plan);
    Require(formal.Succeeded() && VerifyMapRuntimeTestDiagnostics(*formal.value, false).Succeeded(), "normal save removes test scheduling");
    plan.testRunDiagnostics = true;
    Require(ApplyMapRuntimeBoot(*formal.value, plan).Succeeded(), "formal to test switching restores one call");
    plan.enabled = false;
    const auto disabled = ApplyMapRuntimeBoot(*installed.value, plan);
    Require(disabled.Succeeded() && VerifyMapRuntimeBoot(*disabled.value, false).Succeeded()
        && VerifyMapRuntimeTestDiagnostics(*disabled.value, false).Succeeded(), "uninstall removes both boot and test call");
    plan.enabled = true; plan.ownsExisting = false;
    Require(!ApplyMapRuntimeBoot(original, plan).Succeeded(), "old SDK without diagnostics is refused for TestRun");
    auto moved = *installed.value;
    const auto begin = moved.find("call AjStartTestDiagnostics()");
    moved.erase(begin, std::string("call AjStartTestDiagnostics()").size());
    moved.insert(moved.rfind("endfunction"), "call AjStartTestDiagnostics()\n");
    plan.ownsExisting = true;
    Require(!ApplyMapRuntimeBoot(moved, plan).Succeeded(), "moved diagnostic is not silently reordered");
    auto duplicated = *installed.value;
    duplicated.insert(duplicated.rfind("endfunction"), "call AjStartTestDiagnostics()\n");
    Require(!ApplyMapRuntimeBoot(duplicated, plan).Succeeded(), "duplicate diagnostic is not silently deleted");
}
void LuaUsage() {
    const auto analyze = [](std::string_view source) { return AnalyzeMapRuntimeUsage(source, MapRuntimeScriptLanguage::Lua); };
    const auto plain = analyze("-- AjInitialize()\nlocal t = {text = [[AudastraUIAvailable()]]}\nfunction main() print(t.text) end\n");
    Require(plain.Succeeded() && !plain.value->required && !plain.value->unresolvedDynamicCalls,
        "ordinary Lua tables, strings and comments do not use the native runtime");
    for (const auto source : {"AjInitialize()", "jass.AudastraUIAvailable()", "local callback = AjPump",
        "ExecuteFunc('AjInitialize')", "jass['AjModuleLoad']()"}) {
        Require(analyze(source).value->required, "real Lua entry and callback references require runtime and will be rejected by save policy");
    }
    Require(analyze("_G[ [=[AjInitialize]=] ]()").value->required, "long-string global lookup detected");
    Require(analyze("ExecuteFunc(target)").value->unresolvedDynamicCalls, "dynamic Lua entry remains incomplete");
    Require(analyze("_G['\\065jInitialize']()").value->unresolvedDynamicCalls, "unsupported escape decoding is not absence proof");
    Require(!analyze("--[[unfinished").Succeeded() && !analyze("local x='unfinished").Succeeded(), "incomplete Lua tokens do not prove no dependencies");
    Require(!analyze(std::string(16 * 1024 * 1024 + 1, ' ')).Succeeded(), "Lua source analysis is bounded");
}
}
void LargeScript() {
    // Real maps exceed one million tokens well inside the 16 MiB script limit (about two million here).
    std::string body;
    body.reserve(12 * 1024 * 1024);
    for (int i = 0; i < 100000; ++i)
        body += "set udg_counter=udg_counter+1\nset udg_flag=not udg_flag\nset udg_value=udg_value*2\n";
    const std::string script = "globals\ninteger udg_counter=0\nboolean udg_flag=false\ninteger udg_value=1\nendglobals\n"
        "function filler takes nothing returns nothing\n" + body + "endfunction\n" + std::string(original);
    auto installed = ApplyMapRuntimeBoot(script, {});
    Require(installed.Succeeded(), "large real-map script is not refused by a lexer budget below the script size limit");
    Require(VerifyMapRuntimeBoot(*installed.value, true).Succeeded(), "large installed script verifies");
}
int main() {
    try { Boot(); Identity(); TestDiagnostics(); LuaUsage(); LargeScript(); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "map runtime boot tests passed\n";
}
