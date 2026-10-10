#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Script/LuaAuthorCode.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "../MapDataFixtures.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <stop_token>

// 原创小 MPQ 的真实公共保存往返；不启动游戏，不宣称 native/进图签收。
namespace {
using namespace War3Map;
using namespace War3Map::Services;
using Script::LuaAuthorCodeEditor;
using Trigger::TriggerCodeEditor;

constexpr std::string_view kOriginalHeader = "-- 原生 Lua 作者头部\nNativeCounter = 1\nNativeHeaderTrigger = CreateTrigger()\n"
    "function NativeHelper() return NativeCounter end\n";
constexpr std::string_view kEditedHeader = "-- 原生 Lua 作者头部，编辑后\nNativeCounter = 41\nNativeHeaderTrigger = CreateTrigger()\n"
    "function NativeHelper() return NativeCounter end\n";
constexpr std::string_view kOriginalBody = "function InitTrig_ExistingLua()\n    gg_trg_ExistingLua = CreateTrigger()\nend\n";
constexpr std::string_view kEditedBody = "-- 原生 Lua 自定义触发器\nfunction InitTrig_ExistingLua()\n"
    "    gg_trg_ExistingLua = CreateTrigger()\n    NativeCounter = NativeHelper() + 1\nend\n";

void Require(bool value, std::string_view reason) { if (!value) throw std::runtime_error(std::string(reason)); }
std::string Codes(const auto& result) {
    std::string text;
    for (const auto& diagnostic : result.diagnostics) text += diagnostic.code + ": " + diagnostic.message + "\n";
    return text;
}
bool HasCode(const auto& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& value) { return value.code == code; });
}
template<class T> T Take(Result<T> result, std::string_view label) {
    if (!result.Succeeded() || !result.value) throw std::runtime_error(std::string(label) + ": " + Codes(result));
    return std::move(*result.value);
}
void Good(const Status& status, std::string_view label) { Require(status.Succeeded(), std::string(label) + ": " + Codes(status)); }
std::wstring Env(const wchar_t* key) {
    SetLastError(ERROR_SUCCESS);
    const auto size = GetEnvironmentVariableW(key, nullptr, 0);
    if (!size) {
        const auto error = GetLastError();
        Require(error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND,
            "read process-local opt-in environment");
        return {};
    }
    std::wstring text(size, L'\0');
    SetLastError(ERROR_SUCCESS);
    const auto count = GetEnvironmentVariableW(key, text.data(), size);
    if (!count) {
        const auto error = GetLastError();
        Require(error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND,
            "read process-local opt-in environment");
        return {};
    }
    Require(count < size, "read process-local opt-in environment");
    text.resize(count);
    return text;
}
std::vector<std::byte> Bytes(std::string_view text) {
    const auto first = reinterpret_cast<const std::byte*>(text.data()); return {first, first + text.size()};
}
std::string Text(std::span<const std::byte> bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }
std::string Hash(const std::filesystem::path& path) { return Take(Sha256File(path), "actual file SHA"); }
std::string Read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary); Require(static_cast<bool>(stream), "read original fixture/receipt");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void WriteExact(const std::filesystem::path& path, std::string_view text) {
    if (std::filesystem::exists(path)) { Require(Read(path) == text, "existing owned fixture differs; refusing overwrite"); return; }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary); stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    Require(static_cast<bool>(stream), "write original small catalog fixture");
}
std::vector<std::byte> Settings(std::int32_t version, std::int32_t language, std::string name = "Native Lua C4 original fixture") {
    MapData::MapSettings settings;
    settings.formatVersion = version; settings.scriptLanguage = language; settings.tileset = 'L'; settings.custom.emplace();
    settings.mapName = std::move(name); settings.description = "Original synthetic core/save fixture; no game runtime proof";
    settings.cameraBounds = {-128.0f, -128.0f, 128.0f, 128.0f, -128.0f, 128.0f, 128.0f, -128.0f};
    if (version == 31) { settings.gameVersion = {1, 32, 0, 0}; settings.graphicsModes = 3; settings.gameDataVersion = 1; }
    auto bytes = Take(settings.Serialize(), "serialize original W3I");
    const auto parsed = Take(MapData::MapSettings::Parse(bytes), "actually parse W3I");
    Require(parsed.SourceFormatVersion() == version && parsed.scriptLanguage == language, "actual W3I layout/language");
    return bytes;
}
LuaMapTargetRequest Target() {
    const auto ydwe = std::filesystem::path(Env(L"FORGE_YDWE_ROOT"));
    Require(ydwe.is_absolute(), "FORGE_YDWE_ROOT must be an absolute local readonly installation");
    const auto executable = ydwe / "bin/lua.exe";
    LuaMapTargetRequest request;
    request.checker = Take(Script::CaptureLuaSyntaxChecker(executable, Hash(executable),
        std::vector<std::filesystem::path>{executable.parent_path()}), "capture actual Lua checker");
    auto& target = request.profile.target; target.identity = "native-lua-c4-original-fixture/lua53";
    target.handleTypes = {{"placement", "handle"}, {"trigger", "handle"}, {"boolexpr", "handle"},
        {"triggercondition", "handle"}, {"triggeraction", "handle"}};
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
    api("InitBlizzard", "nothing", {}); api("InitGenericPlayerSlots", "nothing", {});
    api("SetPlayers", "nothing", {"integer"}); api("SetTeams", "nothing", {"integer"}); api("SetGamePlacement", "nothing", {"placement"});
    api("CreateTrigger", "trigger", {}); api("Condition", "boolexpr", {"code"});
    api("TriggerAddCondition", "triggercondition", {"trigger", "boolexpr"}); api("TriggerAddAction", "triggeraction", {"trigger", "code"});
    api("ConditionalTriggerExecute", "nothing", {"trigger"});
    target.constants = {{"CAMERA_MARGIN_LEFT", "integer"}, {"CAMERA_MARGIN_RIGHT", "integer"},
        {"CAMERA_MARGIN_BOTTOM", "integer"}, {"CAMERA_MARGIN_TOP", "integer"}, {"MAP_PLACEMENT_USE_MAP_SETTINGS", "placement"}};
    return request;
}

