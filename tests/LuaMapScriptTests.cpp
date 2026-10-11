#include "War3MapCore/Script/LuaMapScript.h"
#include "War3MapCore/Script/ClassicLuaSaveBundle.h"
#include "War3MapCore/Script/LuaAuthorCode.h"
#include "War3MapCore/Lua/LuaSyntax.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/GeneratedObjectSymbol.h"
#include "../src/ScriptBuild/ExternalProcess.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Script;
namespace {
void Require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc); file << text;
    Require(static_cast<bool>(file), "write failed: " + path.string());
}
std::string Diagnostics(const std::vector<Diagnostic>& values) {
    std::string text;
    for (const auto& value : values) text += value.code + ": " + value.message + "\n";
    return text;
}
MapData::SettingsRawcode Raw(std::string_view value) {
    Require(value.size() == 4, "fixture rawcode length");
    MapData::SettingsRawcode result{};
    for (std::size_t index = 0; index < 4; ++index) result[index] = static_cast<std::byte>(value[index]);
    return result;
}
LuaTargetProfile Profile() {
    LuaTargetProfile result;
    result.identity = "original-map-native-stubs/lua53/explicit-classic12";
    for (const auto* name : {"player", "unit", "item", "destructable", "rect", "camerasetup", "sound", "weathereffect",
            "trigger", "boolexpr", "triggercondition", "triggeraction", "camerafield", "unitstate", "playercolor",
            "racepreference", "mapcontrol", "startlocprio", "placement"}) result.handleTypes.emplace_back(name, "handle");
    const auto api = [&](const char* name, const char* returns, std::initializer_list<const char*> args) {
        LuaApiSignature entry{name, returns, {}};
        for (const auto* arg : args) entry.parameters.emplace_back(arg);
        result.apis.push_back(std::move(entry));
    };
    api("CreateTrigger", "trigger", {}); api("Condition", "boolexpr", {"code"});
    api("TriggerAddCondition", "triggercondition", {"trigger", "boolexpr"});
    api("TriggerAddAction", "triggeraction", {"trigger", "code"});
    api("ConditionalTriggerExecute", "nothing", {"trigger"});
    api("TestBindings", "nothing", {"unit", "item", "destructable", "rect", "camerasetup", "sound", "string", "unitstate"});
    api("FourCC", "integer", {"string"}); api("Player", "player", {"integer"});
    api("CreateUnit", "unit", {"player", "integer", "real", "real", "real"});
    api("CreateBlightedGoldmine", "unit", {"player", "real", "real", "real"});
    api("GetUnitState", "real", {"unit", "unitstate"}); api("SetUnitState", "nothing", {"unit", "unitstate", "real"});
    api("SetResourceAmount", "nothing", {"unit", "integer"});
    for (const auto* name : {"SetHeroLevel", "SetHeroStr", "SetHeroAgi", "SetHeroInt"}) api(name, "nothing", {"unit", "integer", "boolean"});
    api("ConvertPlayerColor", "playercolor", {"integer"}); api("SetUnitColor", "nothing", {"unit", "playercolor"});
    api("SetUnitAcquireRange", "nothing", {"unit", "real"}); api("SelectHeroSkill", "nothing", {"unit", "integer"});
    api("IssueImmediateOrder", "boolean", {"unit", "string"});
    api("UnitAddItemToSlotById", "boolean", {"unit", "integer", "integer"});
    api("WaygateSetDestination", "nothing", {"unit", "real", "real"}); api("WaygateActivate", "nothing", {"unit", "boolean"});
    api("GetRectCenterX", "real", {"rect"}); api("GetRectCenterY", "real", {"rect"});
    api("CreateItem", "item", {"integer", "real", "real"});
    api("CreateDestructable", "destructable", {"integer", "real", "real", "real", "real", "integer"});
    api("CreateDestructableZ", "destructable", {"integer", "real", "real", "real", "real", "real", "integer"});
    api("CreateSound", "sound", {"string", "boolean", "boolean", "boolean", "integer", "integer", "string"});
    api("SetSoundParamsFromLabel", "nothing", {"sound", "string"});
    for (const auto* name : {"SetSoundDuration", "SetSoundChannel", "SetSoundVolume"}) api(name, "nothing", {"sound", "integer"});
    for (const auto* name : {"SetSoundPitch", "SetSoundDistanceCutoff"}) api(name, "nothing", {"sound", "real"});
    api("SetSoundDistances", "nothing", {"sound", "real", "real"});
    api("SetSoundConeAngles", "nothing", {"sound", "real", "real", "integer"});
    api("SetSoundConeOrientation", "nothing", {"sound", "real", "real", "real"});
    api("Rect", "rect", {"real", "real", "real", "real"}); api("AddWeatherEffect", "weathereffect", {"rect", "integer"});
    api("EnableWeatherEffect", "nothing", {"weathereffect", "boolean"}); api("CreateCameraSetup", "camerasetup", {});
    api("CameraSetupSetField", "nothing", {"camerasetup", "camerafield", "real", "real"});
    api("CameraSetupSetDestPosition", "nothing", {"camerasetup", "real", "real", "real"});
    api("SetPlayerTechMaxAllowed", "nothing", {"player", "integer", "integer"});
    api("SetPlayerTechResearched", "nothing", {"player", "integer", "integer"});
    for (const auto* name : {"SetPlayerStartLocation", "ForcePlayerStartLocation", "SetPlayerTeam"}) api(name, "nothing", {"player", "integer"});
    api("SetPlayerColor", "nothing", {"player", "playercolor"}); api("SetPlayerRacePreference", "nothing", {"player", "racepreference"});
    api("SetPlayerRaceSelectable", "nothing", {"player", "boolean"}); api("SetPlayerController", "nothing", {"player", "mapcontrol"});
    api("SetPlayerSlotAvailable", "nothing", {"player", "mapcontrol"});
    for (const auto* name : {"SetPlayerAllianceStateAllyBJ", "SetPlayerAllianceStateVisionBJ", "SetPlayerAllianceStateControlBJ", "SetPlayerAllianceStateFullControlBJ"})
        api(name, "nothing", {"player", "player", "boolean"});
    api("SetStartLocPrioCount", "nothing", {"integer", "integer"});
    api("SetStartLocPrio", "nothing", {"integer", "integer", "integer", "startlocprio"});
    api("GetCameraMargin", "real", {"integer"});
    api("SetCameraBounds", "nothing", {"real", "real", "real", "real", "real", "real", "real", "real"});
    api("SetDayNightModels", "nothing", {"string", "string"});
    api("SetTerrainFogEx", "nothing", {"integer", "real", "real", "real", "real", "real", "real"});
    api("SetWaterBaseColor", "nothing", {"integer", "integer", "integer", "integer"});
    for (const auto* name : {"NewSoundEnvironment", "SetAmbientDaySound", "SetAmbientNightSound", "SetMapName", "SetMapDescription"})
        api(name, "nothing", {"string"});
    api("SetMapMusic", "nothing", {"string", "boolean", "integer"}); api("InitBlizzard", "nothing", {});
    api("InitGenericPlayerSlots", "nothing", {}); api("SetPlayers", "nothing", {"integer"}); api("SetTeams", "nothing", {"integer"});
    api("SetGamePlacement", "nothing", {"placement"}); api("DefineStartLocation", "nothing", {"integer", "real", "real"});
    const auto constants = [&](const char* type, std::initializer_list<const char*> names) {
        for (const auto* name : names) result.constants.push_back({name, type});
    };
    constants("camerafield", {"CAMERA_FIELD_ZOFFSET", "CAMERA_FIELD_ROTATION", "CAMERA_FIELD_ANGLE_OF_ATTACK",
        "CAMERA_FIELD_TARGET_DISTANCE", "CAMERA_FIELD_ROLL", "CAMERA_FIELD_FIELD_OF_VIEW", "CAMERA_FIELD_FARZ", "CAMERA_FIELD_NEARZ"});
    constants("unitstate", {"UNIT_STATE_LIFE", "UNIT_STATE_MANA"});
    constants("integer", {"CAMERA_MARGIN_LEFT", "CAMERA_MARGIN_RIGHT", "CAMERA_MARGIN_BOTTOM", "CAMERA_MARGIN_TOP"});
    constants("racepreference", {"RACE_PREF_RANDOM", "RACE_PREF_HUMAN", "RACE_PREF_ORC", "RACE_PREF_UNDEAD", "RACE_PREF_NIGHTELF"});
    constants("mapcontrol", {"MAP_CONTROL_USER", "MAP_CONTROL_COMPUTER", "MAP_CONTROL_NEUTRAL", "MAP_CONTROL_RESCUABLE"});
    constants("startlocprio", {"MAP_LOC_PRIO_HIGH", "MAP_LOC_PRIO_LOW"});
    constants("placement", {"MAP_PLACEMENT_TEAMS_TOGETHER", "MAP_PLACEMENT_USE_MAP_SETTINGS"});
    return result;
}

