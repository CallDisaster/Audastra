#include "War3MapCore/Services/TerrainPathingSave.h"
#include "TerrainPathingFinalOverlay.h"
#include "TerrainReplayInternal.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/NativeDestructablePathing.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace War3Map::Services {
namespace {
using Output = Result<TerrainReplayCandidate>;
using MapData::TerrainStrokeJournal;
using TerrainPathingFinalDetail::FinalMemberOverlay;

Output Failure(const char* code, const char* message) {
    return Output::Failure("Terrain.History", code, message);
}

// 能力/预算不足仍可保留raw工程。来源、保存站或typed重放失败必须硬拒绝，
// 不能把篡改后的历史当作一次成功的raw恢复。原始原因始终保留。
Output HistoryFailure(std::vector<Diagnostic> diagnostics) {
    const auto budget = [](const auto& item) {
        return item.code == "TERRAIN_REPLAY_BUDGET" || item.code == "TERRAIN_JOURNAL_BUDGET" || item.code == "TERRAIN_HISTORY_BUDGET"
            || item.code == "TERRAIN_PATHING_RESOURCE_BUDGET" || item.code == "TERRAIN_NATIVE_OBJECT_BUDGET"
            || item.code == "TERRAIN_SAVE_PLAN_BUDGET" || item.code == "TERRAIN_FINAL_INPUT_BUDGET"
            || item.code == "EXACT_MEMBER_LIMIT" || item.code == "FILE_READ_BUDGET_EXCEEDED"
            || item.code == "VERIFIED_FILE_BUDGET_EXCEEDED";
    };
    if (!diagnostics.empty() && std::all_of(diagnostics.begin(), diagnostics.end(), budget))
        return {{}, std::move(diagnostics)};
    auto result = Failure("TERRAIN_HISTORY_REPLAY_FAILED", "历史恢复未取得完整的真实重放、保存站或资源身份；未安装任何前缀。");
    for (auto& item : diagnostics) result.diagnostics.push_back(std::move(item));
    return result;
}

bool Charge(std::size_t& remaining, std::size_t count, std::size_t multiple = 1) {
    if (multiple && count > remaining / multiple) return false;
    remaining -= count * multiple;
    return true;
}

Status MatchManifest(const TerrainStrokeJournal& journal, std::string_view sourceSha,
    std::span<const ArchiveExactMember> actual) {
    const auto fail = [] { return Status::Failure("Terrain.History", "TERRAIN_HISTORY_SOURCE_MISMATCH",
        "历史顶层记录不属于当前实际地图的完整成员或缺席身份。"); };
    if (journal.sourceSha256 != sourceSha || journal.members.size() != actual.size()) return fail();
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto& member = actual[i]; const auto& expected = journal.members[i];
        if (expected.name != MapData::TerrainJournalMemberNames[i] || member.name != expected.name
            || member.present != expected.present || member.bytes.size() != expected.bytes
            || (!member.present && !member.bytes.empty())) return fail();
        if (member.present) {
            const auto hash = Sha256(member.bytes);
            if (!hash.Succeeded() || !hash.value) return {hash.diagnostics};
            if (*hash.value != expected.sha256) return fail();
        }
    }
    return Status::Ok();
}
} // namespace