struct Fixture {
    std::filesystem::path root;
    Trigger::UiCatalog catalog;
    Trigger::TriggerDocument original, edited;
    Trigger::EncodedTriggerDocument originalPair, editedPair;
    std::vector<std::byte> settings = Settings(31, 1);
    LuaMapTargetRequest request = Target();
    StableId existing = 0, created = 0;
    std::vector<Trigger::TriggerCodeEditReceipt> receipts;

    explicit Fixture(std::filesystem::path directory) : root(std::move(directory)) {
        Require(root.is_absolute(), "artifact root must be absolute");
        WriteExact(root / "catalog/ui/config", "base\n");
        WriteExact(root / "catalog/ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,整数,integer\n");
        for (const auto* name : {"event.txt", "condition.txt", "action.txt", "call.txt"}) WriteExact(root / "catalog/ui/base" / name, "");
        catalog = Take(Trigger::UiCatalog::Load(root / "catalog"), "load actual synthetic catalog");
        auto constructed = Take(Trigger::WtgWctCodec{}.CreateEmpty(catalog), "create original trusted empty fixture");
        auto header = Take(LuaAuthorCodeEditor::PrepareText(settings, constructed, catalog, {Trigger::CodeBlockKind::GlobalHeader, 0}, kOriginalHeader), "construct source header");
        (void)Take(TriggerCodeEditor::Apply(constructed, catalog, header), "construct original source header transaction");
        const auto custom = Take(LuaAuthorCodeEditor::PrepareNewCustom(settings, constructed, catalog, -1,
            "ExistingLua", kOriginalBody, TriggerCodeEditor::kDefaultMaxEditedBytes, "Original Lua"), "construct original source custom");
        (void)Take(TriggerCodeEditor::Apply(constructed, catalog, custom), "construct original source custom transaction");
        constructed.triggers.back().runOnInit = 1; // 原创源 fixture 的标志，正式编辑验收从下方重新解析后开始。
        (void)Take(Trigger::TriggerDocumentEditor::AddTrigger(constructed, constructed.categories.front().categoryId, "OriginalGui"), "original empty GUI fixture");
        originalPair = Take(Trigger::WtgWctCodec{}.Serialize(constructed, catalog), "encode complete source fixture pair");
        original = Take(Trigger::WtgWctCodec{}.Parse(originalPair.wtg, originalPair.wct, catalog), "actual source pair/source seal");
        Require(original.HasTrustedSourceSeal(), "actual serialized source seal");
        const auto found = std::find_if(original.triggers.begin(), original.triggers.end(), [](const auto& row) { return row.name.text == "ExistingLua"; });
        Require(found != original.triggers.end(), "actual native custom source record"); existing = found->id;
        edited = original;
        auto plan = Take(LuaAuthorCodeEditor::PrepareText(settings, edited, catalog, {Trigger::CodeBlockKind::GlobalHeader, 0}, kEditedHeader), "edit native header");
        receipts.push_back(Take(TriggerCodeEditor::Apply(edited, catalog, plan), "apply sealed header edit"));
        plan = Take(LuaAuthorCodeEditor::PrepareText(settings, edited, catalog, {Trigger::CodeBlockKind::CustomTrigger, existing}, kEditedBody), "edit native custom");
        receipts.push_back(Take(TriggerCodeEditor::Apply(edited, catalog, plan), "apply sealed custom edit"));
        plan = Take(LuaAuthorCodeEditor::PrepareNewCustom(settings, edited, catalog, original.categories.front().categoryId, "NewLua"), "new explicit Lua skeleton");
        created = plan.Target().triggerId;
        receipts.push_back(Take(TriggerCodeEditor::Apply(edited, catalog, plan), "apply sealed new Lua custom"));
        editedPair = Take(Trigger::WtgWctCodec{}.Serialize(edited, catalog), "encoded actual edited pair");
    }
    std::shared_ptr<const Script::PreparedLuaAuthorCode> Capture(const Trigger::TriggerDocument& document) const {
        return std::make_shared<const Script::PreparedLuaAuthorCode>(Take(Script::CaptureNativeLuaAuthorCode(settings, document, catalog), "capture native author seal"));
    }
};

