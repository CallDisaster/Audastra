#include "War3MapCore/Services/TerrainPathingSave.h"

#include <exception>

namespace War3Map::Services {
using Session = MapData::MapDataEditSession;

void TerrainPathingSave::BindRealtimePathing(Session& edits,
    std::shared_ptr<const MapArchiveSession> source, const std::filesystem::path& gameRoot,
    std::size_t maximumBytes) {
    BindRealtimeBuilder(edits,
        [source, gameRoot](const auto& current, const auto& limits) {
            if (!source) return Result<MapData::TerrainSavePlan>::Failure("Terrain.Edit",
                "TERRAIN_REALTIME_PATHING_SOURCE", "实时通行缺少实际归档来源。");
            return PrepareForSave(*source, current, gameRoot, limits);
        }, [source](const auto& plan, const auto& limits) {
            return plan.VerifySourceInputs(*source, limits);
        }, maximumBytes);
}

void TerrainPathingSave::BindRealtimePathing(Session& edits, const WorkspaceSnapshot& source,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes) {
    auto owned = std::make_shared<const WorkspaceSnapshot>(source);
    BindRealtimeBuilder(edits,
        [owned, gameRoot](const auto& current, const auto& limits) {
            return PrepareForSave(*owned, current, gameRoot, limits);
        }, [owned](const auto& plan, const auto& limits) {
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
        using Layer = Result<std::shared_ptr<const std::vector<std::byte>>>;
        MapData::TerrainSavePlanLimits limits;
        limits.maximumBytes = maximumBytes - retained;
        auto plan = factory(current, limits);
        if (!plan.Succeeded() || !plan.value) return {{}, std::move(plan.diagnostics)};
        if (!plan.value->SaveGate().Succeeded()) return {{}, plan.value->SaveGate().diagnostics};
        const auto* wire = &*current.pathing_;
        for (const auto& member : plan.value->Replacements())
            if (member.name == "war3map.wpm") wire = &member.data;
        const auto& native = plan.value->nativePathingProtected_.empty()
            ? current.pathingProtected_ : std::make_shared<const std::vector<std::byte>>(plan.value->nativePathingProtected_);
        if (wire->size() != cells + 16 || !native || native->size() != cells)
            return Layer::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_SHAPE", "保存派生通行与当前网格不一致。");
        auto layer = std::make_shared<std::vector<std::byte>>(*native);
        for (std::size_t index = 0; index < cells; ++index)
            (*layer)[index] |= (*wire)[index + 16] & std::byte{0x20};
        const auto owned = plan.value->OwnedCapacityBytes();
        if (owned > maximumBytes - retained)
            return Layer::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_BUDGET", "实时通行的保存规则驻留超过预算。");
        const auto checked = verify(*plan.value, limits);
        if (!checked.Succeeded()) return {{}, checked.diagnostics};
        auto owner = std::make_shared<const MapData::TerrainSavePlan>(std::move(*plan.value));
        auto previous = std::move(result.verifySources);
        result.verifySources = [owner, verify, limits, previous = std::move(previous)] {
            if (previous) {
                const auto checked = previous();
                if (!checked.Succeeded()) return checked;
            }
            return verify(*owner, limits);
        };
        retained += owned;
        return Layer::Success(std::move(layer));
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