struct Fixture {
    MapData::MapSettings settings;
    MapData::UnitPlacements units;
    MapData::DoodadPlacements doodads;
    MapData::RegionsDocument regions;
    MapData::CamerasDocument cameras;
    MapData::SoundsDocument sounds;
    MapData::StringTableDocument strings;
    Trigger::TriggerDocument document;
    Trigger::UiCatalog catalog;
    LuaTargetProfile target = Profile();
    MapInitializationPlan plan;

    explicit Fixture(const std::filesystem::path& root, bool extendedObjectNames = false) {
        settings.formatVersion = 31; settings.scriptLanguage = 1; settings.tileset = 'L';
        settings.mapName = "TRIGSTR_7"; settings.description = "完整地图 Lua 初始化";
        settings.cameraMargins = {1, 1, 1, 1}; settings.flags = 0x40u | 0x20u | 0x2000u | 0x10000u;
        settings.custom.emplace();
        settings.environment.fogStyle = 1; settings.environment.fogStart = 100; settings.environment.fogEnd = 2000;
        settings.environment.fogDensity = 0.5f; settings.environment.fogColor = {0, 128, 255, 255};
        settings.environment.waterColor = {30, 20, 10, 200}; settings.environment.weather = Raw("WOcw");
        MapData::MapPlayerSettings first; first.number = 0; first.controller = 1; first.race = 1;
        first.flags = 1; first.startX = -100; first.startY = -200; first.allyHighPriorityMask = 4;
        MapData::MapPlayerSettings second; second.number = 2; second.controller = 2; second.race = 2;
        second.startX = 100; second.startY = 200; second.allyLowPriorityMask = 1;
        settings.players = {first, second}; settings.forces = {{9, 5, "盟军"}};
        settings.custom->upgrades = {{1, Raw("Rfoo"), 0, 2}, {1, Raw("Rfoo"), 2, 2}, {1, Raw("Rbar"), 0, 1}};
        settings.custom->techRestrictions = {{4, Raw("hbar")}};
        strings.entries.push_back({7, "owned 中文地图", 0});
        const auto unit = [](std::string id, int owner, int creation, float x) {
            MapData::UnitPlacement value; value.rawcode = std::move(id); value.owner = owner; value.creationId = creation;
            value.position = {x, x + 1, 0}; value.rotation = std::numbers::pi_v<float> * 1.5f;
            value.scale = {1, 1, 1}; value.targetAcquisition = -1;
            value.random = MapData::RandomAnyUnit{1, 0}; return value;
        };
        units.units.push_back(unit("hfoo", 0, 11, 110));
        units.units.back().hitPoints = 50; units.units.back().mana = 30; units.units.back().teamColor = 3;
        units.units.back().targetAcquisition = -2; units.units.back().inventory.push_back({1, "ratf"});
        units.units.push_back(unit("Hamg", 2, 12, 120));
        auto& hero = units.units.back(); hero.heroLevel = 3; hero.heroStrength = 21; hero.heroAgility = 17; hero.heroIntelligence = 35;
        hero.abilities = {{"AHwe", false, 2}, {"Aslo", true, 1}};
        units.units.push_back(unit("ngol", 12, 13, 130)); units.units.back().gold = 23000;
        units.units.push_back(unit("ratf", 15, 14, 140)); units.units.push_back(unit("ratf", 15, 15, 150));
        units.units.push_back(unit("nwgt", 15, 16, 160)); units.units.back().waygateRegionId = 23;
        units.units.push_back(unit("sloc", 0, 17, 170));
        MapData::DoodadPlacement tree; tree.rawcode = "LTlt"; tree.flags = 4; tree.creationId = 20;
        tree.position = {210, 220, 77}; tree.rotation = 0; tree.scale = {1.25f, 1.25f, 1.25f}; tree.variation = 2;
        doodads.doodads.push_back(tree);
        auto archiveOnly = tree; archiveOnly.creationId = 21; archiveOnly.flags = 2; doodads.doodads.push_back(archiveOnly);
        if (extendedObjectNames) {
            units.units[0].rawcode = "I_u_"; units.units[0].creationId = 10000;
            units.units[1].creationId = 123456789;
            units.units[3].rawcode = "h_i_"; units.units[3].creationId = (std::numeric_limits<std::int32_t>::max)();
            doodads.doodads[0].rawcode = "B_d_"; doodads.doodads[0].creationId = 123456;
        }
        MapData::RegionRecord region{}; region.left = -10; region.bottom = -20; region.right = 30; region.top = 40;
        region.name = "Arena"; region.creationNumber = 23; region.weatherId = Raw("RAhr"); regions.entries.push_back(region);
        MapData::CameraRecord camera{}; camera.name = "View"; camera.targetX = 11; camera.targetY = 12;
        camera.zOffset = 13; camera.rotation = 14; camera.angleOfAttack = 15; camera.targetDistance = 16;
        camera.roll = 17; camera.fieldOfView = 18; camera.farClippingPlane = 1900; camera.nearClippingPlane = 20;
        cameras.entries.push_back(camera);
        MapData::SoundRecord sound{}; sound.name = "gg_snd_Unit"; sound.filePath = "Sound\\Units.wav";
        sound.eaxSetting = "DefaultEAXON"; sound.flags = 7; sound.fadeInRate = 10; sound.fadeOutRate = 20;
        sound.volume = 80; sound.pitch = 1.25f; sound.pitchVariance = std::bit_cast<float>(MapData::kSoundDefaultRealBits);
        sound.priority = -1; sound.channel = 3; sound.minDistance = 64; sound.maxDistance = 512;
        sound.distanceCutoff = 1024; sound.coneAngleInside = 90; sound.coneAngleOutside = 180; sound.coneOutsideVolume = 32;
        sound.coneOrientation = {1, 0, 0}; sounds.entries.push_back(sound);
        sound.name = "gg_snd_Music"; sound.flags = 8; sound.filePath = "Music\\Theme.mp3"; sounds.entries.push_back(sound);

        const auto unitBytes = units.Serialize();
        Require(unitBytes.Succeeded(), Diagnostics(unitBytes.diagnostics));
        auto parsedUnits = MapData::ParseUnitPlacements(*unitBytes.value);
        Require(parsedUnits.Succeeded(), Diagnostics(parsedUnits.diagnostics));
        units = std::move(*parsedUnits.value);
        MapScriptInputs inputs; inputs.settings = &settings; inputs.units = &units; inputs.doodads = &doodads;
        inputs.regions = &regions; inputs.cameras = &cameras; inputs.sounds = &sounds; inputs.strings = &strings;
        inputs.terrain = TerrainExtent{9, 9, -512, -512};
        inputs.queries.isItem = [](std::string_view id) { return id == "ratf" || id == "h_i_"; };
        inputs.queries.isBuilding = [](std::string_view id) { return id == "ngol" || id == "nwgt"; };
        inputs.queries.isHero = [](std::string_view id) { return id == "Hamg"; };
        inputs.queries.unitTeamColor = [](std::string_view id) { return id == "Hamg" ? 7 : -1; };
        inputs.queries.ability = [](std::string_view id) {
            ObjectQueries::AbilityInfo info; info.hero = id == "AHwe"; info.autocast = id == "Aslo";
            info.orderOn = "slowon"; info.orderOff = "slowoff"; return info;
        };
        inputs.queries.soundLabel = [](std::string_view) { return std::optional<std::string>{"UnitSound"}; };
        inputs.queries.soundDuration = [](std::string_view) { return std::optional<int>{2000}; };
        const auto planned = BuildMapInitializationPlan(inputs, {MapInitializationPlayerLayout::Classic12});
        Require(planned.Succeeded(), Diagnostics(planned.diagnostics)); plan = *planned.value;
        Require(plan.units.back().startLocation && plan.units.back().symbol.empty()
            && plan.unitCreationOrder.size() == 4 && plan.itemCreationOrder.size() == 2,
            "fixed mode0 sloc must remain an archive fact, not a phantom created unit");
        auto frozenUnits = units;
        frozenUnits.units.clear();
        for (const auto& record : plan.units) frozenUnits.units.push_back(record.placement);
        const auto frozenBytes = frozenUnits.Serialize();
        const auto sourceBytes = units.Serialize();
        Require(frozenBytes.Succeeded() && sourceBytes.Succeeded() && *frozenBytes.value == *unitBytes.value
            && *sourceBytes.value == *unitBytes.value, "Lua capture must own mode0 records without rewriting their bytes");
        Write(root / "ui/config", "base\n"); Write(root / "ui/base/define.txt", "[TriggerTypes]\n");
        Write(root / "ui/base/action.txt", "[TestBindings]\n[[.args]]\ntype=unit\n[[.args]]\ntype=item\n[[.args]]\ntype=destructable\n"
            "[[.args]]\ntype=rect\n[[.args]]\ntype=camerasetup\n[[.args]]\ntype=sound\n[[.args]]\ntype=string\n[[.args]]\ntype=unitstate\n");
        const auto loaded = Trigger::UiCatalog::Load(root);
        Require(loaded.Succeeded(), Diagnostics(loaded.diagnostics)); catalog = *loaded.value;
        Trigger::VariableRecord variable; variable.id = 501; variable.name.text = "Preplaced"; variable.typeName.text = "unit";
        variable.hasDefault = 1; variable.value.text = plan.units[1].symbol; document.variables.push_back(variable);
        const std::vector<std::string> bindings{plan.units[0].symbol, plan.units[3].symbol, plan.doodads[0].symbol,
            plan.regions[0].symbol, plan.cameras[0].symbol, plan.sounds[0].symbol, plan.sounds[1].symbol, "UNIT_STATE_LIFE"};
        Trigger::EcaNode node; node.id = 502; node.type = 2; node.name.text = "TestBindings";
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            Trigger::ArgumentNode argument; argument.id = 510 + index;
            argument.type = index == bindings.size() - 1 ? 3 : 1; argument.value.text = bindings[index];
            node.arguments.push_back(static_cast<std::uint32_t>(document.argumentNodes.size())); document.argumentNodes.push_back(argument);
        }
        document.ecaNodes.push_back(node);
        Trigger::TriggerRecord trigger; trigger.id = 500; trigger.name.text = "InitOwnedBindings"; trigger.runOnInit = 1;
        trigger.ecas = {0}; document.triggers.push_back(trigger);
        std::string symbols = "return {\n";
        for (std::size_t index = 0; index < plan.units.size(); ++index)
            symbols += "unit" + std::to_string(index) + "=\"" + plan.units[index].symbol + "\",\n";
        symbols += "dest=\"" + plan.doodads[0].symbol + "\",region=\"" + plan.regions[0].symbol
            + "\",camera=\"" + plan.cameras[0].symbol + "\",sound=\"" + plan.sounds[0].symbol + "\",music=\"" + plan.sounds[1].symbol + "\"}\n";
        Write(root / "symbols.lua", symbols);
    }
};

