#include "War3MapCore/MapData/TerrainStartLocations.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace War3Map::MapData {
namespace {
Status Fail(const char* message) {
    return Status::Failure("Terrain.StartLocations", "TERRAIN_START_LINKAGE_INVALID", message);
}
bool Same(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
}

Result<MapSettings> ReadTerrainClassicStartSettings(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > TerrainStartSettingsMaximumBytes)
        return Result<MapSettings>::Failure("Terrain.StartLocations", "TERRAIN_START_SETTINGS_BUDGET", "起始位置联动的完整W3I超过1MiB预算。");
    auto parsed = MapSettings::Parse(bytes);
    if (!parsed.Succeeded() || !parsed.value) return parsed;
    const auto wire = parsed.value->Serialize();
    if (parsed.value->formatVersion != 25 || !parsed.value->FullyParsed()
        || !wire.Succeeded() || !wire.value || !std::equal(bytes.begin(), bytes.end(), wire.value->begin(), wire.value->end()))
        return Result<MapSettings>::Failure("Terrain.StartLocations", "TERRAIN_START_SETTINGS_LAYOUT", "起始位置联动需要完整且逐字往返的经典W3I v25。");
    return parsed;
}

Result<std::vector<std::byte>> CanonicalTerrainStartSettings(std::span<const std::byte> bytes) {
    auto parsed = ReadTerrainClassicStartSettings(bytes);
    if (!parsed.Succeeded() || !parsed.value) return {{}, std::move(parsed.diagnostics)};
    parsed.value->mapName.clear(); parsed.value->author.clear();
    parsed.value->description.clear(); parsed.value->recommendedPlayers.clear();
    return parsed.value->Serialize();
}

Status VerifyTerrainStartLocationsChange(const UnitPlacements& beforeUnits,
    const UnitPlacements& afterUnits, const MapSettings& beforeSettings, const MapSettings& afterSettings) {
    if (beforeUnits.version != afterUnits.version || beforeUnits.subversion != afterUnits.subversion
        || beforeUnits.recordLayout != PlacementRecordLayout::Classic || afterUnits.recordLayout != PlacementRecordLayout::Classic
        || !IsClassicPlacementVersion(beforeUnits.version, beforeUnits.subversion)
        || beforeSettings.formatVersion != 25 || afterSettings.formatVersion != 25
        || !beforeSettings.FullyParsed() || !afterSettings.FullyParsed()
        || beforeSettings.players.size() > 16 || afterSettings.players.size() > 16)
        return Fail("起始位置联动不能改变经典布局或超出16个玩家。");
    const auto validSource = [](const UnitPlacements& units, const MapSettings& settings) {
        // 每个玩家至多一个起点；先限数量，再作至多16次完整身份扫描。
        // 未先限量时，恶意重复sloc会把线性codec预算变成平方工作。
        if (std::count_if(units.units.begin(), units.units.end(), [](const auto& unit) { return unit.rawcode == "sloc"; }) > 16)
            return false;
        std::array<bool, 16> players{}, owners{};
        for (const auto& player : settings.players) {
            if (player.number < 0 || player.number >= 16 || players[player.number]
                || !std::isfinite(player.startX) || !std::isfinite(player.startY)) return false;
            players[player.number] = true;
        }
        for (const auto& unit : units.units) if (unit.rawcode == "sloc") {
            if (unit.owner < 0 || unit.owner >= 16 || owners[unit.owner] || !players[unit.owner]
                || !std::isfinite(unit.position[0]) || !std::isfinite(unit.position[1])
                || std::count_if(units.units.begin(), units.units.end(), [&](const auto& other) {
                    return other.creationId == unit.creationId;
                }) != 1) return false;
            owners[unit.owner] = true;
        }
        return true;
    };
    if (!validSource(beforeUnits, beforeSettings) || !validSource(afterUnits, afterSettings))
        return Fail("起始位置来源需要有限坐标、唯一玩家和唯一creationId。");
    auto expected = beforeSettings;
    std::size_t moved = 0;
    for (const auto& unit : afterUnits.units) {
        if (unit.rawcode != "sloc") continue;
        if (unit.owner < 0 || unit.owner >= 16 || !std::isfinite(unit.position[0]) || !std::isfinite(unit.position[1])
            || std::count_if(afterUnits.units.begin(), afterUnits.units.end(), [&](const auto& other) { return other.creationId == unit.creationId; }) != 1
            || std::count_if(afterUnits.units.begin(), afterUnits.units.end(), [&](const auto& other) { return other.rawcode == "sloc" && other.owner == unit.owner; }) != 1)
            return Fail("起始位置必须具有唯一creationId、唯一玩家和有限坐标。");
        const auto old = std::find_if(beforeUnits.units.begin(), beforeUnits.units.end(), [&](const auto& other) { return other.creationId == unit.creationId; });
        if (old != beforeUnits.units.end()) {
            if (old->rawcode != "sloc" || old->owner != unit.owner
                || std::count_if(beforeUnits.units.begin(), beforeUnits.units.end(), [&](const auto& other) { return other.creationId == unit.creationId; }) != 1)
                return Fail("起始位置的creationId或所属玩家发生歧义。");
            if (Same(old->position[0], unit.position[0]) && Same(old->position[1], unit.position[1])) continue;
        }
        if (std::count_if(expected.players.begin(), expected.players.end(), [&](const auto& player) { return player.number == unit.owner; }) != 1)
            return Fail("起始位置没有唯一对应的W3I玩家。");
        auto player = std::find_if(expected.players.begin(), expected.players.end(), [&](const auto& p) { return p.number == unit.owner; });
        player->startX = unit.position[0]; player->startY = unit.position[1]; ++moved;
    }
    for (const auto& old : beforeUnits.units) if (old.rawcode == "sloc") {
        if (std::count_if(beforeUnits.units.begin(), beforeUnits.units.end(), [&](const auto& other) { return other.creationId == old.creationId; }) != 1
            || std::count_if(afterUnits.units.begin(), afterUnits.units.end(), [&](const auto& other) { return other.creationId == old.creationId && other.rawcode == "sloc" && other.owner == old.owner; }) != 1)
            return Fail("起始位置不能被删除、重用creationId或更换玩家。");
    }
    if (!moved) return Fail("复合起始位置记录必须实际移动或增加起始位置。");
    const auto generated = expected.Serialize(), actual = afterSettings.Serialize();
    if (!generated.Succeeded() || !generated.value) return {generated.diagnostics};
    if (!actual.Succeeded() || !actual.value) return {actual.diagnostics};
    return *generated.value == *actual.value ? Status::Ok()
        : Fail("W3I除真实PrepareUnits产生的startX/startY之外还有变化。");
}

} // namespace War3Map::MapData