Result<TerrainReplayCandidate> TerrainPathingSave::RestoreHistoryJournal(const MapDocument& original,
    std::span<const std::byte> terrain, std::span<const std::byte> pathing,
    const TerrainStrokeJournal& journal, const MapData::MapDataEditSession& expected,
    const std::filesystem::path& gameRoot, const TerrainReplayLimits& limits,
    const std::string& sourceSha, const HistoryMemberReader& physicalReader) {
    // 此入口仅由两个真实来源重载分派，源SHA和reader从同一宿主生成。
    // journal历史正文始终活到全部回调、规则owner与SavePlan销毁之后。
    const TerrainReplayLimits hard;
    if (!limits.maximumResourceBytes || limits.maximumResourceBytes > hard.maximumResourceBytes
        || !limits.maximumWork || limits.maximumWork > hard.maximumWork
        || !limits.maximumCandidateBytes || limits.maximumCandidateBytes > hard.maximumCandidateBytes)
        return Failure("TERRAIN_REPLAY_LIMITS", "历史恢复预算只能收紧且不能为零。");
    // Core仅以只读span校验typed域及固定十项metadata；实际大副本统一在下面预收。
    auto preparation = TerrainReplayDetail::PrepareDraft(original, expected, journal, limits);
    if (!preparation.Succeeded() || !preparation.value) return HistoryFailure(std::move(preparation.diagnostics));
    if (!journal.history)
        return Failure("TERRAIN_HISTORY_SOURCE_MISMATCH", "历史工厂缺少完整的线性保存站记录。");
    const auto& history = *journal.history;
    auto& remaining = preparation.value->remaining;
    std::vector<std::string> names;
    for (const auto name : MapData::TerrainJournalMemberNames) names.emplace_back(name);
    ArchiveExactMemberLimits readLimits;
    readLimits.memberCount = names.size();

    // 输入历史、转换后的typed副本、历史文档解析及保存站全文比较先收费。
    // CreateReplay另核网格/通行基线和每笔历史，不用本层预算替代其限制。
    std::size_t baselineBytes = 0;
    for (const auto& member : history.baselineMembers) {
        if (member.bytes.size() > MapData::TerrainJournalMaximumBytes - baselineBytes)
            return Failure("TERRAIN_REPLAY_BUDGET", "历史结构正文超过固定预算。");
        baselineBytes += member.bytes.size();
        if (!Charge(remaining.maximumCandidateBytes, member.bytes.capacity(), 4)
            || !Charge(remaining.maximumWork, member.bytes.size(), 4))
            return Failure("TERRAIN_REPLAY_BUDGET", "历史结构常驻、文档复制和来源核对超过恢复预算。");
    }
    if (!Charge(remaining.maximumCandidateBytes, history.baselinePathingLayers.capacity(), 2)
        || !Charge(remaining.maximumWork, history.baselinePathingLayers.size(), 2)
        || !Charge(remaining.maximumCandidateBytes, journal.baselinePathingLayers.capacity(), 3)
        || !Charge(remaining.maximumWork, journal.baselinePathingLayers.size(), 3))
        return Failure("TERRAIN_REPLAY_BUDGET", "旧epoch通行分层超过恢复预算。");
    // BaselineJournal复制完整typed栈，拆后缀时另有一个有界vector；先收数组和
    // 所有可变长字段，不能把仅复制intents的PrepareDraft当成完整typed副本预算。
    if (!Charge(remaining.maximumCandidateBytes, journal.strokes.capacity(), sizeof(MapData::TerrainJournalStroke))
        || !Charge(remaining.maximumCandidateBytes, journal.strokes.size(), sizeof(MapData::TerrainJournalStroke) * 2))
        return Failure("TERRAIN_REPLAY_BUDGET", "历史typed数组及前后缀索引超过恢复预算。");
    for (const auto& stroke : journal.strokes) {
        const auto textCopy = [&](const std::string& value) {
            return Charge(remaining.maximumCandidateBytes, value.capacity() + 1, 2)
                && Charge(remaining.maximumWork, value.size());
        };
        if (!Charge(remaining.maximumCandidateBytes, stroke.intent.points.capacity(), sizeof(MapData::TerrainStrokePoint) * 2)
            || !Charge(remaining.maximumWork, stroke.intent.points.size(), 2)
            || !textCopy(stroke.intent.brush.groundTexture) || !textCopy(stroke.rulesSha256)
            || (stroke.intent.palette && (!textCopy(stroke.intent.palette->expectedRawcode)
                || !textCopy(stroke.intent.palette->replacementRawcode)))
            || (stroke.intent.blockPaste && !textCopy(stroke.intent.blockPaste->expectedBlockSha256)))
            return Failure("TERRAIN_REPLAY_BUDGET", "历史typed采样点或文本副本超过恢复预算。");
        for (const auto* delta : {stroke.ordinaryDoodads ? static_cast<const MapData::TerrainPlacementJournalDelta*>(&*stroke.ordinaryDoodads) : nullptr,
                 stroke.units ? static_cast<const MapData::TerrainPlacementJournalDelta*>(&*stroke.units) : nullptr,
                 stroke.startLocations ? static_cast<const MapData::TerrainPlacementJournalDelta*>(&stroke.startLocations->units) : nullptr,
                 stroke.startLocations ? &stroke.startLocations->settings : nullptr}) {
            if (delta && (!textCopy(delta->beforeSha256) || !textCopy(delta->afterSha256)))
                return Failure("TERRAIN_REPLAY_BUDGET", "历史摆放完整摘要副本超过恢复预算。");
        }
        if (stroke.ordinaryDoodads) {
            const auto& edit = *stroke.ordinaryDoodads;
            if (!Charge(remaining.maximumCandidateBytes, edit.inserted.capacity())
                || !Charge(remaining.maximumWork, edit.inserted.size()))
                return Failure("TERRAIN_REPLAY_BUDGET", "历史普通对象副本超过恢复预算。");
        }
        if (stroke.units && (!Charge(remaining.maximumCandidateBytes, stroke.units->inserted.capacity())
            || !Charge(remaining.maximumWork, stroke.units->inserted.size())))
            return Failure("TERRAIN_REPLAY_BUDGET", "历史单位副本超过恢复预算。");
        if (stroke.startLocations) for (const auto* edit : {
            static_cast<const MapData::TerrainPlacementJournalDelta*>(&stroke.startLocations->units), &stroke.startLocations->settings}) {
            if (!Charge(remaining.maximumCandidateBytes, edit->inserted.capacity()) || !Charge(remaining.maximumWork, edit->inserted.size()))
                return Failure("TERRAIN_REPLAY_BUDGET", "历史起点副本超过恢复预算。");
        }
    }
    // 初次/最终来源读取与其比较副本共存；把读取帽也收紧到已扣除历史正文后的余额，
    // 不能先读完大成员才发现三份实际来源驻留已超出小预算。
    readLimits.memberBytes = readLimits.totalBytes = std::min({readLimits.totalBytes,
        remaining.maximumResourceBytes, remaining.maximumCandidateBytes / 3, remaining.maximumWork / 4});
    if (!readLimits.totalBytes)
        return Failure("TERRAIN_REPLAY_BUDGET", "完整物理保存站读取没有剩余预算。");
    auto actual = physicalReader(names, readLimits);
    if (!actual.Succeeded() || !actual.value) return HistoryFailure(std::move(actual.diagnostics));
    for (const auto& member : *actual.value)
        if (!Charge(remaining.maximumCandidateBytes, member.bytes.capacity(), 3)
            || !Charge(remaining.maximumWork, member.bytes.size(), 4))
            return Failure("TERRAIN_REPLAY_BUDGET", "实际保存站完整来源驻留或比较超过恢复预算。");
    if (!TerrainReplayDetail::SourceMatches(original, terrain, pathing, *actual.value))
        return Failure("TERRAIN_HISTORY_SOURCE_MISMATCH", "当前工程的原字节不属于实际物理地图。");
    const auto manifest = MatchManifest(journal, sourceSha, *actual.value);
    if (!manifest.Succeeded()) return {{}, manifest.diagnostics};

    auto baselineJournal = MapData::TerrainHistoryBaselineJournal(journal);
    if (!baselineJournal.Succeeded() || !baselineJournal.value) return HistoryFailure(std::move(baselineJournal.diagnostics));
    auto historicalView = FinalMemberOverlay::PrepareBaseline(history.baselineMembers,
        std::min(MapData::TerrainSavePlanLimits{}.maximumBytes, remaining.maximumCandidateBytes));
    if (!historicalView.Succeeded() || !historicalView.value) return HistoryFailure(std::move(historicalView.diagnostics));
    if (!Charge(remaining.maximumCandidateBytes, historicalView.value->MetadataBytes()))
        return Failure("TERRAIN_REPLAY_BUDGET", "历史读取索引超过剩余候选预算。");
    const HistoryMemberReader historicalReader = [&](auto requested, const auto& maximum) {
        return historicalView.value->Read(physicalReader, requested, maximum, nullptr);
    };
    const MapData::TerrainPathingResources::NativeObjectValidator validator = ObjectData::InspectNativeTerrainObjects;
    const auto resourcesFor = [&](const HistoryMemberReader& reader) -> TerrainReplayDetail::ResourceSource {
        return {
            [&, reader](const auto& before, const auto& after, const auto& doodads, bool enforce, auto maximumBytes)
                -> Result<MapData::TerrainPathingResources> {
                auto result = MapData::TerrainPathingResources::CaptureWaterBrushInputs(reader, before, after,
                    gameRoot, enforce, maximumBytes);
                if (!result.Succeeded() || !result.value) return result;
                const auto special = result.value->CaptureWaterSpecialInputs(reader, after, doodads, validator);
                if (!special.Succeeded()) return {{}, special.diagnostics};
                return result;
            },
            [&, reader](const auto& before, const auto& after, const auto* doodads, auto maximumBytes) {
                return MapData::TerrainPathingResources::CaptureInputs(reader, before, after,
                    gameRoot, maximumBytes, true, &validator, doodads);
            },
            [reader](const auto& owner, auto maximumBytes) { return owner.VerifyReadbackMembers(reader, maximumBytes, true); }
        };
    };
    auto historicalResources = resourcesFor(historicalReader);
    auto physicalResources = resourcesFor(physicalReader);
    std::vector<MapArchiveMember> oldMembers;
    for (std::size_t i = 2; i < history.baselineMembers.size(); ++i) {
        const auto& member = history.baselineMembers[i];
        if (member.present) oldMembers.push_back({member.name, member.bytes});
    }
    const auto oldDocument = MapDocument::FromMembers(oldMembers);
    oldMembers.clear();
    const auto savedIndex = static_cast<std::size_t>(history.savedIndex);
    const auto currentIndex = static_cast<std::size_t>(history.currentIndex);
    TerrainStrokeJournal suffix;
    suffix.sourceSha256 = journal.sourceSha256; suffix.members = journal.members;
    suffix.baselinePathingLayers = journal.baselinePathingLayers;
    auto& prefix = *baselineJournal.value;
    suffix.strokes.assign(std::make_move_iterator(prefix.strokes.begin() + savedIndex),
        std::make_move_iterator(prefix.strokes.end()));
    prefix.strokes.resize(savedIndex);
    TerrainReplayDetail::Accounting accounting;
    auto candidate = TerrainReplayDetail::CreateReplay(historicalResources, oldDocument,
        history.baselineMembers[0].bytes, history.baselineMembers[1].bytes,
        std::span(preparation.value->intents).first(savedIndex), gameRoot, remaining, prefix.sourceSha256,
        historicalReader, &prefix, &accounting);
    if (!candidate.Succeeded() || !candidate.value) return HistoryFailure(std::move(candidate.diagnostics));
    const auto prefixOwnerCount = candidate.value->waterOwners.size();
    std::shared_ptr<const MapData::TerrainPathingResources> stationResources;
    std::size_t epochRetainedBytes = 0;

    // 保存站必须通过同一真实SavePlan。只把完整typed重放的当前成员当作G1，
    // G0固定为历史结构；贴图、模型和规则均保持当前真实map/game winner。
    {
        auto& edits = *candidate.value->session;
        auto replacements = edits.Replacements();
        const auto removals = edits.Removals();
        auto finalOverlay = FinalMemberOverlay::Prepare(replacements, removals,
            std::min(MapData::TerrainSavePlanLimits{}.maximumBytes, accounting.limits.maximumCandidateBytes - accounting.historyBytes));
        if (!finalOverlay.Succeeded() || !finalOverlay.value) return HistoryFailure(std::move(finalOverlay.diagnostics));
        MapData::TerrainSavePlanLimits planLimits;
        planLimits.maximumBytes = std::min({planLimits.maximumBytes,
            (accounting.limits.maximumWork - accounting.work) / 8,
            (accounting.limits.maximumCandidateBytes - accounting.historyBytes) / 4,
            accounting.limits.maximumResourceBytes - accounting.resourceBytes,
            accounting.limits.maximumResourceBytes - accounting.ownedResources});
        if (planLimits.maximumBytes <= finalOverlay.value->MetadataBytes())
            return Failure("TERRAIN_REPLAY_BUDGET", "实际保存站计划没有剩余预算。");
        planLimits.maximumBytes -= finalOverlay.value->MetadataBytes();
        const HistoryMemberReader finalReader = [&](auto requested, const auto& maximum) {
            return finalOverlay.value->Read(historicalReader, requested, maximum, nullptr);
        };
        const MapData::TerrainSavePlan::NativeResourceCapture capture = [&](const auto& before, const auto& after,
            const auto*, auto maximumBytes) {
            return MapData::TerrainPathingResources::CaptureNativePairWithObjects(historicalReader, finalReader,
                before, after, gameRoot, maximumBytes, validator);
        };
        auto plan = MapData::TerrainSavePlan::PrepareHistory(edits, prefix.sourceSha256,
            historicalReader, gameRoot, planLimits, capture);
        if (!plan.Succeeded() || !plan.value) return HistoryFailure(std::move(plan.diagnostics));
        if (!plan.value->SaveGate().Succeeded()) return HistoryFailure(plan.value->SaveGate().diagnostics);
        if (plan.value->OwnedCapacityBytes() > accounting.limits.maximumCandidateBytes - accounting.historyBytes
            || plan.value->cumulativeBytes_ > (accounting.limits.maximumWork - accounting.work) / 8)
            return Failure("TERRAIN_REPLAY_BUDGET", "实际保存站计划的保留容量或累计工作超过恢复预算。");
        accounting.work += plan.value->cumulativeBytes_ * 8;
        if (plan.value->pathingResources_) {
            const auto& owner = *plan.value->pathingResources_;
            if (owner.CaptureBudgetBytes() > accounting.limits.maximumResourceBytes - accounting.resourceBytes
                || owner.OwnedCapacityBytes() > accounting.limits.maximumResourceBytes - accounting.ownedResources)
                return Failure("TERRAIN_REPLAY_BUDGET", "保存站真实规则owner超过前后缀累计资源预算。");
            accounting.resourceBytes += owner.CaptureBudgetBytes();
            accounting.ownedResources += owner.OwnedCapacityBytes();
            // owner持有自己的来源身份与解析正文，不借用此block的overlay/span。
            // 保存站专用native依赖也须活到最后的实际winner复核，不能只复核笔刷owner。
            stationResources = plan.value->pathingResources_;
            const auto& originalOwner = owner.nativeSourceResources_ ? *owner.nativeSourceResources_ : owner;
            const auto checked = originalOwner.VerifyReadbackMembers(historicalReader, planLimits.maximumBytes, true);
            if (!checked.Succeeded()) return HistoryFailure(checked.diagnostics);
            if (owner.nativeSourceResources_) {
                const auto finalChecked = owner.VerifyReadbackMembers(finalReader, planLimits.maximumBytes, true);
                if (!finalChecked.Succeeded()) return HistoryFailure(finalChecked.diagnostics);
            }
        }
        const auto station = edits.Snapshot();
        auto stationTerrain = MapData::SerializeMapTerrain(*edits.Terrain());
        if (!stationTerrain.Succeeded() || !stationTerrain.value) return HistoryFailure(std::move(stationTerrain.diagnostics));
        const auto wpmOutput = std::find_if(plan.value->Replacements().begin(), plan.value->Replacements().end(),
            [](const auto& member) { return member.name == "war3map.wpm"; });
        const auto& stationPathing = wpmOutput == plan.value->Replacements().end() ? history.baselineMembers[1].bytes : wpmOutput->data;
        if (!TerrainReplayDetail::SourceMatches(station, *stationTerrain.value, stationPathing, *actual.value))
            return Failure("TERRAIN_HISTORY_STATION_MISMATCH", "真实typed重放与保存计划没有重现物理保存站的全部十成员及缺席状态。");
        const auto planBytes = plan.value->OwnedCapacityBytes();
        const auto leaseBudget = std::min({MapData::TerrainJournalMaximumBytes,
            accounting.limits.maximumCandidateBytes - accounting.historyBytes - planBytes,
            (accounting.limits.maximumWork - accounting.work) / 2});
        if (!leaseBudget)
            return Failure("TERRAIN_REPLAY_BUDGET", "保存站epoch冻结没有剩余克隆或扫描预算。");
        // Core在FreezeForSave前先核这个实际剩余帽，成功后再将epoch常驻量纳入
        // 同一后缀账本；不能只在取得大克隆之后才检查它的容量。
        auto lease = edits.PrepareTerrainHistoryForSave(leaseBudget);
        if (!lease.Succeeded() || !lease.value) return HistoryFailure(std::move(lease.diagnostics));
        if (lease.value->PreparationBytes() > leaseBudget || lease.value->RetainedBytes() > leaseBudget)
            return Failure("TERRAIN_REPLAY_BUDGET", "保存站epoch冻结计量超过事先收紧的恢复预算。");
        epochRetainedBytes = lease.value->RetainedBytes();
        accounting.historyBytes += epochRetainedBytes;
        accounting.work += lease.value->PreparationBytes() * 2;
        const auto committed = plan.value->CommitSaved(edits, original);
        if (!committed.Succeeded()) return HistoryFailure(committed.diagnostics);
        const auto stationLayers = edits.SerializePathingRestrictionDraft();
        if (!stationLayers.Succeeded() || !stationLayers.value) return HistoryFailure(stationLayers.diagnostics);
        if (*stationLayers.value != journal.baselinePathingLayers)
            return Failure("TERRAIN_HISTORY_STATION_MISMATCH", "实际保存站通行P和已保存限制层不等于工程物理基线，不能从raw赋权。");
        const auto installed = edits.CommitTerrainHistorySave(*lease.value);
        if (!installed.Succeeded()) return HistoryFailure(installed.diagnostics);
    }

    if (!suffix.strokes.empty()) {
        const auto savedTerrain = candidate.value->session->Terrain();
        auto replayed = TerrainReplayDetail::Replay(physicalResources, *candidate.value->session, *savedTerrain,
            pathing.size(), std::span(preparation.value->intents).subspan(savedIndex), gameRoot,
            accounting, sourceSha, &suffix, &candidate.value->receipts);
        if (!replayed.Succeeded() || !replayed.value) return HistoryFailure(std::move(replayed.diagnostics));
        for (auto& owner : *replayed.value) candidate.value->waterOwners.push_back(std::move(owner));
    }
    if (candidate.value->receipts.size() != journal.strokes.size())
        return Failure("TERRAIN_HISTORY_REPLAY_FAILED", "完整typed栈没有逐槽产生真实回执，未安装任何前缀。");
    if (currentIndex < candidate.value->receipts.size()) {
        // Replay逆退会发布新的inverse Command，不能把这些持久副本只算作临时网格。
        // 每条原回执至多逆退一次，原全栈Retained上界保守覆盖全部inverse；
        // epoch本身的冻结基线没有再克隆，另行剔除以免大旧基线重复收费。
        const auto inverseBytes = accounting.historyBytes - epochRetainedBytes;
        auto undoWork = accounting.limits.maximumWork - accounting.work;
        if (inverseBytes > accounting.limits.maximumCandidateBytes - accounting.historyBytes
            || !Charge(undoWork, inverseBytes, 2)
            || !Charge(undoWork, candidate.value->session->Terrain()->vertices.size(),
                (candidate.value->receipts.size() - currentIndex) * 4))
            return Failure("TERRAIN_REPLAY_BUDGET", "跨保存站逆退的真实历史副本或累计扫描超过恢复预算。");
        accounting.historyBytes += inverseBytes;
        accounting.work = accounting.limits.maximumWork - undoWork;
    }
    for (std::size_t i = candidate.value->receipts.size(); i > currentIndex; --i) {
        const auto reversed = candidate.value->session->Replay(candidate.value->receipts[i - 1], false);
        if (!reversed.Succeeded()) return HistoryFailure(reversed.diagnostics);
    }
    candidate.value->currentIndex = currentIndex;
    candidate.value->savedIndex = savedIndex;
    // 没有额外说明命令时，FinishDraft只是完整字节与P/savedS/currentS核对。
    // 有Redo尾时禁止新增说明命令截断它；不能回报一条实际已失效的Redo回执。
    if (currentIndex != candidate.value->receipts.size()) {
        if (!candidate.value->session->MatchesTerrainJournalDraft(expected))
            return Failure("TERRAIN_JOURNAL_OUTPUT_MISMATCH", "带Redo尾的历史当前状态不等于完整工程草稿。");
    } else {
        const auto finished = TerrainReplayDetail::FinishDraft(*candidate.value, expected, *preparation.value);
        if (!finished.Succeeded()) return HistoryFailure(finished.diagnostics);
    }
    for (std::size_t i = 0; i < candidate.value->waterOwners.size(); ++i) {
        const auto& resourceSource = i < prefixOwnerCount ? historicalResources : physicalResources;
        const auto checked = resourceSource.verify(candidate.value->waterOwners[i], MapData::TerrainSavePlanLimits{}.maximumBytes);
        if (!checked.Succeeded()) return HistoryFailure(checked.diagnostics);
    }
    if (stationResources) {
        const auto& originalOwner = stationResources->nativeSourceResources_ ? *stationResources->nativeSourceResources_ : *stationResources;
        const auto oldChecked = originalOwner.VerifyReadbackMembers(historicalReader, MapData::TerrainSavePlanLimits{}.maximumBytes, true);
        if (!oldChecked.Succeeded()) return HistoryFailure(oldChecked.diagnostics);
        if (stationResources->nativeSourceResources_) {
            // 完整十成员已证明保存站G1就是当前物理来源，故无需保留临时最终overlay。
            const auto savedChecked = stationResources->VerifyReadbackMembers(physicalReader, MapData::TerrainSavePlanLimits{}.maximumBytes, true);
            if (!savedChecked.Succeeded()) return HistoryFailure(savedChecked.diagnostics);
        }
    }
    auto finalActual = physicalReader(names, readLimits);
    if (!finalActual.Succeeded() || !finalActual.value) return HistoryFailure(std::move(finalActual.diagnostics));
    const auto finalManifest = MatchManifest(journal, sourceSha, *finalActual.value);
    if (!finalManifest.Succeeded()) return {{}, finalManifest.diagnostics};
    if (!TerrainReplayDetail::SourceMatches(original, terrain, pathing, *finalActual.value))
        return Failure("TERRAIN_HISTORY_SOURCE_MISMATCH", "恢复期间实际来源发生变化，未安装任何前缀。");
    return candidate;
}

} // namespace War3Map::Services
