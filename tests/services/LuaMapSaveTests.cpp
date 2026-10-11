#include "War3MapCore/Hash.h"
#include "War3MapCore/Script/ScriptSaveBundle.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "../MapDataFixtures.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::Services;

void Require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::string Codes(const auto& result) {
    std::string text;
    for (const auto& diagnostic : result.diagnostics)
        text += diagnostic.code + ": " + diagnostic.message + "\n";
    return text;
}

bool HasCode(const auto& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& diagnostic) { return diagnostic.code == code; });
}

std::vector<std::byte> Bytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
}

std::string Text(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    Require(static_cast<bool>(output), "write original fixture: " + path.string());
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "read actual checker evidence: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string FileHash(const std::filesystem::path& path) {
    const auto result = Sha256File(path);
    Require(result.Succeeded() && result.value.has_value(), Codes(result));
    return *result.value;
}

std::vector<std::byte> Settings(std::int32_t version, std::int32_t language,
    std::string name = "Original Lua save fixture") {
    MapData::MapSettings settings;
    settings.formatVersion = version;
    settings.scriptLanguage = language;
    settings.tileset = 'L';
    settings.custom.emplace();
    settings.mapName = std::move(name);
    settings.description = "Original synthetic map, no game execution claim";
    settings.cameraBounds = {-128.0f, -128.0f, 128.0f, 128.0f, -128.0f, 128.0f, 128.0f, -128.0f};
    if (version == 31) {
        settings.gameVersion = {1, 32, 0, 0};
        settings.graphicsModes = 3;
        settings.gameDataVersion = 1;
    }
    const auto encoded = settings.Serialize();
    Require(encoded.Succeeded() && encoded.value.has_value(), Codes(encoded));
    const auto decoded = MapData::MapSettings::Parse(*encoded.value);
    Require(decoded.Succeeded() && decoded.value->SourceFormatVersion() == version
        && decoded.value->scriptLanguage == language, "source fixture must actually parse its W3I version/language");
    return *encoded.value;
}

LuaMapTargetRequest Target(const Script::LuaSyntaxChecker& checker) {
    LuaMapTargetRequest request;
    request.checker = checker;
    auto& target = request.profile.target;
    target.identity = "original-map-save-fixture/explicit-lua53";
    target.handleTypes = {{"placement", "handle"}};
    const auto api = [&](const char* name, const char* returns, std::initializer_list<const char*> parameters) {
        Script::LuaApiSignature entry{name, returns, {}};
        for (const auto* parameter : parameters) entry.parameters.emplace_back(parameter);
        target.apis.push_back(std::move(entry));
    };
    api("GetCameraMargin", "real", {"integer"});
    api("SetCameraBounds", "nothing", {"real", "real", "real", "real", "real", "real", "real", "real"});
    api("SetDayNightModels", "nothing", {"string", "string"});
    for (const auto* name : {"NewSoundEnvironment", "SetAmbientDaySound", "SetAmbientNightSound", "SetMapName", "SetMapDescription"})
        api(name, "nothing", {"string"});
    api("SetMapMusic", "nothing", {"string", "boolean", "integer"});
    api("InitBlizzard", "nothing", {});
    api("InitGenericPlayerSlots", "nothing", {});
    api("SetPlayers", "nothing", {"integer"});
    api("SetTeams", "nothing", {"integer"});
    api("SetGamePlacement", "nothing", {"placement"});
    target.constants = {{"CAMERA_MARGIN_LEFT", "integer"}, {"CAMERA_MARGIN_RIGHT", "integer"},
        {"CAMERA_MARGIN_BOTTOM", "integer"}, {"CAMERA_MARGIN_TOP", "integer"}, {"MAP_PLACEMENT_USE_MAP_SETTINGS", "placement"}};
    return request;
}

Script::LuaSyntaxChecker ActualChecker() {
    const auto size = GetEnvironmentVariableW(L"FORGE_YDWE_ROOT", nullptr, 0);
    Require(size > 1, "FORGE_YDWE_ROOT required; real Lua 5.3 execution may not be skipped");
    std::wstring root(size, L'\0');
    const auto count = GetEnvironmentVariableW(L"FORGE_YDWE_ROOT", root.data(), size);
    Require(count > 0 && count < size, "read actual test process YDWE root");
    root.resize(count);
    const auto executable = std::filesystem::path(root) / "bin/lua.exe";
    const auto captured = Script::CaptureLuaSyntaxChecker(executable, FileHash(executable),
        std::vector<std::filesystem::path>{executable.parent_path()});
    Require(captured.Succeeded() && captured.value && captured.value->Identity(), Codes(captured));
    return *captured.value;
}

