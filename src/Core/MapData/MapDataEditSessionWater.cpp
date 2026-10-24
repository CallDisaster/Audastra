#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/TerrainCliffEditing.h"
#include "War3MapCore/MapData/TerrainRampEditing.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainWaterEdit(
    const TerrainPathingResources& resources, const TerrainWaterBrush& brush,
    std::span<const TerrainStrokePoint> points, bool followDoodads, const TerrainWaterBrushLimits& limits) const {
    return PrepareTerrainStructuralEdit(resources, &brush, nullptr, points, followDoodads, limits);
}

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainCliffEdit(
    const TerrainPathingResources& resources, const TerrainBrush& brush,
    std::span<const TerrainStrokePoint> points, bool followDoodads, const TerrainWaterBrushLimits& limits) const {
    if (brush.kind != TerrainBrushKind::CliffLevel || brush.levelDelta < -1 || brush.levelDelta > 1
        || (!brush.size && brush.radius > 7) || (brush.size && (*brush.size < 1 || *brush.size > 8)))
        return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "TERRAIN_CLIFF_BRUSH",
            "原版悬崖入口需要CliffLevel、-1/0/+1层级变化及经典1到8尺寸。");
    return PrepareTerrainStructuralEdit(resources, nullptr, &brush, points, followDoodads, limits);
}

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainStructuralEdit(
    const TerrainPathingResources& resources, const TerrainWaterBrush* water, const TerrainBrush* cliff,
    std::span<const TerrainStrokePoint> points, bool followDoodads, const TerrainWaterBrushLimits& limits,
    TerrainCornerRampAlgorithm cornerAlgorithm) const {
    using Output = Result<PreparedTerrainEdit>;
    if ((water == nullptr) == (cliff == nullptr))
        return Output::Failure("MapData.Edit", "TERRAIN_STRUCTURAL_BRUSH", "一次地形事务必须明确选择水刷、普通悬崖或拐角坡道。");
    const bool corner = cliff && cliff->kind == TerrainBrushKind::CornerRamp;
    const bool straight = cliff && cliff->kind == TerrainBrushKind::StraightRamp;
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

    // 分阶段计量：规则身份和DOO解析先完成，随后才有算法候选；算法辅助表
    // 销毁后，按真实净差准备撤销和联动。不能把根本不写的整份WPM预收三遍。
    // 经典DOO的最小嵌套记录占4字节；每份展开结构不超过8倍wire。
    static_assert(sizeof(UnitPlacement) <= 8 * 91 && sizeof(DoodadPlacement) <= 8 * 42
        && sizeof(SpecialDoodadPlacement) <= 8 * 16 && sizeof(PlacementItemSet) <= 8 * 4
        && sizeof(PlacementItemChoice) <= 8 * 8 && sizeof(PlacementInventoryItem) <= 8 * 8
        && sizeof(PlacementAbility) <= 8 * 12, "Recalculate the bounded placement wire multiplier after layout growth");
    // 产品入口捕获v2起点时同样查询完整5x5坡道域；在任何该查询前已核单次512预算，
    // 会话在本笔总账预收它。Core候选自己的独立复核另计入result.work。
    std::size_t work = water && water->algorithm == TerrainWaterAlgorithm::NativeRamps ? 512 : 0;
    if (work > limits.maximumWork) return budgetFailure();
    const auto reserve = [](std::size_t& used, std::size_t limit, std::size_t count, std::size_t element) {
        if (element && count > (limit - used) / element) return false;
        used += count * element;
        return true;
    };
    // VerifyWaterBrushInput顺序释放两份canonical序列化缓冲，不与候选并存；
    // 仍须先按本笔上限限制其输入大小，不能借较大的资源捕获预算绕过本笔预算。
    for (const auto* grid : {terrainOriginal_.get(), terrain_.get()}) {
        std::size_t identityBytes = 37;
        if (identityBytes > limits.maximumCandidateBytes
            || !reserve(identityBytes, limits.maximumCandidateBytes, grid->vertices.size(), 7)
            || !reserve(identityBytes, limits.maximumCandidateBytes,
                grid->header.groundTilesets.size() + grid->header.cliffTilesets.size(), 4)
            || !reserve(work, limits.maximumWork, grid->vertices.size(), 1)) return budgetFailure();
    }
    const auto doodadWireBytes = current_[doodadIndex] ? current_[doodadIndex]->size() : 0;
    // 解析阶段的双布局判定最多同时持有两份展开结构；算法阶段仅保留其成功结果与原字节。
    std::size_t parseBytes = sizeof(DoodadPlacements), liveBytes = sizeof(DoodadPlacements);
    if (parseBytes > limits.maximumCandidateBytes
        || !reserve(parseBytes, limits.maximumCandidateBytes, doodadWireBytes, 32)
        || !reserve(liveBytes, limits.maximumCandidateBytes, doodadWireBytes, 10)
        || !reserve(work, limits.maximumWork, doodadWireBytes, 2)
        || liveBytes == limits.maximumCandidateBytes || work == limits.maximumWork) return budgetFailure();
    if (current_[doodadIndex] && original_[doodadIndex] && current_[doodadIndex] != original_[doodadIndex]) {
        // 私有谱系复核会同时解析旧/新DOO并临时还原special段；撤销后的集合也可能比当前磁盘大。
        if (!reserve(parseBytes, limits.maximumCandidateBytes, original_[doodadIndex]->size(), 32)
            || !reserve(work, limits.maximumWork, original_[doodadIndex]->size(), 8)
            || !reserve(work, limits.maximumWork, doodadWireBytes, 8)) return budgetFailure();
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
        const auto objectBound = resources.VerifyWaterBrushDoodads(doodads, original_[doodadIndex] ? std::span<const std::byte>{*original_[doodadIndex]} : std::span<const std::byte>{});
        if (!objectBound.Succeeded()) return {{}, objectBound.diagnostics};
        if (current_[doodadIndex] && original_[doodadIndex]) {
            // 规则owner已经绑定当前完整DOO。落笔只消费特殊模型形状及其私有谱系；
            // 普通对象的XY/旋转/增删另由正式保存的双阶段G0/G1解释，不在编辑时误当仅Z。
            const auto provenance = VerifyTerrainSpecialDoodadChange(*original_[doodadIndex], *current_[doodadIndex]);
            if (!provenance.Succeeded()) return {{}, provenance.diagnostics};
        }
        auto algorithmLimits = limits;
        algorithmLimits.maximumCandidateBytes -= liveBytes;
        algorithmLimits.maximumWork -= work;
        auto candidate = [&]() -> Result<TerrainWaterStrokeCandidate> {
            if (water) return PrepareTerrainWaterStroke(*terrain_, *resources.WaterBrushRules(), *water,
                points, doodads, resources.WaterSpecialShapes(), algorithmLimits);
            if (corner) return PrepareTerrainCornerRampStroke(*terrain_, *resources.WaterBrushRules(), *cliff,
                points, doodads, resources.WaterSpecialShapes(), algorithmLimits, cornerAlgorithm);
            if (straight) return PrepareTerrainStraightRampStroke(*terrain_, *resources.WaterBrushRules(), *cliff,
                points, doodads, resources.WaterSpecialShapes(), algorithmLimits);
            auto anchor = CaptureTerrainStrokeAnchor(*terrain_, points.front());
            if (!anchor.Succeeded() || !anchor.value) return {{}, std::move(anchor.diagnostics)};
            const TerrainCliffBrush brush{cliff->shape, cliff->size.value_or(cliff->radius + 1), cliff->cliffTexture,
                cliff->seed, cliff->anchor.value_or(*anchor.value), cliff->levelDelta};
            return PrepareTerrainCliffStroke(*terrain_, *resources.WaterBrushRules(), brush,
                points, doodads, resources.WaterSpecialShapes(), algorithmLimits);
        }();
        if (!candidate.Succeeded() || !candidate.value) return {{}, std::move(candidate.diagnostics)};
        // result.work已包括算法输入检查、唯一候选复制、全图差分与派生刷新，不重复收费。
        if (!reserve(work, limits.maximumWork, candidate.value->result.work, 1)) return budgetFailure();
        const auto& changed = candidate.value->result.changed;
        const auto& grid = candidate.value->terrain;
        const auto area = std::size_t(changed.x1 - changed.x0) * (changed.y1 - changed.y0);
        // 这遍只计实际改变范围内的blight净差，后面在分配WPM前预收真实需要的联动。
        if (!reserve(work, limits.maximumWork, area, 1)) return budgetFailure();
        std::size_t blightCells = 0;
        const auto pathingWidth = (grid.header.width - 1) * 4, pathingHeight = (grid.header.height - 1) * 4;
        for (auto y = changed.y0; y < changed.y1; ++y) for (auto x = changed.x0; x < changed.x1; ++x) {
            const auto index = std::size_t(y) * grid.header.width + x;
            if (terrain_->vertices[index].IsBlight() == grid.vertices[index].IsBlight()) continue;
            const auto x0 = x ? x * 4 - 2 : 0, y0 = y ? y * 4 - 2 : 0;
            blightCells += std::size_t(std::min(pathingWidth, x * 4 + 2) - x0)
                * (std::min(pathingHeight, y * 4 + 2) - y0);
        }
        if (blightCells && !pathing_)
            return Output::Failure("MapData.Edit", "MAP_PATHING_UNAVAILABLE", "本笔改变了荒芜，但缺少可解析的源WPM，不能提交失配的地形。");

        // 算法辅助表已销毁：此阶段仅计存活的owned地形、真实覆盖的16x16区块和后续联动。
        std::size_t finalBytes = liveBytes;
        if (!reserve(finalBytes, limits.maximumCandidateBytes, 1,
                sizeof(Command) + sizeof(PreparedTerrainEdit) + sizeof(TerrainGridSnapshot) + 2 * sizeof(std::shared_ptr<void>))
            || !reserve(finalBytes, limits.maximumCandidateBytes, grid.vertices.capacity(), sizeof(TerrainVertex))
            || !reserve(finalBytes, limits.maximumCandidateBytes, grid.tiles.capacity(), sizeof(TerrainTile))
            || !reserve(finalBytes, limits.maximumCandidateBytes,
                grid.header.groundTilesets.capacity() + grid.header.cliffTilesets.capacity(), sizeof(std::string))
            || !reserve(finalBytes, limits.maximumCandidateBytes, points.size(), sizeof(TerrainStrokePoint))) return budgetFailure();
        for (const auto* table : {&grid.header.groundTilesets, &grid.header.cliffTilesets})
            for (const auto& name : *table)
                if (!reserve(finalBytes, limits.maximumCandidateBytes, name.capacity() + 1, 1)) return budgetFailure();
        if (!changed.Empty()) {
            const auto x0 = changed.x0 / 16 * 16, y0 = changed.y0 / 16 * 16;
            const auto x1 = std::min((changed.x1 + 15) / 16 * 16, grid.header.width);
            const auto y1 = std::min((changed.y1 + 15) / 16 * 16, grid.header.height);
            const auto chunkVertices = std::size_t(x1 - x0) * (y1 - y0);
            const auto chunks = std::size_t((x1 - x0 + 15) / 16) * ((y1 - y0 + 15) / 16);
            // 两份raw区块读取及一次比较；finalize本身另有一次blight净差扫描。
            if (!reserve(finalBytes, limits.maximumCandidateBytes, chunkVertices, 14)
                || !reserve(finalBytes, limits.maximumCandidateBytes, chunks, 2 * sizeof(TerrainChunk))
                || !reserve(work, limits.maximumWork, chunkVertices, 3)
                || !reserve(work, limits.maximumWork, area, 1)) return budgetFailure();
        }
        if (changed.headerChanged && !reserve(finalBytes, limits.maximumCandidateBytes,
            grid.header.groundTilesets.size() + grid.header.cliffTilesets.size(), 2 * (sizeof(std::string) + 16))) return budgetFailure();
        if (blightCells) {
            // 一次patched复制和一次before回执复制；真实blight净差已知，不再整份比较。
            if (!reserve(finalBytes, limits.maximumCandidateBytes, pathing_->size(), 2)
                || !reserve(work, limits.maximumWork, pathing_->size(), 2)
                || !reserve(work, limits.maximumWork, area + blightCells, 1)) return budgetFailure();
        }
        const bool specialChanged = !candidate.value->result.removedSpecialSourceRecords.empty();
        if (specialChanged || (followDoodads && !changed.Empty())) {
            // 只在确实进入跟随分支时预留序列化/回读峰值。DOO已持有的10倍wire不重复收费。
            if (!reserve(finalBytes, limits.maximumCandidateBytes, doodadWireBytes, 54)
                || !reserve(work, limits.maximumWork, doodadWireBytes, 4)) return budgetFailure();
            if (followDoodads) for (const auto index : {unitIndex, Index(MapDataKind::Settings)})
                if (current_[index] && (!reserve(finalBytes, limits.maximumCandidateBytes, current_[index]->size(), 64)
                    || !reserve(work, limits.maximumWork, current_[index]->size(), 4))) return budgetFailure();
        }
        doodads = std::move(candidate.value->doodads);
        auto next = std::make_shared<TerrainGridSnapshot>(std::move(candidate.value->terrain));
        // 水刷保存须另跑CaptureNative/G0/G1。这里若预跑等价保存门，会在拒绝水变化
        // 之前先解析并扫描整份WPM；省去这次重复工作，明确保持NOT_INSPECTED而非成功。
        auto prepared = PrepareTerrainCandidate(std::move(next), candidate.value->result.changed, points,
            followDoodads, true, true, false, &doodads, specialChanged, resources.WaterBrushRules()->maximumCliffLevel,
            limits.maximumWork - work);
        if (prepared.value) {
            if (!reserve(work, limits.maximumWork, prepared.value->waterPreparationWork_, 1)) return budgetFailure();
            if (water) {
                prepared.value->waterBrush_ = *water;
                prepared.value->waterAnchorVerified_ = water->algorithm == TerrainWaterAlgorithm::NativeRamps;
            } else {
                prepared.value->brush_ = *cliff;
                if (corner) prepared.value->cornerRampAlgorithm_ = cornerAlgorithm;
            }
            const auto digestBytes = resources.WaterBrushDependencyBudgetBytes();
            auto journalPeak = finalBytes, journalWork = work;
            if (points.size() <= 4096 && digestBytes
                && reserve(journalPeak, limits.maximumCandidateBytes, digestBytes, 1)
                && reserve(journalWork, limits.maximumWork, digestBytes, 3)) {
                // 新记录的wire编码/散列也计入本笔实际峰值和累计工作。
                finalBytes = journalPeak; work = journalWork;
                const auto fingerprint = resources.WaterBrushDependencyFingerprint();
                if (fingerprint.Succeeded() && fingerprint.value) {
                    prepared.value->waterRulesSha256_ = *fingerprint.value;
                    if (cliff && !corner && !straight) prepared.value->cliffEnforceWaterLimits_ = resources.WaterBrushRules()->enforceWaterLimits;
                    if (water) prepared.value->waterIntentOptions_ = TerrainWaterStrokeOptions{water->kind, water->shape, water->size,
                        water->cliffTexture, water->randomSeed, resources.WaterBrushRules()->enforceWaterLimits, water->algorithm};
                }
            } // 旧Core兼容owner、超域或journal额外预算不足仍可编辑，但不记录typed来源。
            if (corner && prepared.value->waterRulesSha256_.empty())
                return Output::Failure("MapData.Edit", "TERRAIN_CORNER_RAMP_JOURNAL_REQUIRED",
                    "拐角坡道必须保留实际规则和完整DOO的typed恢复身份；缺失依赖或预算时整笔拒绝。");
            if (straight && prepared.value->waterRulesSha256_.empty())
                return Output::Failure("MapData.Edit", "TERRAIN_STRAIGHT_RAMP_JOURNAL_REQUIRED",
                    "一级直坡必须保留实际规则和完整DOO的typed恢复身份；缺失依赖或预算时整笔拒绝。");
            prepared.value->waterResult_ = candidate.value->result;
            prepared.value->waterPreparationWork_ = work;
            prepared.value->waterPreparationBytes_ = finalBytes;
        }
        return prepared;
    } catch (const std::bad_alloc&) {
        return budgetFailure();
    } catch (const std::length_error&) {
        return budgetFailure();
    }
}

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainCornerRampEdit(
    const TerrainPathingResources& resources, const TerrainBrush& brush,
    std::span<const TerrainStrokePoint> points, bool followDoodads, const TerrainWaterBrushLimits& limits,
    TerrainCornerRampAlgorithm algorithm) const {
    if (!IsCanonicalTerrainCornerRampBrush(brush) || points.size() != 1)
        return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "TERRAIN_CORNER_RAMP_BRUSH",
            "拐角坡道只接受单个canonical落点和创建/移除选项。");
    if (!resources.WaterBrushRules() || !resources.WaterBrushRules()->enforceWaterLimits)
        return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "TERRAIN_CORNER_RAMP_RULE_OWNER",
            "拐角坡道必须使用固定enforceWaterLimits=true的实际完整规则owner，供typed恢复精确重现。");
    return PrepareTerrainStructuralEdit(resources, nullptr, &brush, points, followDoodads, limits, algorithm);
}

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainStraightRampEdit(
    const TerrainPathingResources& resources, const TerrainBrush& brush,
    std::span<const TerrainStrokePoint> points, bool followDoodads, const TerrainWaterBrushLimits& limits) const {
    if (!IsCanonicalTerrainStraightRampBrush(brush) || points.size() != 1)
        return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "TERRAIN_STRAIGHT_RAMP_BRUSH",
            "一级直坡只接受单个canonical落点和创建/移除选项。");
    if (!resources.WaterBrushRules() || !resources.WaterBrushRules()->enforceWaterLimits)
        return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "TERRAIN_STRAIGHT_RAMP_RULE_OWNER",
            "一级直坡必须使用固定enforceWaterLimits=true的实际完整规则owner，供typed恢复精确重现。");
    return PrepareTerrainStructuralEdit(resources, nullptr, &brush, points, followDoodads, limits);
}

} // namespace War3Map::MapData