void UndoRedo(Fixture& fixture) {
    for (auto it = fixture.receipts.rbegin(); it != fixture.receipts.rend(); ++it)
        Good(TriggerCodeEditor::Replay(fixture.edited, fixture.catalog, *it, false), "undo same sealed receipt");
    const auto undone = Take(Trigger::WtgWctCodec{}.Serialize(fixture.edited, fixture.catalog), "serialize undo");
    Require(undone.wtg == fixture.originalPair.wtg && undone.wct == fixture.originalPair.wct, "undo returns exact original WTG/WCT bytes");
    for (const auto& receipt : fixture.receipts) Good(TriggerCodeEditor::Replay(fixture.edited, fixture.catalog, receipt, true), "redo same sealed receipt");
    const auto redone = Take(Trigger::WtgWctCodec{}.Serialize(fixture.edited, fixture.catalog), "serialize redo");
    Require(redone.wtg == fixture.editedPair.wtg && redone.wct == fixture.editedPair.wct, "redo returns exact edited WTG/WCT bytes");
    const auto* created = Trigger::TriggerDocumentEditor::FindTrigger(fixture.edited, fixture.created);
    Require(created && created->customText.text == "function InitTrig_NewLua()\n    gg_trg_NewLua = CreateTrigger()\nend\n",
        "new Lua wrapper emits its correct WE-named Lua skeleton and no JASS takes/endfunction");
}

