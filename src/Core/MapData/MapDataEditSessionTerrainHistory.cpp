#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/PathingRestrictionEditing.h"
#include "War3MapCore/Hash.h"
#include "TerrainHistorySourceShadow.h"
#include "TerrainJournalDelta.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace War3Map::MapData {
namespace {
template<class T> Result<T> HistoryFailure(const char* code, const char* message) {
    return Result<T>::Failure("Terrain.History", code, message);
}
// 保存站的命令集合必须恰为线性历史的前savedIndex条。多分区交错撤销后，
// 前缀内部可按分区线性化重排；恢复时仍逐字节核对重放出的物理保存站。
template<class List>
bool SavedPrefixMatches(const List& saved, const List& line, std::size_t savedIndex, std::uint64_t session) {
    if (saved.size() != savedIndex || savedIndex > line.size()) return false;
    std::unordered_set<std::uint64_t> expected;
    for (const auto& command : saved)
        if (!command || command->sessionIdentity != session || !expected.insert(command->logicalIdentity).second) return false;
    for (std::size_t i = 0; i < savedIndex; ++i)
        if (!line[i] || line[i]->sessionIdentity != session || !expected.contains(line[i]->logicalIdentity)) return false;
    return true;
}
std::string HistoryName(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.starts_with("map/")) name.erase(0, 4);
    return name;
}
} // namespace

Result<MapDataEditSession::TerrainHistoryLine> MapDataEditSession::CaptureTerrainHistoryLine(std::size_t maximumBytes) const {
    using Output = Result<TerrainHistoryLine>;
    const auto fail = [](const char* code) { return HistoryFailure<TerrainHistoryLine>(code,
        "草稿已保留；无法验证完整地形撤销历史，未恢复部分撤销栈。"); };
    if (!maximumBytes || maximumBytes > TerrainJournalMaximumBytes || cursor_ > history_.size()
        || history_.size() > 4096) return fail("TERRAIN_HISTORY_BUDGET");
    const auto cost = MeasureTerrainJournalDraft(maximumBytes, 64u * 1024u * 1024u);
    if (!cost.Succeeded() || !cost.value) return fail("TERRAIN_HISTORY_BUDGET");
    try {
        TerrainHistoryLine line;
        // 单一后进先出历史：与只有一个外层撤销栈时完全相同。返回失败码或空。
        const auto captureGlobal = [&]() -> const char* {
            line.commands.reserve(std::min(history_.size(), std::size_t{128}));
            std::vector<std::shared_ptr<const Command>> currentPrefix;
            std::size_t logicalCursor = 0;
            for (std::size_t i = 0; i < history_.size(); ++i) {
                if (i == cursor_) currentPrefix.assign(line.commands.begin(), line.commands.begin() + logicalCursor);
                const auto& command = history_[i];
                if (!command || command->sessionIdentity != sessionIdentity_ || !command->logicalIdentity)
                    return "TERRAIN_HISTORY_BRANCH";
                if (!command->logicalForward) {
                    if (!logicalCursor || line.commands[logicalCursor - 1]->logicalIdentity != command->logicalIdentity)
                        return "TERRAIN_HISTORY_BRANCH";
                    --logicalCursor;
                } else if (logicalCursor < line.commands.size()
                    && line.commands[logicalCursor]->logicalIdentity == command->logicalIdentity) {
                    // Qt的真实Redo追加同一logicalIdentity；保留最初正向命令供typed重放。
                    ++logicalCursor;
                } else {
                    if (std::any_of(line.commands.begin(), line.commands.end(), [&](const auto& old) {
                        return old->logicalIdentity == command->logicalIdentity;
                    })) return "TERRAIN_HISTORY_BRANCH";
                    line.commands.resize(logicalCursor);
                    if (line.commands.size() == 128) return "TERRAIN_HISTORY_BUDGET";
                    line.commands.push_back(command); ++logicalCursor;
                }
            }
            if (cursor_ == history_.size()) currentPrefix.assign(line.commands.begin(), line.commands.begin() + logicalCursor);
            if (currentPrefix.size() > line.commands.size()
                || !std::equal(currentPrefix.begin(), currentPrefix.end(), line.commands.begin(), [](const auto& a, const auto& b) {
                    return a->logicalIdentity == b->logicalIdentity;
                })) return "TERRAIN_HISTORY_BRANCH";
            line.currentIndex = currentPrefix.size();
            return nullptr;
        };
        if (const auto failed = captureGlobal()) {
            // 世界、设置、声音分区交错撤销后不再是单一后进先出；按分区线性化并核对成员字节链。
            if (std::string_view(failed) != "TERRAIN_HISTORY_BRANCH") return fail(failed);
            auto lanes = CaptureLaneHistoryLine();
            if (!lanes) return fail("TERRAIN_HISTORY_BRANCH");
            if (lanes->commands.size() > 128) return fail("TERRAIN_HISTORY_BUDGET");
            line = std::move(*lanes);
        }
        // 每个栈槽必须恰好对应一个真实typed命令；四说明字段可另合并，不能偷占Qt索引。
        auto prefix = ExportTerrainJournalCommands(line.commands, false, true);
        if (!prefix.Succeeded() || !prefix.value || prefix.value->strokes.size() != line.commands.size())
            return fail("TERRAIN_HISTORY_UNEXPRESSIBLE");
        return Output::Success(std::move(line));
    } catch (const std::bad_alloc&) { return fail("TERRAIN_HISTORY_BUDGET"); }
      catch (const std::length_error&) { return fail("TERRAIN_HISTORY_BUDGET"); }
}

