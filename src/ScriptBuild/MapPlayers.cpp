#include "MapSections.h"

#include "ScriptNames.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace War3Map::Script::detail {
namespace {

std::string Raw(const MapData::SettingsRawcode& raw) {
    std::string text;
    for (const auto byte : raw) text += static_cast<char>(byte);
    return text;
}

std::string PlayerOf(std::int32_t number) { return "Player(" + std::to_string(number) + ")"; }

const std::vector<MapData::MapPlayerSettings>& Players(const MapContext& context) {
    static const std::vector<MapData::MapPlayerSettings> none;
    return context.in.settings != nullptr ? context.in.settings->players : none;
}

std::string RacePreference(std::int32_t race) {
    switch (race) {
    case 1: return "RACE_PREF_HUMAN";
    case 2: return "RACE_PREF_ORC";
    case 3: return "RACE_PREF_UNDEAD";
    case 4: return "RACE_PREF_NIGHTELF";
    default: return "RACE_PREF_RANDOM";  // 仅 0；未知值已在 WritePlayers 拒绝
    }
}

std::string Controller(std::int32_t controller) {
    switch (controller) {
    case 2: return "MAP_CONTROL_COMPUTER";
    case 3: return "MAP_CONTROL_NEUTRAL";
    case 4: return "MAP_CONTROL_RESCUABLE";
    default: return "MAP_CONTROL_USER";  // 仅 1；未知值已在 WritePlayers 拒绝
    }
}

// 地形集 → 环境音效名；昼夜模型按地形集分组。
std::string TilesetAmbient(char tileset) {
    static const std::map<char, const char*> names = {
        {'A', "Ashenvale"}, {'B', "Barrens"}, {'C', "Felwood"}, {'D', "Dungeon"}, {'F', "LordaeronFall"},
        {'G', "Dungeon"}, {'I', "Icecrown"}, {'J', "DalaranRuins"}, {'K', "BlackCitadel"}, {'L', "LordaeronSummer"},
        {'N', "Northrend"}, {'O', "Outland"}, {'Q', "VillageFall"}, {'V', "Village"}, {'W', "LordaeronWinter"},
        {'X', "Dalaran"}, {'Y', "CityScape"}, {'Z', "SunkenRuins"}};
    const auto found = names.find(tileset);
    return found == names.end() ? "LordaeronSummer" : found->second;
}

std::string DayNightGroup(char tileset) {
    // 经典 WE 的昼夜模型分组；G 与 D、Y 与 X 不能合并。
    // 对照：War3Net.Build.Core/Providers/LightEnvironmentProvider.cs。
    switch (tileset) {
    case 'A': return "Ashenvale";
    case 'C': return "Felwood";
    case 'D': return "Dungeon";
    case 'G': return "Underground";
    case 'J':
    case 'X': return "Dalaran";
    case 'B': case 'F': case 'I': case 'K': case 'L': case 'N':
    case 'O': case 'Q': case 'V': case 'W': case 'Y': case 'Z': return "Lordaeron";
    default: return {};
    }
}

std::string Unit255(std::uint8_t value) { return Real3(value / 255.0); }

} // namespace

