#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/PathingRestrictionEditing.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
template<class T> Result<T> HistoryFailure(const char* code, const char* message) {
    return Result<T>::Failure("Terrain.History", code, message);
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
        "草稿已保留；地形历史不在一座保存站的完整线性typed窗口内，未恢复部分撤销栈。"); };
    if (!maximumBytes || maximumBytes > TerrainJournalMaximumBytes || cursor_ > history_.size()
        || history_.size() > 4096) return fail("TERRAIN_HISTORY_BUDGET");
    const auto cost = MeasureTerrainJournalDraft(maximumBytes, 64u * 1024u * 1024u);
    if (!cost.Succeeded() || !cost.value) return fail("TERRAIN_HISTORY_BUDGET");
    try {
        TerrainHistoryLine line;
        line.commands.reserve(std::min(history_.size(), std::size_t{128}));
        std::vector<std::shared_ptr<const Command>> currentPrefix;
        std::size_t logicalCursor = 0;
        for (std::size_t i = 0; i < history_.size(); ++i) {
            if (i == cursor_) currentPrefix.assign(line.commands.begin(), line.commands.begin() + logicalCursor);
            const auto& command = history_[i];
            if (!command || command->sessionIdentity != sessionIdentity_ || !command->logicalIdentity)
                return fail("TERRAIN_HISTORY_BRANCH");
            if (!command->logicalForward) {
                if (!logicalCursor || line.commands[logicalCursor - 1]->logicalIdentity != command->logicalIdentity)
                    return fail("TERRAIN_HISTORY_BRANCH");
                --logicalCursor;
            } else if (logicalCursor < line.commands.size()
                && line.commands[logicalCursor]->logicalIdentity == command->logicalIdentity) {
                // Qt的真实Redo追加同一logicalIdentity；保留最初正向命令供typed重放。
                ++logicalCursor;
            } else {
                if (std::any_of(line.commands.begin(), line.commands.end(), [&](const auto& old) {
                    return old->logicalIdentity == command->logicalIdentity;
                })) return fail("TERRAIN_HISTORY_BRANCH");
                line.commands.resize(logicalCursor);
                if (line.commands.size() == 128) return fail("TERRAIN_HISTORY_BUDGET");
                line.commands.push_back(command); ++logicalCursor;
            }
        }
        if (cursor_ == history_.size()) currentPrefix.assign(line.commands.begin(), line.commands.begin() + logicalCursor);
        if (currentPrefix.size() > line.commands.size()
            || !std::equal(currentPrefix.begin(), currentPrefix.end(), line.commands.begin(), [](const auto& a, const auto& b) {
                return a->logicalIdentity == b->logicalIdentity;
            })) return fail("TERRAIN_HISTORY_BRANCH");
        line.currentIndex = currentPrefix.size();
        // 每个栈槽必须恰好对应一个真实typed命令；四说明字段可另合并，不能偷占Qt索引。
        auto prefix = ExportTerrainJournalCommands(line.commands, false);
        if (!prefix.Succeeded() || !prefix.value || prefix.value->strokes.size() != line.commands.size())
            return fail("TERRAIN_HISTORY_UNEXPRESSIBLE");
        return Output::Success(std::move(line));
    } catch (const std::bad_alloc&) { return fail("TERRAIN_HISTORY_BUDGET"); }
      catch (const std::length_error&) { return fail("TERRAIN_HISTORY_BUDGET"); }
}