std::optional<MapDataEditSession::TerrainHistoryLine> MapDataEditSession::CaptureLaneHistoryLine() const {
    // 核心自身的Undo/Redo游标不与外层分区混用。
    if (cursor_ != history_.size()) return std::nullopt;
    using CommandList = std::vector<std::shared_ptr<const Command>>;
    struct Lane { CommandList commands; std::size_t cursor = 0; };
    std::map<std::uint32_t, Lane> lanes;
    std::unordered_set<std::uint64_t> alive;
    for (const auto& command : history_) {
        if (!command || command->sessionIdentity != sessionIdentity_ || !command->logicalIdentity) return std::nullopt;
        auto& lane = lanes[CommandLane(*command)];
        const auto identity = command->logicalIdentity;
        if (!command->logicalForward) {
            if (!lane.cursor || lane.commands[lane.cursor - 1]->logicalIdentity != identity) return std::nullopt;
            --lane.cursor;
        } else if (lane.cursor < lane.commands.size() && lane.commands[lane.cursor]->logicalIdentity == identity) {
            ++lane.cursor; // 重做保留最初正向命令。
        } else {
            if (alive.contains(identity)) return std::nullopt;
            for (auto i = lane.cursor; i < lane.commands.size(); ++i) alive.erase(lane.commands[i]->logicalIdentity);
            lane.commands.resize(lane.cursor);
            lane.commands.push_back(command); ++lane.cursor; alive.insert(identity);
        }
    }
    CommandList applied, pending;
    for (const auto& [identity, lane] : lanes) {
        applied.insert(applied.end(), lane.commands.begin(), lane.commands.begin() + static_cast<std::ptrdiff_t>(lane.cursor));
        // 被拒并移出外层栈的未应用步骤不再导出为可重做历史。
        if (identity < MapDataEditLaneCount)
            pending.insert(pending.end(), lane.commands.begin() + static_cast<std::ptrdiff_t>(lane.cursor), lane.commands.end());
    }
    // 每个分区内逻辑身份递增即分区顺序；按身份合并保持各分区顺序不变。
    const auto byIdentity = [](const auto& a, const auto& b) { return a->logicalIdentity < b->logicalIdentity; };
    std::sort(applied.begin(), applied.end(), byIdentity);
    std::sort(pending.begin(), pending.end(), byIdentity);
    const auto find = [](const CommandList& list, std::uint64_t identity) -> std::shared_ptr<const Command> {
        const auto found = std::find_if(list.begin(), list.end(), [&](const auto& command) { return command->logicalIdentity == identity; });
        return found == list.end() ? nullptr : *found;
    };
    TerrainHistoryLine line;
    line.currentIndex = applied.size();
    line.commands = applied;
    line.commands.insert(line.commands.end(), pending.begin(), pending.end());
    // 保存站与当前位置都必须是线性历史的前缀：恢复先重现物理保存站，再逆退到当前位置。
    // 两者为包含关系时，较小的集合排在前面；各段内部按身份递增，分区顺序不变。
    // 不是包含关系时保持“已应用在前”，导出会按分支拒绝，新的地图保存仍可冻结保存站。
    if (terrainHistoryEpoch_ && !terrainHistoryEpoch_->savedCommands.empty()) {
        std::unordered_set<std::uint64_t> saved;
        for (const auto& command : terrainHistoryEpoch_->savedCommands) if (command) saved.insert(command->logicalIdentity);
        const auto isApplied = [&](const auto& command) { return find(applied, command->logicalIdentity) != nullptr; };
        const bool savedApplied = std::all_of(saved.begin(), saved.end(), [&](auto identity) { return find(applied, identity) != nullptr; });
        const bool appliedSaved = std::all_of(applied.begin(), applied.end(), [&](const auto& command) {
            return saved.contains(command->logicalIdentity);
        }) && std::all_of(saved.begin(), saved.end(), [&](auto identity) {
            return find(applied, identity) || find(pending, identity);
        });
        if (savedApplied || appliedSaved) {
            const auto rank = [&](const auto& command) {
                const bool inSaved = saved.contains(command->logicalIdentity), live = isApplied(command);
                return savedApplied ? (inSaved ? 0 : live ? 1 : 2) : (live ? 0 : inSaved ? 1 : 2);
            };
            std::stable_sort(line.commands.begin(), line.commands.end(), [&](const auto& a, const auto& b) {
                const auto left = rank(a), right = rank(b);
                return left != right ? left < right : a->logicalIdentity < b->logicalIdentity;
            });
        }
    }
    // 跨分区重排只有在每个成员的字节链仍首尾相接时才可重放。
    for (std::size_t kind = 0; kind < current_.size(); ++kind) {
        const std::optional<std::vector<std::byte>>* running = nullptr;
        for (const auto& command : history_) {
            const auto found = std::find_if(command->changes.begin(), command->changes.end(),
                [&](const auto& change) { return Index(change.kind) == kind; });
            if (found != command->changes.end()) { running = &found->before; break; }
        }
        if (!running) continue;
        for (std::size_t i = 0; i < line.commands.size(); ++i) {
            if (i == line.currentIndex && *running != current_[kind]) return std::nullopt;
            for (const auto& change : line.commands[i]->changes) {
                if (Index(change.kind) != kind) continue;
                if (change.before != *running) return std::nullopt;
                running = &change.after;
            }
        }
        if (line.currentIndex == line.commands.size() && *running != current_[kind]) return std::nullopt;
    }
    return line;
}

