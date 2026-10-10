#include "War3MapCore/Script/MapInitializationPlan.h"
#include "War3MapCore/MapData/GeneratedObjectSymbol.h"

#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

namespace {

using namespace War3Map;
using namespace War3Map::Script;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool HasCode(const Result<MapInitializationPlan>& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& diagnostic) { return diagnostic.code == code; });
}

void Rejected(const Result<MapInitializationPlan>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value && HasCode(result, code),
        "expected no partial plan and diagnostic " + std::string(code));
}

MapData::UnitPlacement Unit(std::string rawcode, std::int32_t owner, std::int32_t id) {
    MapData::UnitPlacement record;
    record.rawcode = std::move(rawcode); record.owner = owner; record.creationId = id;
    record.position = {64.0f, -128.0f, 32.0f}; record.rotation = -0.5f;
    record.scale = {1.0f, 1.0f, 1.0f}; record.targetAcquisition = -1.0f;
    record.heroLevel = 1;
    return record;
}

MapData::DoodadPlacement Doodad(std::string rawcode, std::uint8_t flags, std::int32_t id) {
    MapData::DoodadPlacement record;
    record.rawcode = std::move(rawcode); record.flags = flags; record.creationId = id;
    record.scale = {1.0f, 1.0f, 1.0f}; record.position = {32.0f, 64.0f, 5.0f};
    return record;
}

MapData::RegionRecord Region(std::string name, std::int32_t id) {
    MapData::RegionRecord record{};
    record.name = std::move(name); record.creationNumber = id;
    record.left = -128.0f; record.bottom = -256.0f; record.right = 256.0f; record.top = 128.0f;
    return record;
}

MapData::CameraRecord Camera(std::string name) {
    MapData::CameraRecord record{};
    record.name = std::move(name); record.nearClippingPlane = 16.0f;
    record.targetDistance = 1600.0f; record.farClippingPlane = 8000.0f; record.fieldOfView = 70.0f;
    return record;
}

MapData::SoundRecord Sound(std::string name, std::string path, bool music = false) {
    MapData::SoundRecord record{};
    const auto defaultReal = std::bit_cast<float>(MapData::kSoundDefaultRealBits);
    record.name = std::move(name); record.filePath = std::move(path); record.flags = music ? 8u : 3u;
    record.volume = record.priority = record.channel = record.coneOutsideVolume = MapData::kSoundDefaultInteger;
    record.pitch = record.pitchVariance = record.minDistance = record.maxDistance = record.distanceCutoff
        = record.coneAngleInside = record.coneAngleOutside = defaultReal;
    record.coneOrientation = {defaultReal, defaultReal, defaultReal};
    return record;
}

struct Fixture {
    MapData::MapSettings settings;
    MapData::UnitPlacements units;
    MapData::DoodadPlacements doodads;
    MapData::RegionsDocument regions;
    MapData::CamerasDocument cameras;
    MapData::SoundsDocument sounds;
    MapData::StringTableDocument strings;
    MapScriptInputs inputs;
    std::map<std::string, int> calls;

    Fixture() {
        settings.formatVersion = 25; settings.tileset = 'L'; settings.custom.emplace();
        settings.mapName = "Owned map"; settings.description = "Original description";
        settings.cameraBounds = {-1000.0f, -500.0f, 1000.0f, 500.0f, -1000.0f, 500.0f, 1000.0f, -500.0f};
        inputs.settings = &settings; inputs.units = &units; inputs.doodads = &doodads;
        inputs.regions = &regions; inputs.cameras = &cameras; inputs.sounds = &sounds;
        inputs.queries.isItem = [this](std::string_view id) {
            ++calls["item:" + std::string(id)]; return id == "ratf" || id == "amdt";
        };
        inputs.queries.isBuilding = [this](std::string_view id) {
            ++calls["building:" + std::string(id)]; return id == "hbar" || id == "hbld" || id == "nbld";
        };
        inputs.queries.isHero = [this](std::string_view id) {
            ++calls["hero:" + std::string(id)]; return id == "Hpal";
        };
        inputs.queries.unitTeamColor = [this](std::string_view id) {
            ++calls["color:" + std::string(id)]; return id == "hbar" ? 3 : -1;
        };
        inputs.queries.ability = [this](std::string_view id) {
            ++calls["ability:" + std::string(id)];
            ObjectQueries::AbilityInfo fact;
            fact.hero = false; fact.autocast = true; fact.orderOn = "innerfire"; fact.orderOff = "innerfireoff";
            return fact;
        };
        inputs.queries.soundLabel = [this](std::string_view path) -> std::optional<std::string> {
            ++calls["label:" + std::string(path)]; return "FrozenLabel";
        };
        inputs.queries.soundDuration = [this](std::string_view path) -> std::optional<int> {
            ++calls["duration:" + std::string(path)]; return 1234;
        };
    }
};