struct Source {
    std::filesystem::path path;
    std::string hash;
    std::vector<std::byte> settings;
    std::vector<MapArchiveMember> members;
};

class Fixture {
public:
    std::filesystem::path root;
    Trigger::UiCatalog catalog;
    Trigger::TriggerDocument edited;
    Trigger::EncodedTriggerDocument originalPair, editedPair;
    LuaMapTargetRequest request;

    explicit Fixture(std::filesystem::path directory, bool classicOnly = false)
        : root(std::move(directory)), request(Target(classicOnly ? Script::LuaSyntaxChecker{} : ActualChecker())) {
        Require(std::filesystem::create_directories(root), "use a new private test artifact directory");
        Write(root / "catalog/ui/config", "base\n");
        Write(root / "catalog/ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,整数,integer\n");
        for (const auto* name : {"event.txt", "condition.txt", "action.txt", "call.txt"})
            Write(root / "catalog/ui/base" / name, "");
        auto loaded = Trigger::UiCatalog::Load(root / "catalog");
        Require(loaded.Succeeded() && loaded.value.has_value(), Codes(loaded));
        catalog = std::move(*loaded.value);

        MapDataFixtures::Fixture wtg, wct;
        wtg.Raw("WTG!"); wtg.Int(7); wtg.Int(0); wtg.Int(2); wtg.Int(1);
        wtg.String("Counter"); wtg.String("integer"); wtg.Int(1); wtg.Int(0);
        wtg.Int(1); wtg.Int(1); wtg.String("1"); wtg.Int(0);
        wct.Int(1); wct.String(""); wct.Int(0); wct.Int(0);
        originalPair = {std::move(wtg.bytes), std::move(wct.bytes)};
        auto parsed = Trigger::WtgWctCodec{}.Parse(originalPair.wtg, originalPair.wct, catalog);
        Require(parsed.Succeeded() && parsed.value && parsed.value->HasTrustedSourceSeal(), Codes(parsed));
        Require(parsed.value->variables.size() == 1 && parsed.value->variables[0].value.text == "1",
            "original GUI integer variable parsed with source seal");
        edited = std::move(*parsed.value);
        edited.variables[0].value.text = "27";
        const auto encoded = Trigger::WtgWctCodec{}.Serialize(edited, catalog);
        Require(encoded.Succeeded() && encoded.value.has_value(), Codes(encoded));
        editedPair = *encoded.value;
    }