Result<MapDataEditSession::TerrainHistorySaveLease> MapDataEditSession::PrepareTerrainHistoryForSave(
    std::size_t maximumBytes, bool restoringAtOrigin) const {
    using Output = Result<TerrainHistorySaveLease>;
    const auto fail = [](const char* code, const char* message) { return HistoryFailure<TerrainHistorySaveLease>(code, message); };
    if (!terrainOriginal_ || !pathingOriginal_ || (history_.empty() && !restoringAtOrigin))
        return fail("TERRAIN_HISTORY_NOT_AVAILABLE", "没有需要冻结的地形typed历史。");
    if (!maximumBytes || maximumBytes > TerrainJournalMaximumBytes)
        return fail("TERRAIN_HISTORY_BUDGET", "历史冻结预算只能在64MiB内收紧且不能为零。");
    auto line = CaptureTerrainHistoryLine(maximumBytes);
    if (!line.Succeeded() || !line.value) return {{}, line.diagnostics};
    if (line.value->commands.empty() && !restoringAtOrigin)
        return fail("TERRAIN_HISTORY_NOT_AVAILABLE", "保存站之前没有可恢复的typed命令。");
    if (terrainHistoryEpoch_ && terrainHistoryEpoch_->savedSourceRevision != terrainSourceRevision_)
        return fail("TERRAIN_HISTORY_SOURCE_GAP", "草稿已保留；上一次实际地图保存缺少可核验的历史，无法重新接续更早的撤销位置。");
    const auto cost = MeasureTerrainJournalDraft(maximumBytes, 64u * 1024u * 1024u);
    constexpr std::size_t overhead = 16u * 1024u;
    if (!cost.Succeeded() || !cost.value || maximumBytes < overhead
        || cost.value->retainedBytes > (maximumBytes - overhead) / 2
        || (terrainHistoryEpoch_ && terrainHistoryEpoch_->retainedBytes > maximumBytes - overhead - cost.value->retainedBytes * 2)
        || terrainSourceRevision_ == std::numeric_limits<std::uint64_t>::max())
        return fail("TERRAIN_HISTORY_BUDGET", "旧epoch、真实命令别名与冻结成员的累计驻留超过历史预算。");
    try {
        auto snapshot = FreezeForSave(maximumBytes - cost.value->retainedBytes - overhead);
        if (!snapshot.Succeeded() || !snapshot.value) return fail("TERRAIN_HISTORY_BUDGET", "冻结旧epoch的原生成员超过历史预算。");
        auto state = std::make_shared<TerrainHistoryState>();
        state->baseline = terrainHistoryEpoch_ ? terrainHistoryEpoch_->baseline : *snapshot.value;
        state->sourceShadow = terrainHistoryEpoch_ ? terrainHistoryEpoch_->sourceShadow : nullptr;
        state->savedIndex = line.value->currentIndex;
        state->savedCommands.assign(line.value->commands.begin(), line.value->commands.begin() + state->savedIndex);
        state->retainedBytes = terrainHistoryEpoch_ ? terrainHistoryEpoch_->retainedBytes : cost.value->retainedBytes * 2 + overhead;
        state->savedSourceRevision = terrainSourceRevision_ + 1;
        TerrainHistorySaveLease lease; lease.state_ = std::move(state);
        lease.checkpoint_ = std::move(*snapshot.value); lease.previousEpoch_ = terrainHistoryEpoch_;
        lease.retainedBytes_ = lease.state_->retainedBytes;
        lease.preparationBytes_ = cost.value->retainedBytes * 2 + overhead
            + (terrainHistoryEpoch_ ? terrainHistoryEpoch_->retainedBytes : 0);
        lease.sessionIdentity_ = sessionIdentity_; lease.revision_ = revision_; lease.sourceRevision_ = terrainSourceRevision_;
        return Output::Success(std::move(lease));
    } catch (const std::bad_alloc&) { return fail("TERRAIN_HISTORY_BUDGET", "冻结历史分配失败，地图仍可正常保存。"); }
      catch (const std::length_error&) { return fail("TERRAIN_HISTORY_BUDGET", "冻结历史大小无效，地图仍可正常保存。"); }
}

