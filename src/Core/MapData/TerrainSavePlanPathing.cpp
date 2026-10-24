#include "War3MapCore/MapData/TerrainSavePlan.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/TerrainNativeUnitsProof.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <new>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.SavePlan";
Status Reject(const char* code, std::string message) {
    return Status::Failure(kSubsystem, code, std::move(message));
}
bool SameSha(std::span<const std::byte> bytes, const std::optional<std::string>& expected) {
    const auto actual = Sha256(bytes);
    return expected && actual.Succeeded() && actual.value && *actual.value == *expected;
}
}

Result<std::vector<std::byte>> TerrainSavePlan::RebuildPathing(const MapDataEditSession& edits,
    const MapArchiveSession* source, const WorkspaceSnapshot* workspace, const std::filesystem::path& gameRoot,
    const TerrainSavePlanLimits& limits, std::size_t& total, const NativeResourceCapture* nativeCapture) {
    using Output = Result<std::vector<std::byte>>;
    std::optional<TerrainNativeUnitsProof> unitsProof;
    const auto& sourceUnits = edits.original_[MapDataEditSession::Index(MapDataKind::Units)];
    const auto& currentUnits = edits.current_[MapDataEditSession::Index(MapDataKind::Units)];
    if (sourceUnits != currentUnits) {
        if (!sourceUnits || !currentUnits)
            return Output::Failure(kSubsystem, "TERRAIN_NATIVE_UNITS_EXISTENCE", "地形组合保存不能新增或删除整个Units成员；已存在成员内的实例可编辑。");
        if (total > limits.maximumBytes)
            return Output::Failure(kSubsystem, "TERRAIN_SAVE_PLAN_BUDGET", "地形计划没有剩余Units核对预算。");
        auto required = TerrainNativeUnitsProof::RequiredWorkingBytes(
            sourceUnits->size(), currentUnits->size(), limits.maximumBytes - total);
        if (!required.Succeeded() || !required.value) return {{}, std::move(required.diagnostics)};
        total += *required.value; // 在parser、writer及其歧义试读分配前预收。
        auto proof = TerrainNativeUnitsProof::Prepare(*sourceUnits, *currentUnits, sourceSha256_,
            edits.SessionIdentity(), edits.Revision(), edits.TerrainSourceRevision(), *required.value);
        if (!proof.Succeeded() || !proof.value) return {{}, std::move(proof.diagnostics)};
        unitsProof = std::move(*proof.value);
    }
    const auto shape = edits.VerifyTerrainCandidateForSave(edits.terrain_, edits.pathing_,
        edits.current_[MapDataEditSession::Index(MapDataKind::Doodads)], nullptr, nullptr, nullptr, true,
        unitsProof ? &*unitsProof : nullptr);
    if (!shape.Succeeded()) return {{}, shape.diagnostics};
    if ((!source && !workspace) || total > limits.maximumBytes)
        return Output::Failure(kSubsystem, "TERRAIN_PATHING_REBUILD_SOURCE", "通行重建缺少唯一来源或剩余预算。");
    const auto& currentDoodads = edits.current_[MapDataEditSession::Index(MapDataKind::Doodads)];
    auto captured = nativeCapture ? (*nativeCapture)(*edits.terrainOriginal_, *edits.terrain_,
        currentDoodads ? &*currentDoodads : nullptr, limits.maximumBytes - total) : source
        ? TerrainPathingResources::CaptureNative(*source, *edits.terrainOriginal_, *edits.terrain_, gameRoot, limits.maximumBytes - total)
        : TerrainPathingResources::CaptureNative(*workspace, *edits.terrainOriginal_, *edits.terrain_, gameRoot, limits.maximumBytes - total);
    if (!captured.Succeeded() || !captured.value) return {{}, std::move(captured.diagnostics)};
    // 原版G0/G1仍消费同一范围规则；仅四个说明字段可在全文核对后形成不同输出。
    // 资源owner保留旧来源身份，并另外冻结该次完整W3I供写前/写后精确核对。
    const auto settings = captured.value->BindNativeSettingsOutput(edits);
    if (!settings.Succeeded()) return {{}, settings.diagnostics};
    const auto reserve = [&](std::size_t bytes) {
        if (bytes > limits.maximumBytes - total) return false;
        total += bytes;
        return true;
    };
    if (!reserve(captured.value->CaptureBudgetBytes()))
        return Output::Failure(kSubsystem, "TERRAIN_SAVE_PLAN_BUDGET", "完整通行规则超过保存计划预算。");
    const auto& oldWpm = *edits.pathingOriginal_;
    const auto& savedIntent = *edits.originalPathingRestrictions_;
    const auto& intent = *edits.pathingRestrictions_;
    const auto cells = oldWpm.size() - 16;
    // 先完整验证原图；不通过 source & ~S 反推原版贡献，S可以与地形贡献重叠。
    {
        auto original = captured.value->GenerateNative(*edits.terrainOriginal_, TerrainNativeStage::Before, limits.maximumBytes - total);
        if (!original.Succeeded() || !original.value) return {{}, std::move(original.diagnostics)};
        if (original.value->size() != cells || savedIntent.size() != cells || intent.size() != cells)
            return Output::Failure(kSubsystem, "TERRAIN_PATHING_REBUILD_SHAPE", "原版通行重算与来源WPM或限制意图大小不一致。");
        if (!reserve(original.value->size()))
            return Output::Failure(kSubsystem, "TERRAIN_SAVE_PLAN_BUDGET", "旧通行重算超过保存计划预算。");
        const auto width = (edits.terrainOriginal_->header.width - 1) * 4;
        for (std::size_t i = 0; i < cells; ++i) {
            const auto expected = (*original.value)[i] | savedIntent[i];
            if ((oldWpm[i + 16] & std::byte{0xee}) != expected)
                return Output::Failure(kSubsystem, "TERRAIN_PATHING_SOURCE_UNEXPLAINED",
                    "源通行格(" + std::to_string(i % width) + "," + std::to_string(i / width)
                    + ")无法由当前原版规则及已保存限制解释，差异位="
                    + std::to_string(std::to_integer<unsigned>((oldWpm[i + 16] & std::byte{0xee}) ^ expected))
                    + "；保留原图，请核对外部通行修改或尚未支持的对象规则。");
        }
    }
    auto generated = captured.value->GenerateNative(*edits.terrain_, TerrainNativeStage::After, limits.maximumBytes - total);
    if (!generated.Succeeded() || !generated.value) return {{}, std::move(generated.diagnostics)};
    if (generated.value->size() != cells)
        return Output::Failure(kSubsystem, "TERRAIN_PATHING_REBUILD_SHAPE", "新通行重算与来源网格大小不一致。");
    if (!reserve(generated.value->size()) || !reserve(cells) || !reserve(oldWpm.size()))
        return Output::Failure(kSubsystem, "TERRAIN_SAVE_PLAN_BUDGET", "通行输出与保存后基线超过保存计划预算。");
    std::vector<std::byte> output(oldWpm);
    std::vector<std::byte> protectedSource(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        output[i + 16] = (oldWpm[i + 16] & std::byte{0x11}) | (*generated.value)[i] | intent[i];
        protectedSource[i] = (*generated.value)[i] & std::byte{kPathingRestrictionBits};
    }
    auto draftSha = Sha256(*edits.pathing_);
    auto intentSha = Sha256(intent);
    if (!draftSha.Succeeded() || !draftSha.value) return {{}, std::move(draftSha.diagnostics)};
    if (!intentSha.Succeeded() || !intentSha.value) return {{}, std::move(intentSha.diagnostics)};
    pathingDraftSha256_ = std::move(*draftSha.value);
    pathingIntentSha256_ = std::move(*intentSha.value);
    nativePathingProtected_ = std::move(protectedSource);
    pathingResources_ = std::make_shared<const TerrainPathingResources>(std::move(*captured.value));
    return Output::Success(std::move(output));
}

