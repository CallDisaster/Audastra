#include "War3MapCore/Services/TerrainPathingSave.h"

#include <exception>
#include <type_traits>

namespace War3Map::Services {
using Session = MapData::MapDataEditSession;

namespace {
template<class Source>
auto RealtimeFactory(std::shared_ptr<const Source> source, std::filesystem::path gameRoot,
    std::vector<MemberReplacement> finalMembers) {
    auto members = std::make_shared<const std::vector<MemberReplacement>>(std::move(finalMembers));
    return [source, gameRoot = std::move(gameRoot), members](const auto& current, const auto& limits) {
        const auto fail = [](const char* code, const char* message) {
            return Result<MapData::TerrainSavePlan>::Failure("Terrain.Edit", code, message);
        };
        if (!source) return fail("TERRAIN_REALTIME_PATHING_SOURCE", "实时通行缺少实际来源。");
        auto bounded = limits;
        if (members->capacity() > limits.maximumBytes / sizeof(MemberReplacement))
            return fail("TERRAIN_REALTIME_PATHING_BUDGET", "实时通行的覆盖元数据超过预算。");
        std::size_t retained = members->capacity() * sizeof(MemberReplacement);
        const auto reserve = [&](std::size_t bytes) {
            if (retained >= limits.maximumBytes || bytes > limits.maximumBytes - retained) return false;
            retained += bytes;
            return true;
        };
        for (const auto& member : *members)
            if (!reserve(member.name.capacity() + 1) || !reserve(member.data.capacity()))
                return fail("TERRAIN_REALTIME_PATHING_BUDGET", "实时通行的物编覆盖超过预算。");
        if constexpr (std::is_same_v<Source, WorkspaceSnapshot>) {
            if (source->files.capacity() > limits.maximumBytes / sizeof(WorkspaceFile)
                || !reserve(sizeof(WorkspaceSnapshot) + source->files.capacity() * sizeof(WorkspaceFile))
                || !reserve(source->root.native().capacity() * sizeof(std::filesystem::path::value_type))
                || !reserve(source->aggregateSha256.capacity()))
                return fail("TERRAIN_REALTIME_PATHING_BUDGET", "目录来源清单超过实时通行预算。");
            for (const auto& file : source->files)
                if (!reserve(file.relativePath.native().capacity() * sizeof(std::filesystem::path::value_type))
                    || !reserve(file.sha256.capacity()))
                    return fail("TERRAIN_REALTIME_PATHING_BUDGET", "目录来源名称超过实时通行预算。");
        }
        if (retained >= limits.maximumBytes)
            return fail("TERRAIN_REALTIME_PATHING_BUDGET", "覆盖没有留下通行重建预算。");
        bounded.maximumBytes -= retained;
        return TerrainPathingSave::PrepareForSave(*source, current, gameRoot, bounded, *members);
    };
}
} // namespace

void TerrainPathingSave::BindRealtimePathing(Session& edits,
    std::shared_ptr<const MapArchiveSession> source, const std::filesystem::path& gameRoot,
    std::size_t maximumBytes, std::vector<MemberReplacement> finalMembers) {
    BindRealtimeBuilder(edits,
        RealtimeFactory(source, gameRoot, std::move(finalMembers)), [source](const auto& plan, const auto& limits) {
            return plan.VerifySourceInputs(*source, limits);
        }, maximumBytes);
}

void TerrainPathingSave::BindRealtimePathing(Session& edits, const WorkspaceSnapshot& source,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes, std::vector<MemberReplacement> finalMembers) {
    auto owned = std::make_shared<const WorkspaceSnapshot>(source);
    BindRealtimeBuilder(edits,
        RealtimeFactory(owned, gameRoot, std::move(finalMembers)), [owned](const auto& plan, const auto& limits) {
            return plan.VerifySourceInputs(*owned, limits);
        }, maximumBytes);
}

void TerrainPathingSave::BindRealtimeBuilder(Session& edits, RealtimePlanFactory plan,
    RealtimeVerifier verify, std::size_t maximumBytes) {
    edits.realtimePathingBuilder_ = [plan = std::move(plan), verify = std::move(verify), maximumBytes]
        (const auto& current, const auto& prepared) {
        return PrepareRealtimePathing(current, prepared, plan, verify, maximumBytes);
    };
}

Result<std::shared_ptr<const std::vector<std::byte>>> TerrainPathingSave::CaptureRealtimeLayer(
    const Session& current, const RealtimePlanFactory& factory, const RealtimeVerifier& verify,
    std::size_t maximumBytes, std::size_t& retained, std::function<Status()>& verifySources) {
    using Layer = Result<std::shared_ptr<const std::vector<std::byte>>>;
    MapData::TerrainSavePlanLimits limits;
    limits.maximumBytes = maximumBytes - retained;
    auto plan = factory(current, limits);
    if (!plan.Succeeded() || !plan.value) return {{}, std::move(plan.diagnostics)};
    if (!plan.value->SaveGate().Succeeded()) return {{}, plan.value->SaveGate().diagnostics};
    const auto cells = current.pathing_->size() - 16;
    const auto* wire = &*current.pathing_;
    for (const auto& member : plan.value->Replacements())
        if (member.name == "war3map.wpm") wire = &member.data;
    const auto* native = plan.value->nativePathingProtected_.empty()
        ? current.pathingProtected_.get() : &plan.value->nativePathingProtected_;
    if (wire->size() != cells + 16 || !native || native->size() != cells)
        return Layer::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_SHAPE", "保存派生通行与当前网格不一致。");
    auto layer = std::make_shared<std::vector<std::byte>>(*native);
    // 保存保护层只含0x0e限制位；水深、荒芜和其余原生派生位来自最终WPM。
    for (std::size_t index = 0; index < cells; ++index)
        (*layer)[index] |= (*wire)[index + 16] & std::byte{0xe0};
    const auto owned = plan.value->OwnedCapacityBytes();
    if (owned > maximumBytes - retained)
        return Layer::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_BUDGET", "实时通行的保存规则驻留超过预算。");
    const auto checked = verify(*plan.value, limits);
    if (!checked.Succeeded()) return {{}, checked.diagnostics};
    auto owner = std::make_shared<const MapData::TerrainSavePlan>(std::move(*plan.value));
    auto previous = std::move(verifySources);
    verifySources = [owner, verify, limits, previous = std::move(previous)] {
        if (previous) {
            const auto checked = previous();
            if (!checked.Succeeded()) return checked;
        }
        return verify(*owner, limits);
    };
    retained += owned;
    return Layer::Success(std::move(layer));
}

Status TerrainPathingSave::RefreshRealtimePathing(Session& candidate, const WorkspaceSnapshot& source,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes) try {
    if (!maximumBytes || maximumBytes > 64u * 1024u * 1024u || !candidate.pathing_
        || candidate.pathing_->size() < 16 || candidate.pathing_->size() - 16 >= maximumBytes)
        return Status::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_BUDGET", "目录候选的实时通行需要完整WPM和有界剩余预算。");
    auto owned = std::make_shared<const WorkspaceSnapshot>(source);
    const auto factory = RealtimeFactory(owned, gameRoot, {});
    const RealtimeVerifier verify = [owned](const auto& plan, const auto& limits) {
        return plan.VerifySourceInputs(*owned, limits);
    };
    std::size_t retained = candidate.pathing_->size() - 16;
    if (candidate.realtimeNativePathing_) {
        if (candidate.realtimeNativePathing_->capacity() >= maximumBytes - retained)
            return Status::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_BUDGET", "已有实时层没有留下刷新预算。");
        retained += candidate.realtimeNativePathing_->capacity();
    }
    std::function<Status()> verifySources;
    auto layer = CaptureRealtimeLayer(candidate, factory, verify, maximumBytes, retained, verifySources);
    if (!layer.Succeeded() || !layer.value) return {std::move(layer.diagnostics)};
    const auto checked = verifySources();
    if (!checked.Succeeded()) return checked;
    candidate.realtimeNativePathing_ = std::move(*layer.value);
    candidate.realtimePathingUnavailable_ = false;
    return Status::Ok();
} catch (const std::exception&) {
    return Status::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_PREPARATION", "目录候选的实时通行准备失败，未发布候选。");
}

Result<Session::RealtimePathingResult> TerrainPathingSave::PrepareRealtimePathing(
    const Session& edits, const Session::PreparedTerrainEdit& prepared,
    const RealtimePlanFactory& factory, const RealtimeVerifier& verify, std::size_t maximumBytes) try {
    using Output = Result<Session::RealtimePathingResult>;
    const auto failure = [](const char* code, const char* message) {
        return Output::Failure("Terrain.Edit", code, message);
    };
    if (!edits.pathing_ || !edits.pathingOriginal_ || edits.pathing_->size() < 16)
        return failure("TERRAIN_REALTIME_PATHING_SOURCE", "实时通行需要唯一的原生WPM来源。");
    const auto cells = edits.pathing_->size() - 16;
    if (!maximumBytes || maximumBytes > 64u * 1024u * 1024u || cells > maximumBytes / 2)
        return failure("TERRAIN_REALTIME_PATHING_BUDGET", "实时通行前后态超过本次预算。");
    std::size_t retained = cells * 2;
    Session::RealtimePathingResult result;
    const auto capture = [&](const Session& current) -> Result<std::shared_ptr<const std::vector<std::byte>>> {
        return CaptureRealtimeLayer(current, factory, verify, maximumBytes, retained, result.verifySources);
    };
    if ((!edits.realtimeNativePathing_ && MapData::TerrainSavePlan::HasTerrainChanges(edits))
        || edits.realtimePathingUnavailable_) {
        auto before = capture(edits);
        if (!before.Succeeded() || !before.value) return {{}, std::move(before.diagnostics)};
        result.beforeNativeLayer = std::move(*before.value);
    }
    std::size_t copied = 0;
    auto frozen = edits.FreezeForSave(maximumBytes - retained, &copied);
    if (!frozen.Succeeded() || !frozen.value) return {{}, std::move(frozen.diagnostics)};
    retained += copied;
    // FreezeForSave实际分配的是非const对象。这个私有候选未发布，只有本作用域持有；
    // 关闭回调后重放同一密封命令，保存分支与真实提交完全相同，不递归准备派生层。
    auto shadow = std::const_pointer_cast<Session>(std::move(*frozen.value));
    shadow->realtimePathingBuilder_ = {};
    auto applied = shadow->ApplyPreparedTerrainEdit(prepared);
    if (!applied.Succeeded()) return {{}, std::move(applied.diagnostics)};
    auto after = capture(*shadow);
    if (!after.Succeeded() || !after.value) return {{}, std::move(after.diagnostics)};
    result.nativeLayer = std::move(*after.value);
    return Output::Success(std::move(result));
} catch (const std::exception&) {
    return Result<Session::RealtimePathingResult>::Failure("Terrain.Edit",
        "TERRAIN_REALTIME_PATHING_PREPARATION", "实时通行准备失败，活动地形未修改。");
}

} // namespace War3Map::Services