Status MapDataEditSession::CommitTerrainHistorySave(const TerrainHistorySaveLease& lease) {
    const auto fail = [](const char* code) { return Status::Failure("Terrain.History", code,
        "地图保存结果不受影响；可选撤销历史的会话、修订或保存代际不一致，保留raw草稿。"); };
    if (!lease.state_ || !lease.state_->baseline || !lease.checkpoint_ || !lease.sessionIdentity_)
        return fail("TERRAIN_HISTORY_LEASE_INVALID");
    if (lease.sessionIdentity_ != sessionIdentity_ || lease.revision_ != revision_
        || lease.sourceRevision_ == std::numeric_limits<std::uint64_t>::max()
        || terrainSourceRevision_ != lease.sourceRevision_ + 1 || terrainHistoryEpoch_ != lease.previousEpoch_
        || terrainOriginal_ != lease.checkpoint_->terrain_
        || original_ != lease.checkpoint_->current_
        || originalPathingRestrictions_ != lease.checkpoint_->pathingRestrictions_)
        return fail("TERRAIN_HISTORY_LEASE_STALE");
    if (resizeState_ || lease.checkpoint_->resizeState_) {
        const auto* expectedShadow = lease.checkpoint_->TerrainHistoryShadow();
        const auto* actualShadow = TerrainHistoryShadow();
        // typed历史能重放扩裁迁移，不能凭保存成功推造另一次显式Bake。
        // 首次和重复保存都须能重现实际SHD；不相同只停用可选冷历史，地图已保存。
        if (!expectedShadow || !actualShadow || expectedShadow->name != actualShadow->name
            || expectedShadow->present != actualShadow->present || expectedShadow->bytes != actualShadow->bytes)
            return fail("TERRAIN_HISTORY_EPOCH_LIMIT");
    }
    if (lease.previousEpoch_ && !lease.checkpoint_->IsDirty()) {
        const auto sameLayer = [](const auto& a, const auto& b) { return bool(a) == bool(b) && (!a || *a == *b); };
        // 未编辑的重复保存只有真实读回基线完全相同才能保留原station；
        // 资源/物编变化导致WPM或P不同，也不能仅凭IsDirty=false假称同一epoch。
        if (pathingOriginal_ != lease.checkpoint_->pathingOriginal_
            || !sameLayer(pathingProtected_, lease.checkpoint_->pathingProtected_)
            || !sameLayer(originalPathingRestrictions_, lease.checkpoint_->originalPathingRestrictions_)
            || savedSpecialDoodads_ != lease.checkpoint_->savedSpecialDoodads_
            || savedBoundsState_ != lease.checkpoint_->savedBoundsState_
            || savedStartLocations_ != lease.checkpoint_->savedStartLocations_)
            return fail("TERRAIN_HISTORY_EPOCH_LIMIT");
    }
    terrainHistoryEpoch_ = lease.state_;
    return Status::Ok();
}

