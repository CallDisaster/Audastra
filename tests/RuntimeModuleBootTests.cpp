#include "War3MapCore/Script/MapRuntimeBoot.h"
#include "War3MapCore/Hash.h"
#include <stdexcept>

using namespace War3Map::Script;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string Source(const ScriptProfileModuleLoader& loader) {
    return "globals\nstring AjExpectedRuntimeIdentity=\"\"\nboolean AjRuntimeModuleExpected=false\nendglobals\nfunction " +
        loader.functionName + " takes string path, boolean anonymous returns boolean\nreturn true\nendfunction\n"
        "function original_init takes nothing returns nothing\ncall StartCampaignAI(Player(12),\"original.blob\")\nendfunction\n"
        "function AjStartRuntimeModule takes nothing returns nothing\nendfunction\n"
        "function AjStartTestDiagnostics takes nothing returns nothing\nendfunction\n"
        "function main takes nothing returns nothing\n    call original_init()\n    call Marker()\nendfunction\n";
}
}
void RuntimeModuleBootTests() {
    const auto loader = DefaultScriptModuleLoaders().at(0);
    const auto original = Source(loader);
    MapRuntimeBootPlan plan; plan.modulePayloadSha256 = std::string(64, 'c');
    unsigned ai = 0, module = 0;
    plan.resolvePayloadIdentity = [&]() { ++ai; return War3Map::Result<std::string>::Success(std::string(64, 'a')); };
    plan.resolveModulePayloadIdentity = [&](const auto& selected) {
        ++module; Require(selected == loader, "selected exact profile loader");
        return War3Map::Result<std::string>::Success(plan.modulePayloadSha256);
    };
    auto installed = ApplyMapRuntimeBoot(original, plan);
    if (!installed.Succeeded()) throw std::runtime_error(installed.diagnostics.front().message);
    Require(ai == 0 && module == 1, "module path captures only selected product");
    Require(VerifyMapRuntimeBoot(*installed.value, true, loader).Succeeded(), "module final contract");
    Require(VerifyMapRuntimeIdentity(*installed.value, plan.modulePayloadSha256).Succeeded(), "module content identity");
    Require(!VerifyMapRuntimeBoot(*installed.value, true).Succeeded(), "module cannot be accepted under AI ownership");
    Require(installed.value->find("warvk_loader_raw.ai") == std::string::npos, "no second AI start");
    const auto initBegin = original.find("function original_init"), initEnd = original.find("endfunction", initBegin) + 11;
    const auto init = original.substr(initBegin, initEnd - initBegin);
    Require(installed.value->find(init) != std::string::npos, "original initialization byte-identical");
    const auto body = original.substr(original.find("    call original_init"), std::string("    call original_init()\n    call Marker()\n").size());
    Require(installed.value->find(body) != std::string::npos, "original main calls keep exact order and parameters");
    Require(!ApplyMapRuntimeBoot(*installed.value, plan).Succeeded(), "unowned shim not adopted");
    plan.ownsExisting = true; plan.ownedModuleLoader = loader;
    const auto repeated = ApplyMapRuntimeBoot(*installed.value, plan);
    Require(repeated.Succeeded() && repeated.value == installed.value, "module repeated save idempotent");
    plan.testRunDiagnostics = true;
    auto test = ApplyMapRuntimeBoot(*installed.value, plan);
    Require(test.Succeeded() && VerifyMapRuntimeTestDiagnostics(*test.value, true).Succeeded(), "test-only tail diagnostic");
    Require(ApplyMapRuntimeBoot(*test.value, plan).value == test.value, "test module repeated save idempotent");
    plan.enabled = false;
    auto removed = ApplyMapRuntimeBoot(*test.value, plan);
    auto expectedRemoved = original;
    expectedRemoved.replace(expectedRemoved.find("AjExpectedRuntimeIdentity=\"\"") + std::string("AjExpectedRuntimeIdentity=").size(), 2,
        "\"" + plan.modulePayloadSha256 + "\"");
    Require(removed.Succeeded() && removed.value == expectedRemoved, "disable preserves all original initialization and resets carrier flag");
    plan.enabled = true; plan.ownedModuleLoader.reset(); plan.ownsExisting = false;
    for (const auto change : {std::string("boolean anonymous"), std::string("returns boolean")}) {
        auto wrong = original; wrong.replace(wrong.find(change), change.size(), "integer wrong");
        Require(!ApplyMapRuntimeBoot(wrong, plan).Succeeded(), "wrong loader signature refused");
    }
    auto duplicate = original + "native " + loader.functionName + " takes string p, boolean a returns boolean\n";
    Require(!ApplyMapRuntimeBoot(duplicate, plan).Succeeded(), "duplicate declaration refused");
    auto early = original; early.insert(early.find("    call Marker"), "    return\n");
    Require(!ApplyMapRuntimeBoot(early, plan).Succeeded(), "early main return has no proven completion hook");
    auto oldClient = original; oldClient.erase(oldClient.find("boolean AjRuntimeModuleExpected"), std::string("boolean AjRuntimeModuleExpected=false\n").size());
    Require(!ApplyMapRuntimeBoot(oldClient, plan).Succeeded(), "old client cannot enter unbounded handshake race");
    auto mutableFlag = original + "function mutable takes nothing returns nothing\nset AjRuntimeModuleExpected=false\nendfunction\n";
    Require(!ApplyMapRuntimeBoot(mutableFlag, plan).Succeeded(), "mutable policy rejected");
    plan.moduleLoaders.clear();
    Require(!ApplyMapRuntimeBoot(original, plan).Succeeded(), "function alone without profile declaration gives no qualification");
    plan.moduleLoaders = {loader}; plan.ownsExisting = true; plan.ownedModuleLoader = loader;
    auto changed = *installed.value;
    changed.insert(changed.find("endfunction", changed.find("call AjStartRuntimeModule")), "    call Another()\n");
    Require(!ApplyMapRuntimeBoot(changed, plan).Succeeded(), "moved tail not silently repaired");
    auto shimChanged = *installed.value;
    shimChanged.replace(shimChanged.find("warvk_runtime_module.dll"), std::string("warvk_runtime_module.dll").size(), "different.dll");
    Require(!ApplyMapRuntimeBoot(shimChanged, plan).Succeeded(), "modified shim not deleted");
    auto commented = *installed.value;
    commented.insert(commented.find("function AudastraRuntimeModuleLoad") + std::string("function AudastraRuntimeModuleLoad takes nothing returns nothing").size(), " // author note");
    Require(!ApplyMapRuntimeBoot(commented, plan).Succeeded(), "author comments in owned helper are not silently erased");
    auto single = original;
    const std::string otherCall = "call StartCampaignAI(Player(12),\"original.blob\")\n";
    single.erase(single.find(otherCall), otherCall.size());
    MapRuntimeBootPlan aiPlan; aiPlan.payloadSha256 = std::string(64, 'a'); aiPlan.testRunDiagnostics = true;
    const auto aiBuilt = ApplyMapRuntimeBoot(single, aiPlan);
    Require(aiBuilt.Succeeded(), "legacy AI test artifact installs");
    auto nowMixed = *aiBuilt.value;
    nowMixed.insert(nowMixed.find("endfunction", nowMixed.find("function original_init")), otherCall);
    aiPlan.ownsExisting = true; aiPlan.modulePayloadSha256 = std::string(64, 'c');
    const auto upgraded = ApplyMapRuntimeBoot(nowMixed, aiPlan);
    Require(upgraded.Succeeded() && VerifyMapRuntimeBoot(*upgraded.value, true, loader).Succeeded() &&
        VerifyMapRuntimeTestDiagnostics(*upgraded.value, true).Succeeded(), "AI to module migration relocates diagnostic without preserving stale AI call");
    auto nowSingle = *upgraded.value; nowSingle.erase(nowSingle.find(otherCall), otherCall.size());
    aiPlan.ownedModuleLoader = loader;
    const auto downgraded = ApplyMapRuntimeBoot(nowSingle, aiPlan);
    Require(downgraded.Succeeded() && VerifyMapRuntimeBoot(*downgraded.value, true).Succeeded() &&
        VerifyMapRuntimeTestDiagnostics(*downgraded.value, true).Succeeded(), "module to AI migration cleans owned helper and resets SDK carrier");
    auto wrongIdentity = loader; wrongIdentity.anonymous = !loader.anonymous;
    Require(ScriptModuleLoaderIdentity(loader).value != ScriptModuleLoaderIdentity(wrongIdentity).value, "loader availability and options bound into identity");
    wrongIdentity.functionName = "unsafe()";
    Require(!ScriptModuleLoaderIdentity(wrongIdentity).Succeeded(), "profile cannot inject arbitrary source");
    ScriptToolchainProfile profile; profile.profileId = "module-fixture";
    profile.toolRoot = profile.uiRoot = profile.mainSourceDirectory = "C:/synthetic-toolchain";
    const auto oldProfile = ValidateScriptToolchainProfile(profile);
    profile.moduleLoaders = {loader};
    const auto newProfile = ValidateScriptToolchainProfile(profile);
    Require(oldProfile.Succeeded() && newProfile.Succeeded() && oldProfile.value != newProfile.value, "declarations bind complete profile identity");
    profile.moduleLoaders.push_back(loader);
    Require(!ValidateScriptToolchainProfile(profile).Succeeded(), "duplicate loader identities rejected before profile capture");
}
