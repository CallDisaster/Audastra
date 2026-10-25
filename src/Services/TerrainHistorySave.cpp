#include "War3MapCore/Services/TerrainHistorySave.h"

#include "War3MapCore/MapData/TerrainPathingResources.h"

#include <array>
#include <new>
#include <stdexcept>

namespace War3Map::Services {

bool TerrainHistorySave::NeedsSourceShadowBinding(const Lease& lease) noexcept {
    return lease.NeedsSourceShadowBinding();
}

Result<TerrainHistorySave::Lease> TerrainHistorySave::BindSourceShadow(const MemberReader& reader,
    const Lease& lease, std::size_t maximumBytes) try {
    const auto budget = lease.SourceShadowReadBudget(maximumBytes);
    if (!budget.Succeeded() || !budget.value) return {{}, budget.diagnostics};
    // 已有最早epoch的真实绑定时原样继承；不能读较新的物理来源覆盖它。
    if (!*budget.value) return Result<Lease>::Success(lease);
    const std::array<std::string, 2> names{"war3map.w3e", "war3map.shd"};
    ArchiveExactMemberLimits limits;
    limits.memberCount = names.size(); limits.memberBytes = limits.totalBytes = *budget.value;
    auto members = reader(names, limits);
    if (!members.Succeeded() || !members.value) return {{}, std::move(members.diagnostics)};
    return lease.BindSourceShadow(std::move(*members.value), maximumBytes);
} catch (const std::bad_alloc&) {
    return Result<Lease>::Failure("Terrain.History", "TERRAIN_HISTORY_BUDGET", "保存前旧SHD捕获分配失败，未改变活动地图或历史。");
} catch (const std::length_error&) {
    return Result<Lease>::Failure("Terrain.History", "TERRAIN_HISTORY_BUDGET", "保存前旧SHD捕获大小超过预算，未改变活动地图或历史。");
}

Result<TerrainHistorySave::Lease> TerrainHistorySave::BindSourceShadow(const MapArchiveSession& source,
    const Lease& lease, std::size_t maximumBytes) {
    return BindSourceShadow([&](auto names, const auto& limits) {
        return source.CaptureExactMembers(names, limits);
    }, lease, maximumBytes);
}

Result<TerrainHistorySave::Lease> TerrainHistorySave::BindSourceShadow(const WorkspaceSnapshot& source,
    const Lease& lease, std::size_t maximumBytes) {
    return BindSourceShadow([&](auto names, const auto& limits) {
        return MapData::TerrainPathingResources::ReadDirectoryMembers(source, names, limits, true);
    }, lease, maximumBytes);
}

} // namespace War3Map::Services