Result<TerrainStrokeJournal> MapDataEditSession::ExportTerrainHistoryJournal(std::size_t maximumBytes) const {
    const auto fail = [](const char* code, const char* message) { return HistoryFailure<TerrainStrokeJournal>(code, message); };
    if (!terrainHistoryEpoch_) return fail("TERRAIN_HISTORY_NOT_AVAILABLE", "当前会话没有一座已冻结的地图保存站。");
    const auto& epoch = *terrainHistoryEpoch_;
    if (terrainSourceRevision_ != epoch.savedSourceRevision)
        return fail("TERRAIN_HISTORY_SOURCE_GAP", "草稿已保留；最近一次地图保存未能建立可核验的历史锚点。");
    if (!maximumBytes || maximumBytes > TerrainJournalMaximumBytes || epoch.retainedBytes > maximumBytes)
        return fail("TERRAIN_HISTORY_BUDGET", "冻结历史超过64MiB或宿主收紧的预算。");
    auto line = CaptureTerrainHistoryLine(maximumBytes - epoch.retainedBytes);
    if (!line.Succeeded() || !line.value) return {{}, line.diagnostics};
    if (!SavedPrefixMatches(epoch.savedCommands, line.value->commands, epoch.savedIndex, sessionIdentity_))
        return fail("TERRAIN_HISTORY_BRANCH", "草稿已保留；保存站所在旧分支已被新编辑截断，未伪造clean位置。");
    auto journal = ExportTerrainJournalCommands(line.value->commands, true, true);
    if (!journal.Succeeded() || !journal.value) return journal;
    TerrainJournalHistory history;
    history.savedIndex = static_cast<std::uint32_t>(epoch.savedIndex);
    history.currentIndex = static_cast<std::uint32_t>(line.value->currentIndex);
    history.frozenBaseline = epoch.baseline;
    history.frozenSourceShadow = epoch.sourceShadow;
    history.frozenRetainedBytes = epoch.retainedBytes;
    journal.value->history = std::move(history);
    return journal;
}

Status MapDataEditSession::ReplayMapMemberJournal(std::span<const MapMemberJournalEdit> records) {
    if (records.empty() || records.size() > 5)
        return Status::Failure("MapData.History", "MAP_HISTORY_MEMBER_COUNT", "普通地图数据历史事务的成员数量无效。");
    std::vector<MemberChange> changes;
    for (const auto& record : records) {
        const auto valid = ValidateMapMemberJournalEdit(record);
        if (!valid.Succeeded()) return valid;
        if (!changes.empty() && changes.back().kind >= record.kind)
            return Status::Failure("MapData.History", "MAP_HISTORY_MEMBER_ORDER", "普通地图数据历史的成员重复或顺序无效。");
        const auto& current = current_[static_cast<std::size_t>(record.kind)];
        if (bool(current) != record.beforePresent)
            return Status::Failure("MapData.History", "MAP_HISTORY_MEMBER_STALE", "地图数据历史中的成员存在性与当前会话不同。");
        auto next = Detail::ApplyPlacementDelta(current ? std::span<const std::byte>(*current) : std::span<const std::byte>{},
            record.delta, 0, "MAP_HISTORY_MEMBER_DELTA", "MAP_HISTORY_MEMBER_STALE", "MAP_HISTORY_MEMBER_OUTPUT");
        if (!next.Succeeded() || !next.value) return {next.diagnostics};
        if (record.kind == MapDataKind::Settings && startLocations_) {
            const auto parsed = MapSettings::Parse(*next.value);
            if (!parsed.Succeeded() || !parsed.value || !startLocations_->MatchesPlayers(*parsed.value))
                return Status::Failure("MapData.History", "MAP_HISTORY_START_LOCATIONS", "改动起始位置的历史必须使用原有联动记录。");
        }
        const auto prior = changes.size();
        const auto prepared = Prepare(record.kind, std::move(next), changes);
        if (!prepared.Succeeded()) return prepared;
        if (prior == changes.size())
            return Status::Failure("MapData.History", "MAP_HISTORY_NO_CHANGE", "地图数据历史中包含空编辑。");
    }
    return Commit(std::move(changes));
}