bool WriteUpgrades(MapContext& context) {
    const auto* settings = context.in.settings;
    if (settings == nullptr || !settings->custom || settings->custom->upgrades.empty()) return false;
    auto& out = context.out;
    WriteSectionBanner(out, "Upgrades");
    std::vector<std::string> functions;
    for (const auto& player : Players(context)) {
        // 每个科技只写最终状态：最高的已研究等级，或第一个不可用等级。按首次出现的顺序输出。
        std::vector<std::string> order;
        std::map<std::string, std::pair<int, std::int32_t>> state; // id → (可用性, 等级)
        for (const auto& upgrade : settings->custom->upgrades) {
            if ((upgrade.playerMask & (1u << player.number)) == 0) continue;
            if (upgrade.availability != 0 && upgrade.availability != 2) continue;
            const std::string id = Raw(upgrade.id);
            auto [found, inserted] = state.try_emplace(id, upgrade.availability, upgrade.level);
            if (inserted) {
                order.push_back(id);
            } else if (upgrade.availability == 2 && found->second.first == 2) {
                found->second.second = std::max(found->second.second, upgrade.level);
            } else if (upgrade.availability == 0 && found->second.first == 0) {
                found->second.second = std::min(found->second.second, upgrade.level);
            }
        }
        std::vector<std::string> lines;
        for (const auto& id : order) {
            const auto& [availability, level] = state.at(id);
            if (availability == 0) {
                lines.push_back("call SetPlayerTechMaxAllowed( " + PlayerOf(player.number) + ", " + Rawcode(id) + ", " + std::to_string(level) + " )");
            } else {
                lines.push_back("call SetPlayerTechResearched( " + PlayerOf(player.number) + ", " + Rawcode(id) + ", " + std::to_string(level + 1) + " )");
            }
        }
        if (lines.empty()) continue;
        const std::string name = "InitUpgrades_Player" + std::to_string(player.number);
        functions.push_back(name);
        out.Line("function " + name + " takes nothing returns nothing");
        out.Begin();
        for (const auto& line : lines) out.Line(line);
        out.End();
        out.Line("endfunction");
        out.Line("");
    }
    out.Line("function InitUpgrades takes nothing returns nothing");
    out.Begin();
    for (const auto& name : functions) out.Line("call " + name + "(  )");
    out.End();
    out.Line("endfunction");
    out.Line("");
    return true;
}

bool WriteTechRestrictions(MapContext& context) {
    const auto* settings = context.in.settings;
    if (settings == nullptr || !settings->custom || settings->custom->techRestrictions.empty()) return false;
    auto& out = context.out;
    WriteSectionBanner(out, "Tech Tree");
    out.Line("function InitTechTree takes nothing returns nothing");
    out.Begin();
    for (const auto& player : Players(context)) {
        for (const auto& restriction : settings->custom->techRestrictions) {
            if ((restriction.playerMask & (1u << player.number)) == 0) continue;
            out.Line("call SetPlayerTechMaxAllowed( " + PlayerOf(player.number) + ", " + Rawcode(Raw(restriction.id)) + ", 0 )");
        }
    }
    out.End();
    out.Line("endfunction");
    out.Line("");
    return true;
}