void OwnedFactsAndDeterministicOrder() {
    Fixture fixture;
    fixture.units.units = {Unit("hfoo", 1, 30), Unit("hbar", 1, 12), Unit("nbld", 12, 9),
        Unit("nbld", 15, 10), Unit("hbld", 0, 99), Unit("nfoo", 12, 44), Unit("nfoo", 15, 2),
        Unit("hfoo", 1, 4), Unit("hfoo", 0, 3), Unit("ratf", 15, 3), Unit("amdt", 15, 17),
        Unit("ratf", 15, 28), Unit("sloc", 0, 1)};
    fixture.units.units[0].abilities.push_back({"A001", true, 2});
    fixture.units.units[7].abilities.push_back({"A001", false, 0});
    fixture.units.units[0].flags = 2;
    fixture.units.units[0].unknownOwnerBytes = {0x12, 0x34};
    fixture.units.units[0].waygateRegionId = 91;
    fixture.regions.entries.push_back(Region("Destination", 91));
    fixture.cameras.entries.push_back(Camera("Overview"));
    fixture.sounds.entries = {Sound("Bell", "Sound\\Bell.wav"), Sound("Bell2", "Sound\\Bell.wav"),
        Sound("gg_snd_Theme", "Music\\Theme.mp3", true)};
    fixture.doodads.doodads = {Doodad("LTlt", 0, 70), Doodad("B00A", 2, 81), Doodad("DTsh", 4, 80)};
    fixture.doodads.specialDoodads.push_back({"ZPsh", 1, {4, 8}});
    fixture.settings.custom->upgrades.push_back({1, {std::byte{'R'}, std::byte{'h'}, std::byte{'m'}, std::byte{'e'}}, 1, 2});
    fixture.settings.custom->techRestrictions.push_back({1, {std::byte{'h'}, std::byte{'f'}, std::byte{'o'}, std::byte{'o'}}});
    fixture.settings.mapName = "TRIGSTR_7";
    fixture.strings.entries.push_back({7, "Captured title", 0});
    fixture.inputs.strings = &fixture.strings;

    auto built = BuildMapInitializationPlan(fixture.inputs);
    Require(built.Succeeded() && built.value.has_value(), "capture actual records without trigger/catalog dependencies");
    auto& plan = *built.value;
    Require(plan.unitCreationOrder == std::vector<std::size_t>({2, 3, 4, 1, 5, 6, 8, 7, 0}),
        "neutral buildings, player buildings, neutral units and player units preserve owner/creation ordering");
    Require(plan.itemCreationOrder == std::vector<std::size_t>({10, 11, 9}), "items retain reverse source order within rawcode");
    Require(plan.doodadCreationOrder == std::vector<std::size_t>({2, 0}), "scripted doodads have stable rawcode ordering");
    Require(plan.units.size() == 13 && plan.units[12].startLocation && plan.units[12].symbol.empty(), "start location retained as archive fact");
    Require(plan.doodads.size() == 3 && !plan.doodads[1].scripted && plan.doodads[1].symbol.empty()
        && plan.specialDoodads.size() == 1 && plan.specialDoodads[0].position[1] == 8, "archive driven doodads are not lost or emitted twice");
    Require(plan.units[0].symbol == "gg_unit_hfoo_0030" && plan.units[9].symbol == "gg_item_ratf_0003"
        && plan.doodads[0].symbol == "gg_dest_LTlt_0070", "source generated symbol naming");
    Require(plan.symbols.size() == 19, "complete source bound inventory, including music string but excluding archive only objects");
    const auto music = std::find_if(plan.symbols.begin(), plan.symbols.end(), [](const auto& symbol) { return symbol.name == "gg_snd_Theme"; });
    Require(music != plan.symbols.end() && music->type == "string" && music->member == "war3map.w3s"
        && music->sourceIndex == 2, "music is an owned string, not a sound handle");
    Require(plan.units[0].waygateRegionSymbol == plan.regions[0].symbol && plan.units[1].typeTeamColor == 3,
        "waygate and effective type color bind to captured facts");
    Require(plan.units[0].abilities[0].effective.orderOn == "innerfire"
        && plan.units[7].abilities[0].effective.orderOff == "innerfireoff", "abilities freeze resolved metadata");
    Require(plan.mainOrder == std::vector<MapInitStage>({MapInitStage::Sounds, MapInitStage::Regions, MapInitStage::Cameras,
        MapInitStage::Upgrades, MapInitStage::TechTree, MapInitStage::Destructables, MapInitStage::Items, MapInitStage::Units,
        MapInitStage::Blizzard, MapInitStage::Globals, MapInitStage::CustomTriggers, MapInitStage::InitializationTriggers}),
        "main honors actual initialization stage order");
    Require(fixture.calls["item:hfoo"] == 1 && fixture.calls["building:hfoo"] == 1 && fixture.calls["hero:hfoo"] == 1
        && fixture.calls["color:hfoo"] == 1 && fixture.calls["ability:A001"] == 1, "query each effective key once");
    Require(fixture.calls["label:Sound\\Bell.wav"] == 1 && fixture.calls["duration:Sound\\Bell.wav"] == 1,
        "same path sound facts captured once");
    Require(!fixture.calls.contains("item:sloc") && !fixture.calls.contains("building:ratf")
        && !fixture.calls.contains("duration:Music\\Theme.mp3"), "no fabricated unit facts for start markers/items/music");
    fixture.units.units[0].rawcode = "xxxx"; fixture.units.units[0].abilities.clear();
    fixture.settings.mapName = "Changed"; fixture.regions.entries[0].name = "Changed";
    fixture.strings.entries[0].value = "Changed title"; fixture.sounds.entries[0].filePath = "Changed.wav";
    fixture.inputs.queries = {};
    Require(plan.units[0].placement.rawcode == "hfoo" && plan.units[0].placement.flags == 2
        && plan.units[0].placement.unknownOwnerBytes == std::array<std::uint8_t, 2>{0x12, 0x34}, "owned placement retains archive fields");
    Require(plan.units[0].abilities.size() == 1 && plan.settings.mapName == "TRIGSTR_7"
        && plan.strings->entries[0].value == "Captured title" && plan.regions[0].region.name == "Destination"
        && plan.sounds[0].sound.filePath == "Sound\\Bell.wav" && plan.sounds[0].label == "FrozenLabel"
        && plan.sounds[0].duration == 1234, "input mutation and callback lifetime cannot alter a captured plan");
}

