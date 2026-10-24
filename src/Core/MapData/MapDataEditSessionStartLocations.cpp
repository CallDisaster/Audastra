#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainStartLocations.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace War3Map::MapData {
namespace {
Status Refuse() {
    return Status::Failure("Terrain.StartLocations", "TERRAIN_START_STATE_CHANGED",
        "起始位置或完整Units已脱离真实联动事务，不能从原始覆盖取得地形保存资格。");
}
}

Result<std::shared_ptr<const MapDataEditSession::TerrainStartLocationsState>>
MapDataEditSession::TerrainStartLocationsState::Capture(std::span<const std::byte> bytes,
    const UnitPlacements& units, const MapSettings& settings) {
    using Output = Result<std::shared_ptr<const TerrainStartLocationsState>>;
    if (bytes.empty() || bytes.size() > TerrainJournalMaximumBytes || settings.formatVersion != 25
        || !settings.FullyParsed() || settings.players.size() > 16
        || units.recordLayout != PlacementRecordLayout::Classic || !IsClassicPlacementVersion(units.version, units.subversion))
        return {{}, Refuse().diagnostics};
    auto state = std::make_shared<TerrainStartLocationsState>();
    for (const auto& player : settings.players) {
        if (player.number < 0 || player.number >= 16 || !std::isfinite(player.startX) || !std::isfinite(player.startY)
            || std::any_of(state->players.begin(), state->players.begin() + state->count,
                [&](const auto& other) { return other.number == player.number; })) return {{}, Refuse().diagnostics};
        state->players[state->count++] = {player.number, std::bit_cast<std::uint32_t>(player.startX), std::bit_cast<std::uint32_t>(player.startY)};
    }
    // 先限起点数量，再作至多16次完整身份扫描，避免恶意sloc数量形成平方工作量。
    if (std::count_if(units.units.begin(), units.units.end(), [](const auto& unit) { return unit.rawcode == "sloc"; }) > 16)
        return {{}, Refuse().diagnostics};
    std::array<bool, 16> seen{};
    for (const auto& unit : units.units) if (unit.rawcode == "sloc") {
        if (unit.owner < 0 || unit.owner >= 16 || seen[unit.owner] || !std::isfinite(unit.position[0]) || !std::isfinite(unit.position[1])
            || std::count_if(units.units.begin(), units.units.end(), [&](const auto& other) { return other.creationId == unit.creationId; }) != 1
            || std::none_of(settings.players.begin(), settings.players.end(), [&](const auto& player) { return player.number == unit.owner; }))
            return {{}, Refuse().diagnostics};
        seen[unit.owner] = true;
    }
    const auto sha = Sha256(bytes);
    if (!sha.Succeeded() || !sha.value) return {{}, sha.diagnostics};
    state->unitsBytes = bytes.size(); state->unitsSha256 = *sha.value;
    return Output::Success(std::move(state));
}

bool MapDataEditSession::TerrainStartLocationsState::MatchesPlayers(const MapSettings& settings) const noexcept {
    if (settings.players.size() != count) return false;
    for (std::size_t i = 0; i < count; ++i)
        if (settings.players[i].number != players[i].number
            || std::bit_cast<std::uint32_t>(settings.players[i].startX) != players[i].x
            || std::bit_cast<std::uint32_t>(settings.players[i].startY) != players[i].y) return false;
    return true;
}

bool MapDataEditSession::TerrainStartLocationsState::Matches(std::span<const std::byte> units, const MapSettings& settings) const {
    return MatchesPlayers(settings) && MatchesUnits(units);
}

bool MapDataEditSession::TerrainStartLocationsState::MatchesUnits(std::span<const std::byte> units) const {
    if (units.size() != unitsBytes) return false;
    const auto sha = Sha256(units);
    return sha.Succeeded() && sha.value && *sha.value == unitsSha256;
}

void MapDataEditSession::TerrainStartLocationsState::RestorePlayers(MapSettings& settings) const noexcept {
    for (std::size_t i = 0; i < count && i < settings.players.size(); ++i) {
        settings.players[i].startX = std::bit_cast<float>(players[i].x);
        settings.players[i].startY = std::bit_cast<float>(players[i].y);
    }
}

