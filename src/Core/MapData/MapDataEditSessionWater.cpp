#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainWaterEdit(
    const TerrainPathingResources& resources, const TerrainWaterBrush& brush,
    std::span<const TerrainStrokePoint> points, bool followDoodads, const TerrainWaterBrushLimits& limits) const {
    using Output = Result<PreparedTerrainEdit>;
    const auto budgetFailure = [] {
        return Output::Failure("MapData.Edit", "TERRAIN_WATER_SESSION_BUDGET",
            "水刷地形候选、撤销区块、通行性或摆放联动超出本笔预算，尚未修改会话。");
    };
    if (!terrain_ || !terrainOriginal_)
        return Output::Failure("MapData.Edit", "MAP_TERRAIN_UNAVAILABLE", "地形来源或当前草稿不可编辑。");
    if (terrainSourceRevision_ == std::numeric_limits<std::uint64_t>::max())
        return Output::Failure("MapData.Edit", "TERRAIN_SOURCE_REVISION_EXHAUSTED", "地形来源修订已用尽，请重新打开地图。");
    const TerrainWaterBrushLimits hard;
    if (!limits.maximumCandidateBytes || limits.maximumCandidateBytes > hard.maximumCandidateBytes
        || !limits.maximumWork || limits.maximumWork > hard.maximumWork
        || !limits.maximumPoints || limits.maximumPoints > hard.maximumPoints
        || !limits.maximumRecursionDepth || limits.maximumRecursionDepth > hard.maximumRecursionDepth)
        return Output::Failure("MapData.Edit", "TERRAIN_WATER_LIMITS", "水刷预算只能收紧硬上限，不能为零。");
    if (points.empty() || points.size() > limits.maximumPoints)
        return Output::Failure("MapData.Edit", "TERRAIN_WATER_POINTS", "水刷点数量不在显式预算内。");
    if (!resources.WaterBrushRules())
        return Output::Failure("MapData.Edit", "TERRAIN_WATER_RULE_OWNER", "水刷必须使用实际捕获的编辑规则，不能用保存规则或手工参数替代。");

    const auto doodadIndex = Index(MapDataKind::Doodads), unitIndex = Index(MapDataKind::Units);
    const auto available = [this](std::size_t index) {
        // Ready/Missing来源之后的显式增删同样属于已知草稿；损坏、歧义和只读来源不是缺失。
        return !preservedSources_[index]
            && (states_[index] == MapDataState::Missing || states_[index] == MapDataState::Ready);
    };
    if (!available(doodadIndex))
        return Output::Failure("MapData.Edit", "MAP_DOODADS_UNSAFE", "无法确认实际装饰物来源，不能以空表代替水刷输入。");
    if (followDoodads && !available(unitIndex))
        return Output::Failure("MapData.Edit", "MAP_UNITS_UNSAFE", "单位来源不可读，不能准备地面高度跟随。");

    // 在任何候选或DOO解析分配前，预留整图最坏净差的撤销区块、两份WPM和
    // 摆放解析/序列化/回读容量。经典DOO的最小嵌套记录占4字节，固定对象及
    // 嵌套容器的峰值用64倍wire上界收取；这会保守拒绝超大摆放表，不降低parser门限。
    // 水算法只获剩余预算，内部仍只有一份owned W3E副本。
    static_assert(sizeof(UnitPlacement) <= 8 * 91 && sizeof(DoodadPlacement) <= 8 * 42
        && sizeof(SpecialDoodadPlacement) <= 8 * 16 && sizeof(PlacementItemSet) <= 8 * 4
        && sizeof(PlacementItemChoice) <= 8 * 8 && sizeof(PlacementInventoryItem) <= 8 * 8
        && sizeof(PlacementAbility) <= 8 * 12, "Recalculate the bounded placement wire multiplier after layout growth");
    std::size_t reservedBytes = 0, reservedWork = 0;
    const auto reserve = [](std::size_t& used, std::size_t limit, std::size_t count, std::size_t element) {
        if (element && count > (limit - used) / element) return false;
        used += count * element;
        return true;
    };
    if (!reserve(reservedBytes, limits.maximumCandidateBytes, 1, sizeof(Command) + sizeof(PreparedTerrainEdit))
        || !reserve(reservedBytes, limits.maximumCandidateBytes, terrain_->vertices.size(), 14)
        || !reserve(reservedBytes, limits.maximumCandidateBytes,
            std::size_t((terrain_->header.width + 15) / 16) * ((terrain_->header.height + 15) / 16),
            2 * sizeof(TerrainChunk))
        || !reserve(reservedBytes, limits.maximumCandidateBytes,
            terrain_->header.groundTilesets.size() + terrain_->header.cliffTilesets.size(), 2 * (sizeof(std::string) + 16))
        || !reserve(reservedBytes, limits.maximumCandidateBytes, points.size(), sizeof(TerrainStrokePoint))
        || !reserve(reservedWork, limits.maximumWork, terrain_->vertices.size(), 6)) return budgetFailure();
    if (pathing_ && (!reserve(reservedBytes, limits.maximumCandidateBytes, pathing_->size(), 2)
        || !reserve(reservedWork, limits.maximumWork, pathing_->size(), 3))) return budgetFailure();
    for (const auto index : {doodadIndex, unitIndex, Index(MapDataKind::Settings)}) {
        if (index == unitIndex && !followDoodads) continue;
        if (current_[index] && (!reserve(reservedBytes, limits.maximumCandidateBytes, current_[index]->size(), 64)
            || !reserve(reservedWork, limits.maximumWork, current_[index]->size(), 4))) return budgetFailure();
    }
    if (reservedBytes == limits.maximumCandidateBytes || reservedWork == limits.maximumWork) return budgetFailure();
    // VerifyWaterBrushInput顺序释放两份canonical序列化缓冲，不与候选并存；
    // 仍须先按本笔上限限制其输入大小，不能借较大的资源捕获预算绕过本笔预算。
    for (const auto* grid : {terrainOriginal_.get(), terrain_.get()}) {
        std::size_t identityBytes = 37;
        if (identityBytes > limits.maximumCandidateBytes
            || !reserve(identityBytes, limits.maximumCandidateBytes, grid->vertices.size(), 7)
            || !reserve(identityBytes, limits.maximumCandidateBytes,
                grid->header.groundTilesets.size() + grid->header.cliffTilesets.size(), 4)) return budgetFailure();
    }

    try {
        const auto bound = resources.VerifyWaterBrushInput(*terrainOriginal_, *terrain_);
        if (!bound.Succeeded()) return {{}, bound.diagnostics};
        DoodadPlacements doodads;
        if (current_[doodadIndex]) {
            auto parsed = ParseDoodadPlacements(*current_[doodadIndex]);
            if (!parsed.Succeeded() || !parsed.value) return {{}, std::move(parsed.diagnostics)};
            doodads = std::move(*parsed.value);
        }
        // 本批尚无special资源形状捕获器，实际非空记录必须交算法明确拒绝，不能删掉或伪造空DOO。
        auto algorithmLimits = limits;
        algorithmLimits.maximumCandidateBytes -= reservedBytes;
        algorithmLimits.maximumWork -= reservedWork;
        auto candidate = PrepareTerrainWaterStroke(*terrain_, *resources.WaterBrushRules(), brush,
            points, doodads, {}, algorithmLimits);
        if (!candidate.Succeeded() || !candidate.value) return {{}, std::move(candidate.diagnostics)};
        auto next = std::make_shared<TerrainGridSnapshot>(std::move(candidate.value->terrain));
        auto prepared = PrepareTerrainCandidate(std::move(next), candidate.value->result.changed, points,
            followDoodads, true, true, true, &doodads);
        if (prepared.value) {
            prepared.value->waterBrush_ = brush;
            prepared.value->waterResult_ = candidate.value->result;
        }
        return prepared;
    } catch (const std::bad_alloc&) {
        return budgetFailure();
    } catch (const std::length_error&) {
        return budgetFailure();
    }
}

} // namespace War3Map::MapData