void EmptyAndPlayerProfiles() {
    Fixture fixture;
    fixture.inputs.queries = {};
    auto empty = BuildMapInitializationPlan(fixture.inputs);
    Require(empty.Succeeded() && empty.value->symbols.empty(), "empty map needs no object query");
    Require(empty.value->mainOrder == std::vector<MapInitStage>({MapInitStage::Sounds, MapInitStage::Regions,
        MapInitStage::Units, MapInitStage::Blizzard, MapInitStage::Globals, MapInitStage::CustomTriggers,
        MapInitStage::InitializationTriggers}), "only unused optional stages are absent");
    fixture.settings.formatVersion = 31; fixture.settings.scriptLanguage = 1;
    fixture.settings.graphicsModes = 3; fixture.settings.gameDataVersion = 1; fixture.settings.gameVersion = {1, 32, 0, 0};
    Require(BuildMapInitializationPlan(fixture.inputs).Succeeded(), "legal empty v31 metadata must not be blanket rejected");
    fixture.units.units.push_back(Unit("sloc", 0, 1));
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_PLAYER_LAYOUT_REQUIRED");
    MapInitializationOptions extended{MapInitializationPlayerLayout::Extended24};
    auto chosen = BuildMapInitializationPlan(fixture.inputs, extended);
    Require(chosen.Succeeded() && chosen.value->playerSlotCount == 24 && chosen.value->neutralAggressive == 24
        && chosen.value->neutralPassive == 27 && chosen.value->settings.graphicsModes == 3,
        "explicit source profile owns neutral numbering and preserves modern metadata");
    MapData::MapPlayerSettings player;
    player.number = 23; player.controller = 1; player.race = 0;
    fixture.settings.players.push_back(player);
    Require(BuildMapInitializationPlan(fixture.inputs, extended).Succeeded(), "explicit 24 player profile accepts slot 23");
    Rejected(BuildMapInitializationPlan(fixture.inputs, {MapInitializationPlayerLayout::Classic12}), "MAP_INIT_PLAYER_ID_INVALID");
    fixture.settings.players[0].enemyHighPriorityMask = 1;
    Rejected(BuildMapInitializationPlan(fixture.inputs, extended), "MAP_INIT_ENEMY_PRIORITY_UNSUPPORTED");
    fixture.settings.players[0].enemyHighPriorityMask = 0;
    fixture.settings.forceMaxCameraZoom = 0;
    Rejected(BuildMapInitializationPlan(fixture.inputs, extended), "MAP_INIT_SETTINGS_EXTENSION_UNSUPPORTED");
}

