#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Script::SlkMapMemberProtocol {
constexpr std::array<std::string_view, 9> kMembers{
    "war3map.w3u", "war3map.w3a", "war3map.w3t", "war3map.w3h", "war3map.w3q",
    "war3map.w3d", "war3map.w3b", "war3map.wts", "war3map.w3i"};
// 固定安装 core/info.lua 的数据成员清单；这里只声明路径协议，不移植解析实现。
constexpr std::string_view kMapSources[]{
    "war3mapmisc.txt",
    "units\\abilitydata.slk", "units\\destructabledata.slk", "doodads\\doodads.slk",
    "units\\abilitybuffdata.slk", "units\\upgradedata.slk", "units\\itemdata.slk",
    "units\\unitui.slk", "units\\unitdata.slk", "units\\unitbalance.slk",
    "units\\unitabilities.slk", "units\\unitweapons.slk",
    "units\\campaignunitstrings.txt", "units\\humanunitstrings.txt", "units\\neutralunitstrings.txt",
    "units\\nightelfunitstrings.txt", "units\\orcunitstrings.txt", "units\\undeadunitstrings.txt",
    "units\\campaignunitfunc.txt", "units\\humanunitfunc.txt", "units\\neutralunitfunc.txt",
    "units\\nightelfunitfunc.txt", "units\\orcunitfunc.txt", "units\\undeadunitfunc.txt",
    "units\\campaignabilitystrings.txt", "units\\commonabilitystrings.txt", "units\\humanabilitystrings.txt",
    "units\\neutralabilitystrings.txt", "units\\nightelfabilitystrings.txt", "units\\orcabilitystrings.txt",
    "units\\undeadabilitystrings.txt", "units\\itemabilitystrings.txt",
    "units\\campaignabilityfunc.txt", "units\\commonabilityfunc.txt", "units\\humanabilityfunc.txt",
    "units\\neutralabilityfunc.txt", "units\\nightelfabilityfunc.txt", "units\\orcabilityfunc.txt",
    "units\\undeadabilityfunc.txt", "units\\itemabilityfunc.txt",
    "units\\campaignupgradestrings.txt", "units\\neutralupgradestrings.txt", "units\\nightelfupgradestrings.txt",
    "units\\humanupgradestrings.txt", "units\\orcupgradestrings.txt", "units\\undeadupgradestrings.txt",
    "units\\campaignupgradefunc.txt", "units\\humanupgradefunc.txt", "units\\neutralupgradefunc.txt",
    "units\\nightelfupgradefunc.txt", "units\\orcupgradefunc.txt", "units\\undeadupgradefunc.txt",
    "units\\itemstrings.txt", "units\\itemfunc.txt",
    "ui\\miscdata.txt", "units\\miscdata.txt", "units\\miscgame.txt"};
struct TableAliases { std::string_view name, first, second; };
constexpr TableAliases kTableSources[]{
    {"ability", "table\\ability.ini", "war3map.w3a.ini"},
    {"destructable", "table\\destructable.ini", "war3map.w3b.ini"},
    {"doodad", "table\\doodad.ini", "war3map.w3d.ini"},
    {"buff", "table\\buff.ini", "war3map.w3h.ini"},
    {"upgrade", "table\\upgrade.ini", "war3map.w3q.ini"},
    {"item", "table\\item.ini", "war3map.w3t.ini"},
    {"unit", "table\\unit.ini", "war3map.w3u.ini"},
    {"misc", "table\\misc.ini", {}},
    {"w3i", "table\\w3i.ini", "war3map.w3i.ini"},
    {"txt", "table\\txt.ini", "war3map.txt.ini"},
    {"imp", "table\\imp.ini", "war3map.imp.ini"},
    {"doo", "table\\doo.ini", "war3map.doo.ini"}};
constexpr std::string_view kLocales[]{"w3x2lni\\locale\\w3i.lng", "w3x2lni\\locale\\lml.lng"};
constexpr std::string_view kFrontendProfile = "fixed-info-proxy-exact-frontend-20261001-v2";

inline const std::vector<std::string>& FixedNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (auto name : kMembers) out.emplace_back(name);
        for (auto name : kMapSources) out.emplace_back(name);
        for (auto name : kMembers) out.push_back("map\\" + std::string(name));
        for (auto name : kMapSources) out.push_back("map\\" + std::string(name));
        for (const auto& table : kTableSources) {
            out.emplace_back(table.first);
            if (!table.second.empty()) out.emplace_back(table.second);
        }
        for (auto name : kLocales) out.emplace_back(name);
        return out;
    }();
    return names;
}
}