    Source MakeSource(std::string_view name, std::int32_t version, std::int32_t language,
        std::initializer_list<std::string_view> scripts, bool payload = false,
        std::span<const MapArchiveMember> extra = {}) const {
        const auto directory = root / name;
        std::filesystem::create_directory(directory);
        Source source;
        source.path = directory / "source.w3x";
        source.settings = Settings(version, language);
        source.members = {{"war3map.w3i", source.settings}, {"war3map.wtg", originalPair.wtg},
            {"war3map.wct", originalPair.wct}, {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
            {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()}, {"unmanaged.bin", Bytes("original opaque bytes")}};
        for (const auto script : scripts) source.members.push_back({std::string(script),
            Bytes(script.ends_with(".lua") ? "function config() end\nfunction main() end\n"
                : "function config takes nothing returns nothing\nendfunction\nfunction main takes nothing returns nothing\nendfunction\n")});
        if (payload) {
            // 原创不透明字节只测试精确成员存在性；没有可执行AI正文，也不解析载荷。
            source.members.push_back({"tiara.ai", {std::byte{0xff}, std::byte{0}, std::byte{0x41}}});
            source.members.push_back({"tiara.blp", {std::byte{0x17}, std::byte{0}, std::byte{0x92}}});
        }
        source.members.insert(source.members.end(), extra.begin(), extra.end());
        const auto written = WriteVerifiedArchive(source.path, source.members);
        Require(written.Succeeded(), Codes(written));
        source.hash = FileHash(source.path);
        return source;
    }

    void Freeze(MapSaveTask& task, const Source& source) const {
        {
            const auto opened = MapArchiveSession::Open(source.path);
            Require(opened.Succeeded() && opened.value.has_value(), Codes(opened));
            const auto frozen = task.FreezeArchive(*opened.value);
            Require(frozen.Succeeded(), Codes(frozen));
        } // 真正运行时源会话已经销毁，只能使用任务拥有的快照。
        task.target = source.path.parent_path() / "candidate.w3x";
        task.triggerChanges = true;
        task.replacements = {{"war3map.wtg", editedPair.wtg}, {"war3map.wct", editedPair.wct}};
    }

    Result<ScriptSavePreparationState> Prepare(MapSaveTask& task, LuaMapTargetRequest selected) const {
        ScriptSaveInputSources sources;
        sources.triggers = std::make_unique<Trigger::TriggerDocument>(edited);
        sources.catalog = std::make_unique<Trigger::UiCatalog>(catalog);
        sources.layers = catalog.Layers();
        sources.expectedEffectiveLayers = catalog.Layers();
        sources.uiRoot = root / "catalog";
        sources.luaTarget = std::move(selected);
        sources.scriptMember = sources.luaTarget->dialect == Script::LuaMapSaveDialect::Classic127YDWE ? "war3map.j" : "war3map.lua";
        sources.documentRevision = 17;
        return PrepareScriptSaveInputs(task, std::move(sources));
    }

    void RequirePrepared(MapSaveTask& task, const LuaMapTargetRequest& selected) const {
        const auto result = Prepare(task, selected);
        Require(result.Succeeded() && task.script && task.script->luaTarget && !task.scriptSkipped, Codes(result));
        Require(task.script->toolchain.toolRoot.empty(), "Lua does not silently acquire a JASS tool root");
        Require(task.scriptLayers == catalog.Layers(), "actual effective catalog layers are retained");
    }
};

bool SameReplacements(const std::vector<MemberReplacement>& a, const std::vector<MemberReplacement>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& left, const auto& right) {
        return left.name == right.name && left.data == right.data;
    });
}

void PreservedSource(const Source& source) {
    Require(FileHash(source.path) == source.hash, "source archive SHA changed");
    const auto reopened = MapArchiveSession::Open(source.path);
    Require(reopened.Succeeded() && reopened.value.has_value(), Codes(reopened));
    for (const auto& original : source.members) {
        const auto actual = reopened.value->ReadMember(original.name);
        Require(actual.Succeeded() && actual.value && *actual.value == original.data,
            "source member changed: " + original.name);
    }
}

void RejectedWithoutPublication(const MapSaveTask& task, const Source& source) {
    Require(!task.succeeded && !task.nextSession && task.written.empty() && task.backup.empty()
        && !std::filesystem::exists(task.target), "rejected save published a candidate or baseline");
    PreservedSource(source);
}

void Run(MapSaveTask& task) {
    MapSaveService::Run(task);
    std::cout << "case=" << task.target.parent_path().filename().string() << " succeeded=" << task.succeeded
        << " rebuilt=" << task.scriptRebuilt << " source_sha=" << task.sourceSha256
        << " workspace=" << task.scriptWorkspace.string() << '\n';
}

