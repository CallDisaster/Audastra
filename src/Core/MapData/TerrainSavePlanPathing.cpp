#include "War3MapCore/MapData/TerrainSavePlan.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/TerrainNativeUnitsProof.h"
#include "War3MapCore/MapData/TerrainNativeDoodadsProof.h"
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
    std::optional<TerrainNativeDoodadsProof> doodadsProof;
    const auto& sourceDoodads = edits.original_[MapDataEditSession::Index(MapDataKind::Doodads)];
    const auto& currentDoodads = edits.current_[MapDataEditSession::Index(MapDataKind::Doodads)];
    if (sourceDoodads != currentDoodads && sourceDoodads && currentDoodads) {
        if (total > limits.maximumBytes)
            return Output::Failure(kSubsystem, "TERRAIN_SAVE_PLAN_BUDGET", "地形计划没有剩余普通DOO核对预算。");
        const auto required = TerrainNativeDoodadsProof::RequiredWorkingBytes(
            sourceDoodads->size(), currentDoodads->size(), limits.maximumBytes - total);
        if (!required.Succeeded() || !required.value) return {{}, required.diagnostics};
        total += *required.value;
        auto proof = TerrainNativeDoodadsProof::Prepare(edits, sourceSha256_, *required.value);
        if (!proof.Succeeded() || !proof.value) return {{}, std::move(proof.diagnostics)};
        doodadsProof = std::move(*proof.value);
    }
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
        unitsProof ? &*unitsProof : nullptr, doodadsProof ? &*doodadsProof : nullptr);
    if (!shape.Succeeded()) return {{}, shape.diagnostics};
    // 扩裁仍先消费同一套完整摆放证明；不能把typed形状证明当作任意DOO/Units授权。
    if (edits.HasTerrainResizeChanges()) return RebuildResizePathing(edits, limits, total, nativeCapture);
    if ((!source && !workspace && !nativeCapture) || total > limits.maximumBytes)
        return Output::Failure(kSubsystem, "TERRAIN_PATHING_REBUILD_SOURCE", "通行重建缺少唯一来源或剩余预算。");
    auto captured = nativeCapture ? (*nativeCapture)(*edits.terrainOriginal_, *edits.terrain_,
        currentDoodads ? &*currentDoodads : nullptr, limits.maximumBytes - total) : source
        ? TerrainPathingResources::CaptureNative(*source, *edits.terrainOriginal_, *edits.terrain_, gameRoot, limits.maximumBytes - total)
        : TerrainPathingResources::CaptureNative(*workspace, *edits.terrainOriginal_, *edits.terrain_, gameRoot, limits.maximumBytes - total);
    if (!captured.Succeeded() || !captured.value) return {{}, std::move(captured.diagnostics)};
    // G0始终核会话真实旧W3I和窄域字段/范围事务。双阶段G1另外来自最终成员，
    // 两者最终全文和可玩区必须相同，不能因切换owner而跳过原始字段门。
    auto& resource = *captured.value;
    auto& sourceResource = resource.nativeSourceResources_ ? *resource.nativeSourceResources_ : resource;
    const auto settings = sourceResource.BindNativeSettingsOutput(edits);
    if (!settings.Succeeded()) return {{}, settings.diagnostics};
    if (resource.nativeSourceResources_) {
        const auto finalSettings = std::find_if(resource.dependencies_.begin(), resource.dependencies_.end(),
            [](const auto& dependency) { return dependency.name == "war3map.w3i"; });
        const auto& expectedBounds = sourceResource.nativeAfterBounds_ ? sourceResource.nativeAfterBounds_ : sourceResource.nativeBounds_;
        if (finalSettings == resource.dependencies_.end() || !finalSettings->mapPresent
            || !sourceResource.nativeSettingsOutputSha_ || finalSettings->sha256 != *sourceResource.nativeSettingsOutputSha_
            || finalSettings->bytes != sourceResource.nativeSettingsOutputBytes_ || !expectedBounds || !resource.nativeBounds_
            || resource.nativeBounds_->left != expectedBounds->left || resource.nativeBounds_->bottom != expectedBounds->bottom
            || resource.nativeBounds_->right != expectedBounds->right || resource.nativeBounds_->top != expectedBounds->top)
            return Output::Failure(kSubsystem, "TERRAIN_PATHING_RESOURCE_CHANGED", "最终W3I全文或可玩区不等于来源绑定的完整会话事务。");
    }
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