void QualificationAndSealedIdentity(const Fixture& fixture) {
    const auto native = fixture.Capture(fixture.edited);
    Good(native->Verify(fixture.edited, fixture.catalog), "current native sealed identity");
    Require(native->Identity() && native->Identity()->sourceSettingsSha256 == Take(Sha256(fixture.settings), "actual source W3I SHA"), "source W3I bytes bound");
    const auto sourceJass = Settings(31, 0);
    auto denied = Script::CaptureNativeLuaAuthorCode(sourceJass, fixture.edited, fixture.catalog);
    Require(!denied.Succeeded() && HasCode(denied, "LUA_AUTHOR_SOURCE_LANGUAGE"), "JASS source cannot qualify despite a Lua target");
    const auto legacy = Settings(25, 0);
    denied = Script::CaptureNativeLuaAuthorCode(legacy, fixture.edited, fixture.catalog);
    Require(!denied.Succeeded() && HasCode(denied, "LUA_AUTHOR_SOURCE_LANGUAGE"), "classic v25 cannot infer native Lua from target existence");
    auto blocked = LuaAuthorCodeEditor::PrepareText(sourceJass, fixture.edited, fixture.catalog, {Trigger::CodeBlockKind::GlobalHeader, 0}, "broken(");
    Require(!blocked.Succeeded() && HasCode(blocked, "LUA_AUTHOR_SOURCE_LANGUAGE"), "Lua wrapper editing is source-language gated");
    auto unfinished = LuaAuthorCodeEditor::PrepareText(fixture.settings, fixture.edited, fixture.catalog, {Trigger::CodeBlockKind::GlobalHeader, 0}, "broken(");
    Require(unfinished.Succeeded(), "author can stage incomplete Lua through the shared edit transaction");
    auto invalid = fixture.edited;
    (void)Take(TriggerCodeEditor::Apply(invalid, fixture.catalog, *unfinished.value), "stage unfinished Lua");
    Require(!Script::CaptureNativeLuaAuthorCode(fixture.settings, invalid, fixture.catalog).Succeeded(), "save capture still rejects unfinished syntax");
    auto mutated = fixture.edited; ++mutated.triggers.front().id;
    Require(!native->Verify(mutated, fixture.catalog).Succeeded(), "stable identity tampering cannot reuse the source seal");
    mutated = fixture.edited; mutated.wct.customCode.encoding = Trigger::TextEncoding::Latin1;
    Require(!native->Verify(mutated, fixture.catalog).Succeeded(), "encoding metadata cannot reuse the source seal");
    mutated = fixture.edited; ++mutated.triggers.front().origin.offset;
    Require(!native->Verify(mutated, fixture.catalog).Succeeded(), "origin metadata cannot reuse the source seal");

    auto forged = fixture.request; forged.profile.target.nativeAuthorCode = native; forged.changeLanguageToLua = true;
    auto selected = PrepareMapScriptTargetSelection(sourceJass, sourceJass, fixture.edited, fixture.catalog, forged, 31);
    Require(!selected.Succeeded() && HasCode(selected, "LUA_AUTHOR_SOURCE_LANGUAGE"), "direct public target API rejects cross-language author seal");
    const auto otherLua = Settings(31, 1, "another source's real W3I");
    selected = PrepareMapScriptTargetSelection(otherLua, otherLua, fixture.edited, fixture.catalog, forged, 31);
    Require(!selected.Succeeded() && HasCode(selected, "LUA_AUTHOR_SOURCE_IDENTITY"), "direct public target API rejects another Lua source W3I identity");
    selected = PrepareMapScriptTargetSelection(fixture.settings, fixture.settings, fixture.edited, fixture.catalog, forged, 31);
    Require(selected.Succeeded() && selected.value && selected.value->Profile()->target.nativeAuthorCode,
        "direct public target API accepts matching real native source and current body seal");
}