void SuccessfulReopen(const Fixture& fixture, const Source& source, const MapSaveTask& task) {
    Require(task.executed && task.succeeded && task.scriptRebuilt && task.nextSession, Codes(task));
    const auto candidateHash = FileHash(task.target);
    Require(task.nextSession->SourceSha256() == candidateHash, "AfterSave did not bind actual candidate SHA");
    const auto reopened = MapArchiveSession::Open(task.target);
    Require(reopened.Succeeded() && reopened.value.has_value(), Codes(reopened));
    const std::array<std::string, 4> scriptNames{
        "war3map.lua", "scripts\\war3map.lua", "war3map.j", "scripts\\war3map.j"};
    const std::array<const MapArchiveSession*, 2> sessions{task.nextSession.get(), &*reopened.value};
    for (const auto* archive : sessions) {
        const auto exact = archive->CaptureExactMembers(scriptNames);
        Require(exact.Succeeded() && exact.value && exact.value->size() == 4, Codes(exact));
        Require((*exact.value)[0].present && !(*exact.value)[1].present && !(*exact.value)[2].present
            && !(*exact.value)[3].present, "saved map must contain exactly one canonical root script");
        const auto script = Text((*exact.value)[0].bytes);
        Require(script.find("function config()") != std::string::npos && script.find("function main()") != std::string::npos
            && script.find("udg_Counter") != std::string::npos && script.find("27") != std::string::npos,
            "full config/main and edited GUI initialization absent from Lua");
        const auto savedSettings = archive->ReadMember("war3map.w3i");
        Require(savedSettings.Succeeded() && savedSettings.value.has_value(), Codes(savedSettings));
        const auto settings = MapData::MapSettings::Parse(*savedSettings.value);
        Require(settings.Succeeded() && settings.value->SourceFormatVersion() == 31
            && settings.value->scriptLanguage == 1 && *savedSettings.value == Settings(31, 1),
            "selected language must be Lua while every other W3I byte is retained");
        const auto wtg = archive->ReadMember("war3map.wtg"), wct = archive->ReadMember("war3map.wct");
        Require(wtg.Succeeded() && wct.Succeeded() && *wtg.value == fixture.editedPair.wtg
            && *wct.value == fixture.editedPair.wct, "WTG and WCT pair were not committed together");
        const auto triggers = Trigger::WtgWctCodec{}.Parse(*wtg.value, *wct.value, fixture.catalog);
        Require(triggers.Succeeded() && triggers.value->variables.size() == 1
            && triggers.value->variables[0].value.text == "27", "actual reopened GUI edit is absent");
        const auto unmanaged = archive->ReadMember("unmanaged.bin");
        Require(unmanaged.Succeeded() && *unmanaged.value == Bytes("original opaque bytes"), "unmanaged bytes changed");
        const auto scriptHash = Sha256((*exact.value)[0].bytes);
        Require(scriptHash.Succeeded(), Codes(scriptHash));
        const auto work = task.scriptWorkspace / "lua-check";
        Require(FileHash(work / "war3map.lua") == *scriptHash.value
            && Read(work / "lua-syntax.log").find("FORGE_LUA_SYNTAX_OK_V1 " + *scriptHash.value) != std::string::npos,
            "saved Lua lacks the real Lua 5.3 checker result for its exact bytes");
    }
    PreservedSource(source);
    std::cout << "candidate=" << task.target.string() << " sha=" << candidateHash << '\n';
}

void SavesLuaAndExplicitlySwitchesLanguage(Fixture& fixture) {
    const auto lua = fixture.MakeSource("lua-v31", 31, 1, {"war3map.lua"});
    MapSaveTask task;
    fixture.Freeze(task, lua);
    fixture.RequirePrepared(task, fixture.request);
    Run(task);
    SuccessfulReopen(fixture, lua, task);
    Require(task.generatedScriptMembers == std::vector<std::string>{"war3map.lua"},
        "generated script binding excluded members not owned by this save");

    const auto jass = fixture.MakeSource("jass-to-lua-v31", 31, 0,
        {"war3map.j", "scripts\\war3map.j", "scripts\\war3map.lua"});
    MapSaveTask switched;
    fixture.Freeze(switched, jass);
    auto selection = fixture.request;
    selection.changeLanguageToLua = true;
    fixture.RequirePrepared(switched, selection);
    Require(switched.replacements.size() == 2 && switched.removals.empty(),
        "target preparation prematurely staged language or old-script removal");
    Run(switched);
    SuccessfulReopen(fixture, jass, switched);
    Require(switched.generatedScriptMembers.size() == 4 && switched.generatedScriptMembers.front() == "war3map.lua",
        "language switch lost generated or explicitly removed script members");
    for (const auto* name:{"war3map.j","scripts\\war3map.j","scripts\\war3map.lua"})
        Require(std::find(switched.generatedScriptMembers.begin(),switched.generatedScriptMembers.end(),name)
            !=switched.generatedScriptMembers.end(), "removed script was not bound to save chain");
}