void FixedModeZeroPreservesOwnedPlacementsAndSymbols() {
    Fixture fixture;
    fixture.units.units = {Unit("I_u_", 0, 10000), Unit("h_i_", 15, std::numeric_limits<std::int32_t>::max()),
        Unit("sloc", 0, 5), Unit("hfoo", 0, 8)};
    for (auto& value : fixture.units.units) value.random = MapData::RandomAnyUnit{1, 0};
    fixture.units.units.back().random = std::monostate{};
    fixture.inputs.queries.isItem = [](std::string_view id) { return id == "h_i_"; };
    fixture.doodads.doodads.push_back(Doodad("B_d_", 0, 123456));
    const auto source = fixture.units.Serialize();
    Require(source.Succeeded(), "fixed mode0 records serialize without a normalization shim");
    auto read = MapData::ParseUnitPlacements(*source.value);
    Require(read.Succeeded(), "fixed mode0 records read through the real placement codec");
    fixture.units = std::move(*read.value);
    const auto built = BuildMapInitializationPlan(fixture.inputs);
    Require(built.Succeeded(), "known fixed mode0 unit/item/start-location records enter the owned plan");
    const auto& plan = *built.value;
    Require(plan.units.size() == 4 && plan.unitCreationOrder == std::vector<std::size_t>({3, 0})
        && plan.itemCreationOrder == std::vector<std::size_t>({1}) && plan.units[2].startLocation
        && plan.units[2].symbol.empty(), "sloc stays an archive start marker instead of becoming a created unit");
    Require(!plan.units[0].item && plan.units[1].item, "unit/item facts do not follow rawcode capitalization");
    const auto unit = MapData::GeneratedObjectSymbol("unit", "I_u_", 10000);
    const auto item = MapData::GeneratedObjectSymbol("item", "h_i_", std::numeric_limits<std::int32_t>::max());
    const auto dest = MapData::GeneratedObjectSymbol("dest", "B_d_", 123456);
    Require(unit.Succeeded() && item.Succeeded() && dest.Succeeded()
        && plan.units[0].symbol == *unit.value && plan.units[0].symbol == "gg_unit_I_u__10000"
        && plan.units[1].symbol == *item.value && plan.units[1].symbol == "gg_item_h_i__2147483647"
        && plan.doodads[0].symbol == *dest.value && plan.doodads[0].symbol == "gg_dest_B_d__123456",
        "shared generated symbol contract keeps safe underscores and full creation numbers for every kind");
    auto owned = fixture.units;
    owned.units.clear();
    for (const auto& record : plan.units) owned.units.push_back(record.placement);
    const auto captured = owned.Serialize();
    const auto unchanged = fixture.units.Serialize();
    Require(captured.Succeeded() && unchanged.Succeeded() && *captured.value == *source.value
        && *unchanged.value == *source.value, "neither capture nor owned records rewrite mode0 or any placement byte");
    fixture.units.units[0].random = MapData::RandomGlobalTable{9, 2};
    const auto* frozen = std::get_if<MapData::RandomAnyUnit>(&plan.units[0].placement.random);
    Require(frozen && frozen->level == 1 && frozen->itemClass == 0,
        "later provider mutation cannot turn the captured fixed record into a random table");

    for (const auto marker : {"uDNR", "bDNR", "iDNR"}) {
        for (const auto& mode : std::array<MapData::RandomUnitConfig, 2>{std::monostate{}, MapData::RandomAnyUnit{1, 0}}) {
            fixture.units.units = {Unit(marker, 0, 1)};
            fixture.units.units.front().random = mode;
            Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_RANDOM_PLACEMENT_UNSUPPORTED");
        }
    }
    for (const auto ordinary : {"I_u_", "h_i_", "sloc"}) {
        for (const auto& mode : std::array<MapData::RandomUnitConfig, 6>{
                 MapData::RandomAnyUnit{}, MapData::RandomAnyUnit{-1, 0}, MapData::RandomAnyUnit{2, 0},
                 MapData::RandomAnyUnit{1, 1}, MapData::RandomGlobalTable{9, 2}, MapData::RandomCustomTable{}}) {
            fixture.units.units = {Unit(ordinary, 0, 1)};
            fixture.units.units.front().random = mode;
            Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_RANDOM_PLACEMENT_UNSUPPORTED");
        }
    }
    fixture.units.units = {Unit("I!u_", 0, 7)};
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_RAWCODE_INVALID");
    fixture.units.units = {Unit("I_u_", 0, -1)};
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_UNIT_LAYOUT_INVALID");
}

