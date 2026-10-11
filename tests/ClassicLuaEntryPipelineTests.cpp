#include "War3MapCore/Script/ClassicLuaEntryPipeline.h"
#include "War3MapCore/Script/ClassicLuaCompatibility.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Script;

void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string Codes(const auto& result) {
    std::string text;
    for (const auto& item : result.diagnostics) text += item.code + ": " + item.message + "\n";
    return text;
}
bool HasCode(const auto& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& item) { return item.code == code; });
}
std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> result(text.size());
    if (!text.empty()) std::memcpy(result.data(), text.data(), text.size());
    return result;
}
std::string Text(const ScriptBaseInput& input) {
    if (input.bytes.empty()) return {};
    return {reinterpret_cast<const char*>(input.bytes.data()), input.bytes.size()};
}
std::shared_ptr<const ScriptBaseInputs> Bases(std::string_view common, std::string_view blizzard) {
    const std::vector<MemberReplacement> replacements{{"common.j", Bytes(common)}, {"blizzard.j", Bytes(blizzard)}};
    ScriptBaseInputsRequest request;
    request.policy = ScriptBasePolicy::MapGameOnly; request.gameLibraryVersion = 24; request.replacements = replacements;
    const auto result = CaptureScriptBaseInputs(request);
    Require(result.Succeeded(), Codes(result));
    return std::make_shared<const ScriptBaseInputs>(*result.value);
}
void Changed(std::string& text, std::string_view before, std::string_view after) {
    const auto found = text.find(before);
    Require(found != std::string::npos, "original fixture mutation has an exact source");
    text.replace(found, before.size(), after);
}

constexpr std::string_view kCommon = "native TestNative takes integer value returns nothing\n";
constexpr std::string_view kBlizzard =
    "function TestHelper takes nothing returns nothing\ncall TestNative(7)\nendfunction\n";
constexpr std::string_view kEntry =
    "function config takes nothing returns nothing\nendfunction\n"
    "function main takes nothing returns nothing\ncall TestHelper()\nendfunction\n";