void FailedBuildKeepsOriginalTransaction(Fixture& fixture) {
    const auto source = fixture.MakeSource("generation-rejected", 31, 0, {"war3map.j", "scripts\\war3map.j"});
    MapSaveTask task;
    fixture.Freeze(task, source);
    auto selection = fixture.request;
    selection.changeLanguageToLua = true;
    std::erase_if(selection.profile.target.apis, [](const auto& api) { return api.name == "SetMapName"; });
    fixture.RequirePrepared(task, selection);
    const auto before = task.replacements;
    const auto removals = task.removals;
    Run(task);
    Require(task.executed && !task.scriptRebuilt && HasCode(task, "LUA_MAP_API_MISSING")
        && HasCode(task, "REPACK_SCRIPT_REJECTED"), Codes(task));
    Require(SameReplacements(before, task.replacements) && task.removals == removals,
        "failed complete-script generation changed replacements or language/removals");
    RejectedWithoutPublication(task, source);
}

void StaleTargetKeepsOriginalTransaction(Fixture& fixture) {
    for (const bool changeSettings : {false, true}) {
        const auto source = fixture.MakeSource(changeSettings ? "stale-w3i" : "stale-document", 31, 0, {"war3map.j"});
        MapSaveTask task;
        fixture.Freeze(task, source);
        auto selection = fixture.request;
        selection.changeLanguageToLua = true;
        fixture.RequirePrepared(task, selection);
        if (changeSettings) task.replacements.push_back({"war3map.w3i", Settings(31, 0, "Later authored title")});
        else ++task.script->documentRevision;
        const auto before = task.replacements;
        const auto removals = task.removals;
        Run(task);
        Require(!task.scriptRebuilt && HasCode(task, changeSettings ? "LUA_MAP_SETTINGS_STALE" : "LUA_MAP_TARGET_STALE"), Codes(task));
        Require(SameReplacements(before, task.replacements) && task.removals == removals,
            "stale target changed authored transaction data");
        RejectedWithoutPublication(task, source);
    }
}

void RejectsUnselectedAndClassicTargets(Fixture& fixture) {
    const auto classic = fixture.MakeSource("classic-refused", 25, 0, {"war3map.j"});
    MapSaveTask legacy;
    fixture.Freeze(legacy, classic);
    const auto before = legacy.replacements;
    auto selection = fixture.request;
    selection.changeLanguageToLua = true;
    const auto incompatible = fixture.Prepare(legacy, selection);
    Require(!incompatible.Succeeded() && !legacy.script && HasCode(incompatible, "LUA_MAP_TARGET_VERSION_UNSUPPORTED"), Codes(incompatible));
    Require(SameReplacements(before, legacy.replacements) && legacy.removals.empty(), "v25 refusal altered transaction");
    RejectedWithoutPublication(legacy, classic);

    const auto unboundSource = fixture.MakeSource("uncaptured-checker", 31, 1, {"war3map.lua"});
    MapSaveTask unbound;
    fixture.Freeze(unbound, unboundSource);
    selection = fixture.request;
    selection.checker = {};
    const auto missing = fixture.Prepare(unbound, selection);
    Require(!missing.Succeeded() && !unbound.script && HasCode(missing, "LUA_MAP_TARGET_CHECKER_UNCAPTURED"), Codes(missing));
    RejectedWithoutPublication(unbound, unboundSource);

    const auto noTargetSource = fixture.MakeSource("v31-without-target", 31, 1, {"war3map.lua"});
    MapSaveTask noTarget;
    fixture.Freeze(noTarget, noTargetSource);
    Run(noTarget);
    Require(HasCode(noTarget, "SAVE_MAP_FORMAT_UNSUPPORTED"), Codes(noTarget));
    RejectedWithoutPublication(noTarget, noTargetSource);

    const auto jass = fixture.MakeSource("language-switch-not-selected", 31, 0, {"war3map.j"});
    MapSaveTask unselected;
    fixture.Freeze(unselected, jass);
    const auto refused = fixture.Prepare(unselected, fixture.request);
    Require(!refused.Succeeded() && !unselected.script
        && HasCode(refused, "LUA_MAP_TARGET_LANGUAGE_SELECTION_REQUIRED"), Codes(refused));
    RejectedWithoutPublication(unselected, jass);
}