void MapDataEditSession::InitializeTerrainStartLocations(const MapDocument& source) {
    const auto& units = original_[Index(MapDataKind::Units)];
    const auto& settings = original_[Index(MapDataKind::Settings)];
    if (!units || !settings || settings->size() > TerrainStartSettingsMaximumBytes
        || !source.Units() || !source.Settings()) return;
    auto state = TerrainStartLocationsState::Capture(*units, *source.Units(), *source.Settings());
    if (state.Succeeded() && state.value) startLocations_ = savedStartLocations_ = std::move(*state.value);
}

Status MapDataEditSession::PrepareTerrainStartLocations(Command& command, const UnitPlacements& beforeUnits,
    const UnitPlacements& afterUnits, const std::vector<MemberChange>& changes) const {
    if (!startLocations_) return Status::Ok(); // 不扩大旧版/非经典来源的可保存域。
    const auto& unitsBefore = current_[Index(MapDataKind::Units)];
    if (!unitsBefore) return Status::Ok();
    const MemberChange* unitChange = nullptr;
    const MemberChange* settingsChange = nullptr;
    for (const auto& change : changes) {
        if (change.kind == MapDataKind::Units) unitChange = &change;
        if (change.kind == MapDataKind::Settings) settingsChange = &change;
    }
    if (!unitChange || !unitChange->after) return Status::Ok();
    // 没有实际起点联动时，PrepareUnits已逐起点确认XY/owner/creationId未变。
    // 只刷新完整Units身份，不读取W3I；FTJ9的Units预算不能暗中触发额外W3I codec。
    if (!settingsChange) {
        if (afterUnits.recordLayout != PlacementRecordLayout::Classic
            || !IsClassicPlacementVersion(afterUnits.version, afterUnits.subversion)
            || !startLocations_->MatchesUnits(*unitsBefore)) return Status::Ok();
        const auto sha = Sha256(*unitChange->after);
        if (!sha.Succeeded() || !sha.value) return {sha.diagnostics};
        auto next = std::make_shared<TerrainStartLocationsState>(*startLocations_);
        next->unitsBytes = unitChange->after->size(); next->unitsSha256 = *sha.value;
        command.startsBefore = startLocations_; command.startsAfter = std::move(next);
        return Status::Ok();
    }
    const auto& before = current_[Index(MapDataKind::Settings)];
    if (!before) return Status::Ok();
    const auto oldSettings = ReadTerrainClassicStartSettings(*before);
    if (!oldSettings.Succeeded() || !oldSettings.value || !startLocations_->Matches(*unitsBefore, *oldSettings.value))
        return Status::Ok(); // raw覆盖之后不能再借一次普通typed操作洗成可信来源。
    const auto& outputSettings = settingsChange ? settingsChange->after : before;
    if (!outputSettings) return Refuse();
    auto parsed = ReadTerrainClassicStartSettings(*outputSettings);
    if (!parsed.Succeeded() || !parsed.value) return {parsed.diagnostics};
    if (settingsChange) {
        const auto linked = VerifyTerrainStartLocationsChange(beforeUnits, afterUnits, *oldSettings.value, *parsed.value);
        if (!linked.Succeeded()) return linked;
    }
    const auto next = TerrainStartLocationsState::Capture(*unitChange->after, afterUnits, *parsed.value);
    if (!next.Succeeded() || !next.value) return {next.diagnostics};
    command.startsBefore = startLocations_; command.startsAfter = *next.value;
    return Status::Ok();
}

Status MapDataEditSession::NormalizeTerrainStartLocationsForSave(MapSettings& current, const MapSettings& original) const {
    bool same = current.players.size() == original.players.size();
    if (same) for (std::size_t i = 0; i < original.players.size(); ++i)
        same &= current.players[i].number == original.players[i].number
            && std::bit_cast<std::uint32_t>(current.players[i].startX) == std::bit_cast<std::uint32_t>(original.players[i].startX)
            && std::bit_cast<std::uint32_t>(current.players[i].startY) == std::bit_cast<std::uint32_t>(original.players[i].startY);
    if (same) return Status::Ok();
    const auto& oldUnits = original_[Index(MapDataKind::Units)];
    const auto& newUnits = current_[Index(MapDataKind::Units)];
    if (!savedStartLocations_ || !startLocations_ || !oldUnits || !newUnits
        || !savedStartLocations_->Matches(*oldUnits, original) || !startLocations_->Matches(*newUnits, current)) return Refuse();
    savedStartLocations_->RestorePlayers(current);
    return Status::Ok();
}

} // namespace War3Map::MapData