void InitializerContracts(const Fixture& fixture) {
    const auto check = [&](std::string_view body, bool accepted, std::string_view code = "LUA_AUTHOR_INITIALIZER_MISSING") {
        auto document = fixture.edited;
        const auto plan = Take(LuaAuthorCodeEditor::PrepareText(fixture.settings, document, fixture.catalog,
            {Trigger::CodeBlockKind::CustomTrigger, fixture.existing}, body), "initializer contract edit");
        (void)Take(TriggerCodeEditor::Apply(document, fixture.catalog, plan), "initializer contract sealed apply");
        const auto result = Script::CaptureNativeLuaAuthorCode(fixture.settings, document, fixture.catalog);
        Require(accepted ? result.Succeeded() : (!result.Succeeded() && HasCode(result, code)),
            std::string("actual Lua initializer declaration qualification: ") + Codes(result));
    };
    check("-- function InitTrig_ExistingLua() end\nText = [=[function InitTrig_ExistingLua() end]=]\n", false);
    check("local function InitTrig_ExistingLua() end\n", false);
    check("local InitTrig_ExistingLua\nInitTrig_ExistingLua = function() end\n", false);
    check("Container = {InitTrig_ExistingLua = function() end}\n", false);
    check("function Outer() function InitTrig_ExistingLua() end end\n", false);
    check("if true then function InitTrig_ExistingLua() end end\n", false);
    check("for InitTrig_ExistingLua = function() end, 2 do end\n", false);
    check("InitTrig_ExistingLua = function() gg_trg_ExistingLua = CreateTrigger() end\n", true);
    check("function InitTrig_ExistingLua() end\nlocal InitTrig_ExistingLua = nil\n", false, "LUA_AUTHOR_INITIALIZER_SHADOWED");
    const auto shadow = [&](Trigger::CodeBlockTarget target, std::string_view body, bool accepted) {
        auto document = fixture.edited;
        const auto plan = Take(LuaAuthorCodeEditor::PrepareText(fixture.settings, document, fixture.catalog, target, body), "cross-block lexical shadow edit");
        (void)Take(TriggerCodeEditor::Apply(document, fixture.catalog, plan), "cross-block lexical shadow apply");
        const auto result = Script::CaptureNativeLuaAuthorCode(fixture.settings, document, fixture.catalog);
        Require(accepted ? result.Succeeded() : !result.Succeeded() && HasCode(result, "LUA_AUTHOR_INITIALIZER_SHADOWED"),
            std::string("same chunk top-level local initialization contract: ") + Codes(result));
    };
    shadow({Trigger::CodeBlockKind::GlobalHeader, 0}, "local InitTrig_ExistingLua = nil\n", false);
    shadow({Trigger::CodeBlockKind::GlobalHeader, 0}, "local RunInitializationTriggers = nil\n", false);
    shadow({Trigger::CodeBlockKind::CustomTrigger, fixture.created},
        "function InitTrig_NewLua() end\nlocal InitTrig_ExistingLua = nil\n", false);
    shadow({Trigger::CodeBlockKind::GlobalHeader, 0}, "local PrivateHelper = 1\n", true);
    auto disabled = fixture.edited;
    auto* record = Trigger::TriggerDocumentEditor::FindTrigger(disabled, fixture.existing);
    Require(record != nullptr, "original native custom fixture");
    record->enabled = 0;
    const auto incomplete = Take(LuaAuthorCodeEditor::PrepareText(fixture.settings, disabled, fixture.catalog,
        {Trigger::CodeBlockKind::CustomTrigger, fixture.existing}, "unfinished("), "edit disabled author source");
    (void)Take(TriggerCodeEditor::Apply(disabled, fixture.catalog, incomplete), "retain disabled incomplete author source");
    const auto retained = Script::CaptureNativeLuaAuthorCode(fixture.settings, disabled, fixture.catalog);
    Require(retained.Succeeded() && retained.value && retained.value->Triggers().size() == 1,
        "disabled native author text is sealed/preserved and is not emitted or syntax-gated as active code");
    auto target = fixture.request.profile.target;
    target.nativeAuthorCode = std::make_shared<const Script::PreparedLuaAuthorCode>(*retained.value);
    const auto generated = Script::GenerateLuaTriggerUnit(disabled, fixture.catalog, target);
    Require(generated.Succeeded() && generated.text.find("unfinished(") == std::string::npos,
        "disabled incomplete native text remains WCT-only and is absent from generated Lua");
    target.nativeAuthorCode.reset();
    Require(!Script::GenerateLuaTriggerUnit(disabled, fixture.catalog, target).Succeeded(),
        "classic migration still rejects disabled custom source without native qualification");
    Lua::SyntaxOptions options; std::stop_source stop; options.cancel = stop.get_token(); stop.request_stop();
    const auto cancelled = Script::CaptureNativeLuaAuthorCode(fixture.settings, fixture.edited, fixture.catalog, options);
    Require(!cancelled.value && HasCode(cancelled, "LUA_AUTHOR_CANCELLED"), "cancel is a hard author capture gate without partial seal");
    options = {}; options.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    const auto expired = Script::CaptureNativeLuaAuthorCode(fixture.settings, fixture.edited, fixture.catalog, options);
    Require(!expired.value && HasCode(expired, "LUA_AUTHOR_DEADLINE"), "deadline is a hard author capture gate without partial seal");
}