void BoundsAndStrings() {
    Fixture fixture;
    fixture.inputs.terrain = TerrainExtent{65, 33, -4096.0f, -2048.0f};
    fixture.settings.cameraMargins = {1, 2, 3, 4};
    fixture.settings.environment.weather = {std::byte{'R'}, std::byte{'A'}, std::byte{'h'}, std::byte{'r'}};
    const auto built = BuildMapInitializationPlan(fixture.inputs);
    Require(built.Succeeded() && built.value->weatherBounds == std::array<double, 4>{-4096, -2048, 4096, 2048},
        "global weather spans full terrain, not cropped camera bounds");
    Require(built.value->cameraBounds == std::array<float, 8>{-3968, -1664, 3840, 1536, -3968, 1536, 3840, -1664},
        "camera uses terrain plus left/right/bottom/top margins");
    fixture.inputs.terrain.reset();
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_WEATHER_TERRAIN_MISSING");
    fixture.settings.environment.weather = {};
    fixture.inputs.terrain = TerrainExtent{1, 33, 0, 0};
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_TERRAIN_INVALID");
    fixture.inputs.terrain = TerrainExtent{5, 5, 0, 0};
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_CAMERA_BOUNDS_INVALID");
    fixture.inputs.terrain.reset();
    fixture.settings.mapName = "TRIGSTR_7";
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_STRING_TABLE_MISSING");
    fixture.inputs.strings = &fixture.strings;
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_STRING_REFERENCE_INVALID");
    fixture.strings.entries = {{7, "Map title", 0}, {7, "Duplicate", 0}};
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_STRING_REFERENCE_INVALID");
    fixture.strings.entries.pop_back();
    Require(BuildMapInitializationPlan(fixture.inputs).Succeeded(), "unique WTS reference accepted without mutating source name");
    fixture.settings.tileset = '?';
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_ENVIRONMENT_UNKNOWN");
}

void SourceBindingAndQueryFailures() {
    Fixture fixture;
    fixture.units.units = {Unit("hfoo", 0, 8), Unit("hfoo", 1, 8)};
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_SYMBOL_COLLISION");
    fixture.units.units.pop_back();
    fixture.inputs.queries.isItem = {};
    auto missing = BuildMapInitializationPlan(fixture.inputs);
    Rejected(missing, "MAP_INIT_OBJECT_QUERY_MISSING");
    Require(missing.diagnostics.front().message.find("war3mapUnits.doo 记录[0]") != std::string::npos,
        "diagnostic names real member and record index without claiming a byte offset");
    fixture.inputs.queries.isItem = [](std::string_view) -> bool { throw std::runtime_error("query failed"); };
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_QUERY_FAILED");
    fixture.inputs.queries.isItem = [](std::string_view) { return false; };
    fixture.units.units[0].waygateRegionId = 92;
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_WAYGATE_UNBOUND");
    fixture.regions.entries = {Region("One", 92), Region("Two", 92)};
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_REGION_ID_INVALID");
    fixture.regions.entries = {Region("A-B", 92), Region("A B", 93)};
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_SYMBOL_COLLISION");
    fixture.regions.entries = {Region("One", 92)};
    fixture.units.units[0].abilities = {{"A001", false, 0}};
    fixture.inputs.queries.ability = {};
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_ABILITY_QUERY_MISSING");
    fixture.inputs.queries.ability = [](std::string_view) {
        ObjectQueries::AbilityInfo info; info.known = false; return info;
    };
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_ABILITY_UNKNOWN");
    fixture.units.units[0].abilities.clear();
    fixture.units.units[0].inventory = {{0, "ratf"}, {0, "amdt"}};
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_INVENTORY_SLOT_INVALID");
}