void Capture(const std::filesystem::path& root) {
    const auto path = root / "owned-bases.w3x";
    const std::vector<MemberReplacement> members{{"common.j", Bytes(kCommon)}, {"scripts/common.j", Bytes("// alias\n")}};
    Require(WriteVerifiedArchive(path, members).Succeeded(), "write original map base fixture");
    const auto archive = MapArchiveSession::Open(path);
    Require(archive.Succeeded(), Codes(archive));
    const auto gameRoot = root / "owned-game";
    std::filesystem::create_directory(gameRoot);
    const std::vector<MemberReplacement> low{{"common.j", Bytes("// lower priority\n")}, {"blizzard.j", Bytes("// lower priority\n")}};
    const std::vector<MemberReplacement> high{{"common.j", Bytes("// patch common\n")}, {"scripts/blizzard.j", Bytes(kBlizzard)}};
    Require(WriteVerifiedArchive(gameRoot / "war3.mpq", low).Succeeded(), "write original base game archive");
    Require(WriteVerifiedArchive(gameRoot / "war3patch.mpq", high).Succeeded(), "write original patch game archive");
    const auto game = Game::GameDataSource::MountStrict(gameRoot);
    Require(game.Succeeded(), Codes(game));
    ScriptBaseInputsRequest request;
    request.policy = ScriptBasePolicy::MapGameOnly; request.archive = &*archive.value; request.game = &*game.value;
    request.gameLibraryVersion = 24; request.documentRevision = 83;
    auto captured = CaptureScriptBaseInputs(request);
    Require(captured.Succeeded(), Codes(captured));
    Require(captured.value->common.origin == ScriptBaseOrigin::MapArchive && Text(captured.value->common) == kCommon
        && captured.value->common.sourceRoot == path && captured.value->sourceMapSha256 == archive.value->SourceSha256()
        && captured.value->documentRevision == 83, "no-layout capture freezes actual map source and revision");
    Require(captured.value->blizzard.origin == ScriptBaseOrigin::GameArchive
        && captured.value->blizzard.archiveName == "war3.mpq" && Text(captured.value->blizzard) == "// lower priority\n",
        "root script name has priority over scripts alias independently of MPQ priority");
    const std::vector<MemberReplacement> edits{{"COMMON.J", Bytes("// final common\n")}, {"SCRIPTS/blizzard.j", Bytes(kBlizzard)}};
    request.replacements = edits;
    captured = CaptureScriptBaseInputs(request);
    Require(captured.Succeeded() && captured.value->common.origin == ScriptBaseOrigin::MapReplacement
        && Text(captured.value->common) == "// final common\n" && Text(captured.value->blizzard) == kBlizzard,
        "final replacement view wins over map and game bytes for each base");
    Require(VerifyScriptBaseInputs(*captured.value, ScriptBasePolicy::MapGameOnly).Succeeded(), "frozen no-layout snapshot verifies");
    auto changed = *captured.value; changed.common.bytes.front() ^= std::byte{1};
    Require(!VerifyScriptBaseInputs(changed, ScriptBasePolicy::MapGameOnly).Succeeded(), "changed frozen bytes reject old SHA");
    changed = *captured.value; changed.common.origin = ScriptBaseOrigin::Layout;
    Require(!VerifyScriptBaseInputs(changed, ScriptBasePolicy::MapGameOnly).Succeeded(), "map/game snapshot cannot claim a layout origin");
    changed = *captured.value; changed.common.memberName = "unrelated.j";
    Require(!VerifyScriptBaseInputs(changed, ScriptBasePolicy::MapGameOnly).Succeeded(), "wrong base member is not a common library");
    changed = *captured.value; changed.gameLibraryVersion = 20;
    Require(!VerifyScriptBaseInputs(changed, ScriptBasePolicy::MapGameOnly).Succeeded(), "frozen semantic policy cannot change independently");

    request.replacements = {}; request.archive = nullptr;
    const auto fromGame = CaptureScriptBaseInputs(request);
    Require(fromGame.Succeeded() && Text(fromGame.value->common) == "// patch common\n"
        && fromGame.value->common.archiveName == "war3patch.mpq", "actual game patch priority is preserved");
    request.gameLibraryVersion.reset();
    Require(HasCode(CaptureScriptBaseInputs(request), "SCRIPT_BASE_GAME_POLICY_UNBOUND"), "standalone source policy must be explicit");
    request.gameLibraryVersion = 27;
    Require(HasCode(CaptureScriptBaseInputs(request), "SCRIPT_BASE_GAME_POLICY_INVALID"), "version macro is not a semantic policy");
    request.gameLibraryVersion = 20;
    const auto old = CaptureScriptBaseInputs(request);
    Require(old.Succeeded() && old.value->allowReturnBug, "generic map/game capture preserves explicit old semantics");
    Require(!CheckClassicLuaEntryScript(kEntry, std::make_shared<const ScriptBaseInputs>(*old.value)).Succeeded(),
        "classic Lua cannot use return-bug semantics");
    request.game = nullptr; request.gameLibraryVersion = 24;
    ScriptToolchainLayout ignoredLayout;
    request.layout = &ignoredLayout;
    Require(HasCode(CaptureScriptBaseInputs(request), "SCRIPT_BASE_MAP_GAME_MISSING"), "MapGameOnly never falls back to layout");
    request.layout = nullptr; request.policy = ScriptBasePolicy::BoundLayout;
    Require(HasCode(CaptureScriptBaseInputs(request), "SCRIPT_BASE_REQUEST_INVALID"), "ordinary JASS still needs a bound layout");
    request.policy = ScriptBasePolicy::MapGameThenLayout;
    Require(HasCode(CaptureScriptBaseInputs(request), "SCRIPT_BASE_REQUEST_INVALID"), "legacy map/game/layout policy still needs a layout");
    request.policy = ScriptBasePolicy::MapGameOnly; request.replacements = edits;
    const std::vector<std::string> removals{"common.j"}; request.removals = removals;
    Require(HasCode(CaptureScriptBaseInputs(request), "SCRIPT_BASE_EDIT_CONFLICT"), "delete and replace are a conflict");
    request.removals = {};
    auto duplicate = edits; duplicate.push_back({"common.j", Bytes("// conflicting common\n")}); request.replacements = duplicate;
    Require(HasCode(CaptureScriptBaseInputs(request), "SCRIPT_BASE_EDIT_CONFLICT"), "canonical replacement collisions reject");
    const std::vector<MemberReplacement> oversized{{"common.j", std::vector<std::byte>(16 * 1024 * 1024 + 1)}};
    request.replacements = oversized;
    Require(HasCode(CaptureScriptBaseInputs(request), "SCRIPT_BASE_LIMIT"), "base bytes are bounded before freezing");
}