Result<MapDataEditSession::TerrainHistorySaveLease> MapDataEditSession::PrepareTerrainHistoryForSave(std::size_t maximumBytes) const {
    using Output = Result<TerrainHistorySaveLease>;
    const auto fail = [](const char* code, const char* message) { return HistoryFailure<TerrainHistorySaveLease>(code, message); };
    if (!terrainOriginal_ || !pathingOriginal_ || history_.empty())
        return fail("TERRAIN_HISTORY_NOT_AVAILABLE", "没有需要冻结的地形typed历史。");
    if (!maximumBytes || maximumBytes > TerrainJournalMaximumBytes)
        return fail("TERRAIN_HISTORY_BUDGET", "历史冻结预算只能在64MiB内收紧且不能为零。");
    auto line = CaptureTerrainHistoryLine(maximumBytes);
    if (!line.Succeeded() || !line.value) return {{}, line.diagnostics};
    if (line.value->commands.empty())
        return fail("TERRAIN_HISTORY_NOT_AVAILABLE", "保存站之前没有可恢复的typed命令。");
    if (!terrainHistoryEpoch_ && line.value->currentIndex != line.value->commands.size())
        return fail("TERRAIN_HISTORY_SAVE_REDO", "草稿已保留；首次保存前已有旧epoch的Redo命令，本版未把它重绑定为新来源笔划。");
    if (terrainHistoryEpoch_ && (terrainHistoryEpoch_->savedSourceRevision != terrainSourceRevision_
        || IsDirty() || line.value->currentIndex != terrainHistoryEpoch_->savedIndex
        || terrainHistoryEpoch_->savedIndex > line.value->commands.size()
        || !std::equal(terrainHistoryEpoch_->savedCommands.begin(), terrainHistoryEpoch_->savedCommands.end(),
            line.value->commands.begin(), [](const auto& a, const auto& b) { return a->logicalIdentity == b->logicalIdentity; })))
        return fail("TERRAIN_HISTORY_EPOCH_LIMIT", "草稿已保留；本版只恢复一座地图保存站，第二旧epoch未保存为typed撤销历史。");
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
        state->savedIndex = line.value->currentIndex;
        state->savedCommands.assign(line.value->commands.begin(), line.value->commands.begin() + state->savedIndex);
        state->retainedBytes = terrainHistoryEpoch_ ? terrainHistoryEpoch_->retainedBytes : cost.value->retainedBytes * 2 + overhead;
        state->savedSourceRevision = terrainSourceRevision_ + 1;
        TerrainHistorySaveLease lease; lease.state_ = std::move(state);
        lease.checkpoint_ = std::move(*snapshot.value); lease.previousEpoch_ = terrainHistoryEpoch_;
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
    if (lease.previousEpoch_) {
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
        return fail("TERRAIN_HISTORY_EPOCH_LIMIT", "草稿已保留；历史已跨第二旧epoch，不能恢复为一座保存站撤销栈。");
    if (!maximumBytes || maximumBytes > TerrainJournalMaximumBytes || epoch.retainedBytes > maximumBytes)
        return fail("TERRAIN_HISTORY_BUDGET", "冻结历史超过64MiB或宿主收紧的预算。");
    auto line = CaptureTerrainHistoryLine(maximumBytes - epoch.retainedBytes);
    if (!line.Succeeded() || !line.value) return {{}, line.diagnostics};
    if (!epoch.savedIndex || epoch.savedIndex > line.value->commands.size()
        || epoch.savedCommands.size() != epoch.savedIndex
        || !std::equal(epoch.savedCommands.begin(), epoch.savedCommands.end(), line.value->commands.begin(), [](const auto& a, const auto& b) {
            return a->sessionIdentity == b->sessionIdentity && a->logicalIdentity == b->logicalIdentity;
        })) return fail("TERRAIN_HISTORY_BRANCH", "草稿已保留；保存站所在旧分支已被新编辑截断，未伪造clean位置。");
    auto journal = ExportTerrainJournalCommands(line.value->commands, true);
    if (!journal.Succeeded() || !journal.value) return journal;
    TerrainJournalHistory history;
    history.savedIndex = static_cast<std::uint32_t>(epoch.savedIndex);
    history.currentIndex = static_cast<std::uint32_t>(line.value->currentIndex);
    history.frozenBaseline = epoch.baseline;
    history.frozenRetainedBytes = epoch.retainedBytes;
    journal.value->history = std::move(history);
    return journal;
}

std::string MapDataEditSession::TerrainHistoryStateFingerprint() const {
    if (!terrainHistoryEpoch_) return {};
    if (terrainSourceRevision_ != terrainHistoryEpoch_->savedSourceRevision) return "FTJE:TERRAIN_HISTORY_EPOCH_LIMIT";
    auto line = CaptureTerrainHistoryLine(TerrainJournalMaximumBytes);
    if (!line.Succeeded() || !line.value)
        return "FTJE:" + (line.diagnostics.empty() ? std::string{"unavailable"} : line.diagnostics.front().code);
    const auto& epoch = *terrainHistoryEpoch_;
    if (epoch.savedCommands.size() != epoch.savedIndex || epoch.savedIndex > line.value->commands.size()
        || !std::equal(epoch.savedCommands.begin(), epoch.savedCommands.end(), line.value->commands.begin(),
            [](const auto& a, const auto& b) { return a->logicalIdentity == b->logicalIdentity; })) return "FTJE:TERRAIN_HISTORY_BRANCH";
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
    if (!history.frozenBaseline) return history.frozenRetainedBytes ? Status::Failure("Terrain.History",
        "TERRAIN_HISTORY_INVALID", "没有冻结旧epoch却携带内存资格字段。") : Status::Ok();
    if (!history.baselineMembers.empty() || !history.baselinePathingLayers.empty() || !history.frozenRetainedBytes
        || history.frozenRetainedBytes > TerrainJournalMaximumBytes)
        return Status::Failure("Terrain.History", "TERRAIN_HISTORY_INVALID", "冻结旧epoch与wire材料不能混用。");
    const auto& baseline = *history.frozenBaseline;
    if (!baseline.terrainOriginal_ || !baseline.pathingOriginal_ || !baseline.pathingProtected_ || !baseline.originalPathingRestrictions_)
        return Status::Failure("Terrain.History", "TERRAIN_HISTORY_INVALID", "冻结旧epoch缺少真实地形和P/savedS。");
    auto members = baseline.TerrainHistoryBaselineMembers(TerrainJournalMaximumBytes);
    if (!members.Succeeded() || !members.value) return {members.diagnostics};
    auto layers = EncodePathingRestrictionDraft(*baseline.terrainOriginal_, *baseline.pathingOriginal_, *baseline.pathingOriginal_,
        *baseline.pathingProtected_, *baseline.originalPathingRestrictions_, *baseline.originalPathingRestrictions_);
    if (!layers.Succeeded() || !layers.value) return {layers.diagnostics};
    std::size_t used = 128u * 1024u + layers.value->size() + journal.baselinePathingLayers.size();
    if (used > TerrainJournalMaximumBytes) return Status::Failure("Terrain.History", "TERRAIN_HISTORY_BUDGET", "历史两层P超过工程预算。");
    for (const auto& member : *members.value) {
        if (member.bytes.size() > TerrainJournalMaximumBytes - used)
            return Status::Failure("Terrain.History", "TERRAIN_HISTORY_BUDGET", "历史完整十成员超过工程预算。");
        used += member.bytes.size();
    }
    history.baselineMembers = std::move(*members.value);
    history.baselinePathingLayers = std::move(*layers.value);
    history.frozenBaseline.reset(); history.frozenRetainedBytes = 0;
    return Status::Ok();
}
} // namespace War3Map::MapData