Script::LuaTriggerUnit EmitAndVerifySpans(const Fixture& fixture) {
    auto target = fixture.request.profile.target;
    const auto classic = Script::GenerateLuaTriggerUnit(fixture.edited, fixture.catalog, target);
    Require(!classic.Succeeded(), "default JASS-to-Lua migration continues rejecting author custom code");
    target.nativeAuthorCode = fixture.Capture(fixture.edited);
    auto native = Script::GenerateLuaTriggerUnit(fixture.edited, fixture.catalog, target);
    Require(native.Succeeded(), "native Lua author unit emits all typed GUI and original Lua blocks");
    const auto initStart = native.text.find("function InitCustomTriggers()\n");
    const auto runStart = native.text.find("function RunInitializationTriggers()\n");
    Require(initStart != std::string::npos && runStart != std::string::npos && initStart < runStart,
        "actual generated initializer entry points exist in their emitted order");
    const auto initialization = std::string_view(native.text).substr(initStart, runStart - initStart);
    const auto runInitialization = std::string_view(native.text).substr(runStart);
    std::cout << "ACTUAL_LUA_INITIALIZATION_BEGIN\n" << initialization << runInitialization
        << "ACTUAL_LUA_INITIALIZATION_END\n";
    Require(initialization.find("    InitTrig_ExistingLua()\n") != std::string::npos
        && initialization.find("    InitTrig_NewLua()\n") != std::string::npos,
        "actual WE initializer calls occur inside InitCustomTriggers, not merely author declarations");
    Require(runInitialization == "function RunInitializationTriggers()\n"
        "    (function() local v0 = gg_trg_ExistingLua; return ConditionalTriggerExecute(v0) end)();\nend\n",
        "only original runOnInit trigger executes through the existing single-evaluation native-call contract");
    std::size_t header = 0, custom = 0;
    for (const auto& span : native.sourceSpans) {
        Require(span.byteOffset <= native.text.size() && span.byteLength <= native.text.size() - span.byteOffset, "actual bounded emitted range");
        const auto text = std::string_view(native.text).substr(span.byteOffset, span.byteLength);
        if (span.kind == Script::LuaSourceSpanKind::AuthorHeader) {
            ++header; Require(span.nodeId == 0 && span.triggerId == 0 && text == fixture.edited.wct.customCode.text, "actual original header bytes/explicit zero identity span");
        } else if (span.kind == Script::LuaSourceSpanKind::AuthorCustom) {
            ++custom;
            const auto record = std::find_if(fixture.edited.triggers.begin(), fixture.edited.triggers.end(),
                [&](const auto& value) { return value.id == span.triggerId; });
            Require(record != fixture.edited.triggers.end() && span.nodeId == record->id && text == record->customText.text,
                "actual custom body bytes and stable source span");
        } else Require(span.nodeId != 0, "only explicit AuthorHeader permits zero source identity");
    }
    Require(header == 1 && custom == 2, "every active original/edited/new Lua author block actually mapped");
    const auto syntax = Lua::CheckSyntax(native.text, "actual-native-unit.lua");
    Require(syntax.Succeeded(), "native emission uses same compile-only Lua 5.3 kernel");
    return native;
}