void Pipeline() {
    const auto bases = Bases(kCommon, kBlizzard);
    const auto passed = CheckClassicLuaEntryScript(kEntry, bases);
    Require(passed.Succeeded(), Codes(passed));
    Require(passed.script == kEntry && passed.syntaxChecked && passed.baseInputs
        && passed.baseInputs->common.sha256 == bases->common.sha256 && !passed.checkedBy.empty(),
        "complete JASS check preserves source bytes and actual base identity");
    Require(passed.stages.size() == 2 && std::all_of(passed.stages.begin(), passed.stages.end(),
        [](const auto& stage) { return stage.exitCode == 0 && stage.output.empty(); }), "checks stay in process without stage files");
    const auto reject = [&](std::string_view code, const auto& result) {
        Require(!result.Succeeded() && !result.syntaxChecked && result.script.empty(), "failed input has no saveable output: " + Codes(result));
        if (!code.empty()) Require(HasCode(result, code), "expected rejection code: " + std::string(code) + "\n" + Codes(result));
    };
    reject("CLASSIC_LUA_ENTRY_CHECK_POLICY", CheckClassicLuaEntryScript(kEntry, {}));
    reject("CLASSIC_LUA_ENTRY_SOURCE_LIMIT", CheckClassicLuaEntryScript({}, bases));
    reject("JASS_UNDECLARED", CheckClassicLuaEntryScript(
        "function main takes nothing returns nothing\ncall MissingNative()\nendfunction\n", bases));
    reject("JASS_TYPE", CheckClassicLuaEntryScript(kEntry, Bases(kCommon,
        "function TestHelper takes nothing returns nothing\ncall TestNative(\"bad\")\nendfunction\n")));
    reject("JASS_UNKNOWN_TYPE", CheckClassicLuaEntryScript(kEntry, Bases(
        "native TestNative takes missingtype value returns nothing\n", kBlizzard)));
    Jass::JassCheckOptions options; options.syntaxOnly = true;
    reject("CLASSIC_LUA_ENTRY_CHECK_POLICY", CheckClassicLuaEntryScript(kEntry, bases, options));
    options = {}; options.returnBug = true;
    reject("CLASSIC_LUA_ENTRY_CHECK_POLICY", CheckClassicLuaEntryScript(kEntry, bases, options));
    options = {}; options.maxTokens = 2;
    reject("JASS_TOKEN_BUDGET", CheckClassicLuaEntryScript(kEntry, bases, options));
    options = {}; options.maxAstNodes = 1;
    reject("JASS_AST_NODE_BUDGET", CheckClassicLuaEntryScript(kEntry, bases, options));
    options = {}; options.maxTotalBytes = 5;
    reject("JASS_TOTAL_BYTES_BUDGET", CheckClassicLuaEntryScript(kEntry, bases, options));
    options = {}; options.maxDuration = std::chrono::milliseconds::zero();
    reject("JASS_CHECK_DEADLINE", CheckClassicLuaEntryScript(kEntry, bases, options));
    std::stop_source stop; stop.request_stop(); options = {}; options.cancel = stop.get_token();
    reject("JASS_CHECK_CANCELLED", CheckClassicLuaEntryScript(kEntry, bases, options));
    options = {}; options.maxBodyWorkers = 0;
    reject("JASS_CHECK_OPTIONS", CheckClassicLuaEntryScript(kEntry, bases, options));
    reject("CLASSIC_LUA_ENTRY_CHECK_INCOMPLETE", CheckClassicLuaEntryScript(
        "//# +nosemanticerror\nfunction main takes nothing returns nothing\ncall MissingNative()\nendfunction\n", bases));
    reject("CLASSIC_LUA_ENTRY_PRAGMA_UNSUPPORTED", CheckClassicLuaEntryScript("//# +rb\n" + std::string(kEntry), bases));
    reject("CLASSIC_LUA_ENTRY_PRAGMA_UNSUPPORTED", CheckClassicLuaEntryScript(kEntry, Bases("//# +rb\n" + std::string(kCommon), kBlizzard)));
    const auto literal = CheckClassicLuaEntryScript(
        "function main takes nothing returns nothing\nlocal string note = \"//# +rb\"\nendfunction\n", bases);
    Require(literal.Succeeded(), "a pragma-like string literal is ordinary text: " + Codes(literal));
    auto mutableBases = std::make_shared<ScriptBaseInputs>(*bases);
    std::string mutableEntry(kEntry);
    const auto owned = CheckClassicLuaEntryScript(mutableEntry, mutableBases, {}, [&](std::string_view stage) {
        if (stage != "classic-lua-entry-inputs") return;
        mutableEntry.assign("not jass"); mutableBases->common.bytes.clear();
    });
    Require(owned.Succeeded() && owned.script == kEntry && Text(owned.baseInputs->common) == kCommon,
        "progress callback cannot substitute original input buffers");
    reject("CLASSIC_LUA_ENTRY_CHECK_FAILED", CheckClassicLuaEntryScript(kEntry, bases, {}, [](std::string_view) {
        throw std::runtime_error("original callback failure");
    }));
}