std::string MapDataEditSession::TerrainHistoryStateFingerprint() const {
    if (!terrainHistoryEpoch_) return {};
    if (terrainSourceRevision_ != terrainHistoryEpoch_->savedSourceRevision) return "FTJE:TERRAIN_HISTORY_SOURCE_GAP";
    auto line = CaptureTerrainHistoryLine(TerrainJournalMaximumBytes);
    if (!line.Succeeded() || !line.value)
        return "FTJE:" + (line.diagnostics.empty() ? std::string{"unavailable"} : line.diagnostics.front().code);
    const auto& epoch = *terrainHistoryEpoch_;
    if (!SavedPrefixMatches(epoch.savedCommands, line.value->commands, epoch.savedIndex, sessionIdentity_))
        return "FTJE:TERRAIN_HISTORY_BRANCH";
    // 每个ID均由本会话真实不可变Command授予，raw/source另有完整scope；
    // 此处仅做本会话CAS，不重读旧网格或把内存身份写进FTJE。
    std::string scope = "FTJE-command-scope:" + std::to_string(sessionIdentity_) + ":"
        + std::to_string(epoch.savedSourceRevision) + ":" + std::to_string(epoch.savedIndex) + ":"
        + std::to_string(line.value->currentIndex) + ":";
    for (const auto& command : line.value->commands) scope += std::to_string(command->logicalIdentity) + ";";
    const auto digest = Sha256Text(scope);
    return digest.Succeeded() && digest.value ? "FTJE:" + *digest.value : std::string{};
}

Result<std::vector<ArchiveExactMember>> MapDataEditSession::TerrainHistoryBaselineMembers(std::size_t maximumBytes) const {
    using Output = Result<std::vector<ArchiveExactMember>>;
    const auto fail = [] { return HistoryFailure<std::vector<ArchiveExactMember>>("TERRAIN_HISTORY_BUDGET",
        "历史固定结构成员缺失、重复或超过物化预算，未授权raw恢复。"); };
    if (!terrainOriginal_ || !pathingOriginal_ || maximumBytes > TerrainJournalMaximumBytes || maximumBytes < 4096
        || terrainOriginal_->vertices.size() > (maximumBytes - 4096) / 8) return fail();
    try {
        std::size_t used = 4096 + terrainOriginal_->vertices.size() * 8;
        // 先收齐将复制的十成员，再序列化W3E和分配vector。
        for (const auto& original : original_) if (original) {
            if (original->size() > maximumBytes - used) return fail(); used += original->size();
        }
        for (const auto& preserved : preservedSources_) if (preserved) for (const auto& source : preserved->sources) {
            if (source.data.size() > maximumBytes - used) return fail(); used += source.data.size();
        }
        if (pathingOriginal_->size() > maximumBytes - used) return fail();
        auto terrain = SerializeMapTerrain(*terrainOriginal_);
        if (!terrain.Succeeded() || !terrain.value) return {{}, terrain.diagnostics};
        std::vector<ArchiveExactMember> members; members.reserve(TerrainJournalMemberNames.size());
        for (std::size_t i = 0; i < TerrainJournalMemberNames.size(); ++i) {
            ArchiveExactMember member; member.name = TerrainJournalMemberNames[i];
            if (i == 0) { member.present = true; member.bytes = std::move(*terrain.value); }
            else if (i == 1) { member.present = true; member.bytes = *pathingOriginal_; }
            else {
                const std::vector<std::byte>* found = nullptr;
                for (std::size_t kind = 0; kind < original_.size(); ++kind) {
                    if (original_[kind] && HistoryName(memberNames_[kind]) == member.name) {
                        if (found) return fail(); found = &*original_[kind];
                    }
                    if (preservedSources_[kind]) for (const auto& source : preservedSources_[kind]->sources)
                        if (HistoryName(source.name) == member.name) { if (found) return fail(); found = &source.data; }
                }
                if (found) { member.present = true; member.bytes = *found; }
            }
            members.push_back(std::move(member));
        }
        return Output::Success(std::move(members));
    } catch (const std::bad_alloc&) { return fail(); }
      catch (const std::length_error&) { return fail(); }
}