void WritePlayers(MapContext& context) {
    auto& out = context.out;
    const auto& players = Players(context);
    const std::uint32_t mapFlags = context.in.settings != nullptr ? context.in.settings->flags : 0;
    for (const auto& player : players) {
        if (MapData::KnownPlayerController(player.controller) && MapData::KnownPlayerRace(player.race)) continue;
        context.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Map", "MAP_PLAYER_ENUM_UNSUPPORTED",
            "war3map.w3i 玩家 " + std::to_string(player.number) + " 的控制者或种族未知：controller="
                + std::to_string(player.controller) + "，race=" + std::to_string(player.race)
                + "；不能退回用户/随机种族默认值。",
            OriginSpan{"war3map.w3i", 0, 0, {}}});
        return;
    }
    WriteSectionBanner(out, "Players");
    out.Line("function InitCustomPlayerSlots takes nothing returns nothing");
    out.Line("");
    out.Begin();
    for (std::size_t index = 0; index < players.size(); ++index) {
        const auto& player = players[index];
        const std::string p = PlayerOf(player.number);
        out.Line("// Player " + std::to_string(player.number));
        out.Line("call SetPlayerStartLocation( " + p + ", " + std::to_string(index) + " )");
        if ((player.flags & 1u) != 0) out.Line("call ForcePlayerStartLocation( " + p + ", " + std::to_string(index) + " )");
        out.Line("call SetPlayerColor( " + p + ", ConvertPlayerColor(" + std::to_string(player.number) + ") )");
        out.Line("call SetPlayerRacePreference( " + p + ", " + RacePreference(player.race) + " )");
        const bool selectable = player.race == 0 || (mapFlags & 0x20u) == 0;
        out.Line("call SetPlayerRaceSelectable( " + p + ", " + (selectable ? "true" : "false") + " )");
        out.Line("call SetPlayerController( " + p + ", " + Controller(player.controller) + " )");
        out.Line("");
    }
    out.End();
    out.Line("endfunction");
    out.Line("");

    out.Line("function InitCustomTeams takes nothing returns nothing");
    out.Begin();
    const std::vector<MapData::MapForceSettings> none;
    const auto& forces = context.in.settings != nullptr ? context.in.settings->forces : none;
    for (std::size_t force = 0; force < forces.size(); ++force) {
        std::vector<std::int32_t> members;
        for (const auto& player : players) {
            if ((forces[force].playerMask & (1u << player.number)) != 0) members.push_back(player.number);
        }
        out.Line("// Force: " + forces[force].name);
        for (const auto member : members) out.Line("call SetPlayerTeam( " + PlayerOf(member) + ", " + std::to_string(force) + " )");
        const auto pairs = [&](const char* title, std::uint32_t flag, const char* function) {
            out.Line("");
            out.Line(title);
            if ((forces[force].flags & flag) == 0) return;
            for (const auto a : members) {
                for (const auto b : members) {
                    if (a != b) out.Line(std::string("call ") + function + "( " + PlayerOf(a) + ", " + PlayerOf(b) + ", true )");
                }
            }
        };
        pairs("//   Allied", 0x1u, "SetPlayerAllianceStateAllyBJ");
        pairs("//   Shared Vision", 0x8u, "SetPlayerAllianceStateVisionBJ");
        if ((forces[force].flags & 0x10u) != 0) pairs("//   Shared Control", 0x10u, "SetPlayerAllianceStateControlBJ");
        if ((forces[force].flags & 0x20u) != 0) pairs("//   Advanced Control", 0x20u, "SetPlayerAllianceStateFullControlBJ");
        out.Line("");
    }
    out.End();
    out.Line("endfunction");
    out.Line("");

    out.Line("function InitAllyPriorities takes nothing returns nothing");
    out.Begin();
    for (std::size_t index = 0; index < players.size(); ++index) {
        std::vector<std::pair<std::size_t, bool>> priorities;
        for (std::size_t other = 0; other < players.size(); ++other) {
            const std::uint32_t bit = 1u << players[other].number;
            if (other == index) continue;
            if ((players[index].allyHighPriorityMask & bit) != 0) priorities.emplace_back(other, true);
            else if ((players[index].allyLowPriorityMask & bit) != 0) priorities.emplace_back(other, false);
        }
        if (priorities.empty()) continue;
        out.Line("");
        out.Line("call SetStartLocPrioCount( " + std::to_string(index) + ", " + std::to_string(priorities.size()) + " )");
        for (std::size_t k = 0; k < priorities.size(); ++k) {
            out.Line("call SetStartLocPrio( " + std::to_string(index) + ", " + std::to_string(k) + ", " +
                std::to_string(priorities[k].first) + ", " + (priorities[k].second ? "MAP_LOC_PRIO_HIGH" : "MAP_LOC_PRIO_LOW") + " )");
        }
    }
    out.End();
    out.Line("endfunction");
    out.Line("");
}