struct Fixture {
    LuaTargetProfile target;
    MapInitializationPlan plan;
    std::string common, blizzard = "// original empty BJ fixture\n";

    void Native(const char* name, const char* returns, std::initializer_list<const char*> parameters) {
        LuaApiSignature api{name, returns, {}};
        common += "native " + std::string(name) + " takes ";
        std::size_t index = 0;
        for (const auto* parameter : parameters) {
            if (index) common += ", ";
            common += std::string(parameter) + " p" + std::to_string(index++); api.parameters.emplace_back(parameter);
        }
        if (!index) common += "nothing";
        common += " returns " + std::string(returns) + "\n"; target.apis.push_back(std::move(api));
    }
    Fixture() {
        target.identity = "original-classic-entry-dependency-fixture";
        for (const auto* name : {"player", "trigger", "alliancetype", "rect", "weathereffect"}) {
            target.handleTypes.emplace_back(name, "handle"); common += "type " + std::string(name) + " extends handle\n";
        }
        Native("TriggerEvaluate", "boolean", {"trigger"}); Native("TriggerExecute", "nothing", {"trigger"});
        Native("SetPlayerAlliance", "nothing", {"player", "player", "alliancetype", "boolean"});
        Native("GetCameraMargin", "real", {"integer"}); Native("NewSoundEnvironment", "nothing", {"string"});
        Native("SetDayNightModels", "nothing", {"string", "string"});
        Native("SetCameraBounds", "nothing", {"real", "real", "real", "real", "real", "real", "real", "real"});
        Native("Rect", "rect", {"real", "real", "real", "real"});
        Native("AddWeatherEffect", "weathereffect", {"rect", "integer"});
        Native("EnableWeatherEffect", "nothing", {"weathereffect", "boolean"});
        common += "globals\n";
        for (const auto* name : {"ALLIANCE_PASSIVE", "ALLIANCE_HELP_REQUEST", "ALLIANCE_HELP_RESPONSE", "ALLIANCE_SHARED_XP",
            "ALLIANCE_SHARED_SPELLS", "ALLIANCE_SHARED_VISION", "ALLIANCE_SHARED_CONTROL", "ALLIANCE_SHARED_ADVANCED_CONTROL"}) {
            target.constants.push_back({name, "alliancetype"}); common += "constant alliancetype " + std::string(name) + " = null\n";
        }
        for (const auto* name : {"CAMERA_MARGIN_LEFT", "CAMERA_MARGIN_RIGHT", "CAMERA_MARGIN_BOTTOM", "CAMERA_MARGIN_TOP"}) {
            target.constants.push_back({name, "integer"}); common += "constant integer " + std::string(name) + " = 0\n";
        }
        common += "integer Counter = 0\ninteger array Counters\ncode Callback = null\nendglobals\n";
        const auto compatibility = BuildClassicLuaCompatibility(target);
        Require(compatibility.Succeeded(), Codes(compatibility));
        target.apis.insert(target.apis.end(), compatibility.providedApis.begin(), compatibility.providedApis.end());
        MapData::MapSettings settings; settings.formatVersion = 25; settings.tileset = 'L'; settings.custom.emplace();
        MapData::RegionsDocument regions;
        MapData::RegionRecord region; region.left = -64; region.bottom = -64; region.right = 64; region.top = 64;
        region.name = "Original"; region.creationNumber = 17;
        region.weatherId = {std::byte{'R'}, std::byte{'A'}, std::byte{'I'}, std::byte{'N'}};
        regions.entries.push_back(region);
        MapScriptInputs inputs; inputs.settings = &settings; inputs.regions = &regions;
        const auto built = BuildMapInitializationPlan(inputs); Require(built.Succeeded(), Codes(built)); plan = *built.value;
    }
    LuaMapScriptResult Generate() const {
        const auto generated = GenerateLuaMapScript(plan, {}, {}, target, {}, {}, {true});
        Require(generated.Succeeded(), Codes(generated)); return generated;
    }
    Status Validate(const LuaMapScriptResult& generated, const Jass::JassCheckOptions& options = {}) const {
        return ValidateClassicLuaDependencies(plan, generated, target, *Bases(common, blizzard), options);
    }
};