// 合入LuaMapScriptTests.cpp的匿名命名空间，调用ClassicBundle(fixture)。
// 追加头文件ClassicLuaSaveBundle.h、LuaAuthorCode.h、Lua/LuaSyntax.h。
// 依赖最终7参数GenerateLuaMapScript合同，仅在root集成后编译。
void ClassicBundle(const Fixture& fixture) {
    auto plan = fixture.plan;
    plan.settings.formatVersion = 25;
    plan.settings.scriptLanguage = 0;
    plan.settings.graphicsModes = 0;
    const auto settingsBytes = plan.settings.Serialize();
    Require(settingsBytes.Succeeded(), Diagnostics(settingsBytes.diagnostics));
    const auto parsed = MapData::MapSettings::Parse(*settingsBytes.value);
    Require(parsed.Succeeded() && parsed.value->SourceFormatVersion() == 25, "classic fixture must capture real v25 bytes");
    plan.settings = *parsed.value;
    auto target = fixture.target;
    target.handleTypes.emplace_back("alliancetype", "handle");
    target.apis.push_back({"TriggerEvaluate", "boolean", {"trigger"}});
    target.apis.push_back({"TriggerExecute", "nothing", {"trigger"}});
    target.apis.push_back({"SetPlayerAlliance", "nothing", {"player", "player", "alliancetype", "boolean"}});
    for (const auto name : {"ALLIANCE_PASSIVE", "ALLIANCE_HELP_REQUEST", "ALLIANCE_HELP_RESPONSE", "ALLIANCE_SHARED_XP",
            "ALLIANCE_SHARED_SPELLS", "ALLIANCE_SHARED_VISION", "ALLIANCE_SHARED_CONTROL", "ALLIANCE_SHARED_ADVANCED_CONTROL"})
        target.constants.push_back({name, "alliancetype"});
    const auto generated = GenerateLuaMapScript(plan, fixture.document, fixture.catalog, target, {}, {}, {true});
    Require(generated.Succeeded(), Diagnostics(generated.diagnostics));
    const auto bundle = BuildClassicLuaSaveBundle(plan, generated, target);
    Require(bundle.Succeeded() && bundle.modules.size() == 5, Diagnostics(bundle.diagnostics));
    Require(bundle.generatedScriptSha256 == generated.scriptSha256 && bundle.sourceMapSha256 == generated.sourceMapSha256,
        "bundle must retain the actual complete Lua and source mapping identities");
    const auto reserialized = plan.settings.Serialize();
    Require(reserialized.Succeeded() && *reserialized.value == *settingsBytes.value, "classic settings bytes changed during bundle build");
    const auto entryHash = Sha256Text(bundle.jassEntry);
    Require(entryHash.Succeeded() && *entryHash.value == bundle.jassSha256, "JASS entry hash does not bind text");
    bool exactSource = false;
    for (const auto& module : bundle.modules) {
        const auto hash = Sha256Text(module.text);
        Require(hash.Succeeded() && *hash.value == module.sha256 && Lua::CheckSyntax(module.text, "@" + module.member).Succeeded(),
            "a classic module lacks valid syntax or exact byte identity");
        if (module.member == "audastra\\map.lua") exactSource = module.text == generated.text;
    }
    Require(exactSource, "bundle must not rewrite the typed T generated body");
    const auto main = bundle.jassEntry.find("function main takes nothing returns nothing");
    const auto config = bundle.jassEntry.find("function config takes nothing returns nothing");
    Require(config != std::string::npos && config < main
        && bundle.jassEntry.substr(config, main - config).find("Cheat(") == std::string::npos,
        "lobby configuration must stay entirely on the production JASS path");
    const auto bootstrap = bundle.jassEntry.find("audastra.bootstrap", main);
    const auto day = bundle.jassEntry.find("call SetAmbientDaySound(", main);
    const auto night = bundle.jassEntry.find("call SetAmbientNightSound(", main);
    const auto music = bundle.jassEntry.find("call SetMapMusic(", main);
    const auto objects = bundle.jassEntry.find("audastra.objects", main);
    const auto blizzard = bundle.jassEntry.find("call InitBlizzard()", main);
    const auto continuation = bundle.jassEntry.find("audastra.continue", main);
    Require(main != std::string::npos && bootstrap != std::string::npos && day != std::string::npos
        && night != std::string::npos && music != std::string::npos && objects != std::string::npos
        && blizzard != std::string::npos && continuation != std::string::npos
        && bootstrap < day && day < night && night < music && music < objects && objects < blizzard && blizzard < continuation,
        "classic environment, ambient/music, object, Blizzard, and T phases changed order");
    Require(!BuildClassicLuaSaveBundle(fixture.plan, generated, target).Succeeded(), "v31 source cannot silently become classic");
    const auto defaultLua = GenerateLuaMapScript(plan, fixture.document, fixture.catalog, target);
    Require(defaultLua.Succeeded() && !BuildClassicLuaSaveBundle(plan, defaultLua, target).Succeeded(),
        "native single-main output cannot be treated as split classic output");
    auto changed = generated;
    changed.text += "\n-- changed after generation\n";
    Require(!BuildClassicLuaSaveBundle(plan, changed, target).Succeeded(), "modified generated bytes must be rejected");
    changed = generated; changed.classicObjectsContinuation.clear();
    Require(!BuildClassicLuaSaveBundle(plan, changed, target).Succeeded(), "missing object phase must be rejected");
    changed = generated; changed.classicMainContinuation = "other";
    Require(!BuildClassicLuaSaveBundle(plan, changed, target).Succeeded(), "unbound continuation name must be rejected");
    changed = generated; changed.requiredApis.push_back("bad-symbol");
    Require(!BuildClassicLuaSaveBundle(plan, changed, target).Succeeded(), "invalid global binding cannot reach module source");
    auto unknown = plan; unknown.settings.players.front().controller = 99;
    Require(!BuildClassicLuaSaveBundle(unknown, generated, target).Succeeded(), "unknown player controller must not get a default");
    auto mixed = target; mixed.nativeAuthorCode = std::make_shared<const PreparedLuaAuthorCode>();
    Require(!BuildClassicLuaSaveBundle(plan, generated, mixed).Succeeded(), "classic pure T contract rejects author-code credentials");
}