void SaveAndReopen(const Fixture& fixture, const Script::LuaTriggerUnit& unit) {
    const auto source = fixture.root / "native-lua-source.w3x", candidate = fixture.root / "native-lua-candidate.w3x";
    Require(!std::filesystem::exists(candidate), "one fixed candidate only; refusing replacement or duplicate acceptance map");
    const std::vector<MapArchiveMember> members{{"war3map.w3i", fixture.settings}, {"war3map.wtg", fixture.originalPair.wtg},
        {"war3map.wct", fixture.originalPair.wct}, {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
        {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()}, {"war3map.lua", Bytes("function config() end\nfunction main() end\n")},
        {"war3map.j", Bytes("obsolete JASS identity; must be removed only from candidate")}, {"unmanaged.bin", Bytes("C4 original opaque bytes")}};
    if (!std::filesystem::exists(source)) (void)Take(WriteVerifiedArchive(source, members), "write original small source MPQ");
    auto original = Take(MapArchiveSession::Open(source), "open original synthetic source");
    for (const auto& member : members) Require(Take(original.ReadMember(member.name), "actual source member") == member.data, "exact original fixture identity before save");
    const auto before = Hash(source);
    const auto checkerHash = Hash(fixture.request.checker.Identity()->executable);
    MapSaveTask task;
    Good(task.FreezeArchive(original), "owned source archive snapshot"); task.target = candidate; task.triggerChanges = true;
    task.replacements = {{"war3map.wtg", fixture.editedPair.wtg}, {"war3map.wct", fixture.editedPair.wct}};
    struct WorkspaceEvidence {
        MapSaveTask& task;
        ~WorkspaceEvidence() noexcept {
            try { std::cout << "script_workspace=" << task.scriptWorkspace.string() << " task_succeeded=" << task.succeeded << '\n'; } catch (...) {}
        }
    } evidence{task};
    ScriptSaveInputSources inputs;
    inputs.triggers = std::make_unique<Trigger::TriggerDocument>(fixture.edited);
    inputs.catalog = std::make_unique<Trigger::UiCatalog>(fixture.catalog);
    inputs.layers = fixture.catalog.Layers(); inputs.expectedEffectiveLayers = fixture.catalog.Layers();
    inputs.uiRoot = fixture.root / "catalog"; inputs.luaTarget = fixture.request; inputs.scriptMember = "war3map.lua"; inputs.documentRevision = 31;
    const auto prepared = PrepareScriptSaveInputs(task, std::move(inputs));
    Require(prepared.Succeeded() && task.script && task.script->luaTarget && task.script->luaTarget->Profile()->target.nativeAuthorCode
        && task.script->luaTarget->Profile()->target.nativeAuthorCode->Identity()->sourceSettingsSha256 == Take(Sha256(fixture.settings), "W3I SHA"),
        "public save preparation captures original-source native permission: " + Codes(prepared));
    task.beforePublish = [&] {
        return Hash(source) == before ? Status::Ok() : Status::Failure("NativeLuaReal", "SOURCE_LEASE_CHANGED", "original source identity changed");
    };
    const auto started = std::chrono::steady_clock::now();
    MapSaveService::Run(task);
    Require(Hash(source) == before && Hash(fixture.request.checker.Identity()->executable) == checkerHash,
        "source/checker file identities remain unchanged after actual public save attempt");
    Require(task.executed && task.succeeded && task.scriptRebuilt && !task.scriptSkipped && task.nextSession && task.written == candidate,
        "actual common MapSaveService completed rebuild/write/reopen: " + Codes(task));
    auto reopened = Take(MapArchiveSession::Open(candidate), "actual candidate reopen");
    const std::array<std::string, 4> names{"war3map.lua", "scripts\\war3map.lua", "war3map.j", "scripts\\war3map.j"};
    const auto exact = Take(reopened.CaptureExactMembers(names), "canonical script member presence");
    Require(exact.size() == 4 && exact[0].present && !exact[1].present && !exact[2].present && !exact[3].present,
        "candidate has one canonical Lua script and obsolete JASS removed");
    const auto script = Text(exact[0].bytes); const auto scriptSha = Take(Sha256(exact[0].bytes), "actual saved script SHA");
    const auto previousCandidate = fixture.root.parent_path() / "native-lua-code-acceptance/native-lua-candidate.w3x";
    if (std::filesystem::exists(previousCandidate)) {
        const auto previousSha = Hash(previousCandidate);
        Require(previousSha == "c47f38d1fc512a20ef8401b36ddd6f6e8b25b1ecab4f0f3c2938ad0b4ca72c2d", "previous tiny acceptance candidate identity");
        const auto previous = Take(MapArchiveSession::Open(previousCandidate), "previous readonly acceptance candidate");
        Require(Take(previous.ReadMember("war3map.lua"), "previous saved script") == exact[0].bytes
            && Hash(previousCandidate) == previousSha, "new lexical guard preserves exact prior valid saved Lua and old candidate");
        std::cout << "previousCandidateSha=" << previousSha << " unchanged=1 exactScriptReplay=1\n";
    }
    const auto unitOffset = script.find(unit.text);
    Require(unitOffset != std::string::npos, "full saved war3map.lua actually contains complete proven native trigger unit");
    for (const auto& span : unit.sourceSpans)
        Require(std::string_view(script).substr(unitOffset + span.byteOffset, span.byteLength) == std::string_view(unit.text).substr(span.byteOffset, span.byteLength),
            "full saved script preserves actual typed source-range bytes");
    Require(script.find("function config()") != std::string::npos && script.find("function main()") != std::string::npos
        && Lua::CheckSyntax(script, "actual-saved-war3map.lua").Succeeded(), "full config/main and shared compile-only kernel acceptance");
    const auto savedWtg = Take(reopened.ReadMember("war3map.wtg"), "saved WTG");
    const auto savedWct = Take(reopened.ReadMember("war3map.wct"), "saved WCT");
    Require(Take(reopened.ReadMember("war3map.w3i"), "saved W3I") == fixture.settings
        && savedWtg == fixture.editedPair.wtg && savedWct == fixture.editedPair.wct
        && Take(reopened.ReadMember("unmanaged.bin"), "unmanaged") == Bytes("C4 original opaque bytes"),
        "actual candidate exact W3I/WTG/WCT/unmanaged bytes");
    const auto replayed = Take(Trigger::WtgWctCodec{}.Parse(savedWtg, savedWct, fixture.catalog), "reopened actual pair");
    Require(replayed.wct.customCode.text == kEditedHeader && replayed.triggers.size() == 3
        && std::any_of(replayed.triggers.begin(), replayed.triggers.end(), [](const auto& row) { return row.name.text == "ExistingLua" && row.customText.text == kEditedBody; })
        && std::any_of(replayed.triggers.begin(), replayed.triggers.end(), [](const auto& row) { return row.name.text == "NewLua" && row.customText.text.starts_with("function InitTrig_NewLua()"); }),
        "actual reopened native author header/custom/new block edits");
    const auto work = task.scriptWorkspace / "lua-check";
    Require(Hash(work / "war3map.lua") == scriptSha && Read(work / "lua-syntax.log").find("FORGE_LUA_SYNTAX_OK_V1 " + scriptSha) != std::string::npos,
        "actual external Lua checker marker binds exact saved map script");
    const auto candidateSha = Hash(candidate);
    Require(task.nextSession->SourceSha256() == candidateSha, "AfterSave candidate identity");
    for (const auto& member : members) Require(Take(original.ReadMember(member.name), "source unchanged member") == member.data, "original source member retained");
    std::cout << "source=" << source.string() << " sha=" << before << " candidate=" << candidate.string() << " sha=" << candidateSha
        << " scriptSha=" << scriptSha << " checkerSha=" << checkerHash << " elapsed="
        << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << '\n';
    FinalizeScriptWorkspace(task);
}
} // namespace