void LateLeaseFailureDoesNotPublish(Fixture& fixture) {
    const auto source = fixture.MakeSource("publish-refused", 31, 0, {"war3map.j"});
    MapSaveTask task;
    fixture.Freeze(task, source);
    auto selection = fixture.request;
    selection.changeLanguageToLua = true;
    fixture.RequirePrepared(task, selection);
    bool called = false;
    task.beforePublish = [&] {
        called = true;
        return Status::Failure("LuaMapSaveTest", "TEST_PUBLISH_REFUSED", "original test lease changed");
    };
    Run(task);
    Require(called && task.scriptRebuilt && HasCode(task, "TEST_PUBLISH_REFUSED"), Codes(task));
    // 这里完整编译已经成功，允许任务持有完整结果；晚失败仍绝不发布归档。
    RejectedWithoutPublication(task, source);
}

void OpaqueMemHackPresenceRejectsLua(Fixture& fixture) {
    const auto source = fixture.MakeSource("opaque-memhack-refused", 31, 0, {"war3map.j"}, true);
    MapSaveTask task;
    fixture.Freeze(task, source);
    auto selection = fixture.request;
    selection.changeLanguageToLua = true;
    fixture.RequirePrepared(task, selection);
    const auto before = task.replacements;
    Run(task);
    Require(!task.scriptRebuilt && HasCode(task, "LUA_MEMHACK_UNSUPPORTED"), Codes(task));
    Require(SameReplacements(before, task.replacements) && task.removals.empty(), "opaque dependency rejection altered transaction");
    RejectedWithoutPublication(task, source);
}

std::vector<MapArchiveMember> ClassicLibraries() {
    // 原创小型声明夹具；不复制游戏或YDWE脚本，也不执行这些空函数。
    std::string common =
        "type placement extends handle\ntype player extends handle\ntype trigger extends handle\ntype alliancetype extends handle\n"
        "native ConvertPlacement takes integer i returns placement\n"
        "native ConvertAllianceType takes integer i returns alliancetype\n"
        "native Cheat takes string text returns nothing\n"
        "native SetMapName takes string text returns nothing\n"
        "native SetMapDescription takes string text returns nothing\n"
        "native SetPlayers takes integer count returns nothing\n"
        "native SetTeams takes integer count returns nothing\n"
        "native SetGamePlacement takes placement mode returns nothing\n"
        "native SetMapMusic takes string music, boolean random, integer index returns nothing\n"
        "native GetCameraMargin takes integer margin returns real\n"
        "native SetCameraBounds takes real a, real b, real c, real d, real e, real f, real g, real h returns nothing\n"
        "native SetDayNightModels takes string day, string night returns nothing\n"
        "native NewSoundEnvironment takes string environment returns nothing\n"
        "native SetPlayerAlliance takes player a, player b, alliancetype kind, boolean value returns nothing\n"
        "native TriggerEvaluate takes trigger which returns boolean\n"
        "native TriggerExecute takes trigger which returns nothing\n"
        "globals\nconstant placement MAP_PLACEMENT_USE_MAP_SETTINGS = ConvertPlacement(0)\n";
    for (const auto* name : {"CAMERA_MARGIN_LEFT", "CAMERA_MARGIN_RIGHT", "CAMERA_MARGIN_BOTTOM", "CAMERA_MARGIN_TOP"})
        common += std::string("constant integer ") + name + " = 0\n";
    for (const auto* name : {"ALLIANCE_PASSIVE", "ALLIANCE_HELP_REQUEST", "ALLIANCE_HELP_RESPONSE", "ALLIANCE_SHARED_XP",
            "ALLIANCE_SHARED_SPELLS", "ALLIANCE_SHARED_VISION", "ALLIANCE_SHARED_CONTROL", "ALLIANCE_SHARED_ADVANCED_CONTROL"})
        common += std::string("constant alliancetype ") + name + " = ConvertAllianceType(0)\n";
    common += "endglobals\n";
    const std::string blizzard =
        "function InitGenericPlayerSlots takes nothing returns nothing\nendfunction\n"
        "function SetAmbientDaySound takes string sound returns nothing\nendfunction\n"
        "function SetAmbientNightSound takes string sound returns nothing\nendfunction\n"
        "function InitBlizzard takes nothing returns nothing\nendfunction\n";
    return {{"common.j", Bytes(common)}, {"blizzard.j", Bytes(blizzard)}};
}