void Reject(const Fixture& fixture, const MapInitializationPlan& plan, const LuaTargetProfile& target, const char* code) {
    const auto result = GenerateLuaMapScript(plan, fixture.document, fixture.catalog, target);
    Require(!result.Succeeded() && result.text.empty() && result.sourceSpans.empty() && result.objectSpans.empty()
        && result.scriptSha256.empty() && result.sourceMapSha256.empty(), "failure returned partial whole map");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& value) { return value.code == code; }),
        Diagnostics(result.diagnostics) + " expected " + code);
}

void SharedGeneratedNamesReachFinalLua(const std::filesystem::path& root) {
    Fixture fixture(root, true);
    const auto unit = MapData::GeneratedObjectSymbol("unit", "I_u_", 10000);
    const auto item = MapData::GeneratedObjectSymbol("item", "h_i_", (std::numeric_limits<std::int32_t>::max)());
    const auto dest = MapData::GeneratedObjectSymbol("dest", "B_d_", 123456);
    const auto hero = MapData::GeneratedObjectSymbol("unit", "Hamg", 123456789);
    Require(unit.Succeeded() && item.Succeeded() && dest.Succeeded() && hero.Succeeded(), "shared symbol fixture invalid");
    Require(fixture.plan.units[0].symbol == *unit.value && *unit.value == "gg_unit_I_u__10000"
        && fixture.plan.units[3].symbol == *item.value && *item.value == "gg_item_h_i__2147483647"
        && fixture.plan.doodads[0].symbol == *dest.value && *dest.value == "gg_dest_B_d__123456"
        && fixture.plan.units[1].symbol == *hero.value, "Lua plan diverged from public unit/item/dest naming");
    const auto generated = GenerateLuaMapScript(fixture.plan, fixture.document, fixture.catalog, fixture.target);
    Require(generated.Succeeded(), Diagnostics(generated.diagnostics));
    const auto checkBinding = [&](const std::string& symbol, const char* api, std::string_view member, std::size_t sourceIndex) {
        Require(std::find(generated.requiredGlobals.begin(), generated.requiredGlobals.end(), symbol) != generated.requiredGlobals.end()
            && generated.triggers.text.find(symbol) != std::string::npos, "typed T reference lost the exact generated object identity");
        std::size_t spans = 0;
        bool creation = false;
        for (const auto& span : generated.objectSpans) {
            if (span.source.name != symbol) continue;
            Require(span.source.member == member && span.source.sourceIndex == sourceIndex
                && span.byteOffset <= generated.text.size() && span.byteLength <= generated.text.size() - span.byteOffset,
                "named object span no longer binds the original record");
            ++spans;
            const auto text = generated.text.substr(span.byteOffset, span.byteLength);
            creation = creation || (text.find(symbol + " = ") != std::string::npos && text.find(api) != std::string::npos);
        }
        Require(spans == 2 && creation, "exact symbol must have both declaration and real creation in final Lua");
    };
    checkBinding(*unit.value, "CreateUnit", "war3mapUnits.doo", 0);
    checkBinding(*item.value, "CreateItem", "war3mapUnits.doo", 3);
    checkBinding(*dest.value, "CreateDestructableZ", "war3map.doo", 0);
    checkBinding(*hero.value, "CreateUnit", "war3mapUnits.doo", 1);
    Require(generated.text.find("gg_unit_hfoo_0011") == std::string::npos
        && generated.text.find("gg_item_ratf_0014") == std::string::npos
        && generated.text.find("gg_dest_LTlt_0020") == std::string::npos
        && generated.text.find("gg_unit_Hamg_0012") == std::string::npos,
        "final Lua retained stale aliases from the unrenamed placement identities");
    Write(root / "large-symbol-map.lua", generated.text);
}
}