int main() {
    try {
        const auto directory = Env(L"FORGE_NATIVE_LUA_CODE_REAL_ROOT");
        const auto ydwe = std::filesystem::path(Env(L"FORGE_YDWE_ROOT"));
        const auto sharedRepository = std::filesystem::path(Env(L"FORGE_REAL_REPO_ROOT"));
        if (directory.empty() || ydwe.empty()) {
            std::cout << "SKIP: FORGE_NATIVE_LUA_CODE_REAL_ROOT and FORGE_YDWE_ROOT are required for native Lua code real save acceptance.\n";
            return 77;
        }
        const auto repository = std::filesystem::path(directory);
        Require(repository.is_absolute() && ydwe.is_absolute(), "configured native Lua acceptance paths must be absolute");
        Require(std::filesystem::is_directory(repository), "configured editor workspace is not a directory");
        if (!sharedRepository.empty()) {
            Require(sharedRepository.is_absolute() && std::filesystem::equivalent(repository, sharedRepository),
                "native Lua acceptance root must match configured FORGE_REAL_REPO_ROOT");
        }
        Fixture fixture(repository / "artifacts/astra-rounds/nightly-maptest-20261006/native-lua-code-acceptance-v2");
        UndoRedo(fixture); QualificationAndSealedIdentity(fixture); InitializerContracts(fixture);
        const auto unit = EmitAndVerifySpans(fixture); SaveAndReopen(fixture, unit);
        std::cout << "native Lua edit/sealed source/compiler/public save/reopen acceptance passed; no game executed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "native Lua real save failure: " << error.what() << '\n'; return 1; }
}