void ClassicSaveTransactions(Fixture& fixture) {
    auto selection = fixture.request;
    selection.dialect = Script::LuaMapSaveDialect::Classic127YDWE;
    auto& target = selection.profile.target;
    target.handleTypes.insert(target.handleTypes.end(), {{"player", "handle"}, {"trigger", "handle"}, {"alliancetype", "handle"}});
    target.apis.push_back({"TriggerEvaluate", "boolean", {"trigger"}});
    target.apis.push_back({"TriggerExecute", "nothing", {"trigger"}});
    target.apis.push_back({"SetPlayerAlliance", "nothing", {"player", "player", "alliancetype", "boolean"}});
    for (const auto* name : {"ALLIANCE_PASSIVE", "ALLIANCE_HELP_REQUEST", "ALLIANCE_HELP_RESPONSE", "ALLIANCE_SHARED_XP",
            "ALLIANCE_SHARED_SPELLS", "ALLIANCE_SHARED_VISION", "ALLIANCE_SHARED_CONTROL", "ALLIANCE_SHARED_ADVANCED_CONTROL"})
        target.constants.push_back({name, "alliancetype"});
    auto libraries = ClassicLibraries();
    const auto source = fixture.MakeSource("classic-complete", 25, 0, {"war3map.j"}, false, libraries);
    MapSaveTask task;
    fixture.Freeze(task, source); task.requireGeneratedNativeByteEquality = true;
    fixture.RequirePrepared(task, selection); Run(task);
    Require(task.succeeded && task.scriptRebuilt && task.nextSession, Codes(task));
    PreservedSource(source);
    const auto reopened = MapArchiveSession::Open(task.target);
    Require(reopened.Succeeded() && reopened.value, Codes(reopened));
    const auto savedSettings = reopened.value->ReadMember("war3map.w3i");
    Require(savedSettings.Succeeded() && savedSettings.value && *savedSettings.value == source.settings,
        "classic save changed source W3I bytes");
    for (const auto* name : {"war3map.j", "audastra\\map.lua", "audastra\\compat.lua", "audastra\\bootstrap.lua",
            "audastra\\objects.lua", "audastra\\continue.lua", "audastra\\generated.manifest"})
        Require(reopened.value->HasMember(name), std::string("missing classic saved member ") + name);
    Require(!reopened.value->HasMember("war3map.lua"), "classic save accidentally emitted v31 root Lua script");
    std::vector<MemberReplacement> owned;
    for (const auto* name : {"war3map.j", "audastra\\map.lua", "audastra\\compat.lua", "audastra\\bootstrap.lua",
            "audastra\\objects.lua", "audastra\\continue.lua", "audastra\\generated.manifest"}) {
        const auto bytes = reopened.value->ReadMember(name); Require(bytes.Succeeded() && bytes.value, Codes(bytes));
        owned.push_back({name, *bytes.value});
    }
    std::vector<MemberReplacement> pending{{"resource/owned.bin", Bytes("untouched")}};
    const auto originalOwned = owned;
    Require(!Script::MergeScriptSaveMembers(pending, {}, owned).Succeeded()
        && pending.size() == 1 && owned.front().data == originalOwned.front().data,
        "ordinary merge must retain its original resource rejection and atomicity");
    std::swap(owned[1], owned[2]);
    Require(!Script::MergeClassicLuaSaveMembers(pending, {}, owned).Succeeded() && pending.size() == 1,
        "classic merge cannot accept an incomplete or reordered bundle");
    owned = originalOwned;
    Require(!Script::MergeClassicLuaSaveMembers(pending, {"audastra/map.lua"}, owned).Succeeded() && pending.size() == 1,
        "classic generated module cannot also be removed");
    Require(Script::MergeClassicLuaSaveMembers(pending, {}, owned).Succeeded() && pending.size() == 8 && owned.empty(),
        "complete verified classic domain merges atomically with unrelated resources");
    const auto lua = reopened.value->ReadMember("audastra\\map.lua");
    Require(lua.Succeeded() && std::string(reinterpret_cast<const char*>(lua.value->data()), lua.value->size()).find("udg_Counter = 27") != std::string::npos,
        "edited typed T globals did not reach classic saved module");

    Source second{task.target, FileHash(task.target), source.settings, {reopened.value->Members().begin(), reopened.value->Members().end()}};
    MapSaveTask repeated;
    fixture.Freeze(repeated, second); repeated.target = task.target.parent_path() / "candidate-second.w3x";
    repeated.requireGeneratedNativeByteEquality = true;
    fixture.RequirePrepared(repeated, selection); Run(repeated);
    Require(repeated.succeeded && repeated.scriptRebuilt, Codes(repeated));
    PreservedSource(second);

    Source tampered = second;
    tampered.path = fixture.root / "classic-tampered" / "source.w3x";
    std::filesystem::create_directory(tampered.path.parent_path());
    for (auto& member : tampered.members) if (member.name == "audastra\\map.lua") member.data.push_back(std::byte{'\n'});
    Require(WriteVerifiedArchive(tampered.path, tampered.members).Succeeded(), "write original modified-module fixture");
    tampered.hash = FileHash(tampered.path);
    MapSaveTask changedSource;
    fixture.Freeze(changedSource, tampered); fixture.RequirePrepared(changedSource, selection); Run(changedSource);
    Require(HasCode(changedSource, "CLASSIC_LUA_GENERATED_SOURCE_CHANGED"), Codes(changedSource));
    RejectedWithoutPublication(changedSource, tampered);

    auto collision = libraries;
    collision.push_back({"audastra\\map.lua", Bytes("-- authored module retained\n")});
    const auto protectedSource = fixture.MakeSource("classic-collision", 25, 0, {"war3map.j"}, false, collision);
    MapSaveTask refused;
    fixture.Freeze(refused, protectedSource); fixture.RequirePrepared(refused, selection); Run(refused);
    Require(HasCode(refused, "CLASSIC_LUA_RESERVED_MEMBER_CONFLICT"), Codes(refused));
    RejectedWithoutPublication(refused, protectedSource);

    const auto noBases = fixture.MakeSource("classic-no-libraries", 25, 0, {"war3map.j"});
    MapSaveTask incomplete;
    fixture.Freeze(incomplete, noBases); fixture.RequirePrepared(incomplete, selection); Run(incomplete);
    Require(!incomplete.scriptRebuilt, "classic save skipped its complete JASS base-library checking");
    RejectedWithoutPublication(incomplete, noBases);

    auto badTarget = selection;
    for (auto& api : badTarget.profile.target.apis) if (api.name == "GetCameraMargin") api.returns = "integer";
    const auto wrong = fixture.MakeSource("classic-wrong-native", 25, 0, {"war3map.j"}, false, libraries);
    MapSaveTask mismatch;
    fixture.Freeze(mismatch, wrong); fixture.RequirePrepared(mismatch, badTarget); Run(mismatch);
    Require(!mismatch.scriptRebuilt, "classic self-reported native signature bypassed trusted common declarations");
    RejectedWithoutPublication(mismatch, wrong);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const bool classicOnly = argc > 1 && std::string_view(argv[1]) == "--classic";
        const auto root = argc > 1 && !classicOnly ? std::filesystem::absolute(argv[1]) : std::filesystem::temp_directory_path()
            / ("forge-lua-map-save-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::cout << "Original synthetic map-save artifacts: " << root.string() << '\n';
        Fixture fixture(root, classicOnly);
        if (classicOnly) {
            ClassicSaveTransactions(fixture);
            std::cout << "Classic Lua: embedded compilation, complete JASS semantics, MPQ reopen, repeated save and source preservation passed. No YDWE or game was used.\n";
            return 0;
        }
        SavesLuaAndExplicitlySwitchesLanguage(fixture);
        FailedBuildKeepsOriginalTransaction(fixture);
        StaleTargetKeepsOriginalTransaction(fixture);
        RejectsUnselectedAndClassicTargets(fixture);
        LateLeaseFailureDoesNotPublish(fixture);
        OpaqueMemHackPresenceRejectsLua(fixture);
        Require(FileHash(fixture.request.checker.Identity()->executable) == fixture.request.checker.Identity()->sha256,
            "installed Lua checker changed");
        std::cout << "Full Lua save: actual Lua 5.3 syntax check, MPQ publication, AfterSave and independent reopen passed.\n"
            << "No author map, AI body, native/config/main execution or game acceptance was used.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lua map save failure: " << error.what() << '\n';
        return 1;
    }
}