int main(int argc, char** argv) {
    try {
        const auto root = argc > 1 ? std::filesystem::absolute(argv[1]) : std::filesystem::temp_directory_path()
            / ("forge-lua-map-" + std::to_string(GetCurrentProcessId()));
        Fixture fixture(root);
        ClassicBundle(fixture);
        const auto generated = GenerateLuaMapScript(fixture.plan, fixture.document, fixture.catalog, fixture.target);
        Require(generated.Succeeded(), Diagnostics(generated.diagnostics));
        Require(generated.text.find("gg_unit_sloc_") == std::string::npos
            && generated.text.find("FourCC(\"sloc\")") == std::string::npos,
            "mode0 start location emitted a fixed unit or global");
        Require(generated.targetSha256.size() == 64 && generated.scriptSha256.size() == 64 && generated.sourceMapSha256.size() == 64,
            "complete result identities absent");
        const auto repeated = GenerateLuaMapScript(fixture.plan, fixture.document, fixture.catalog, fixture.target);
        Require(repeated.text == generated.text && repeated.sourceMapSha256 == generated.sourceMapSha256, "repeat is not deterministic");
        const auto hash = Sha256Text(generated.text);
        Require(hash.Succeeded() && *hash.value == generated.scriptSha256 && generated.scriptSha256 != generated.triggers.scriptSha256,
            "whole map script hash reused fragment identity");
        Require(generated.sourceMapSha256 != generated.triggers.sourceMapSha256, "whole map source map reused fragment identity");
        for (std::size_t index = 0; index < generated.sourceSpans.size(); ++index) {
            const auto& span = generated.sourceSpans[index]; const auto& original = generated.triggers.sourceSpans[index];
            Require(span.byteOffset > original.byteOffset && span.byteOffset <= generated.text.size()
                && span.byteLength <= generated.text.size() - span.byteOffset
                && generated.text.substr(span.byteOffset, span.byteLength) == generated.triggers.text.substr(original.byteOffset, original.byteLength),
                "trigger span does not address same bytes in full map");
        }
        Require(generated.objectSpans.size() == fixture.plan.symbols.size() * 2, "object declaration or creation mappings missing");
        for (const auto& span : generated.objectSpans) Require(span.byteOffset <= generated.text.size()
            && span.byteLength <= generated.text.size() - span.byteOffset && span.byteLength
            && generated.text.substr(span.byteOffset, span.byteLength).find(span.source.name) != std::string::npos,
            "object source range invalid");
        Require(std::find(generated.requiredGlobals.begin(), generated.requiredGlobals.end(), fixture.plan.units[1].symbol) != generated.requiredGlobals.end(),
            "global initializer binding absent from dependency list");
        Require(std::find(generated.requiredConstants.begin(), generated.requiredConstants.end(), "UNIT_STATE_LIFE") != generated.requiredConstants.end(),
            "typed constant dependency absent");
        Write(root / "map.lua", generated.text);

        const auto defaults = GenerateLuaMapScript(fixture.plan, fixture.document, fixture.catalog, fixture.target, {}, {});
        Require(defaults.text == generated.text && defaults.scriptSha256 == generated.scriptSha256
            && defaults.sourceMapSha256 == generated.sourceMapSha256 && defaults.classicMainContinuation.empty()
            && defaults.classicObjectsContinuation.empty(),
            "default native Lua path and source mapping must remain unchanged");
        auto classicTarget = fixture.target;
        std::erase_if(classicTarget.apis, [](const auto& value) {
            return value.name == "InitBlizzard" || value.name == "SetAmbientDaySound" || value.name == "SetAmbientNightSound"
                || value.name == "SetMapMusic" || value.name == "InitGenericPlayerSlots" || value.name == "SetPlayerSlotAvailable"
                || value.name.starts_with("SetPlayerAllianceState");
        });
        const auto split = GenerateLuaMapScript(fixture.plan, fixture.document, fixture.catalog, classicTarget, {}, {}, {true});
        Require(split.Succeeded() && split.classicMainContinuation == kClassicLuaMainContinuation
            && split.classicObjectsContinuation == kClassicLuaObjectsContinuation, Diagnostics(split.diagnostics));
        for (const auto name : {"InitBlizzard", "SetAmbientDaySound", "SetAmbientNightSound", "SetMapMusic",
                "InitGenericPlayerSlots", "SetPlayerSlotAvailable"})
            Require(std::find(split.requiredApis.begin(), split.requiredApis.end(), name) == split.requiredApis.end(),
                std::string("classic JASS stage must not require a Lua substitute for ") + name);
        Require(split.text.find("function config()") == std::string::npos
            && split.text.find("function InitCustomPlayerSlots()") == std::string::npos
            && split.text.find("function InitCustomTeams()") == std::string::npos,
            "classic player config is owned by the production JASS generator");
        const auto mainBegin = split.text.find("function main()"), objects = split.text.find("function AudastraMainObjects()");
        const auto continuation = split.text.find("function AudastraMainAfterBlizzard()");
        const auto globalsAfter = split.text.find("InitGlobals()", continuation);
        const auto triggersAfter = split.text.find("InitCustomTriggers()", continuation);
        const auto initializersAfter = split.text.find("RunInitializationTriggers()", continuation);
        Require(mainBegin != std::string::npos && objects != std::string::npos && continuation != std::string::npos
            && split.text.find("NewSoundEnvironment(", mainBegin) < objects
            && split.text.find("InitSounds()", objects) > objects && split.text.find("InitSounds()", objects) < continuation
            && split.text.find("CreateAllUnits()", objects) > objects && split.text.find("CreateAllUnits()", objects) < continuation
            && globalsAfter != std::string::npos && globalsAfter > continuation
            && triggersAfter != std::string::npos && triggersAfter > globalsAfter
            && initializersAfter != std::string::npos && initializersAfter > triggersAfter,
            "classic Lua phases preserve environment, objects, globals and trigger ordering around JASS stages");
        auto reforgedLayout = fixture.plan;
        reforgedLayout.playerSlotCount = 24; reforgedLayout.neutralAggressive = 24; reforgedLayout.neutralPassive = 27;
        const auto invalidClassic = GenerateLuaMapScript(reforgedLayout, fixture.document, fixture.catalog, classicTarget, {}, {}, {true});
        Require(!invalidClassic.Succeeded() && invalidClassic.text.empty() && invalidClassic.classicObjectsContinuation.empty()
            && invalidClassic.classicMainContinuation.empty()
            && std::any_of(invalidClassic.diagnostics.begin(), invalidClassic.diagnostics.end(), [](const auto& value) {
                return value.code == "CLASSIC_LUA_PLAYER_LAYOUT";
            }), "classic phase split rejects modern 24-slot layout without partial stages");

        auto plan = fixture.plan; plan.mainOrder[0] = MapInitStage::Units; Reject(fixture, plan, fixture.target, "LUA_MAP_INIT_ORDER");
        plan = fixture.plan; plan.symbols.pop_back(); Reject(fixture, plan, fixture.target, "LUA_MAP_SYMBOL_DIRECTORY");
        plan = fixture.plan; plan.symbols.front().type = "unit"; Reject(fixture, plan, fixture.target, "LUA_MAP_SYMBOL_DIRECTORY");
        plan = fixture.plan; plan.unitCreationOrder[0] = plan.unitCreationOrder[1]; Reject(fixture, plan, fixture.target, "LUA_MAP_OBJECT_ORDER");
        plan = fixture.plan; plan.sounds[0].sound.pitchVariance = 0.1f; Reject(fixture, plan, fixture.target, "LUA_SOUND_PARAMETER_UNSUPPORTED");
        plan = fixture.plan; plan.sounds[0].sound.minDistance = std::bit_cast<float>(MapData::kSoundDefaultRealBits);
        Reject(fixture, plan, fixture.target, "LUA_SOUND_DEFAULT_GROUP_MIXED");
        plan = fixture.plan; plan.units[0].placement.random = MapData::RandomAnyUnit{};
        Reject(fixture, plan, fixture.target, "LUA_MAP_RANDOM_DROP_UNSUPPORTED");
        for (const auto marker : {"uDNR", "bDNR", "iDNR"}) {
            for (const auto& mode : std::array<MapData::RandomUnitConfig, 2>{std::monostate{}, MapData::RandomAnyUnit{1, 0}}) {
                plan = fixture.plan; plan.units[0].placement.rawcode = marker; plan.units[0].placement.random = mode;
                Reject(fixture, plan, fixture.target, "LUA_MAP_RANDOM_DROP_UNSUPPORTED");
            }
        }
        for (const auto index : {std::size_t{0}, std::size_t{3}, fixture.plan.units.size() - 1}) {
            for (const auto& mode : std::array<MapData::RandomUnitConfig, 5>{
                     MapData::RandomAnyUnit{-1, 0}, MapData::RandomAnyUnit{2, 0}, MapData::RandomAnyUnit{1, 1},
                     MapData::RandomGlobalTable{9, 2}, MapData::RandomCustomTable{}}) {
                plan = fixture.plan; plan.units[index].placement.random = mode;
                Reject(fixture, plan, fixture.target, "LUA_MAP_RANDOM_DROP_UNSUPPORTED");
            }
        }
        plan = fixture.plan;
        for (auto& record : plan.units) record.placement.random = std::monostate{};
        const auto legacyModes = GenerateLuaMapScript(plan, fixture.document, fixture.catalog, fixture.target);
        Require(legacyModes.Succeeded() && legacyModes.text == generated.text,
            "fixed mode-1 and proven default mode0 must create the same Lua without source normalization");
        plan = fixture.plan; plan.units[5].waygateRegionSymbol.reset(); Reject(fixture, plan, fixture.target, "LUA_MAP_WAYGATE_UNBOUND");
        plan = fixture.plan; plan.settings.mapName = "TRIGSTR_999"; Reject(fixture, plan, fixture.target, "LUA_MAP_STRING_UNRESOLVED");
        auto target = fixture.target;
        std::erase_if(target.apis, [](const auto& value) { return value.name == "CreateUnit"; });
        Reject(fixture, fixture.plan, target, "LUA_MAP_API_MISSING");
        target = fixture.target;
        for (auto& value : target.apis) if (value.name == "CreateUnit") value.parameters[1] = "string";
        Reject(fixture, fixture.plan, target, "LUA_MAP_API_SIGNATURE");
        target = fixture.target;
        std::erase_if(target.constants, [](const auto& value) { return value.name == "CAMERA_FIELD_NEARZ"; });
        Reject(fixture, fixture.plan, target, "LUA_MAP_CONSTANT_MISSING");
        target = fixture.target; target.constants.push_back({fixture.plan.units[0].symbol, "unit"});
        Reject(fixture, fixture.plan, target, "LUA_TARGET_CONSTANT");
        auto document = fixture.document; document.wct.customCode.text = "// deliberately retained custom source";
        const auto retained = GenerateLuaMapScript(fixture.plan, document, fixture.catalog, fixture.target);
        Require(!retained.Succeeded() && retained.text.empty() && document.wct.customCode.text == "// deliberately retained custom source",
            "custom source was translated or mutated");
        SharedGeneratedNamesReachFinalLua(root / "generated-object-names");

        // 基础阶段也生成可执行完整脚本；空对象地图无需不存在的对象API。
        MapData::MapSettings settings; settings.formatVersion = 31; settings.tileset = 'L'; settings.custom.emplace();
        MapScriptInputs inputs; inputs.settings = &settings;
        const auto emptyPlan = BuildMapInitializationPlan(inputs);
        Require(emptyPlan.Succeeded(), Diagnostics(emptyPlan.diagnostics));
        const auto emptyMap = GenerateLuaMapScript(*emptyPlan.value, {}, fixture.catalog, fixture.target);
        Require(emptyMap.Succeeded(), Diagnostics(emptyMap.diagnostics)); Write(root / "empty-map.lua", emptyMap.text);

        wchar_t ydwe[32768]{};
        const auto count = GetEnvironmentVariableW(L"FORGE_YDWE_ROOT", ydwe, 32768);
        Require(count > 0 && count < 32768, "FORGE_YDWE_ROOT required for actual Lua 5.3 execution");
        Script::detail::ProcessRequest request;
        request.executable = std::filesystem::path(ydwe) / "bin/lua.exe";
        request.workingDirectory = root;
        request.arguments = {std::filesystem::path(LUA_MAP_TEST_RUNNER).wstring(), (root / "map.lua").wstring(),
            (root / "symbols.lua").wstring(), (root / "empty-map.lua").wstring()};
        request.outputFile = root / "runtime.log"; request.timeout = std::chrono::seconds(20);
        const auto execution = Script::detail::RunProcess(request);
        Require(execution.Succeeded(), execution.error + execution.output);
        std::cout << execution.output << "Complete Lua map generation, typed binding, rejection and source-map checks passed.\n"
            << "Artifacts: " << root.string() << '\n';
        return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