void WriteMain(MapContext& context, bool upgrades, bool techTree) {
    auto& out = context.out;
    const auto* settings = context.in.settings;
    std::string lightGroup;
    std::string weather;
    std::array<double, 4> weatherBounds{};
    bool hasGlobalWeather = false;
    if (settings != nullptr) {
        const auto light = settings->environment.lightEnvironment != 0
            ? settings->environment.lightEnvironment : settings->tileset;
        lightGroup = DayNightGroup(static_cast<char>(light));
        if (lightGroup.empty()) {
            context.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Map", "MAP_LIGHT_ENVIRONMENT_INVALID",
                "war3map.w3i 的光照环境无效：lightEnvironment=" + std::to_string(settings->environment.lightEnvironment)
                    + "，tileset=" + std::to_string(settings->tileset) + "；未生成猜测的昼夜模型。",
                OriginSpan{"war3map.w3i", 0, 0, {}}});
            return;
        }
        weather = Raw(settings->environment.weather);
        hasGlobalWeather = std::any_of(weather.begin(), weather.end(), [](char byte) { return byte != '\0'; });
        if (hasGlobalWeather) {
            const auto& terrain = context.in.terrain;
            bool valid = terrain && ValidTerrainExtent(*terrain);
            if (valid) {
                // 全局天气覆盖整个地形；w3i 镜头边距只限制镜头，不能裁掉天气范围。
                weatherBounds = {terrain->offsetX, terrain->offsetY,
                    terrain->offsetX + (static_cast<double>(terrain->width) - 1.0) * 128.0,
                    terrain->offsetY + (static_cast<double>(terrain->height) - 1.0) * 128.0};
                valid = std::isfinite(weatherBounds[2]) && std::isfinite(weatherBounds[3])
                    && std::isfinite(static_cast<float>(weatherBounds[2])) && std::isfinite(static_cast<float>(weatherBounds[3]))
                    && static_cast<float>(weatherBounds[2]) > terrain->offsetX
                    && static_cast<float>(weatherBounds[3]) > terrain->offsetY;
            }
            if (!valid) {
                context.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Map", "MAP_WEATHER_TERRAIN_INVALID",
                    "war3map.w3i 已设置全局天气，但 war3map.w3e 的地形范围缺失或无效；无法生成天气区域。",
                    OriginSpan{"war3map.w3e", 0, 0, {}}});
                return;
            }
        }
        if (context.in.terrain && !ValidTerrainExtent(*context.in.terrain)) {
            context.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Map", "MAP_TERRAIN_INVALID",
                "地形范围无效，无法生成镜头边界。", OriginSpan{"war3map.w3e", 0, 0, {}}});
            return;
        }
    }
    WriteSectionBanner(out, "Main Initialization");
    out.Line("//===========================================================================");
    out.Line("function main takes nothing returns nothing");
    out.Begin();
    if (hasGlobalWeather) out.Line("local weathereffect we");
    if (settings != nullptr) {
        auto b = settings->cameraBounds;
        if (const auto& terrain = context.in.terrain) {
            // WE 按地形范围减去 w3i 镜头边距（左、右、下、上，单位格）重新计算，不直接用 w3i 记录的边界。
            const auto& m = settings->cameraMargins;
            const float l = terrain->offsetX + static_cast<float>(m[0]) * 128.0f;
            const float r = static_cast<float>(terrain->offsetX + (static_cast<double>(terrain->width) - 1 - m[1]) * 128.0);
            const float bo = terrain->offsetY + static_cast<float>(m[2]) * 128.0f;
            const float t = static_cast<float>(terrain->offsetY + (static_cast<double>(terrain->height) - 1 - m[3]) * 128.0);
            b = {l, bo, r, t, l, t, r, bo};
        }
        const auto left = [&](float x) { return Real1(x) + " + GetCameraMargin(CAMERA_MARGIN_LEFT)"; };
        const auto right = [&](float x) { return Real1(x) + " - GetCameraMargin(CAMERA_MARGIN_RIGHT)"; };
        const auto bottom = [&](float y) { return Real1(y) + " + GetCameraMargin(CAMERA_MARGIN_BOTTOM)"; };
        const auto top = [&](float y) { return Real1(y) + " - GetCameraMargin(CAMERA_MARGIN_TOP)"; };
        out.Line("call SetCameraBounds( " + left(b[0]) + ", " + bottom(b[1]) + ", " + right(b[2]) + ", " + top(b[3]) + ", " +
            left(b[4]) + ", " + top(b[5]) + ", " + right(b[6]) + ", " + bottom(b[7]) + " )");
        const char tileset = static_cast<char>(settings->tileset);
        out.Line("call SetDayNightModels( \"Environment\\\\DNC\\\\DNC" + lightGroup + "\\\\DNC" + lightGroup + "Terrain\\\\DNC" + lightGroup +
            "Terrain.mdl\", \"Environment\\\\DNC\\\\DNC" + lightGroup + "\\\\DNC" + lightGroup + "Unit\\\\DNC" + lightGroup + "Unit.mdl\" )");
        const auto& environment = settings->environment;
        if ((settings->flags & MapData::kMapFlagTerrainFog) != 0) {
            const auto& fog = environment.fogColor;
            out.Line("call SetTerrainFogEx( " + std::to_string(environment.fogStyle) + ", " + Real1(environment.fogStart) + ", " +
                Real1(environment.fogEnd) + ", " + Real3(environment.fogDensity) + ", " + Unit255(fog.red) + ", " +
                Unit255(fog.green) + ", " + Unit255(fog.blue) + " )");
        }
        if ((settings->flags & 0x10000u) != 0) {
            const auto& water = environment.waterColor;
            out.Line("call SetWaterBaseColor( " + std::to_string(water.red) + ", " + std::to_string(water.green) + ", " +
                std::to_string(water.blue) + ", " + std::to_string(water.alpha) + " )");
        }
        if (hasGlobalWeather) {
            out.Line("set we = AddWeatherEffect( Rect(" + Real1(weatherBounds[0]) + "," + Real1(weatherBounds[1]) + ","
                + Real1(weatherBounds[2]) + "," + Real1(weatherBounds[3]) + "), " + Rawcode(weather) + " )");
            out.Line("call EnableWeatherEffect( we, true )");
        }
        const std::string soundEnvironment = environment.soundEnvironment.empty() ? "Default" : environment.soundEnvironment;
        out.Line("call NewSoundEnvironment( " + QuoteJassString(soundEnvironment) + " )");
        const std::string ambient = TilesetAmbient(tileset);
        out.Line("call SetAmbientDaySound( \"" + ambient + "Day\" )");
        out.Line("call SetAmbientNightSound( \"" + ambient + "Night\" )");
    }
    out.Line("call SetMapMusic( \"Music\", true, 0 )");
    out.Line("call InitSounds(  )");
    out.Line("call CreateRegions(  )");
    if (context.in.cameras != nullptr && !context.in.cameras->entries.empty()) out.Line("call CreateCameras(  )");
    if (upgrades) out.Line("call InitUpgrades(  )");
    if (techTree) out.Line("call InitTechTree(  )");
    if (HasScriptedDestructables(context)) out.Line("call CreateAllDestructables(  )");
    if (HasPlacedItems(context)) out.Line("call CreateAllItems(  )");
    out.Line("call CreateAllUnits(  )");
    out.Line("call InitBlizzard(  )");
    out.Line("call InitGlobals(  )");
    out.Line("call InitCustomTriggers(  )");
    out.Line("call RunInitializationTriggers(  )");
    out.Line("");
    out.End();
    out.Line("endfunction");
    out.Line("");
}