Result<std::vector<std::byte>> TerrainSavePlan::RebuildResizePathing(const MapDataEditSession& edits,
    const TerrainSavePlanLimits& limits, std::size_t& total, const NativeResourceCapture* nativeCapture) {
    using Output = Result<std::vector<std::byte>>;
    const auto proof = edits.VerifyTerrainResizeForSave();
    if (!proof.Succeeded()) return {{}, proof.diagnostics};
    if (!nativeCapture || total >= limits.maximumBytes)
        return Output::Failure(kSubsystem, "TERRAIN_RESIZE_NATIVE_SOURCE", "扩裁保存需要可信Services分别捕获物理旧来源与完整最终成员。");
    const auto& doodads = edits.current_[MapDataEditSession::Index(MapDataKind::Doodads)];
    auto captured = (*nativeCapture)(*edits.terrainOriginal_, *edits.terrain_,
        doodads ? &*doodads : nullptr, limits.maximumBytes - total);
    if (!captured.Succeeded() || !captured.value) return {{}, std::move(captured.diagnostics)};
    auto& resources = *captured.value;
    if (!resources.nativeSourceResources_)
        return Output::Failure(kSubsystem, "TERRAIN_RESIZE_NATIVE_SOURCE", "扩裁保存缺少独立G0，不能使用最终覆盖结果解释物理旧WPM。");
    auto& originalResources = *resources.nativeSourceResources_;
    const auto settings = originalResources.BindNativeResizeSettingsOutput(edits);
    if (!settings.Succeeded()) return {{}, settings.diagnostics};
    const auto finalSettings = std::find_if(resources.dependencies_.begin(), resources.dependencies_.end(),
        [](const auto& dependency) { return dependency.name == "war3map.w3i"; });
    const auto& expectedBounds = originalResources.nativeAfterBounds_ ? originalResources.nativeAfterBounds_ : originalResources.nativeBounds_;
    if (finalSettings == resources.dependencies_.end() || !finalSettings->mapPresent
        || !originalResources.nativeSettingsOutputSha_ || finalSettings->sha256 != *originalResources.nativeSettingsOutputSha_
        || finalSettings->bytes != originalResources.nativeSettingsOutputBytes_ || !expectedBounds || !resources.nativeBounds_
        || resources.nativeBounds_->left != expectedBounds->left || resources.nativeBounds_->bottom != expectedBounds->bottom
        || resources.nativeBounds_->right != expectedBounds->right || resources.nativeBounds_->top != expectedBounds->top)
        return Output::Failure(kSubsystem, "TERRAIN_PATHING_RESOURCE_CHANGED", "扩裁最终W3I或可玩区不是typed会话完整输出。");
    const auto reserve = [&](std::size_t count) {
        if (total > limits.maximumBytes || count > limits.maximumBytes - total) return false;
        total += count; return true;
    };
    if (!reserve(resources.CaptureBudgetBytes()))
        return Output::Failure(kSubsystem, "TERRAIN_SAVE_PLAN_BUDGET", "扩裁G0/G1实际资源超过保存预算。");
    const auto& oldWpm = *edits.pathingOriginal_;
    const auto& savedIntent = *edits.originalPathingRestrictions_;
    const auto& currentWpm = *edits.pathing_;
    const auto& intent = *edits.pathingRestrictions_;
    const auto oldCells = oldWpm.size() - 16, newCells = currentWpm.size() - 16;
    {
        auto generated = resources.GenerateNative(*edits.terrainOriginal_, TerrainNativeStage::Before, limits.maximumBytes - total);
        if (!generated.Succeeded() || !generated.value) return {{}, std::move(generated.diagnostics)};
        if (generated.value->size() != oldCells || savedIntent.size() != oldCells)
            return Output::Failure(kSubsystem, "TERRAIN_PATHING_REBUILD_SHAPE", "扩裁G0与物理旧WPM或savedS不属于同一形状。");
        if (!reserve(generated.value->size()))
            return Output::Failure(kSubsystem, "TERRAIN_SAVE_PLAN_BUDGET", "扩裁旧通行核对超过保存预算。");
        const auto width = (edits.terrainOriginal_->header.width - 1) * 4;
        for (std::size_t i = 0; i < oldCells; ++i) {
            if ((oldWpm[i + 16] & std::byte{0xee}) != ((*generated.value)[i] | savedIntent[i]))
                return Output::Failure(kSubsystem, "TERRAIN_PATHING_SOURCE_UNEXPLAINED",
                    "扩裁物理源通行格(" + std::to_string(i % width) + "," + std::to_string(i / width)
                    + ")不能由真实旧规则及保存意图重现；保留源图。");
        }
    }
    auto generated = resources.GenerateNative(*edits.terrain_, TerrainNativeStage::After, limits.maximumBytes - total);
    if (!generated.Succeeded() || !generated.value) return {{}, std::move(generated.diagnostics)};
    if (generated.value->size() != newCells || intent.size() != newCells)
        return Output::Failure(kSubsystem, "TERRAIN_PATHING_REBUILD_SHAPE", "扩裁G1与新WPM或当前意图不属于同一形状。");
    if (!reserve(generated.value->size()) || !reserve(newCells) || !reserve(currentWpm.size()))
        return Output::Failure(kSubsystem, "TERRAIN_SAVE_PLAN_BUDGET", "扩裁新通行输出及保存基线超过预算。");
    // typed命令已按世界格迁移header、0x11、P、savedS和S；先核完整分层后才消费。
    // 禁止按当前数组索引读取不同shape的物理旧WPM。
    std::vector<std::byte> output(currentWpm), protectedSource(newCells);
    for (std::size_t i = 0; i < newCells; ++i) {
        output[i + 16] = (currentWpm[i + 16] & std::byte{0x11}) | (*generated.value)[i] | intent[i];
        protectedSource[i] = (*generated.value)[i] & std::byte{kPathingRestrictionBits};
    }
    auto draftSha = Sha256(currentWpm), intentSha = Sha256(intent);
    if (!draftSha.Succeeded() || !draftSha.value) return {{}, std::move(draftSha.diagnostics)};
    if (!intentSha.Succeeded() || !intentSha.value) return {{}, std::move(intentSha.diagnostics)};
    pathingDraftSha256_ = std::move(*draftSha.value); pathingIntentSha256_ = std::move(*intentSha.value);
    nativePathingProtected_ = std::move(protectedSource);
    pathingResources_ = std::make_shared<const TerrainPathingResources>(std::move(resources));
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
    const MemberReplacement* shadowOutput = nullptr;
    std::size_t shadowIndex = replacements.size();
    if (edits.HasTerrainResizeChanges()) {
        const auto shadow = std::find_if(replacements_.begin(), replacements_.end(),
            [](const auto& value) { return value.name == "war3map.shd"; });
        if (shadow != replacements_.end()) {
            shadowOutput = &*shadow;
            const auto* draft = edits.TerrainHistoryShadow();
            if (!draft || !draft->present)
                return Reject("TERRAIN_RESIZE_SHADOW_SHAPE", "扩裁阴影输出缺少同一typed草稿。");
            for (std::size_t i = 0; i < replacements.size(); ++i) {
                const auto key = Resources::CanonicalImportPath(replacements[i].name);
                if (!key.Succeeded() || !key.value) return {key.diagnostics};
                if (*key.value != "WAR3MAP.SHD") continue;
                if (shadowIndex != replacements.size() || replacements[i].data != draft->bytes)
                    return Reject("TERRAIN_RESIZE_SHADOW_CONFLICT", "最终列表含重复或非typed迁移的SHD，未安装任何派生输出。");
                shadowIndex = i;
            }
        }
    }
    const auto pathingIndex = existing == replacements.end() ? replacements.size()
        : static_cast<std::size_t>(existing - replacements.begin());
    const auto originalSize = replacements.size();
    // 所有正文复制和vector扩容在赋值前完成；其后仅noexcept move，WPM/SHD一起发布。
    try {
        MemberReplacement next = *output;
        std::optional<MemberReplacement> shadow;
        if (shadowOutput) shadow = *shadowOutput;
        replacements.reserve(originalSize + (pathingIndex == originalSize ? 1 : 0)
            + (shadow && shadowIndex == originalSize ? 1 : 0));
        if (pathingIndex == originalSize) replacements.push_back(std::move(next));
        else replacements[pathingIndex] = std::move(next);
        if (shadow) {
            if (shadowIndex == originalSize) replacements.push_back(std::move(*shadow));
            else replacements[shadowIndex] = std::move(*shadow);
        }
    } catch (const std::bad_alloc&) {
        return Reject("TERRAIN_SAVE_PLAN_ALLOCATION", "准备通行替换失败，未改变保存列表。");
    }
    return Status::Ok();
}