void Dependencies() {
    Fixture fixture;
    const auto generated = fixture.Generate();
    const auto passed = fixture.Validate(generated);
    Require(passed.Succeeded(), Codes(passed));
    Require(!generated.requiredGlobals.empty(), "original region weather exercises map-owned global binding");
    auto copy = fixture; Changed(copy.common, "GetCameraMargin takes integer p0 returns real", "GetCameraMargin takes integer p0 returns integer");
    Require(HasCode(copy.Validate(generated), "CLASSIC_LUA_DEPENDENCY_API"), "actual native return type must exactly match");
    copy = fixture; Changed(copy.common, "SetPlayerAlliance takes player p0, player p1, alliancetype p2, boolean p3",
        "SetPlayerAlliance takes player p0, player p1, alliancetype p2, integer p3");
    Require(HasCode(copy.Validate(generated), "CLASSIC_LUA_DEPENDENCY_API"), "compat underlying native signature is checked even if unused by map");
    copy = fixture; Changed(copy.common, "constant alliancetype ALLIANCE_SHARED_XP", "alliancetype ALLIANCE_SHARED_XP");
    Require(HasCode(copy.Validate(generated), "CLASSIC_LUA_DEPENDENCY_CONSTANT"), "compat underlying constant must actually be constant");
    copy = fixture; Changed(copy.common, "constant integer CAMERA_MARGIN_LEFT = 0", "integer array CAMERA_MARGIN_LEFT");
    Require(HasCode(copy.Validate(generated), "CLASSIC_LUA_DEPENDENCY_CONSTANT"), "a target constant cannot bind to an array");
    copy = fixture;
    for (auto& [name, parent] : copy.target.handleTypes) if (name == "rect") parent = "player";
    Require(HasCode(copy.Validate(copy.Generate()), "CLASSIC_LUA_DEPENDENCY_API"), "self-reported handle ancestry must match actual common declarations");
    copy = fixture; copy.target.apis.push_back({"UnsupportedBJ", "nothing", {}});
    copy.common += "function UnsupportedBJ takes nothing returns nothing\nendfunction\n";
    auto dependency = copy.Generate(); dependency.requiredApis.push_back("UnsupportedBJ");
    Require(HasCode(copy.Validate(dependency), "CLASSIC_LUA_DEPENDENCY_API"), "ordinary JASS functions are not Lua-callable natives");
    copy = fixture; copy.plan.symbols.front().type = "handle";
    Require(HasCode(copy.Validate(generated), "CLASSIC_LUA_DEPENDENCY_GLOBAL"), "map global must match exact generated object type");
    copy = fixture; copy.target.identity += "-changed";
    Require(HasCode(copy.Validate(generated), "CLASSIC_LUA_DEPENDENCY_INPUT"), "target identity cannot change after generation");
    copy = fixture; copy.target.apis.push_back({"UnusedNative", "nothing", {}});
    Require(HasCode(copy.Validate(generated), "CLASSIC_LUA_DEPENDENCY_TARGET_CHANGED"), "full target fingerprint is rebound even for changed unused API");
    dependency = generated; dependency.text += "-- changed\n";
    Require(!fixture.Validate(dependency).Succeeded(), "modified generated Lua does not retain its dependency binding");
    dependency = generated; dependency.requiredGlobals.push_back("Counter");
    Require(fixture.Validate(dependency).Succeeded(), "actual common scalar can supply an engine global");
    for (const auto* name : {"Counters", "Callback", "MissingGlobal"}) {
        dependency = generated; dependency.requiredGlobals.push_back(name);
        Require(HasCode(fixture.Validate(dependency), "CLASSIC_LUA_DEPENDENCY_GLOBAL"), "array/code/missing global is rejected");
    }
    copy = fixture; copy.common = "//# +rb\n" + copy.common;
    Require(HasCode(copy.Validate(generated), "CLASSIC_LUA_ENTRY_PRAGMA_UNSUPPORTED"), "trusted common declarations cannot relax checks with pragmas");
    Jass::JassCheckOptions options; options.maxSymbolDefinitions = 1;
    Require(HasCode(fixture.Validate(generated, options), "CLASSIC_LUA_DEPENDENCY_INDEX_INCOMPLETE"), "partial symbol index is not dependency proof");
    options = {}; options.maxTokens = 1;
    Require(HasCode(fixture.Validate(generated, options), "CLASSIC_LUA_DEPENDENCY_INDEX_INCOMPLETE"), "lexical budget failure cannot authorize API dependencies");
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("audastra-classic-entry-" + std::to_string(GetCurrentProcessId())
        + "-" + std::to_string(GetTickCount64()));
    try {
        Require(std::filesystem::create_directory(root), "create owned fixture directory");
        Capture(root); Pipeline(); Dependencies();
        std::filesystem::remove_all(root);
        std::cout << "classic Lua original entry and dependency checks passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n'; return 1;
    }
}