void WriteConfig(MapContext& context) {
    auto& out = context.out;
    const auto* settings = context.in.settings;
    WriteSectionBanner(out, "Map Configuration");
    out.Line("function config takes nothing returns nothing");
    out.Begin();
    const auto& players = Players(context);
    if (settings != nullptr) {
        out.Line("call SetMapName( " + QuoteJassString(context.Resolve(settings->mapName)) + " )");
        out.Line("call SetMapDescription( " + QuoteJassString(context.Resolve(settings->description)) + " )");
    }
    out.Line("call SetPlayers( " + std::to_string(players.size()) + " )");
    out.Line("call SetTeams( " + std::to_string(players.size()) + " )");
    const bool customForces = settings != nullptr && (settings->flags & 0x40u) != 0;
    const bool hasAllyPriorities = std::any_of(players.begin(), players.end(), [](const auto& player) {
        return player.allyLowPriorityMask != 0 || player.allyHighPriorityMask != 0;
    });
    out.Line(std::string("call SetGamePlacement( ") + (hasAllyPriorities ? "MAP_PLACEMENT_TEAMS_TOGETHER" : "MAP_PLACEMENT_USE_MAP_SETTINGS") + " )");
    out.Line("");
    for (std::size_t index = 0; index < players.size(); ++index) {
        out.Line("call DefineStartLocation( " + std::to_string(index) + ", " + Real1(players[index].startX) + ", " +
            Real1(players[index].startY) + " )");
    }
    out.Line("");
    out.Line("// Player setup");
    out.Line("call InitCustomPlayerSlots(  )");
    // 普通对战图也会保存默认势力表；只有启用自定义势力时才按它分队。
    // WE 的普通玩家槽由大厅配置，不能把默认表中的所有玩家强制设为同队。
    if (customForces) {
        out.Line("call InitCustomTeams(  )");
    } else {
        for (const auto& player : players) {
            out.Line("call SetPlayerSlotAvailable( " + PlayerOf(player.number) + ", MAP_CONTROL_USER )");
        }
        out.Line("call InitGenericPlayerSlots(  )");
    }
    out.Line("call InitAllyPriorities(  )");
    out.End();
    out.Line("endfunction");
    out.Line("");
}

} // namespace War3Map::Script::detail