Status TerrainSavePlan::InstallPathingOutput(const MapDataEditSession& edits,
    std::vector<MemberReplacement>& replacements) const {
    if (!RebuiltPathing()) return Status::Ok();
    if (!saveGate_.Succeeded()) return saveGate_;
    if (sessionIdentity_ != edits.SessionIdentity() || revision_ != edits.Revision()
        || terrainSourceRevision_ != edits.TerrainSourceRevision() || !edits.pathing_
        || !SameSha(*edits.pathing_, pathingDraftSha256_))
        return Reject("TERRAIN_SAVE_PLAN_STALE", "安装通行输出时原会话、修订或草稿已改变。");
    const auto output = std::find_if(replacements_.begin(), replacements_.end(),
        [](const auto& value) { return value.name == "war3map.wpm"; });
    if (output == replacements_.end())
        return Reject("TERRAIN_PATHING_REBUILD_OUTPUT", "通行重建计划没有实际输出。");
    auto existing = replacements.end();
    for (auto it = replacements.begin(); it != replacements.end(); ++it) {
        const auto key = Resources::CanonicalImportPath(it->name);
        if (!key.Succeeded() || !key.value) return {key.diagnostics};
        // CanonicalImportPath 统一为大写，不能漏掉已有草稿或大小写别名。
        if (*key.value != "WAR3MAP.WPM") continue;
        if (existing != replacements.end() || it->data != *edits.pathing_)
            return Reject("TERRAIN_PATHING_REBUILD_CONFLICT", "最终列表存在重复或非当前草稿的WPM，拒绝覆盖。");
        existing = it;
    }
    if (existing == replacements.end() && edits.pathing_ != edits.pathingOriginal_)
        return Reject("TERRAIN_PATHING_REBUILD_CONFLICT", "最终列表遗漏当前通行修改，不能以重建掩盖遗漏。");
    // 分配在发布前完成，失败时保留调用者原列表。
    try {
        MemberReplacement next = *output;
        if (existing == replacements.end()) replacements.push_back(std::move(next));
        else *existing = std::move(next);
    } catch (const std::bad_alloc&) {
        return Reject("TERRAIN_SAVE_PLAN_ALLOCATION", "准备通行替换失败，未改变保存列表。");
    }
    return Status::Ok();
}

