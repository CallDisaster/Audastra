#include "War3MapCore/MapData/MapDataEditSession.h"
#include "MapDataEditSessionTerrainInternal.h"
#include "War3MapCore/MapData/MapPathing.h"
#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <stdexcept>

namespace War3Map::MapData {
namespace {

constexpr std::array<std::string_view, 7> kNativeMembers{
    "war3map.w3i", "war3mapUnits.doo", "war3map.doo", "war3map.w3r",
    "war3map.w3c", "war3map.w3s", "war3map.wts"};

bool SameCoordinate(float first, float second) {
    return std::bit_cast<std::uint32_t>(first) == std::bit_cast<std::uint32_t>(second);
}

Status StartFailure(const char* code, const char* message) {
    return Status::Failure("MapData.Edit", code, message);
}

constexpr std::string_view kTerrainMember = "war3map.w3e";
constexpr std::string_view kPathingMember = "war3map.wpm";
std::atomic<std::uint64_t> nextSessionIdentity{1};

// 历史恢复该笔荒芜与A；自动生成的水/地形位取最近一次真实保存基线。
// 这样跨水深保存后Undo不会把上一代0x40/0x80重新注入当前WPM。
std::byte HistoryPathingValue(std::span<const std::byte> bytes, std::span<const std::byte> source,
    std::span<const std::byte> baseline, std::span<const std::byte> intent, std::size_t index) {
    if (index < 16) return bytes[index];
    return (baseline[index] & std::byte{0xd1}) | (bytes[index] & std::byte{0x20})
        | source[index - 16] | intent[index - 16];
}

bool HistoryPathingMatches(std::span<const std::byte> current, std::span<const std::byte> bytes,
    std::span<const std::byte> source, std::span<const std::byte> baseline, std::span<const std::byte> intent) {
    if (current.size() != bytes.size() || baseline.size() != bytes.size()
        || source.size() + 16 != bytes.size() || intent.size() + 16 != bytes.size()) return false;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        if (current[i] != HistoryPathingValue(bytes, source, baseline, intent, i)) return false;
    return true;
}

std::vector<std::byte> HistoryPathing(std::span<const std::byte> bytes,
    std::span<const std::byte> source, std::span<const std::byte> baseline, std::span<const std::byte> intent) {
    if (baseline.size() != bytes.size() || source.size() + 16 != bytes.size() || intent.size() + 16 != bytes.size())
        throw std::logic_error("pathing history shape changed");
    std::vector<std::byte> result(bytes.begin(), bytes.end());
    for (std::size_t i = 16; i < result.size(); ++i) result[i] = HistoryPathingValue(bytes, source, baseline, intent, i);
    return result;
}
} // namespace

std::size_t MapDataEditSession::Index(MapDataKind kind) noexcept {
    return static_cast<std::size_t>(kind);
}

MapDataEditSession::MapDataEditSession(const MapDocument& source) {
    sessionIdentity_ = nextSessionIdentity.fetch_add(1, std::memory_order_relaxed);
    if (!sessionIdentity_) throw std::overflow_error("map edit session identity exhausted");
    for (std::size_t i = 0; i < states_.size(); ++i) {
        const auto& member = source.Member(static_cast<MapDataKind>(i));
        states_[i] = member.state;
        memberNames_[i] = std::string(kNativeMembers[i]);
        if ((member.state == MapDataState::Ready || member.state == MapDataState::Partial)
            && member.sources.size() == 1 && member.memberName == kNativeMembers[i])
            original_[i] = member.sources.front().data;
        current_[i] = original_[i];
        if (!original_[i] && member.state != MapDataState::Missing) preservedSources_[i] = member;
    }
    if (source.Doodads()) specialDoodads_ = std::make_shared<const std::vector<SpecialDoodadPlacement>>(source.Doodads()->specialDoodads);
    else if (states_[Index(MapDataKind::Doodads)] == MapDataState::Missing)
        specialDoodads_ = std::make_shared<const std::vector<SpecialDoodadPlacement>>();
    savedSpecialDoodads_ = specialDoodads_;
}

Status MapDataEditSession::Apply(MapDataKind kind, Result<std::vector<std::byte>> serialized) {
    std::vector<MemberChange> changes;
    const auto status = Prepare(kind, std::move(serialized), changes);
    return status.Succeeded() ? Commit(std::move(changes)) : status;
}

Status MapDataEditSession::Prepare(MapDataKind kind, Result<std::vector<std::byte>> serialized,
    std::vector<MemberChange>& changes) const {
    const auto index = Index(kind);
    if (index >= states_.size())
        return Status::Failure("MapData.Edit", "MAP_KIND_INVALID", "未知的公共地图成员类别");
    if (!serialized.Succeeded() || !serialized.value) {
        Status result;
        result.diagnostics = std::move(serialized.diagnostics);
        if (result.diagnostics.empty())
            return Status::Failure("MapData.Edit", "MAP_SERIALIZE_FAILED", "地图成员序列化失败");
        return result;
    }
    if (states_[index] != MapDataState::Ready && states_[index] != MapDataState::Missing
        && !(kind == MapDataKind::Settings && states_[index] == MapDataState::Partial))
        return Status::Failure("MapData.Edit", "MAP_SOURCE_UNSAFE",
            "来源损坏、歧义或部分无法解析，不能编辑该成员");
    if (states_[index] != MapDataState::Missing && !original_[index])
        return Status::Failure("MapData.Edit", "MAP_SOURCE_UNSUPPORTED",
            "无法确认唯一的原生公共地图成员，不能以新成员覆盖来源");
    auto after = std::optional(std::move(*serialized.value));
    MapDocument checked;
    checked.members_[index].sources.push_back({memberNames_[index], *after});
    // 布局读取依赖 W3I 来源；校验单个成员时不能丢失当前事务的地图版本。
    if (kind != MapDataKind::Settings) {
        const auto settingsIndex = Index(MapDataKind::Settings);
        const auto* settings = &current_[settingsIndex];
        for (const auto& change : changes) if (change.kind == MapDataKind::Settings) settings = &change.after;
        if (*settings) checked.members_[settingsIndex].sources.push_back({memberNames_[settingsIndex], **settings});
        else if (preservedSources_[settingsIndex]) checked.members_[settingsIndex] = *preservedSources_[settingsIndex];
    }
    checked.ParseMembers();
    const auto settingsIndex = Index(MapDataKind::Settings);
    if (kind != MapDataKind::Settings && !current_[settingsIndex] && preservedSources_[settingsIndex])
        checked.members_[settingsIndex] = *preservedSources_[settingsIndex];
    const auto& state = checked.Member(kind);
    const bool knownSettingsTail = kind == MapDataKind::Settings && state.state == MapDataState::Partial
        && std::all_of(state.diagnostics.begin(), state.diagnostics.end(), [](const Diagnostic& diagnostic) {
            return diagnostic.severity != DiagnosticSeverity::Error
                && (diagnostic.code == "W3I_UNKNOWN_TAIL" || diagnostic.code == "W3I_CUSTOM_TABLES_OMITTED");
        });
    if (state.state != MapDataState::Ready && !knownSettingsTail)
        return Status::Failure("MapData.Edit", "MAP_OUTPUT_UNSAFE",
            "修改后的地图成员存在重复身份或无法作为完整可编辑结构");
    if (after == current_[index]) return Status::Ok();
    changes.push_back({kind, current_[index], std::move(after)});
    return Status::Ok();
}

Status MapDataEditSession::Commit(std::vector<MemberChange> changes) {
    return CommitCommand(Command{std::move(changes), {}, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
}

Status MapDataEditSession::CommitCommand(Command value, const std::function<Status(const EditReceipt&)>& prepublish) {
    if (value.changes.empty() && value.terrain.empty() && !value.headerAfter && !value.pathingAfter && !value.restrictionAfter) return Status::Ok();
    if (revision_ == std::numeric_limits<std::uint64_t>::max() ||
        (!value.logicalIdentity && nextCommandIdentity_ == std::numeric_limits<std::uint64_t>::max()))
        return Status::Failure("MapData.Edit", "MAP_EDIT_ID_EXHAUSTED", "编辑修订或命令身份已用尽。");
    const bool newIdentity = !value.logicalIdentity;
    if (newIdentity) { value.sessionIdentity = sessionIdentity_; value.logicalIdentity = nextCommandIdentity_; }
    if (value.sessionIdentity != sessionIdentity_)
        return Status::Failure("MapData.Edit", "MAP_EDIT_RECEIPT_INVALID", "命令来源不是当前地图会话。");
    if (!value.terrain.empty() && !terrain_)
        return Status::Failure("MapData.Edit", "MAP_TERRAIN_UNAVAILABLE", "地形不可编辑");
    if (value.pathingBefore.has_value() != value.pathingAfter.has_value() ||
        (value.pathingBefore && (!pathing_ || *pathing_ != *value.pathingBefore)))
        return Status::Failure("MapData.Edit", "MAP_PATHING_STALE", "通行性来源已改变，不能提交旧笔划");
    if (static_cast<bool>(value.restrictionBefore) != static_cast<bool>(value.restrictionAfter)
        || (value.restrictionBefore && pathingRestrictions_ != value.restrictionBefore))
        return Status::Failure("MapData.Edit", "MAP_PATHING_STALE", "限制意图层已改变，不能提交旧笔划");
    if (static_cast<bool>(value.specialBefore) != static_cast<bool>(value.specialAfter)
        || (value.specialBefore && value.specialBefore != specialDoodads_))
        return Status::Failure("MapData.Edit", "TERRAIN_SPECIAL_STATE_STALE", "特殊模型事务来源已变，未提交任何成员。");
    value.beforeRevision = revision_; value.afterRevision = revision_ + 1;
    auto command = std::make_shared<const Command>(std::move(value));
    // 在触碰历史或覆盖前完成可能抛出异常的分配，关联成员与地形不能部分提交。
    std::vector<std::optional<std::vector<std::byte>>> next;
    next.reserve(command->changes.size());
    for (const auto& change : command->changes) next.push_back(change.after);
    std::shared_ptr<const TerrainGridSnapshot> nextTerrain = terrain_;
    if (!command->terrain.empty() || command->headerAfter) {
        auto copy = std::make_shared<TerrainGridSnapshot>(*terrain_);
        if (command->headerAfter) copy->header = *command->headerAfter;
        for (const auto& chunk : command->terrain) detail::WriteChunk(*copy, chunk.x0, chunk.y0, chunk.x1, chunk.y1, chunk.after);
        RefreshTerrainDerived(*copy, 0, 0, copy->header.width, copy->header.height);
        nextTerrain = std::move(copy);
    }
    std::optional<std::vector<std::byte>> nextPathing;
    if (command->pathingAfter) nextPathing = command->pathingAfter;
    auto nextRestrictions = command->restrictionAfter;
    history_.reserve(cursor_ + 1);
    const EditReceipt prepared(command, this, revision_, revision_ + 1, true, command->replayed);
    if (prepublish) {
        const auto guardedRevision = revision_;
        const auto guardedCursor = cursor_;
        const auto guardedHistorySize = history_.size();
        const auto guardedSessionIdentity = sessionIdentity_;
        const auto guardedNextIdentity = nextCommandIdentity_;
        const auto guardedTerrainSource = terrainSourceRevision_;
        const auto accepted = prepublish(prepared);
        if (!accepted.Succeeded()) return accepted;
        // 回调可使用公开 API 重入；保留其成功结果，拒绝覆盖已经过期的外层候选。
        if (revision_ != guardedRevision || cursor_ != guardedCursor || history_.size() != guardedHistorySize
            || sessionIdentity_ != guardedSessionIdentity || nextCommandIdentity_ != guardedNextIdentity
            || terrainSourceRevision_ != guardedTerrainSource)
            return Status::Failure("MapData.Edit", "MAP_EDIT_PREPUBLISH_STALE", "发布前回调已改变地图会话，请重新准备候选。");
    }
    // 此后仅发布已分配的命令与覆盖；拒绝候选不消耗身份，也不擦除 redo 历史。
    if (newIdentity) ++nextCommandIdentity_;
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    history_.push_back(command);
    for (std::size_t i = 0; i < command->changes.size(); ++i)
        current_[Index(command->changes[i].kind)].swap(next[i]);
    terrain_ = std::move(nextTerrain);
    if (command->specialAfter) specialDoodads_ = command->specialAfter;
    if (command->pathingAfter) pathing_.swap(nextPathing);
    if (nextRestrictions) pathingRestrictions_.swap(nextRestrictions);
    cursor_ = history_.size();
    const auto beforeRevision = revision_;
    ++revision_;
    lastApplied_ = EditReceipt(command, this, beforeRevision, revision_, true, command->replayed);
    return Status::Ok();
}

Status MapDataEditSession::EditSettings(const MapSettings& requested) {
    auto value = requested;
    std::vector<MemberChange> changes;
    std::vector<Diagnostic> stringDiagnostics;
    std::optional<MapSettings> before;
    if (const auto& bytes = current_[Index(MapDataKind::Settings)]) {
        const auto parsed = MapSettings::Parse(*bytes);
        if (!parsed.Succeeded() || !parsed.value)
            return StartFailure("MAP_START_SOURCE_UNSAFE", "当前玩家设置无法完整解析，不能推导起始位置联动");
        before = *parsed.value;
    }
    // 来源中的未知控制者/种族原样保留；新增玩家或改动过的值必须是已知取值。
    for (const auto& player : value.players) {
        const auto source = before ? std::find_if(before->players.begin(), before->players.end(),
            [&](const auto& candidate) { return candidate.number == player.number; }) : value.players.end();
        const bool known = before && source != before->players.end();
        if ((!known || source->controller != player.controller) && !KnownPlayerController(player.controller))
            return StartFailure("MAP_PLAYER_CONTROLLER_INVALID",
                ("玩家 " + std::to_string(player.number) + " 的控制者只能是 1 用户、2 电脑、3 中立或 4 可营救").c_str());
        if ((!known || source->race != player.race) && !KnownPlayerRace(player.race))
            return StartFailure("MAP_PLAYER_RACE_INVALID",
                ("玩家 " + std::to_string(player.number) + " 的种族只能是 0 可选择、1 人类、2 兽人、3 亡灵或 4 暗夜精灵").c_str());
    }
    if (before) {
        constexpr std::array<std::pair<std::string_view, std::string MapSettings::*>, 3> textFields{{
            {"author", &MapSettings::author}, {"description", &MapSettings::description},
            {"recommended_players", &MapSettings::recommendedPlayers}}};
        std::vector<MapFieldEdit> references;
        const auto snapshot = [&]() -> std::optional<MapDocument> {
            for (const auto& [id, member] : textFields)
                if ((before.value().*member).starts_with("TRIGSTR_") && before.value().*member != value.*member)
                    return Snapshot();
            return std::nullopt;
        }();
        if (snapshot) for (const auto& [id, member] : textFields) {
            const auto& raw = before.value().*member;
            auto& text = value.*member;
            if (!raw.starts_with("TRIGSTR_") || raw == text) continue;
            const auto resolved = snapshot->ResolveString(raw);
            if (resolved.Succeeded() && resolved.value && *resolved.value == text) {
                text = raw;
                continue;
            }
            references.push_back({MapDataKind::Settings, 0, "w3i", std::string(id),
                {MapFieldType::Bytes, 0, 0, raw}, {MapFieldType::Bytes, 0, 0, text}});
        }
        if (!references.empty()) {
            const auto tables = ApplyMapFieldEdits(*snapshot, references);
            if (!tables.Succeeded() || !tables.value) return {tables.diagnostics};
            for (const auto& [id, member] : textFields)
                if ((before.value().*member).starts_with("TRIGSTR_")) value.*member = tables.value->settings.value().*member;
            const auto preparedStrings = Prepare(MapDataKind::Strings, tables.value->strings->Serialize(), changes);
            if (!preparedStrings.Succeeded()) return preparedStrings;
            stringDiagnostics = tables.diagnostics;
        }
    }
    auto status = Prepare(MapDataKind::Settings, value.Serialize(), changes);
    if (!status.Succeeded() || changes.empty()) return status;
    Status success{std::move(stringDiagnostics)};
    const auto commit = [&] {
        auto committed = Commit(std::move(changes));
        return committed.Succeeded() ? std::move(success) : std::move(committed);
    };
    std::unordered_set<std::int32_t> changedPlayers;
    std::unordered_set<std::int32_t> removedPlayers;
    if (before) for (const auto& player : before->players) {
        const auto next = std::find_if(value.players.begin(), value.players.end(),
            [&](const auto& candidate) { return candidate.number == player.number; });
        if (next == value.players.end()) removedPlayers.insert(player.number);
        else if (!SameCoordinate(player.startX, next->startX) || !SameCoordinate(player.startY, next->startY))
            changedPlayers.insert(player.number);
    }
    for (const auto& player : value.players) {
        if (!before || std::none_of(before->players.begin(), before->players.end(),
            [&](const auto& candidate) { return candidate.number == player.number; }))
            changedPlayers.insert(player.number);
    }
    if (changedPlayers.empty() && removedPlayers.empty()) return commit();
    const auto& unitBytes = current_[Index(MapDataKind::Units)];
    if (!unitBytes) {
        if (states_[Index(MapDataKind::Units)] == MapDataState::Missing) return commit();
        return StartFailure("MAP_START_SOURCE_UNSAFE", "单位放置来源不明确，不能确认玩家起始位置的联动对象");
    }
    auto units = ParseUnitPlacements(*unitBytes);
    if (!units.Succeeded() || !units.value)
        return StartFailure("MAP_START_SOURCE_UNSAFE", "单位放置无法解析，不能确认玩家起始位置的联动对象");
    std::unordered_map<std::int32_t, std::size_t> startCounts;
    for (const auto& unit : units.value->units)
        if (unit.rawcode == "sloc") ++startCounts[unit.owner];
    bool linked = false;
    for (auto& unit : units.value->units) {
        if (unit.rawcode != "sloc") continue;
        if (removedPlayers.contains(unit.owner))
            return StartFailure("MAP_START_OWNER_CHANGE_UNSUPPORTED", "玩家仍拥有起始位置，不能直接删除或更改该玩家编号");
        if (!changedPlayers.contains(unit.owner)) continue;
        if (startCounts[unit.owner] != 1)
            return StartFailure("MAP_START_AMBIGUOUS", "同一玩家存在多个起始位置，不能确定应同步的实例");
        const auto player = std::find_if(value.players.begin(), value.players.end(),
            [&](const auto& candidate) { return candidate.number == unit.owner; });
        unit.position[0] = player->startX;
        unit.position[1] = player->startY;
        linked = true;
    }
    if (linked) {
        status = Prepare(MapDataKind::Units, units.value->Serialize(), changes);
        if (!status.Succeeded()) return status;
    }
    return commit();
}
Status MapDataEditSession::EditUnits(const UnitPlacements& value) {
    std::vector<MemberChange> changes;
    const auto status = PrepareUnits(value, changes);
    return status.Succeeded() ? Commit(std::move(changes)) : status;
}

Status MapDataEditSession::EditPlacements(const UnitPlacements* units, const DoodadPlacements* doodads) {
    std::vector<MemberChange> changes;
    if (doodads) {
        if (const auto status = Prepare(MapDataKind::Doodads, doodads->Serialize(), changes); !status.Succeeded()) return status;
    }
    if (units) {
        if (const auto status = PrepareUnits(*units, changes); !status.Succeeded()) return status;
    }
    return Commit(std::move(changes));
}

Status MapDataEditSession::EditPlacementRows(const MapDocument& source, const UnitPlacements* units, const DoodadPlacements* doodads,
    std::vector<PlacementRowLineage> lineage, const std::function<Status(const EditReceipt&)>& prepublish) {
    std::vector<MemberChange> changes;
    if (doodads) if (const auto status = Prepare(MapDataKind::Doodads, doodads->Serialize(), changes); !status.Succeeded()) return status;
    if (units) if (const auto status = PrepareUnits(*units, changes); !status.Succeeded()) return status;
    bool seenUnits = false, seenDoodads = false;
    const auto invalid = [] { return Status::Failure("MapData.Edit", "MAP_PLACEMENT_LINEAGE_INVALID", "摆放操作未提供完整可信的行映射。"); };
    for (const auto& map : lineage) {
        const bool unit = map.kind == MapDataKind::Units;
        if ((!unit && map.kind != MapDataKind::Doodads) || (unit ? seenUnits : seenDoodads)) return invalid();
        (unit ? seenUnits : seenDoodads) = true;
        const auto& bytes = current_[Index(map.kind)]; const auto& member = source.Member(map.kind);
        if (!bytes || member.sources.size() != 1 || member.sources.front().data != *bytes) return invalid();
        if (unit ? (!units || !source.Units()) : (!doodads || !source.Doodads())) return invalid();
        const auto beforeCount = unit ? source.Units()->units.size() : source.Doodads()->doodads.size();
        const auto afterCount = unit ? units->units.size() : doodads->doodads.size();
        if (map.beforeCount != beforeCount || map.afterCount != afterCount || map.beforeToAfter.size() != beforeCount) return invalid();
        std::vector<bool> targets(afterCount, false);
        for (std::size_t row = 0; row < beforeCount; ++row) if (const auto target = map.beforeToAfter[row]) {
            if (*target >= afterCount || targets[*target]) return invalid();
            targets[*target] = true;
            const auto& before = unit ? static_cast<const PlacementTransform&>(source.Units()->units[row]) : static_cast<const PlacementTransform&>(source.Doodads()->doodads[row]);
            const auto& after = unit ? static_cast<const PlacementTransform&>(units->units[*target]) : static_cast<const PlacementTransform&>(doodads->doodads[*target]);
            // 验证显式谱系，绝不根据相同字段反向猜出谱系。
            const auto beforeCreation = unit ? source.Units()->units[row].creationId : source.Doodads()->doodads[row].creationId;
            const auto afterCreation = unit ? units->units[*target].creationId : doodads->doodads[*target].creationId;
            if (before.rawcode != after.rawcode || beforeCreation != afterCreation) return invalid();
        }
    }
    if (seenUnits != (units != nullptr) || seenDoodads != (doodads != nullptr)) return invalid();
    Command command; command.changes = std::move(changes);
    command.placements = std::make_shared<const std::vector<PlacementRowLineage>>(std::move(lineage));
    return CommitCommand(std::move(command), prepublish);
}

Status MapDataEditSession::PrepareUnits(const UnitPlacements& value, std::vector<MemberChange>& changes) const {
    const auto prepared = changes.size();
    auto status = Prepare(MapDataKind::Units, value.Serialize(), changes);
    if (!status.Succeeded() || changes.size() == prepared) return status;
    std::optional<UnitPlacements> before;
    if (const auto& bytes = current_[Index(MapDataKind::Units)]) {
        const auto parsed = ParseUnitPlacements(*bytes);
        if (!parsed.Succeeded() || !parsed.value)
            return StartFailure("MAP_START_SOURCE_UNSAFE", "当前单位放置无法完整解析，不能推导起始位置联动");
        before = *parsed.value;
    }
    std::unordered_map<std::int32_t, const UnitPlacement*> byCreationId;
    std::unordered_map<std::int32_t, std::size_t> startCounts;
    for (const auto& unit : value.units) {
        byCreationId.emplace(unit.creationId, &unit);
        if (unit.rawcode == "sloc") ++startCounts[unit.owner];
    }
    std::unordered_set<std::int32_t> existingStarts;
    std::vector<const UnitPlacement*> movedStarts;
    if (before) for (const auto& unit : before->units) {
        if (unit.rawcode != "sloc") continue;
        existingStarts.insert(unit.creationId);
        const auto next = byCreationId.find(unit.creationId);
        if (next == byCreationId.end() || next->second->rawcode != "sloc")
            return StartFailure("MAP_START_REMOVE_UNSUPPORTED", "不能直接删除起始位置或将其改为其他类型");
        if (next->second->owner != unit.owner)
            return StartFailure("MAP_START_OWNER_CHANGE_UNSUPPORTED", "起始位置绑定玩家设置，暂不支持直接更改所属玩家");
        if (!SameCoordinate(unit.position[0], next->second->position[0])
            || !SameCoordinate(unit.position[1], next->second->position[1]))
            movedStarts.push_back(next->second);
    }
    for (const auto& unit : value.units)
        if (unit.rawcode == "sloc" && !existingStarts.contains(unit.creationId)) movedStarts.push_back(&unit);
    if (movedStarts.empty()) return Status::Ok();
    const auto& settingsBytes = current_[Index(MapDataKind::Settings)];
    if (!settingsBytes)
        return StartFailure("MAP_START_SETTINGS_UNAVAILABLE", "地图玩家设置缺失或来源不明确，不能同步起始位置");
    auto settings = MapSettings::Parse(*settingsBytes);
    if (!settings.Succeeded() || !settings.value)
        return StartFailure("MAP_START_SETTINGS_UNAVAILABLE", "地图玩家设置无法解析，不能同步起始位置");
    for (const auto* unit : movedStarts) {
        if (startCounts[unit->owner] != 1)
            return StartFailure("MAP_START_AMBIGUOUS", "同一玩家存在多个起始位置，不能确定应同步的实例");
        if (std::count_if(settings.value->players.begin(), settings.value->players.end(),
            [&](const auto& player) { return player.number == unit->owner; }) != 1)
            return StartFailure("MAP_START_PLAYER_UNAVAILABLE", "起始位置没有唯一对应的地图玩家，不能写入玩家坐标");
        auto player = std::find_if(settings.value->players.begin(), settings.value->players.end(),
            [&](const auto& candidate) { return candidate.number == unit->owner; });
        player->startX = unit->position[0];
        player->startY = unit->position[1];
    }
    return Prepare(MapDataKind::Settings, settings.value->Serialize(), changes);
}
Status MapDataEditSession::EditDoodads(const DoodadPlacements& value) {
    return Apply(MapDataKind::Doodads, value.Serialize());
}
Status MapDataEditSession::EditRegions(const RegionsDocument& value) {
    return Apply(MapDataKind::Regions, value.Serialize());
}
Status MapDataEditSession::EditCameras(const CamerasDocument& value) {
    return Apply(MapDataKind::Cameras, value.Serialize());
}
Status MapDataEditSession::EditSounds(const SoundsDocument& value) {
    return Apply(MapDataKind::Sounds, value.Serialize());
}
Status MapDataEditSession::EditStrings(const StringTableDocument& value) {
    return Apply(MapDataKind::Strings, value.Serialize());
}

Status MapDataEditSession::SetSerialized(MapDataKind kind, std::span<const std::byte> bytes) {
    return DeserializeAndApply(kind, bytes, false);
}

Status MapDataEditSession::EditSerialized(MapDataKind kind, std::span<const std::byte> bytes) {
    return DeserializeAndApply(kind, bytes, true);
}

Status MapDataEditSession::DeserializeAndApply(MapDataKind kind, std::span<const std::byte> bytes,
    bool linkStartLocations) {
    const auto forward = [](const auto& parsed) -> Status {
        Status status;
        status.diagnostics = parsed.diagnostics;
        if (status.diagnostics.empty())
            return Status::Failure("MapData.Edit", "MAP_SOURCE_INVALID", "工程地图覆盖无法解析");
        return status;
    };
    switch (kind) {
    case MapDataKind::Settings: {
        const auto parsed = MapSettings::Parse(bytes);
        return parsed.Succeeded() && parsed.value
            ? (linkStartLocations ? EditSettings(*parsed.value) : Apply(kind, parsed.value->Serialize()))
            : forward(parsed);
    }
    case MapDataKind::Units: {
        const auto parsed = ParseUnitPlacements(bytes);
        return parsed.Succeeded() && parsed.value
            ? (linkStartLocations ? EditUnits(*parsed.value) : Apply(kind, parsed.value->Serialize()))
            : forward(parsed);
    }
    case MapDataKind::Doodads: {
        const auto parsed = ParseDoodadPlacements(bytes);
        return parsed.Succeeded() && parsed.value ? EditDoodads(*parsed.value) : forward(parsed);
    }
    case MapDataKind::Regions: {
        const auto parsed = ParseRegions(bytes);
        return parsed.Succeeded() && parsed.value ? EditRegions(*parsed.value) : forward(parsed);
    }
    case MapDataKind::Cameras: {
        const auto parsed = ParseCameras(bytes);
        return parsed.Succeeded() && parsed.value ? EditCameras(*parsed.value) : forward(parsed);
    }
    case MapDataKind::Sounds: {
        const auto parsed = ParseSounds(bytes);
        return parsed.Succeeded() && parsed.value ? EditSounds(*parsed.value) : forward(parsed);
    }
    case MapDataKind::Strings: {
        const auto parsed = ParseStringTable(bytes);
        return parsed.Succeeded() && parsed.value ? EditStrings(*parsed.value) : forward(parsed);
    }
    default:
        return Status::Failure("MapData.Edit", "MAP_KIND_INVALID", "未知的公共地图成员类别");
    }
}

Status MapDataEditSession::Remove(MapDataKind kind) {
    const auto index = Index(kind);
    if (index >= states_.size())
        return Status::Failure("MapData.Edit", "MAP_KIND_INVALID", "未知的公共地图成员类别");
    if (kind == MapDataKind::Settings || kind == MapDataKind::Units || kind == MapDataKind::Doodads)
        return Status::Failure("MapData.Edit", "MAP_REQUIRED_MEMBER", "必需地图成员不能直接删除");
    if ((states_[index] != MapDataState::Ready && states_[index] != MapDataState::Missing)
        || !current_[index])
        return Status::Failure("MapData.Edit", "MAP_SOURCE_UNSAFE", "来源不明确或成员已缺席，不能删除");
    return Commit({{kind, current_[index], std::nullopt}});
}

MapDataEditSession::EditReceipt MapDataEditSession::LastEdit() const {
    if (!cursor_) return {};
    const auto& command = history_[cursor_ - 1];
    return EditReceipt(command, this, command->beforeRevision, command->afterRevision, true, command->replayed);
}

std::uint64_t MapDataEditSession::EditReceipt::SessionIdentity() const noexcept { return command_ ? command_->sessionIdentity : 0; }
std::uint64_t MapDataEditSession::EditReceipt::CommandIdentity() const noexcept { return command_ ? command_->logicalIdentity : 0; }
bool MapDataEditSession::EditReceipt::Forward() const noexcept { return command_ && command_->logicalForward == appliedForward_; }
bool MapDataEditSession::EditReceipt::ChangesPlacements() const noexcept {
    return command_ && std::any_of(command_->changes.begin(), command_->changes.end(), [](const auto& change) {
        return change.kind == MapDataKind::Units || change.kind == MapDataKind::Doodads;
    });
}
bool MapDataEditSession::EditReceipt::HasPlacementLineage() const noexcept {
    if (!command_ || !command_->placements) return false;
    return std::all_of(command_->changes.begin(), command_->changes.end(), [this](const auto& change) {
        return (change.kind != MapDataKind::Units && change.kind != MapDataKind::Doodads) ||
            std::any_of(command_->placements->begin(), command_->placements->end(), [&](const auto& map) { return map.kind == change.kind; });
    });
}
std::span<const PlacementRowLineage> MapDataEditSession::EditReceipt::PlacementLineage() const noexcept {
    return command_ && command_->placements ? std::span<const PlacementRowLineage>(*command_->placements) : std::span<const PlacementRowLineage>{};
}
std::optional<std::span<const std::byte>> MapDataEditSession::EditReceipt::PlacementMemberBytes(MapDataKind kind, bool after) const noexcept {
    if (!command_ || (kind != MapDataKind::Units && kind != MapDataKind::Doodads)) return std::nullopt;
    for (const auto& change : command_->changes) if (change.kind == kind) {
        const auto& bytes = after == appliedForward_ ? change.after : change.before;
        if (bytes) return std::span<const std::byte>(*bytes);
    }
    return std::nullopt;
}

Status MapDataEditSession::Replay(const EditReceipt& receipt, bool forward) {
    if (receipt.session_ != this || !receipt.command_ || receipt.SessionIdentity() != sessionIdentity_)
        return Status::Failure("MapData.Edit", "MAP_EDIT_RECEIPT_INVALID", "编辑回执不属于当前地图会话");
    const auto& source = *receipt.command_;
    Command command;
    command.sessionIdentity = source.sessionIdentity; command.logicalIdentity = source.logicalIdentity;
    command.replayed = true;
    command.terrainStroke = source.terrainStroke;
    command.specialBefore = forward ? source.specialBefore : source.specialAfter;
    command.specialAfter = forward ? source.specialAfter : source.specialBefore;
    command.logicalForward = source.logicalForward == forward; command.placements = source.placements;
    for (const auto& change : source.changes) {
        const auto& expected = forward ? change.before : change.after;
        const auto& target = forward ? change.after : change.before;
        if (current_[Index(change.kind)] != expected)
            return Status::Failure("MapData.Edit", "MAP_EDIT_STALE",
                "事务涉及的地图成员已由其他操作修改；未覆盖新内容");
        command.changes.push_back({change.kind, expected, target});
    }
    if (!TerrainMatches(source, forward))
        return Status::Failure("MapData.Edit", "MAP_EDIT_STALE", "事务涉及的地形已由其他操作修改；未覆盖新内容");
    for (const auto& chunk : source.terrain)
        command.terrain.push_back({chunk.x0, chunk.y0, chunk.x1, chunk.y1, forward ? chunk.before : chunk.after,
            forward ? chunk.after : chunk.before});
    if (source.headerAfter) {
        command.headerBefore = forward ? source.headerBefore : source.headerAfter;
        command.headerAfter = forward ? source.headerAfter : source.headerBefore;
    }
    if (source.pathingAfter) {
        const auto& expected = forward ? source.pathingBefore : source.pathingAfter;
        const auto& target = forward ? source.pathingAfter : source.pathingBefore;
        const auto& expectedIntent = source.restrictionAfter
            ? (forward ? source.restrictionBefore : source.restrictionAfter) : pathingRestrictions_;
        const auto& targetIntent = source.restrictionAfter
            ? (forward ? source.restrictionAfter : source.restrictionBefore) : pathingRestrictions_;
        if (!pathing_ || !HistoryPathingMatches(*pathing_, *expected, *pathingProtected_, *pathingOriginal_, *expectedIntent))
            return Status::Failure("MapData.Edit", "MAP_EDIT_STALE", "事务涉及的通行性已由其他操作修改；未覆盖新内容");
        command.pathingBefore = *pathing_;
        command.pathingAfter = HistoryPathing(*target, *pathingProtected_, *pathingOriginal_, *targetIntent);
    }
    if (source.restrictionAfter) {
        const auto& expected = forward ? source.restrictionBefore : source.restrictionAfter;
        if (pathingRestrictions_ != expected)
            return Status::Failure("MapData.Edit", "MAP_EDIT_STALE", "事务涉及的限制意图已由其他操作修改；未覆盖新内容");
        command.restrictionBefore = expected;
        command.restrictionAfter = forward ? source.restrictionAfter : source.restrictionBefore;
    }
    return CommitCommand(std::move(command));
}

void MapDataEditSession::Restore(const Command& command, bool forward) {
    std::vector<std::optional<std::vector<std::byte>>> next;
    next.reserve(command.changes.size());
    for (const auto& change : command.changes) next.push_back(forward ? change.after : change.before);
    std::shared_ptr<const TerrainGridSnapshot> nextTerrain = terrain_;
    if (terrain_ && (!command.terrain.empty() || command.headerAfter)) {
        auto copy = std::make_shared<TerrainGridSnapshot>(*terrain_);
        if (command.headerAfter) copy->header = forward ? *command.headerAfter : *command.headerBefore;
        for (const auto& chunk : command.terrain)
            detail::WriteChunk(*copy, chunk.x0, chunk.y0, chunk.x1, chunk.y1, forward ? chunk.after : chunk.before);
        RefreshTerrainDerived(*copy, 0, 0, copy->header.width, copy->header.height);
        nextTerrain = std::move(copy);
    }
    std::optional<std::vector<std::byte>> nextPathing;
    auto nextRestrictions = forward ? command.restrictionAfter : command.restrictionBefore;
    if (command.pathingAfter) nextPathing = HistoryPathing(
        *(forward ? command.pathingAfter : command.pathingBefore), *pathingProtected_, *pathingOriginal_,
        *(nextRestrictions ? nextRestrictions : pathingRestrictions_));
    for (std::size_t i = 0; i < command.changes.size(); ++i)
        current_[Index(command.changes[i].kind)].swap(next[i]);
    terrain_ = std::move(nextTerrain);
    if (command.specialAfter) specialDoodads_ = forward ? command.specialAfter : command.specialBefore;
    if (command.pathingAfter) pathing_.swap(nextPathing);
    if (nextRestrictions) pathingRestrictions_.swap(nextRestrictions);
}

bool MapDataEditSession::Undo() {
    if (!CanUndo() || revision_ == std::numeric_limits<std::uint64_t>::max()) return false;
    const auto command = history_[cursor_ - 1];
    Restore(*history_[cursor_ - 1], false);
    --cursor_;
    const auto beforeRevision = revision_++;
    lastApplied_ = EditReceipt(command, this, beforeRevision, revision_, false, true);
    return true;
}

bool MapDataEditSession::Redo() {
    if (!CanRedo() || revision_ == std::numeric_limits<std::uint64_t>::max()) return false;
    const auto command = history_[cursor_];
    Restore(*history_[cursor_], true);
    ++cursor_;
    const auto beforeRevision = revision_++;
    lastApplied_ = EditReceipt(command, this, beforeRevision, revision_, true, true);
    return true;
}

bool MapDataEditSession::IsDirty() const noexcept {
    return current_ != original_ || TerrainDirty() || pathing_ != pathingOriginal_
        || (pathingRestrictions_ != originalPathingRestrictions_ && pathingRestrictions_ && originalPathingRestrictions_
            && *pathingRestrictions_ != *originalPathingRestrictions_);
}

std::vector<MapArchiveMember> MapDataEditSession::Replacements() const {
    std::vector<MapArchiveMember> members;
    for (std::size_t i = 0; i < current_.size(); ++i)
        if (current_[i] && current_[i] != original_[i])
            members.push_back({memberNames_[i], *current_[i]});
    if (TerrainDirty()) {
        auto bytes = SerializeMapTerrain(*terrain_);
        if (bytes.Succeeded() && bytes.value) members.push_back({std::string(kTerrainMember), std::move(*bytes.value)});
    }
    if (pathing_ && pathing_ != pathingOriginal_) members.push_back({std::string(kPathingMember), *pathing_});
    return members;
}

std::vector<std::string> MapDataEditSession::Removals() const {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < current_.size(); ++i)
        if (!current_[i] && original_[i]) names.push_back(memberNames_[i]);
    return names;
}

MapDataEditSession::MapDataEditSession(const MapDataEditSession& source, SaveSnapshotTag)
    : states_(source.states_), memberNames_(source.memberNames_), original_(source.original_), current_(source.current_),
      preservedSources_(source.preservedSources_), revision_(source.revision_), sessionIdentity_(source.sessionIdentity_),
      nextCommandIdentity_(source.nextCommandIdentity_), terrainSourceRevision_(source.terrainSourceRevision_),
      specialDoodads_(source.specialDoodads_), savedSpecialDoodads_(source.savedSpecialDoodads_),
      terrainOriginal_(source.terrainOriginal_), terrain_(source.terrain_), pathingOriginal_(source.pathingOriginal_),
      pathing_(source.pathing_), pathingProtected_(source.pathingProtected_), pathingRestrictions_(source.pathingRestrictions_),
      originalPathingRestrictions_(source.originalPathingRestrictions_) {}

std::shared_ptr<const MapDataEditSession> MapDataEditSession::FreezeForSave() const {
    // 不复制Undo历史/回执地址；快照只读且与前台共享不可变地形和私有special状态。
    return std::shared_ptr<const MapDataEditSession>(new MapDataEditSession(*this, SaveSnapshotTag{}));
}

Result<std::shared_ptr<const MapDataEditSession>> MapDataEditSession::FreezeForSave(std::size_t maximumBytes) const {
    using Output = Result<std::shared_ptr<const MapDataEditSession>>;
    const auto fail = [] { return Output::Failure("MapData.Edit", "MAP_SAVE_SNAPSHOT_BUDGET", "冻结会话的原生成员、WPM或诊断复制超过显式预算。"); };
    std::size_t bytes = 0;
    const auto add = [&](std::size_t count, std::size_t element = 1) {
        if (element && count > (maximumBytes - bytes) / element) return false;
        bytes += count * element; return true;
    };
    const auto text = [&](const auto& value) { return add(value.capacity() + 1, sizeof(typename std::decay_t<decltype(value)>::value_type)); };
    if (!add(1, sizeof(MapDataEditSession) + 64)) return fail();
    for (std::size_t i = 0; i < original_.size(); ++i) {
        if (!text(memberNames_[i]) || (original_[i] && !add(original_[i]->capacity()))
            || (current_[i] && !add(current_[i]->capacity()))) return fail();
        if (!preservedSources_[i]) continue;
        const auto& state = *preservedSources_[i];
        if (!text(state.memberName) || !add(state.sources.capacity(), sizeof(MapArchiveMember))
            || !add(state.diagnostics.capacity(), sizeof(Diagnostic))) return fail();
        for (const auto& member : state.sources) if (!text(member.name) || !add(member.data.capacity())) return fail();
        for (const auto& diagnostic : state.diagnostics) {
            if (!text(diagnostic.subsystem) || !text(diagnostic.code) || !text(diagnostic.message)) return fail();
            if (diagnostic.origin && (!text(diagnostic.origin->fingerprint) || !text(diagnostic.origin->source.native()))) return fail();
        }
    }
    if ((pathingOriginal_ && !add(pathingOriginal_->capacity())) || (pathing_ && !add(pathing_->capacity()))) return fail();
    try { return Output::Success(FreezeForSave()); }
    catch (const std::bad_alloc&) { return fail(); }
    catch (const std::length_error&) { return fail(); }
}

void MapDataEditSession::CommitSaved(const MapDocument& saved) {
    CommitSavedBaseline(saved, pathing_, pathingProtected_);
}

void MapDataEditSession::CommitSavedBaseline(const MapDocument& saved,
    std::optional<std::vector<std::byte>> pathing,
    std::shared_ptr<const std::vector<std::byte>> protectedPathing) {
    // 所有复制在交换前完成，分配失败时连来源代际也保持不变。
    auto original = current_;
    const auto savedBoundary = cursor_ ? history_[cursor_ - 1] : std::shared_ptr<const Command>{};
    auto originalPathing = pathing;
    auto savedSpecial = specialDoodads_;
    if (const auto& placed = saved.Doodads(); placed) {
        const auto& actual = placed->specialDoodads;
        const bool same = savedSpecial && actual.size() == savedSpecial->size()
            && std::equal(actual.begin(), actual.end(), savedSpecial->begin(), [](const auto& a, const auto& b) {
                return a.rawcode == b.rawcode && a.variation == b.variation && a.position == b.position;
            });
        // 独立的合法物编保存可建立新来源；相同可信集合保持原指针，保存后Undo仍识别原命令。
        if (!same) savedSpecial = std::make_shared<const std::vector<SpecialDoodadPlacement>>(actual);
    } else if (saved.Member(MapDataKind::Doodads).state == MapDataState::Missing && savedSpecial && !savedSpecial->empty()) {
        savedSpecial = std::make_shared<const std::vector<SpecialDoodadPlacement>>();
    }
    std::array<MapDataState, 7> states{};
    std::array<std::optional<MapMemberState>, 7> preservedSources{};
    for (std::size_t i = 0; i < states_.size(); ++i) {
        const auto& member = saved.Member(static_cast<MapDataKind>(i));
        states[i] = member.state;
        if (!original[i] && member.state != MapDataState::Missing) preservedSources[i] = member;
    }
    static_assert(noexcept(original_.swap(original)));
    static_assert(noexcept(preservedSources_.swap(preservedSources)));
    original_.swap(original);
    terrainJournalSavedBoundary_ = savedBoundary;
    terrainJournalSavedCursor_ = cursor_;
    pathingOriginal_.swap(originalPathing);
    pathing_.swap(pathing);
    pathingProtected_.swap(protectedPathing);
    states_.swap(states);
    preservedSources_.swap(preservedSources);
    terrainOriginal_ = terrain_;
    specialDoodads_ = savedSpecialDoodads_ = std::move(savedSpecial);
    // 不从P|A反推来源P，已保存意图S单独保留。
    originalPathingRestrictions_ = pathingRestrictions_;
    if (terrainSourceRevision_ != std::numeric_limits<std::uint64_t>::max()) ++terrainSourceRevision_;
}

Result<TerrainStrokeJournal> MapDataEditSession::ExportTerrainStrokeJournal() const {
    return ExportTerrainStrokeJournalInternal(true);
}

Result<TerrainStrokeJournal> MapDataEditSession::ExportTerrainStrokeJournalInternal(bool baselineLayers) const {
    using Output = Result<TerrainStrokeJournal>;
    const auto fail = [](const char* code) {
        return Output::Failure("Terrain.Journal", code, "草稿已保留；当前历史不能表达为保存基线后的完整前向笔划，水刷来源未恢复。");
    };
    // history只会追加或截断尾部；保存边界对象仍在同一索引，即证明此前不可变前缀未被替换。
    if (cursor_ < terrainJournalSavedCursor_
        || (terrainJournalSavedCursor_ && history_[terrainJournalSavedCursor_ - 1] != terrainJournalSavedBoundary_))
        return fail("TERRAIN_JOURNAL_SAVED_BOUNDARY");
    // 工程CAS是频繁冷读；原始Undo历史不裁剪，导出只扫描有界尾部。
    if (cursor_ - terrainJournalSavedCursor_ > 4096) return fail("TERRAIN_JOURNAL_BUDGET");
    std::vector<const Command*> active;
    std::unordered_set<std::uint64_t> activeIdentities;
    for (auto i = terrainJournalSavedCursor_; i < cursor_; ++i) {
        const auto& command = *history_[i];
        if (!command.logicalForward) {
            if (active.empty() || active.back()->logicalIdentity != command.logicalIdentity)
                return fail("TERRAIN_JOURNAL_SAVED_BOUNDARY");
            activeIdentities.erase(command.logicalIdentity);
            active.pop_back();
        } else {
            if (!activeIdentities.insert(command.logicalIdentity).second)
                return fail("TERRAIN_JOURNAL_HISTORY_UNEXPRESSIBLE");
            active.push_back(&command);
        }
    }
    if (active.size() > 128) return fail("TERRAIN_JOURNAL_BUDGET");
    TerrainStrokeJournal journal;
    std::size_t points = 0;
    for (const auto* command : active) {
        if (!command->terrainStroke) return fail("TERRAIN_JOURNAL_HISTORY_UNEXPRESSIBLE");
        const auto count = command->terrainStroke->intent.points.size();
        if (!count || count > 4096 - points) return fail("TERRAIN_JOURNAL_BUDGET");
        points += count; journal.strokes.push_back(*command->terrainStroke);
    }
    if (baselineLayers && !journal.strokes.empty()) {
        if (!terrainOriginal_ || !pathingOriginal_ || !pathingProtected_ || !originalPathingRestrictions_)
            return fail("TERRAIN_JOURNAL_BASELINE_MISSING");
        auto layers = EncodePathingRestrictionDraft(*terrainOriginal_, *pathingOriginal_, *pathingOriginal_,
            *pathingProtected_, *originalPathingRestrictions_, *originalPathingRestrictions_);
        if (!layers.Succeeded() || !layers.value) return {{}, layers.diagnostics};
        journal.baselinePathingLayers = std::move(*layers.value);
    }
    return Output::Success(std::move(journal));
}

Status MapDataEditSession::VerifyTerrainJournalBaseline(std::span<const ArchiveExactMember> actual) const {
    const auto fail = [] { return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BASELINE_MISMATCH", "地形记录基线与实际来源的完整成员或缺失项不一致。"); };
    if (actual.size() != TerrainJournalMemberNames.size() || !terrainOriginal_ || !pathingOriginal_) return fail();
    const auto terrain = SerializeMapTerrain(*terrainOriginal_);
    if (!terrain.Succeeded() || !terrain.value) return {terrain.diagnostics};
    const auto canonical = [](std::string name) {
        std::replace(name.begin(), name.end(), '\\', '/');
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name.starts_with("map/")) name.erase(0, 4);
        return name;
    };
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (canonical(actual[i].name) != TerrainJournalMemberNames[i]) return fail();
        const std::vector<std::byte>* expected = nullptr;
        if (i == 0) expected = &*terrain.value;
        else if (i == 1) expected = &*pathingOriginal_;
        else {
            for (std::size_t kind = 0; kind < original_.size(); ++kind) {
                if (original_[kind] && canonical(memberNames_[kind]) == TerrainJournalMemberNames[i]) {
                    if (expected) return fail(); expected = &*original_[kind];
                }
                if (preservedSources_[kind]) for (const auto& source : preservedSources_[kind]->sources)
                    if (canonical(source.name) == TerrainJournalMemberNames[i]) {
                        if (expected) return fail(); expected = &source.data;
                    }
            }
        }
        if (actual[i].present != (expected != nullptr) || (expected && actual[i].bytes != *expected)) return fail();
    }
    return Status::Ok();
}

std::string MapDataEditSession::TerrainJournalStateFingerprint() const {
    // 原始来源和P/savedS/S已有独立scope；CAS这里只编码有界typed前缀，
    // 不为每次Observe重复构造最大64MiB的APR和journal副本。
    auto journal = ExportTerrainStrokeJournalInternal(false);
    const std::string required = RequiresTerrainStrokeJournal() ? "required:" : "optional:";
    if (!journal.Succeeded() || !journal.value)
        return required + (journal.diagnostics.empty() ? "unavailable" : journal.diagnostics.front().code);
    if (journal.value->strokes.empty()) return required + "empty";
    // 仅工程CAS使用占位身份；实际持久化必须由Services补真实source/member身份。
    journal.value->sourceSha256.assign(64, '0');
    for (std::size_t i = 0; i < TerrainJournalMemberNames.size(); ++i)
        journal.value->members.push_back({std::string(TerrainJournalMemberNames[i]), i < 2, i < 2 ? std::string(64, '0') : std::string{}, 0});
    const auto bytes = EncodeTerrainStrokeJournal(*journal.value);
    if (!bytes.Succeeded() || !bytes.value) return required + "unexpressible";
    const auto hash = Sha256(*bytes.value);
    return hash.Succeeded() && hash.value ? required + *hash.value : std::string{};
}

MapDocument MapDataEditSession::Snapshot() const {
    MapDocument document;
    for (std::size_t i = 0; i < current_.size(); ++i) {
        if (current_[i]) document.members_[i].sources.push_back({memberNames_[i], *current_[i]});
        else if (preservedSources_[i]) document.members_[i] = *preservedSources_[i];
    }
    // 保留原生版本头和 LNI 等只读输入参与同一解析，避免改变其它成员的布局判定。
    document.ParseMembers();
    for (std::size_t i = 0; i < current_.size(); ++i) {
        // 不可编辑来源的状态、原始字节和原诊断仍是最初捕获的事实；不重复追加诊断。
        if (!current_[i] && preservedSources_[i]) document.members_[i] = *preservedSources_[i];
    }
    return document;
}

Result<MapDocument> MapDataEditSession::Snapshot(MapDataKind kind, std::size_t maximumBytes) const {
    const auto index = Index(kind);
    if (index >= current_.size())
        return Result<MapDocument>::Failure("MapData.Read", "MAP_KIND_INVALID", "未知的公共地图成员类别");
    std::array<std::size_t, 2> selected{index, Index(MapDataKind::Settings)};
    const auto count = kind == MapDataKind::Units || kind == MapDataKind::Doodads || kind == MapDataKind::Cameras ? 2u : 1u;
    const auto measured = SnapshotByteSize(kind);
    if (!measured.Succeeded() || !measured.value) return {std::nullopt, std::move(measured.diagnostics)};
    if (*measured.value > maximumBytes)
        return Result<MapDocument>::Failure("MapData.Read", "MAP_SNAPSHOT_BUDGET", "只读成员快照超过复制字节预算");
    MapDocument result;
    for (std::size_t i = 0; i < count; ++i) {
        const auto member = selected[i];
        if (current_[member]) result.members_[member].sources.push_back({memberNames_[member], *current_[member]});
        else if (preservedSources_[member]) result.members_[member] = *preservedSources_[member];
    }
    result.ParseMembers();
    for (std::size_t i = 0; i < count; ++i) {
        const auto member = selected[i];
        if (!current_[member] && preservedSources_[member]) result.members_[member] = *preservedSources_[member];
    }
    return Result<MapDocument>::Success(std::move(result));
}

Result<std::size_t> MapDataEditSession::SnapshotByteSize(MapDataKind kind) const {
    const auto index = Index(kind);
    if (index >= current_.size())
        return Result<std::size_t>::Failure("MapData.Read", "MAP_KIND_INVALID", "未知的公共地图成员类别");
    const std::array<std::size_t, 2> selected{index, Index(MapDataKind::Settings)};
    const auto count = kind == MapDataKind::Units || kind == MapDataKind::Doodads || kind == MapDataKind::Cameras ? 2u : 1u;
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto member = selected[i];
        if (i == 0 && !current_[member] && states_[member] != MapDataState::Missing && !original_[member])
            return Result<std::size_t>::Failure("MapData.Read", "MAP_SOURCE_UNSAFE",
                "无法确定唯一的原生成员来源，不能将不可读或歧义来源当作缺失");
        const auto add = [&](std::size_t amount) -> bool {
            if (amount > (std::numeric_limits<std::size_t>::max)() - bytes) return false;
            bytes += amount; return true;
        };
        if (current_[member]) {
            if (!add(current_[member]->size()))
                return Result<std::size_t>::Failure("MapData.Read", "MAP_SNAPSHOT_SIZE_OVERFLOW", "只读成员快照字节数溢出");
        } else if (preservedSources_[member]) for (const auto& source : preservedSources_[member]->sources) {
            if (!add(source.data.size()))
                return Result<std::size_t>::Failure("MapData.Read", "MAP_SNAPSHOT_SIZE_OVERFLOW", "只读成员快照字节数溢出");
        }
    }
    return Result<std::size_t>::Success(bytes);
}

} // namespace War3Map::MapData