Status MaterializeTerrainJournalHistory(TerrainStrokeJournal& journal) {
    if (!journal.history) return Status::Ok();
    auto& history = *journal.history;
    if (!history.frozenBaseline) return history.frozenRetainedBytes || history.frozenSourceShadow ? Status::Failure("Terrain.History",
        "TERRAIN_HISTORY_INVALID", "没有冻结旧epoch却携带内存资格字段。") : Status::Ok();
    if (!history.baselineMembers.empty() || !history.baselinePathingLayers.empty() || history.baselineShadow || !history.frozenRetainedBytes
        || history.frozenRetainedBytes > TerrainJournalMaximumBytes)
        return Status::Failure("Terrain.History", "TERRAIN_HISTORY_INVALID", "冻结旧epoch与wire材料不能混用。");
    const auto& baseline = *history.frozenBaseline;
    if (!baseline.terrainOriginal_ || !baseline.pathingOriginal_ || !baseline.pathingProtected_ || !baseline.originalPathingRestrictions_)
        return Status::Failure("Terrain.History", "TERRAIN_HISTORY_INVALID", "冻结旧epoch缺少真实地形和P/savedS。");
    auto members = baseline.TerrainHistoryBaselineMembers(TerrainJournalMaximumBytes);
    if (!members.Succeeded() || !members.value) return {members.diagnostics};
    const auto baselineP = baseline.savedResizeState_ ? baseline.savedResizeState_->protectedPathing : baseline.pathingProtected_;
    auto layers = EncodePathingRestrictionDraft(*baseline.terrainOriginal_, *baseline.pathingOriginal_, *baseline.pathingOriginal_,
        *baselineP, *baseline.originalPathingRestrictions_, *baseline.originalPathingRestrictions_);
    if (!layers.Succeeded() || !layers.value) return {layers.diagnostics};
    std::size_t used = 128u * 1024u + layers.value->size() + journal.baselinePathingLayers.size();
    if (used > TerrainJournalMaximumBytes) return Status::Failure("Terrain.History", "TERRAIN_HISTORY_BUDGET", "历史两层P超过工程预算。");
    for (const auto& member : *members.value) {
        if (member.bytes.size() > TerrainJournalMaximumBytes - used)
            return Status::Failure("Terrain.History", "TERRAIN_HISTORY_BUDGET", "历史完整十成员超过工程预算。");
        used += member.bytes.size();
    }
    if (history.frozenSourceShadow || std::any_of(journal.strokes.begin(), journal.strokes.end(),
        [](const auto& stroke) { return stroke.intent.resize.has_value(); })) {
        const auto* shadow = history.frozenSourceShadow ? &history.frozenSourceShadow->member_
            : baseline.savedResizeState_ ? &baseline.savedResizeState_->shadow : nullptr;
        if (!shadow || shadow->bytes.size() > TerrainJournalMaximumBytes - used)
            return Status::Failure("Terrain.History", "TERRAIN_HISTORY_INVALID", "扩裁历史缺少真实旧SHD或明确缺失状态。");
        history.baselineShadow = *shadow;
        used += history.baselineShadow->bytes.size();
    }
    history.baselineMembers = std::move(*members.value);
    history.baselinePathingLayers = std::move(*layers.value);
    history.frozenBaseline.reset(); history.frozenSourceShadow.reset(); history.frozenRetainedBytes = 0;
    return Status::Ok();
}
} // namespace War3Map::MapData