Status TerrainSavePlan::CommitSaved(MapDataEditSession& edits, const MapDocument& saved) const {
    if (!saveGate_.Succeeded()) return saveGate_;
    if (!RebuiltPathing()) {
        try { edits.CommitSaved(saved); }
        catch (const std::bad_alloc&) {
            return Reject("TERRAIN_SAVE_PLAN_ALLOCATION", "准备保存基线失败，原编辑状态保留。");
        }
        return Status::Ok();
    }
    if (!edits.terrainOriginal_ || !edits.terrain_ || !edits.pathingOriginal_ || !edits.pathing_
        || !edits.pathingRestrictions_ || !SameSha(*edits.pathingOriginal_, members_[1].sourceSha256)
        || !SameSha(*edits.pathing_, pathingDraftSha256_) || !SameSha(*edits.pathingRestrictions_, pathingIntentSha256_))
        return Reject("TERRAIN_PATHING_COMMIT_STALE", "保存后的通行基线与前台来源、草稿或限制意图不一致。");
    const auto& doodads = edits.current_[MapDataEditSession::Index(MapDataKind::Doodads)];
    if (!doodads || doodads->size() != invariantMembers_[2].outputBytes
        || !SameSha(*doodads, invariantMembers_[2].outputSha256)
        || !saved.Doodads() || saved.Doodads()->OriginalBytes() != *doodads)
        return Reject("TERRAIN_PATHING_COMMIT_STALE", "保存后的DOO与冻结输出或前台完整特殊集合不一致，编辑基线保持不变。");
    const auto& units = edits.current_[MapDataEditSession::Index(MapDataKind::Units)];
    const auto& unitsOutput = invariantMembers_[1];
    if ((units.has_value() != unitsOutput.outputSha256.has_value())
        || (units && (units->size() != unitsOutput.outputBytes || !SameSha(*units, unitsOutput.outputSha256)
            || !saved.Units() || saved.Units()->OriginalBytes() != *units))
        || (!units && saved.Member(MapDataKind::Units).state != MapDataState::Missing))
        return Reject("TERRAIN_PATHING_COMMIT_STALE", "保存后的Units与冻结完整输出或前台成员不同，编辑基线保持不变。");
    const auto original = SerializeMapTerrain(*edits.terrainOriginal_);
    const auto current = SerializeMapTerrain(*edits.terrain_);
    if (!original.Succeeded() || !original.value) return {original.diagnostics};
    if (!current.Succeeded() || !current.value) return {current.diagnostics};
    if (!SameSha(*original.value, members_[0].sourceSha256) || !SameSha(*current.value, members_[0].outputSha256))
        return Reject("TERRAIN_PATHING_COMMIT_STALE", "保存后的通行基线与前台原始或当前地形不一致。");
    const auto output = std::find_if(replacements_.begin(), replacements_.end(),
        [](const auto& value) { return value.name == "war3map.wpm"; });
    if (output == replacements_.end() || !SameSha(output->data, members_[1].outputSha256))
        return Reject("TERRAIN_PATHING_REBUILD_OUTPUT", "保存后的通行计划缺少一致的实际输出。");
    try {
        auto protectedSource = std::make_shared<const std::vector<std::byte>>(nativePathingProtected_);
        std::optional<std::vector<std::byte>> pathing(output->data);
        edits.CommitSavedBaseline(saved, std::move(pathing), std::move(protectedSource));
    } catch (const std::bad_alloc&) {
        return Reject("TERRAIN_SAVE_PLAN_ALLOCATION", "准备新通行基线失败，原编辑状态保留。");
    }
    return Status::Ok();
}

} // namespace War3Map::MapData
