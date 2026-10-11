#include "War3MapCore/Services/MapTestService.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Services/MapTestWorkspace.h"
#include "War3MapCore/VerifiedFile.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace War3Map;
using namespace War3Map::Services;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Write(const std::filesystem::path& path, std::string_view text = "fixture") {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary); output << text; Require(static_cast<bool>(output), "fixture write");
}
template<class T> bool Code(const Result<T>& result, std::string_view code) {
    for (const auto& diagnostic : result.diagnostics) if (diagnostic.code == code) return true;
    return false;
}
bool Code(const Status& result, std::string_view code) {
    for (const auto& diagnostic : result.diagnostics) if (diagnostic.code == code) return true;
    return false;
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("forge-map-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path source = root / "author.w3x", game = root / "game";
    std::string sha;
    Fixture() {
        Write(source, "immutable-map-input"); sha = *Sha256File(source).value;
        for (const auto* name : {"war3.exe", "Game.dll", "war3.mpq", "war3patch.mpq", "storm.dll"}) Write(game / name);
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    MapTestRequest Request(std::string_view name) { return {source, sha, source, game, root / name, false}; }
};
void RequireShortGameCopy(const MapTestLaunchPlan& plan, const Fixture& fixture, std::wstring_view extension) {
    Require(std::filesystem::equivalent(plan.workingDirectory, fixture.game), "working directory is selected game root");
    const auto relative = plan.gameMapCopy.lexically_relative(plan.workingDirectory);
    Require(!relative.empty() && !relative.is_absolute(), "game copy stays beneath selected game root");
    auto component = relative.begin();
    Require(component != relative.end() && *component++ == "Maps", "short copy starts beneath Maps");
    Require(component != relative.end() && *component++ == "Audastra", "short copy belongs to Audastra");
    Require(component != relative.end(), "short copy has a unique task directory");
    const auto token = component++->wstring();
    Require(token.size() == 16 && std::all_of(token.begin(), token.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
    }), "short copy task directory is exactly sixteen hexadecimal characters");
    Require(component != relative.end() && component++->wstring() == L"test" + std::wstring(extension)
        && component == relative.end(), "short copy preserves w3x or w3m extension and has no extra components");
    Require(!std::filesystem::exists(plan.gameMapCopy), "Core plan declares a new copy without writing it or launching game");
}
void CopyAndPlanKeepSource() {
    Fixture f; auto request = f.Request("prepared");
    const auto artifact = PrepareMapTest(request);
    Require(artifact.Succeeded(), "unchanged saved map must produce independent test copy");
    Require(artifact.value->testMap != f.source && artifact.value->testMapSha256 == f.sha, "independent copy byte identity");
    Require(*Sha256File(f.source).value == f.sha, "author source unchanged");
    const auto plan = BuildMapTestLaunchPlan(*artifact.value);
    Require(plan.Succeeded(), "native launch plan needs no YDWE directory or environment probe");
    Require(std::filesystem::equivalent(plan.value->executable, f.game / "war3.exe") && plan.value->arguments.size() == 3
        && plan.value->arguments[0] == L"-window" && plan.value->arguments[1] == L"-loadfile",
        "native plan directly launches selected Warcraft in windowed mode");
    const auto loadfile = std::filesystem::path(plan.value->arguments[2]);
    Require(!loadfile.is_absolute() && plan.value->workingDirectory / loadfile == plan.value->gameMapCopy,
        "loadfile argument uses the short game-relative copy rather than a long workspace path");
    Require(std::filesystem::equivalent(plan.value->workingDirectory, f.game) && plan.value->testMap == artifact.value->testMap,
        "native plan binds game working directory and independent frozen input");
    Require(plan.value->executableSha256 == *Sha256File(f.game / "war3.exe").value
        && plan.value->gameExecutableSha256 == plan.value->executableSha256
        && plan.value->gameModuleSha256 == *Sha256File(f.game / "Game.dll").value, "game executable and module identities are bound");
    RequireShortGameCopy(*plan.value, f, L".w3x");
    const auto repeated = BuildMapTestLaunchPlan(*artifact.value);
    Require(repeated.Succeeded() && repeated.value->gameMapCopy == plan.value->gameMapCopy,
        "same frozen task rebuilds a stable preflight-only launch plan");
    const auto secondArtifact = PrepareMapTest(f.Request("prepared-second")); Require(secondArtifact.Succeeded(), "prepare second independent task");
    const auto second = BuildMapTestLaunchPlan(*secondArtifact.value);
    Require(second.Succeeded() && second.value->gameMapCopy != plan.value->gameMapCopy, "separate frozen tasks have distinct short-copy directories");
    RequireShortGameCopy(*second.value, f, L".w3x");
    Require(*Sha256File(f.source).value == f.sha, "launch-plan copy never writes the author source");
    Require(!std::filesystem::exists(f.root / "ydwe"), "native test has no YDWE installation");
    Require(!PrepareMapTest(f.Request("prepared")).Succeeded(), "existing workspace not reused");
    Write(plan.value->gameMapCopy, "preexisting-short-copy-must-remain");
    const auto existingCopySha = *Sha256File(plan.value->gameMapCopy).value;
    Require(!BuildMapTestLaunchPlan(*artifact.value).Succeeded(), "existing short-copy task directory cannot be reused");
    Require(*Sha256File(plan.value->gameMapCopy).value == existingCopySha, "existing short-copy file is never overwritten");
    Write(artifact.value->testMap, "changed-after-plan");
    Require(!BuildMapTestLaunchPlan(*artifact.value).Succeeded(), "stale candidate cannot launch");
    Require(*Sha256File(plan.value->gameMapCopy).value == existingCopySha && *Sha256File(f.source).value == f.sha,
        "stale input rejection preserves prior short copy and author source");
}
void SlkFailureNeverFallsBack() {
    Fixture f; auto request = f.Request("missing"); request.optimizeSlk = true;
    Require(Code(PrepareMapTest(request), "MAP_TEST_SLK_UNAVAILABLE"), "SLK requires explicit adapter");
    Require(!std::filesystem::exists(request.workspace), "missing adapter does not take workspace ownership");
    request.workspace = f.root / "failed";
    bool called = false;
    const auto failed = PrepareMapTest(request, [&](const MapTestSlkRequest& slk) {
        called = true; Require(slk.inputMap != f.source && slk.expectedInputSha256 == f.sha, "adapter sees only independent input");
        return Status::Failure("fixture", "SLK_SEMANTIC_MISMATCH", "failed semantic check");
    });
    Require(called && Code(failed, "SLK_SEMANTIC_MISMATCH"), "SLK error preserved without fallback");
    Require(!std::filesystem::exists(request.workspace / "test.w3x") && *Sha256File(f.source).value == f.sha, "failed SLK has no ready test map and source unchanged");
    request.workspace = f.root / "successful";
    std::vector<std::string> stages;
    const auto result = PrepareMapTest(request, [](const MapTestSlkRequest& slk) { Write(slk.outputMap, "semantic-approved-slk-fixture"); return Status::Ok(); },
        [&](std::string_view stage) { stages.emplace_back(stage); });
    Require(result.Succeeded() && result.value->optimizedSlk && result.value->testMapSha256 != f.sha
        && result.value->testMapSha256 == *Sha256File(result.value->testMap).value, "typed adapter outcome bound to actual optimized bytes");
    Require(stages == std::vector<std::string>{"copy", "slk", "verify"}, "SLK progress uses stable ordered stage keys");
    request.workspace = f.root / "mutating";
    const auto mutation = PrepareMapTest(request, [](const MapTestSlkRequest& slk) { Write(slk.outputMap, "output"); Write(slk.inputMap, "mutated-input"); return Status::Ok(); });
    Require(Code(mutation, "VERIFIED_FILE_SHA_MISMATCH"), "adapter cannot silently mutate frozen input");
    Require(*Sha256File(f.source).value == f.sha, "converter input mutation never touches author source");
    request.workspace = f.root / "missing-output";
    Require(Code(PrepareMapTest(request, [](const MapTestSlkRequest&) { return Status::Ok(); }), "MAP_TEST_OUTPUT"),
        "successful adapter status cannot substitute for an ordinary output map");
    request.workspace = f.root / "directory-output";
    Require(Code(PrepareMapTest(request, [](const MapTestSlkRequest& slk) {
        std::filesystem::create_directory(slk.outputMap); return Status::Ok();
    }), "MAP_TEST_OUTPUT"), "directory cannot substitute for converted output map");
}
void ProgressStagesAndFinalCopyIdentity() {
    Fixture f; auto request = f.Request("stable-progress");
    std::transform(request.expectedInputSha256.begin(), request.expectedInputSha256.end(), request.expectedInputSha256.begin(), [](char c) {
        return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c;
    });
    std::vector<std::string> stages;
    const auto prepared = PrepareMapTest(request, {}, [&](std::string_view stage) { stages.emplace_back(stage); });
    Require(prepared.Succeeded() && prepared.value->testMapSha256 == f.sha
        && *Sha256File(prepared.value->testMap).value == f.sha, "ordinary copy reuses a normalized SHA already proven by complete verification");
    Require(stages == std::vector<std::string>{"copy", "verify"}, "ordinary preparation emits no SLK stage");
    request = f.Request("callback-mutated-copy"); stages.clear();
    const auto changedCopy = PrepareMapTest(request, {}, [&](std::string_view stage) {
        stages.emplace_back(stage);
        if (stage == "verify") Write(request.workspace / "test.w3x", "changed-after-first-copy-verification");
    });
    Require(Code(changedCopy, "VERIFIED_FILE_SHA_MISMATCH"), "final verification rejects a copied map changed after its first SHA check");
    Require(stages == std::vector<std::string>{"copy", "verify"} && *Sha256File(f.source).value == f.sha,
        "copy mutation rejection keeps source bytes and stage protocol intact");
    request = f.Request("callback-mutated-source");
    const auto changedSource = PrepareMapTest(request, {}, [&](std::string_view stage) {
        if (stage == "copy") Write(f.source, "changed-before-frozen-input-read");
    });
    Require(Code(changedSource, "VERIFIED_FILE_SHA_MISMATCH") && !std::filesystem::exists(request.workspace),
        "input changed by a preparation callback is rejected before acquiring workspace ownership");
}
void LaunchProtectionFailsClosed() {
    Fixture f; auto prepared = PrepareMapTest(f.Request("prepared")); Require(prepared.Succeeded(), "prepare fixture");
    auto artifact = *prepared.value;
    artifact.testMapSha256 = std::string(64, '0');
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "input hash mismatch refuses plan before game copy");
    Require(!std::filesystem::exists(f.game / "Maps"), "failed hash preflight does not acquire game-copy ownership");
    artifact = *prepared.value; artifact.testMap = f.source;
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "author map outside independent workspace is not a prepared artifact");
    artifact = *prepared.value; artifact.testMap = artifact.workspace / ".." / "author.w3x";
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "lexical traversal outside independent workspace is not a prepared artifact");
    artifact = *prepared.value; artifact.testMap = "relative/test.w3x";
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "relative candidate path refused");
    artifact = *prepared.value; artifact.gameRoot = "relative/game";
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "relative game path refused");
    artifact = *prepared.value;
    for (const auto* name : {"war3.exe", "Game.dll", "war3.mpq", "war3patch.mpq", "storm.dll"}) {
        const auto member = f.game / name;
        std::filesystem::remove(member);
        Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "missing selected-game dependency refuses plan");
        std::filesystem::create_directory(member);
        Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "directory cannot stand in for a selected-game file");
        std::filesystem::remove(member); Write(member);
    }
    const auto safePlan = BuildMapTestLaunchPlan(artifact); Require(safePlan.Succeeded(), "complete selected-game fixture yields protected destination");
    artifact.sourceMap = safePlan.value->gameMapCopy;
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "future copy path cannot be declared as protected author map");
    artifact = *prepared.value; artifact.inputMap = safePlan.value->gameMapCopy;
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "future copy path cannot be declared as protected frozen input");
    artifact = *prepared.value; artifact.sourceMap = safePlan.value->gameMapCopy.parent_path() / "other-author.w3x";
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "managed short-copy directory cannot contain protected author input even if not yet present");
    artifact = *prepared.value;
    std::filesystem::create_directory(f.game / "Maps");
    std::filesystem::create_hard_link(f.source, f.game / "Maps/Audastra");
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "preexisting author hardlink cannot become managed game-copy directory");
    Require(*Sha256File(f.source).value == f.sha && *Sha256File(f.game / "Maps/Audastra").value == f.sha,
        "copy-directory conflict neither removes nor writes hardlinked author map");
}
void NativeW3mAndLegacyConfigurationIgnored() {
    Fixture f;
    const auto source = f.root / "classic-author.w3m";
    Write(source, "classic-independent-map"); const auto sourceSha = *Sha256File(source).value;
    const auto legacyRoot = f.root / "legacy-ydwe";
    const auto config = legacyRoot / "bin/EverConfig.cfg";
    Write(config, "[War3Patch]\nOption = 2\nDirName = ../unrelated-game\n[MapTest]\nCopyLocation = ../author\n");
    Write(legacyRoot / "plugin/warcraft3/config.cfg", "[Enable]\nyd_jass_api.dll = 0\n");
    const auto configSha = *Sha256File(config).value;
    auto request = f.Request("classic-prepared"); request.inputMap = source;
    request.sourceMap = source; request.expectedInputSha256 = sourceSha;
    const auto artifact = PrepareMapTest(request); Require(artifact.Succeeded(), "native preparation ignores unused SLK/legacy configuration");
    const auto plan = BuildMapTestLaunchPlan(*artifact.value);
    Require(plan.Succeeded() && std::filesystem::equivalent(plan.value->executable, f.game / "war3.exe"),
        "native launch plan uses selected game even with invalid legacy patch, JAPI and copy configuration");
    RequireShortGameCopy(*plan.value, f, L".w3m");
    Require(plan.value->testMapSha256 == sourceSha && *Sha256File(source).value == sourceSha,
        "classic map and copied archive retain byte identity");
    Require(*Sha256File(config).value == configSha && !std::filesystem::exists(legacyRoot / "YDWE.exe"),
        "native testing neither consumes nor rewrites legacy launcher configuration");
}
std::filesystem::path ArgumentValue(const MapTestLaunchPlan& plan, std::wstring_view flag) {
    const auto found = std::find(plan.arguments.begin(), plan.arguments.end(), flag);
    Require(found != plan.arguments.end() && std::next(found) != plan.arguments.end(), "runtime plan argument exists with value");
    return *std::next(found);
}
void RuntimePlanRequiresIndependentHelpers() {
    Fixture f; auto request = f.Request("runtime-prepared");
    request.runtimeRoot = f.root / "compatible-runtime";
    std::filesystem::create_directory(request.runtimeRoot);
    const auto prepared = PrepareMapTest(request); Require(prepared.Succeeded(), "runtime request freezes map independently");
    auto artifact = *prepared.value;
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "runtime selection cannot silently fall back to bare Warcraft without helpers");
    artifact.runtimeBridge = f.root / "editor/audastra-runtime-bridge.exe";
    artifact.runtimeBootstrap = f.root / "editor/audastra-runtime-bootstrap.dll";
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "missing independent runtime bridge and bootstrap refuse launch");
    Write(artifact.runtimeBridge);
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "missing runtime bootstrap refuses launch despite bridge");
    Write(artifact.runtimeBootstrap);
    const auto plan = BuildMapTestLaunchPlan(artifact);
    Require(plan.Succeeded(), "ordinary independent runtime helpers suffice for Core plan, PE verification belongs to process wrapper");
    Require(std::filesystem::equivalent(plan.value->executable, artifact.runtimeBridge), "runtime plan executes independent bridge");
    RequireShortGameCopy(*plan.value, f, L".w3x");
    Require(plan.value->arguments.size() == 10, "runtime plan has exactly five named argument pairs");
    Require(std::filesystem::equivalent(ArgumentValue(*plan.value, L"--game"), f.game / "war3.exe"), "runtime plan binds selected Warcraft executable");
    Require(!ArgumentValue(*plan.value, L"--map").is_absolute()
        && plan.value->workingDirectory / ArgumentValue(*plan.value, L"--map") == plan.value->gameMapCopy,
        "runtime plan binds this task's short game-relative map copy");
    Require(std::filesystem::equivalent(ArgumentValue(*plan.value, L"--runtime"), request.runtimeRoot), "runtime plan binds explicitly selected runtime root");
    Require(std::filesystem::equivalent(ArgumentValue(*plan.value, L"--bootstrap"), artifact.runtimeBootstrap), "runtime plan binds independent bootstrap helper");
    Require(ArgumentValue(*plan.value, L"--receipt") == plan.value->runtimeReceipt && plan.value->runtimeReceipt.is_absolute()
        && !std::filesystem::exists(plan.value->runtimeReceipt), "plan names an independent receipt without claiming injection or executing runtime");
    auto treeArtifact = artifact;
    treeArtifact.runtimeLayout = MapTestRuntimeLayout::YdweTree;
    const auto treePlan = BuildMapTestLaunchPlan(treeArtifact);
    Require(treePlan.Succeeded() && treePlan.value->runtimeLayout == MapTestRuntimeLayout::YdweTree
        && treePlan.value->arguments.size() == 12 && ArgumentValue(*treePlan.value, L"--runtime-layout") == L"ydwe-tree",
        "Core plan explicitly passes the selected two-level YDWE layout without copying or loading libraries");
    Write(plan.value->runtimeReceipt, "preexisting-runtime-receipt-must-remain");
    const auto receiptSha = *Sha256File(plan.value->runtimeReceipt).value;
    Require(Code(BuildMapTestLaunchPlan(artifact), "MAP_TEST_RUNTIME_RECEIPT"), "preexisting runtime receipt prevents a new launch from claiming its result");
    Require(*Sha256File(plan.value->runtimeReceipt).value == receiptSha, "runtime preflight never overwrites a preexisting receipt");
    Require(std::filesystem::remove(plan.value->runtimeReceipt), "reset this fixture's receipt conflict before independent helper-path negatives");
    artifact.runtimeBridge = "relative/bridge.exe";
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "relative runtime bridge refused");
    artifact = *prepared.value; artifact.runtimeBridge = f.root / "editor/audastra-runtime-bridge.exe";
    artifact.runtimeBootstrap = "relative/bootstrap.dll";
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "relative runtime bootstrap refused");
    artifact.runtimeBootstrap = f.root / "editor/audastra-runtime-bootstrap.dll";
    artifact.runtimeRoot = "relative/runtime";
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "relative runtime directory refused");
    artifact.runtimeRoot = request.runtimeRoot;
    std::filesystem::remove(artifact.runtimeBridge); std::filesystem::create_directory(artifact.runtimeBridge);
    Require(!BuildMapTestLaunchPlan(artifact).Succeeded(), "directory cannot stand in for independent runtime bridge");
    Require(*Sha256File(f.source).value == f.sha && *Sha256File(prepared.value->testMap).value == f.sha
        && !std::filesystem::exists(plan.value->gameMapCopy), "runtime preflight never mutates source or creates a ready game copy");
}
void YdweFallbackFreezesWithoutBecomingAnExplicitRuntime() {
    Fixture f;
    auto request = f.Request("ydwe-fallback-prepared");
    request.runtimeYdweRoot = f.root / "local-ydwe-installation";
    const auto prepared = PrepareMapTest(request);
    Require(prepared.Succeeded() && prepared.value->runtimeRoot.empty()
        && prepared.value->runtimeYdweRoot == request.runtimeYdweRoot,
        "empty runtimeRoot freezes local YDWE metadata without requiring or copying unused DLLs");
    const auto native = BuildMapTestLaunchPlan(*prepared.value);
    Require(native.Succeeded() && native.value->runtimeRoot.empty() && native.value->arguments.size() == 3,
        "Core does not force a fallback dependency before ScriptBuild determines map interface requirements");
    Require(!std::filesystem::exists(request.runtimeYdweRoot), "fallback metadata never creates the configured installation");
    for (const auto* suffix : {"ui", "share/mpq"}) {
        auto subdirectoryRequest = f.Request(std::string("ydwe-") + (std::string_view(suffix) == "ui" ? "ui" : "share-mpq"));
        subdirectoryRequest.runtimeYdweRoot = request.runtimeYdweRoot / suffix;
        const auto subdirectoryPrepared = PrepareMapTest(subdirectoryRequest);
        Require(subdirectoryPrepared.Succeeded() && subdirectoryPrepared.value->runtimeYdweRoot == subdirectoryRequest.runtimeYdweRoot
            && MapTestYdweInstallRoot(subdirectoryPrepared.value->runtimeYdweRoot) == request.runtimeYdweRoot,
            "preparation preserves UI preference identity and the fixed subdirectory resolver binds the parent installation");
        subdirectoryRequest.workspace = request.runtimeYdweRoot / "forbidden-test-copy";
        Require(Code(PrepareMapTest(subdirectoryRequest), "MAP_TEST_WORKSPACE"),
            "UI subdirectory normalization cannot allow writing a test workspace into the parent installation");
    }
    auto explicitRequest = f.Request("flat-precedence-prepared");
    explicitRequest.runtimeRoot = f.root / "selected-flat-runtime";
    explicitRequest.runtimeYdweRoot = "relative-unused-ydwe";
    const auto explicitPrepared = PrepareMapTest(explicitRequest);
    Require(explicitPrepared.Succeeded() && explicitPrepared.value->runtimeYdweRoot.empty()
        && explicitPrepared.value->runtimeRoot == std::filesystem::weakly_canonical(explicitRequest.runtimeRoot),
        "explicit flat override ignores invalid unused local YDWE fallback preferences");
    auto within = f.Request("unused-ydwe-workspace");
    within.runtimeYdweRoot = request.runtimeYdweRoot;
    within.workspace = within.runtimeYdweRoot / "test-copy";
    Require(Code(PrepareMapTest(within), "MAP_TEST_WORKSPACE") && !std::filesystem::exists(within.workspace),
        "test preparation cannot write inside a user-installed fallback runtime");
#ifdef _WIN32
    const auto installation = f.root / "owned-ydwe-with-shared-ui", sharedUi = f.root / "ui";
    Require(std::filesystem::create_directory(installation) && std::filesystem::create_directory(sharedUi), "create only original owned symlink fixture directories");
    const auto uiLink = installation / "ui";
    BOOL linked = CreateSymbolicLinkW(uiLink.c_str(), sharedUi.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2);
    if (!linked && GetLastError() == ERROR_INVALID_PARAMETER) linked = CreateSymbolicLinkW(uiLink.c_str(), sharedUi.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY);
    if (linked) {
        auto linkedRequest = f.Request("native-unused-ui-link"); linkedRequest.runtimeYdweRoot = uiLink;
        const auto linkedPrepared = PrepareMapTest(linkedRequest);
        Require(linkedPrepared.Succeeded() && BuildMapTestLaunchPlan(*linkedPrepared.value).Succeeded(), "unused shared UI link permits ordinary native preparation and plan");
        auto siblingRequest = f.Request("native-shared-ui-sibling"); siblingRequest.runtimeYdweRoot = uiLink;
        const auto siblingPrepared = PrepareMapTest(siblingRequest);
        Require(siblingPrepared.Succeeded() && BuildMapTestLaunchPlan(*siblingPrepared.value).Succeeded(),
            "shared UI target named ui does not make its unrelated parent and sibling workspace a protected installation");
        linkedRequest.workspace = installation / "forbidden-test-copy";
        Require(Code(PrepareMapTest(linkedRequest), "MAP_TEST_WORKSPACE") && !std::filesystem::exists(linkedRequest.workspace),
            "UI directory symlink cannot hide its original installation parent from output containment");
        linkedRequest.workspace = sharedUi / "forbidden-test-copy";
        Require(Code(PrepareMapTest(linkedRequest), "MAP_TEST_WORKSPACE") && !std::filesystem::exists(linkedRequest.workspace),
            "shared UI symlink target remains readonly as an independently protected data root");
        Require(std::filesystem::remove(uiLink), "remove only the exact original owned UI directory symlink");
    } else std::cout << "Optional shared UI containment fixture NOT EXECUTED; CreateSymbolicLink Win32=" << GetLastError() << '\n';
#endif
    Require(*Sha256File(f.source).value == f.sha, "fallback preparation preserves original map byte identity");
}
void StaleSourceAndInstallationWorkspaceRejected() {
    Fixture f; auto request = f.Request("wrong-sha"); request.expectedInputSha256 = std::string(64, '0');
    Require(!PrepareMapTest(request).Succeeded() && !std::filesystem::exists(request.workspace), "stale source fails before creating workspace");
    request = f.Request("unused"); request.workspace = f.game / "test-copy";
    Require(Code(PrepareMapTest(request), "MAP_TEST_WORKSPACE"), "test workspace cannot mutate configured game installation");
    request = f.Request("unused-runtime");
    request.runtimeRoot = f.root / "runtime-installation";
    std::filesystem::create_directory(request.runtimeRoot);
    request.workspace = request.runtimeRoot / "test-copy";
    Require(Code(PrepareMapTest(request), "MAP_TEST_WORKSPACE"), "test workspace cannot mutate configured extension runtime installation");
    Require(!std::filesystem::exists(request.workspace), "runtime installation workspace is refused before creating output");
    Require(*Sha256File(f.source).value == f.sha, "installation workspace rejection preserves author map byte identity");
}
void WorkspaceLeaseCleansOnlyFrozenIdentity() {
    Fixture f;
    const auto owned = f.root / "owned-run";
    Write(owned / "input.w3x", "frozen-input"); Write(owned / "prepared/test.w3x", "frozen-output");
    auto lease = FreezeMapTestWorkspace(owned); Require(lease.Succeeded(), "freeze owned directory identity");
    Require(Code((*lease.value)->Refresh(), "MAP_TEST_REFRESH_NOT_ALLOWED"), "initial nonempty lease cannot acquire new ownership through refresh");
    const auto sha = *Sha256File(owned / "prepared/test.w3x").value;
    Require(ReadVerifiedFile(owned / "prepared/test.w3x", sha).Succeeded(), "freeze read lease compatible with launch preflight");
    Require(!(*lease.value)->Cleanup(false).Succeeded() && std::filesystem::exists(owned / "input.w3x"), "active or unknown game never cleans inputs");
    std::error_code renameError; std::filesystem::rename(owned / "prepared", owned / "replaced", renameError);
    Require(static_cast<bool>(renameError), "original directory handle prevents replacement");
    Write(owned / "unknown-new-file", "external new member");
    Require(!(*lease.value)->Cleanup(true).Succeeded() && std::filesystem::exists(owned / "input.w3x"), "unknown newly added file preserves all frozen input files");
    std::filesystem::remove(owned / "unknown-new-file");
    Require((*lease.value)->Cleanup(true).Succeeded() && !std::filesystem::exists(owned), "terminal cleanup deletes only bound known handles and empty owned dirs");
    Require(*Sha256File(f.source).value == f.sha, "workspace cleanup never touches author source");
}
void EmptyWorkspaceRefreshKeepsRootDuringPreparation() {
    Fixture f;
    const auto owned = f.root / "empty-owned-run";
    Require(std::filesystem::create_directory(owned), "create initially empty task root");
    auto lease = FreezeMapTestWorkspace(owned); Require(lease.Succeeded(), "freeze empty root before preparation");
    std::error_code renameError;
    std::filesystem::rename(owned, f.root / "replaced-owned-run", renameError);
    Require(static_cast<bool>(renameError), "root original handle prevents replacement during preparation");
#ifdef _WIN32
    const auto writer = CreateFileW(owned.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    const auto error = GetLastError();
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    Require(writer == INVALID_HANDLE_VALUE && error == ERROR_SHARING_VIOLATION, "root lease refuses a directory generic-write handle");
#endif
    // 原root租约持续持有，但子文件和子目录是不同对象，转换器仍可正常产生输出。
    Write(owned / "input.w3x", "frozen-input");
    Write(owned / "prepared/test.w3x", "frozen-output");
    Write(owned / "prepared/convert.log", "converter diagnostic");
    const auto sha = *Sha256File(owned / "prepared/test.w3x").value;
    Require((*lease.value)->Refresh().Succeeded(), "refresh takes new member leases without reopening root for DELETE");
    Require(ReadVerifiedFile(owned / "prepared/test.w3x", sha).Succeeded(), "refreshed read lease remains compatible with launch verification");
    Require(Code((*lease.value)->Refresh(), "MAP_TEST_REFRESH_NOT_ALLOWED"), "refresh allowed only once");
    Require((*lease.value)->Cleanup(true).Succeeded() && !std::filesystem::exists(owned), "refreshed snapshot cleans known files and original empty root");
    Require(*Sha256File(f.source).value == f.sha, "refresh and cleanup leave original map untouched");
}
void FailedWorkspaceRefreshRetainsRootAndDisablesCleanup() {
    Fixture f;
    const auto owned = f.root / "failed-refresh-run";
    Require(std::filesystem::create_directory(owned), "create empty failed-refresh fixture");
    auto lease = FreezeMapTestWorkspace(owned); Require(lease.Succeeded(), "freeze failed-refresh empty root");
    Write(owned / "input.w3x", "must-remain");
    const auto blocked = owned / "exclusive-output.w3x";
    Write(blocked, "independent-occupied-output");
    const auto blockedSha = *Sha256File(blocked).value;
#ifdef _WIN32
    const auto lock = CreateFileW(blocked.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "lock converter output before refresh");
    const auto refresh = (*lease.value)->Refresh();
    CloseHandle(lock);
    Require(Code(refresh, "MAP_TEST_CLEANUP_LEASE"), "occupied member refuses refresh read ownership");
#endif
    Require(Code((*lease.value)->Cleanup(true), "MAP_TEST_CLEANUP_INCOMPLETE"), "failed refresh never cleans from a partial inventory");
    Require(std::filesystem::exists(owned / "input.w3x") && std::filesystem::exists(blocked), "failed refresh leaves all files");
    std::error_code renameError;
    std::filesystem::rename(owned, f.root / "replaced-failed-run", renameError);
    Require(static_cast<bool>(renameError), "failed refresh retains original root handle");
    Require(Code((*lease.value)->Refresh(), "MAP_TEST_REFRESH_NOT_ALLOWED"), "failed refresh attempt cannot be retried with a changed inventory");
    lease.value.reset();
    Require(std::filesystem::exists(owned / "input.w3x"), "lease destructor only closes handles and does not delete failed workspace");
    Require(*Sha256File(blocked).value == blockedSha, "failed refresh leaves occupied output unchanged");
    Require(*Sha256File(f.source).value == f.sha, "failed refresh never mutates author source");
}
void WorkspaceFreezeRejectsPreexistingHardlinks() {
    Fixture f;
    const auto external = f.root / "unleased-external.w3x";
    const auto owned = f.root / "prelinked-owned-run";
    Write(external, "independent-hardlink-fixture");
    const auto externalSha = *Sha256File(external).value;
    Require(std::filesystem::create_directory(owned), "create unlocked hardlink fixture directory");
    std::filesystem::create_hard_link(external, owned / "external-alias.w3x");
    Require(Code(FreezeMapTestWorkspace(owned), "MAP_TEST_CLEANUP_IDENTITY"), "preexisting hardlinked member refuses freeze ownership");
    Require(std::filesystem::exists(owned / "external-alias.w3x"), "failed freeze leaves hardlinked member in place");
    Require(*Sha256File(external).value == externalSha, "failed freeze leaves external hardlink source unchanged");
    Require(*Sha256File(f.source).value == f.sha, "failed freeze leaves author source unchanged");
}
void OwnedLuaOptionsDoNotFallBack() {
    Fixture f;
    auto request = f.Request("lua-test");
    request.enableLuaHotReload = true;
    Require(Code(PrepareMapTest(request), "MAP_TEST_LUA_RELOAD_RUNTIME"), "reload cannot silently attach to ordinary runtime");
    Require(!std::filesystem::exists(request.workspace), "invalid reload mode rejected before any workspace write");
    request.useOwnedLuaRuntime = true;
    const auto prepared = PrepareMapTest(request);
    Require(prepared.Succeeded() && prepared.value->useOwnedLuaRuntime && prepared.value->enableLuaHotReload,
        "explicit runtime and reload choice survive frozen map preparation");
    Require(Code(BuildMapTestLaunchPlan(*prepared.value), "MAP_TEST_OWN_LUA_RUNTIME_NOT_READY"),
        "unimplemented owned runtime never falls back to direct game or six DLL path");
    auto changed = *prepared.value; changed.useOwnedLuaRuntime = false;
    Require(Code(BuildMapTestLaunchPlan(changed), "MAP_TEST_LUA_RELOAD_RUNTIME"), "invalid artifact mode refused");
    Require(*Sha256File(f.source).value == f.sha, "new runtime choices never modify source map");
}
}
int main() {
    try { OwnedLuaOptionsDoNotFallBack(); CopyAndPlanKeepSource(); SlkFailureNeverFallsBack(); ProgressStagesAndFinalCopyIdentity(); LaunchProtectionFailsClosed(); NativeW3mAndLegacyConfigurationIgnored(); RuntimePlanRequiresIndependentHelpers(); YdweFallbackFreezesWithoutBecomingAnExplicitRuntime(); StaleSourceAndInstallationWorkspaceRejected(); WorkspaceLeaseCleansOnlyFrozenIdentity(); EmptyWorkspaceRefreshKeepsRootDuringPreparation(); FailedWorkspaceRefreshRetainsRootAndDisablesCleanup(); WorkspaceFreezeRejectsPreexistingHardlinks();
        std::cout << "Map test service tests passed (no YDWE or game process launched)\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