Status TerrainSavePlan::CommitSaved(MapDataEditSession& edits, const MapDocument& saved) const {
    if (!saveGate_.Succeeded()) return saveGate_;
    const auto commitBaseline = [&](std::optional<std::vector<std::byte>> pathing,
        std::shared_ptr<const std::vector<std::byte>> protectedPathing) -> Status {
        std::optional<ArchiveExactMember> shadow;
        if (edits.resizeState_) {
            const auto written = std::find_if(replacements_.begin(), replacements_.end(),
                [](const auto& value) { return value.name == "war3map.shd"; });
            if (written != replacements_.end()) {
                if (sessionIdentity_ != edits.SessionIdentity() || revision_ != edits.Revision()
                    || terrainSourceRevision_ != edits.TerrainSourceRevision())
                    return Reject("TERRAIN_PATHING_COMMIT_STALE", "提交实际写出阴影时原会话、修订或地形来源已改变。");
                shadow = ArchiveExactMember{"war3map.shd", true, written->data};
            } else if (const auto* currentShadow = edits.TerrainHistoryShadow()) shadow = *currentShadow;
            else return Reject("TERRAIN_RESIZE_SHADOW_SOURCE", "提交扩裁基线前缺少真实写出阴影或明确absence。");
        }
        // 显式Bake也可能发生在WPM没有净变化的重复保存里。两条提交路径
        // 都安装同一计划已核过的SHD，不能用是否重建WPM决定阴影基线。
        edits.CommitSavedBaseline(saved, std::move(pathing), std::move(protectedPathing), std::move(shadow));
        return Status::Ok();
    };
    if (!RebuiltPathing()) {
        try { return commitBaseline(edits.pathing_, edits.pathingProtected_); }
        catch (const std::bad_alloc&) {
            return Reject("TERRAIN_SAVE_PLAN_ALLOCATION", "准备保存基线失败，原编辑状态保留。");
        }
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
        return commitBaseline(std::move(pathing), std::move(protectedSource));
    } catch (const std::bad_alloc&) {
        return Reject("TERRAIN_SAVE_PLAN_ALLOCATION", "准备新通行基线失败，原编辑状态保留。");
    }
}

} // namespace War3Map::MapData