void RefusesUnimplementedDataWithoutLosingRecords() {
    Fixture fixture;
    fixture.settings.custom.reset();
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_SETTINGS_INCOMPLETE");
    fixture.settings.custom.emplace();
    fixture.settings.custom->randomItems.emplace_back();
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_RANDOM_TABLE_UNSUPPORTED");
    fixture.settings.custom->randomItems.clear();
    fixture.units.units = {Unit("hfoo", 0, 3)};
    fixture.units.units[0].random = MapData::RandomAnyUnit{};
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_RANDOM_PLACEMENT_UNSUPPORTED");
    fixture.units.units[0].random = std::monostate{};
    fixture.units.units[0].itemSets.push_back({{{"ratf", 100}}});
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_DROP_TABLE_UNSUPPORTED");
    fixture.units.units[0].itemSets = {{}};
    Require(BuildMapInitializationPlan(fixture.inputs).Succeeded(), "empty drop choices remain owned without fabricating a drop callback");
    fixture.units.units[0].skinId = "hfoo";
    Rejected(BuildMapInitializationPlan(fixture.inputs), "CLASSIC_UNITS_LAYOUT_UNSUPPORTED");
    fixture.units.units.clear();
    fixture.doodads.doodads.push_back(Doodad("LTlt", 1, 9));
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_DOODAD_FLAGS_UNSUPPORTED");
    fixture.doodads.doodads[0].flags = 0; fixture.doodads.doodads[0].lifePercent = 40;
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_DESTRUCTABLE_INIT_UNSUPPORTED");
    fixture.doodads.doodads.clear();
    fixture.cameras.entries.push_back(Camera("Extended"));
    fixture.cameras.entries[0].localRotation = std::array<float, 3>{};
    Rejected(BuildMapInitializationPlan(fixture.inputs), "CLASSIC_CAMERAS_LAYOUT_UNSUPPORTED");
    fixture.cameras.entries.clear();
    fixture.sounds.version = 2;
    Rejected(BuildMapInitializationPlan(fixture.inputs), "CLASSIC_SOUNDS_LAYOUT_UNSUPPORTED");
    fixture.sounds.version = 1;
    fixture.regions.entries.push_back(Region("Ambient", 1));
    fixture.regions.entries[0].ambientSound = "gg_snd_Ambient";
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_REGION_SOUND_UNSUPPORTED");
}

void SoundSentinelsAndNonFiniteValues() {
    Fixture fixture;
    fixture.sounds.entries = {Sound("Spatial", "Test.wav")};
    Require(BuildMapInitializationPlan(fixture.inputs).Succeeded(), "exact sound default sentinels are valid finite data");
    fixture.sounds.entries[0].minDistance = 400.0f;
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_SOUND_DEFAULT_GROUP_MIXED");
    fixture.sounds.entries[0].maxDistance = 4000.0f;
    Require(BuildMapInitializationPlan(fixture.inputs).Succeeded(), "uniform explicit sound distance group accepted");
    fixture.sounds.entries[0].coneOrientation[1] = 0;
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_SOUND_DEFAULT_GROUP_MIXED");
    fixture.sounds.entries[0].coneOrientation = {0.0f, 0.0f, 1.0f};
    fixture.sounds.entries[0].pitch = std::numeric_limits<float>::infinity();
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_VALUE_NONFINITE");
    fixture.sounds.entries.clear();
    fixture.units.units.push_back(Unit("hfoo", 0, 1));
    fixture.units.units[0].position[2] = std::numeric_limits<float>::quiet_NaN();
    Rejected(BuildMapInitializationPlan(fixture.inputs), "MAP_INIT_VALUE_NONFINITE");
}

} // namespace

int main() {
    try {
        OwnedFactsAndDeterministicOrder();
        EmptyAndPlayerProfiles();
        FixedModeZeroPreservesOwnedPlacementsAndSymbols();
        BoundsAndStrings();
        SourceBindingAndQueryFailures();
        RefusesUnimplementedDataWithoutLosingRecords();
        SoundSentinelsAndNonFiniteValues();
        std::cout << "Map initialization plan: owned facts, ordering, identity and refusal tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Map initialization plan test failure: " << error.what() << '\n';
        return 1;
    }
}
