#include "TerrainRealtimeInternal.h"
#include "War3MapCore/Services/TerrainEditService.h"

#include <algorithm>
#include <exception>

namespace War3Map::Services {
using Session = MapData::MapDataEditSession;

bool TerrainPathingSave::NeedsRealtimePathing(const Session& current, const PreparedTerrainEdit& prepared) noexcept {
    return current.NeedsRealtimePathing(prepared.Plan());
}

Result<std::shared_ptr<const MapArchiveSession>> TerrainPathingSave::CaptureRealtimeReadSource(const Session& frozen) {
    using Output = Result<std::shared_ptr<const MapArchiveSession>>;
    const auto& binding = frozen.realtimePathingBinding_;
    if (!binding || binding->directory) return Output::Success({});
    if (!binding->archive)
        return Output::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_SOURCE", "后台通行缺少实际归档来源。");
    auto source = binding->archive->FreezeTerrainReadSource();
    if (!source.Succeeded() || !source.value) return {{}, std::move(source.diagnostics)};
    return Output::Success(std::make_shared<const MapArchiveSession>(std::move(*source.value)));
}

Result<Session::PreparedTerrainEdit> TerrainPathingSave::CompleteRealtimeTerrainEdit(const Session& frozen,
    const Session::PreparedTerrainEdit& geometry, std::shared_ptr<const MapArchiveSession> readSource,
    std::size_t additionalOwnedBytes) try {
    using Output = Result<Session::PreparedTerrainEdit>;
    if (geometry.realtimePrepared_ || !frozen.NeedsRealtimePathing(geometry)) return Output::Success(geometry);
    const auto& binding = frozen.realtimePathingBinding_;
    if (!binding || !binding->maximumBytes || binding->maximumBytes > 64u * 1024u * 1024u
        || additionalOwnedBytes >= binding->maximumBytes)
        return Output::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_BUDGET", "后台通行的来源或追加预算无效。");
    if (binding->archive && !readSource) {
        auto captured = CaptureRealtimeReadSource(frozen);
        if (!captured.Succeeded() || !captured.value) return {{}, std::move(captured.diagnostics)};
        readSource = std::move(*captured.value);
    }
    if (readSource && (!binding->archive || !readSource->HasSourceCaptureLease()
        || !binding->archive->HasSameSourceIdentity(*readSource)))
        return Output::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_SOURCE", "后台只读来源不属于这份冻结地形的原文件身份。");
    std::size_t copied = 0;
    auto clone = frozen.FreezeForSave(binding->maximumBytes - additionalOwnedBytes, &copied);
    if (!clone.Succeeded() || !clone.value) return {{}, std::move(clone.diagnostics)};
    if (copied >= binding->maximumBytes - additionalOwnedBytes)
        return Output::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_BUDGET", "后台快照没有留下通行准备预算。");
    auto worker = std::const_pointer_cast<Session>(std::move(*clone.value));
    auto selected = std::make_shared<TerrainRealtimeBinding>(*binding);
    if (readSource) selected->archive = readSource;
    selected->maximumBytes -= additionalOwnedBytes + copied;
    InstallRealtimeBinding(*worker, std::move(selected));
    const auto verifier = std::make_shared<std::function<Status()>>();
    const auto builder = worker->realtimePathingBuilder_;
    worker->realtimePathingBuilder_ = [builder, verifier](const auto& current, const auto& plan) {
        auto result = builder(current, plan);
        if (result.Succeeded() && result.value) {
            *verifier = result.value->verifySources;
            // 此次提交只发生在未发布的私有worker副本；实际活动事务仍在发布前
            // 执行完整复核，避免把同一复核在私有重放里再做一次。
            result.value->verifySources = {};
        }
        return result;
    };
    auto budgetedGeometry = geometry;
    if (worker->pathing_ && worker->pathing_->size() >= 16)
        budgetedGeometry.realtimePathingBytes_ = (worker->pathing_->size() - 16) * 2;
    const auto applied = worker->ApplyPreparedTerrainEdit(budgetedGeometry);
    if (!applied.Succeeded()) return {{}, applied.diagnostics};
    const auto& command = worker->lastApplied_.command_;
    if (!command || !command->nativePathingTouched || !command->nativePathingAfter || !*verifier)
        return Output::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_PREPARATION", "后台没有完成整笔通行与地形候选。");
    auto result = std::move(budgetedGeometry);
    auto qualified = *command;
    // 私有重放不得消费活动会话的命令身份；实际提交仍由原CommitCommand分配。
    qualified.logicalIdentity = 0;
    qualified.sessionIdentity = 0;
    result.command_ = std::make_shared<const Session::Command>(std::move(qualified));
    result.realtimePrepared_ = true;
    result.realtimeVerifier_ = *verifier;
    result.realtimeReadSource_ = std::move(readSource);
    return Output::Success(std::move(result));
} catch (const std::exception&) {
    return Result<Session::PreparedTerrainEdit>::Failure("Terrain.Edit", "TERRAIN_REALTIME_PATHING_PREPARATION",
        "后台通行候选准备失败，活动地形未修改。");
}

Result<PreparedTerrainEdit> TerrainPathingSave::CompleteRealtimeTerrainEdit(const Session& frozen,
    const PreparedTerrainEdit& geometry, std::shared_ptr<const MapArchiveSession> readSource,
    std::size_t additionalOwnedBytes) {
    auto result = CompleteRealtimeTerrainEdit(frozen, geometry.Plan(), std::move(readSource), additionalOwnedBytes);
    if (!result.Succeeded() || !result.value) return {{}, std::move(result.diagnostics)};
    auto prepared = geometry;
    prepared.plan_ = std::move(*result.value);
    return Result<PreparedTerrainEdit>::Success(std::move(prepared));
}

} // namespace War3Map::Services
