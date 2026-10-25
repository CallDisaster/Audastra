#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainBlockPasteEdit(
    const TerrainPathingResources& resources, const TerrainBlockPasteIntent& intent,
    bool followDoodads, const TerrainWaterBrushLimits& limits) const {
    using Output = Result<PreparedTerrainEdit>;
    const auto validated = ValidateTerrainBlockPasteIntent(intent);
    if (!validated.Succeeded()) return {{}, validated.diagnostics};
    const TerrainWaterBrushLimits hard;
    if (!limits.maximumCandidateBytes || limits.maximumCandidateBytes > hard.maximumCandidateBytes
        || !limits.maximumWork || limits.maximumWork > hard.maximumWork
        || !limits.maximumPoints || limits.maximumPoints > hard.maximumPoints
        || !limits.maximumRecursionDepth || limits.maximumRecursionDepth > hard.maximumRecursionDepth)
        return Output::Failure("MapData.Edit", "TERRAIN_BLOCK_LIMITS", "地形块预算只能收紧硬上限，不能为零。");
    if (!terrain_ || !terrainOriginal_)
        return Output::Failure("MapData.Edit", "MAP_TERRAIN_UNAVAILABLE", "地形块缺少可编辑的原始来源或当前网格。");
    if (terrainSourceRevision_ == std::numeric_limits<std::uint64_t>::max())
        return Output::Failure("MapData.Edit", "TERRAIN_SOURCE_REVISION_EXHAUSTED", "地形来源修订已用尽，请重新打开地图。");
    const auto* rules = resources.WaterBrushRules();
    if (!rules || !rules->enforceWaterLimits)
        return Output::Failure("MapData.Edit", "TERRAIN_BLOCK_RULE_OWNER",
            "地形块必须使用固定enforceWaterLimits=true的实际完整规则owner，供恢复和保存复核。");
    const auto doodadIndex = Index(MapDataKind::Doodads), unitIndex = Index(MapDataKind::Units);
    const auto available = [this](std::size_t index) {
        return !preservedSources_[index] && (states_[index] == MapDataState::Missing || states_[index] == MapDataState::Ready);
    };
    if (!available(doodadIndex))
        return Output::Failure("MapData.Edit", "MAP_DOODADS_UNSAFE", "实际装饰物来源不可读，不能以空表代替特殊模型来源。");
    if (followDoodads && !available(unitIndex))
        return Output::Failure("MapData.Edit", "MAP_UNITS_UNSAFE", "单位来源不可读，不能准备高度跟随。");
    const auto budgetFailure = [] {
        return Output::Failure("MapData.Edit", "TERRAIN_BLOCK_SESSION_BUDGET",
            "地形块候选、撤销、通行或对象联动超出本笔预算，尚未修改会话。");
    };
    const auto reserve = [](std::size_t& used, std::size_t limit, std::size_t count, std::size_t element = 1) {
        if (used > limit || (element && count > (limit - used) / element)) return false;
        used += count * element; return true;
    };
    std::size_t work = 0, peak = 0;
    // 同一实际owner校验原始和当前canonical来源；两份序列化顺序释放，
    // 不与后续owned候选相加成虚假的驻留峰值。
    for (const auto* grid : {terrainOriginal_.get(), terrain_.get()}) {
        std::size_t bytes = 37;
        if (!reserve(bytes, limits.maximumCandidateBytes, grid->vertices.size(), 7)
            || !reserve(bytes, limits.maximumCandidateBytes,
                grid->header.groundTilesets.size() + grid->header.cliffTilesets.size(), 4)
            || !reserve(work, limits.maximumWork, bytes, 3)) return budgetFailure();
        // 当前公共owner身份helper使用Sha256(span)，hash chunk与canonical
        // wire同时存活。计两份实际输入上界，不能只收一次序列化。
        const auto wire = bytes;
        if (!reserve(bytes, limits.maximumCandidateBytes, wire)) return budgetFailure();
        peak = std::max(peak, bytes);
    }
    static_assert(sizeof(UnitPlacement) <= 8 * 91 && sizeof(DoodadPlacement) <= 8 * 42
        && sizeof(SpecialDoodadPlacement) <= 8 * 16 && sizeof(PlacementItemSet) <= 8 * 4
        && sizeof(PlacementItemChoice) <= 8 * 8 && sizeof(PlacementInventoryItem) <= 8 * 8
        && sizeof(PlacementAbility) <= 8 * 12, "Recalculate bounded placement wire multiplier after layout growth");
    const auto doodadWireBytes = current_[doodadIndex] ? current_[doodadIndex]->size() : 0;
    std::size_t parseBytes = sizeof(DoodadPlacements), liveBytes = sizeof(DoodadPlacements);
    if (!reserve(parseBytes, limits.maximumCandidateBytes, doodadWireBytes, 32)
        || !reserve(liveBytes, limits.maximumCandidateBytes, doodadWireBytes, 10)
        || !reserve(work, limits.maximumWork, doodadWireBytes, 2)) return budgetFailure();
    if (current_[doodadIndex] && original_[doodadIndex] && current_[doodadIndex] != original_[doodadIndex]) {
        if (!reserve(parseBytes, limits.maximumCandidateBytes, original_[doodadIndex]->size(), 32)
            || !reserve(work, limits.maximumWork, original_[doodadIndex]->size(), 8)
            || !reserve(work, limits.maximumWork, doodadWireBytes, 8)) return budgetFailure();
    }
    peak = std::max(peak, parseBytes);
    if (liveBytes >= limits.maximumCandidateBytes || work >= limits.maximumWork) return budgetFailure();
    try {
        const auto sourceBound = resources.VerifyWaterBrushInput(*terrainOriginal_, *terrain_);
        if (!sourceBound.Succeeded()) return {{}, sourceBound.diagnostics};
        DoodadPlacements doodads;
        if (current_[doodadIndex]) {
            auto parsed = ParseDoodadPlacements(*current_[doodadIndex]);
            if (!parsed.Succeeded() || !parsed.value) return {{}, std::move(parsed.diagnostics)};
            doodads = std::move(*parsed.value);
        }
        const auto objectBound = resources.VerifyWaterBrushDoodads(doodads, original_[doodadIndex]
            ? std::span<const std::byte>{*original_[doodadIndex]} : std::span<const std::byte>{});
        if (!objectBound.Succeeded()) return {{}, objectBound.diagnostics};
        if (current_[doodadIndex] && original_[doodadIndex]) {
            const auto provenance = VerifyTerrainSpecialDoodadChange(*original_[doodadIndex], *current_[doodadIndex]);
            if (!provenance.Succeeded()) return {{}, provenance.diagnostics};
        }
        auto algorithmLimits = limits;
        algorithmLimits.maximumCandidateBytes -= liveBytes; algorithmLimits.maximumWork -= work;
        auto candidate = PrepareTerrainBlockPasteStroke(*terrain_, *rules, intent, resources.WaterSpecialShapes(), algorithmLimits);
        if (!candidate.Succeeded() || !candidate.value) return {{}, std::move(candidate.diagnostics)};
        if (!reserve(work, limits.maximumWork, candidate.value->result.work)) return budgetFailure();
        auto algorithmPeak = liveBytes;
        if (!reserve(algorithmPeak, limits.maximumCandidateBytes, candidate.value->peakBytes)) return budgetFailure();
        peak = std::max(peak, algorithmPeak);

        const auto& changed = candidate.value->result.changed;
        const auto& grid = candidate.value->terrain;
        const auto area = std::size_t(changed.x1 - changed.x0) * (changed.y1 - changed.y0);
        if (!reserve(work, limits.maximumWork, area)) return budgetFailure();
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
            return Output::Failure("MapData.Edit", "MAP_PATHING_UNAVAILABLE", "粘贴改变了荒芜，但缺少可解析的源WPM，整笔未提交。");

        // 算法scratch已释放，以下只收真实候选容量及同一公共finalize会分配的载荷。
        std::size_t finalBytes = liveBytes;
        if (!reserve(finalBytes, limits.maximumCandidateBytes, 1,
                sizeof(Command) + sizeof(PreparedTerrainEdit) + sizeof(TerrainGridSnapshot) + 2 * sizeof(std::shared_ptr<void>))
            || !reserve(finalBytes, limits.maximumCandidateBytes, grid.vertices.capacity(), sizeof(TerrainVertex))
            || !reserve(finalBytes, limits.maximumCandidateBytes, grid.tiles.capacity(), sizeof(TerrainTile))
            || !reserve(finalBytes, limits.maximumCandidateBytes,
                grid.header.groundTilesets.capacity() + grid.header.cliffTilesets.capacity(), sizeof(std::string))
            || !reserve(finalBytes, limits.maximumCandidateBytes, intent.expectedBlockSha256.capacity() + 1)) return budgetFailure();
        for (const auto* table : {&grid.header.groundTilesets, &grid.header.cliffTilesets})
            for (const auto& name : *table)
                if (!reserve(finalBytes, limits.maximumCandidateBytes, name.capacity() + 1)) return budgetFailure();
        if (!changed.Empty()) {
            const auto x0 = changed.x0 / 16 * 16, y0 = changed.y0 / 16 * 16;
            const auto x1 = std::min((changed.x1 + 15) / 16 * 16, grid.header.width);
            const auto y1 = std::min((changed.y1 + 15) / 16 * 16, grid.header.height);
            const auto vertices = std::size_t(x1 - x0) * (y1 - y0);
            const auto chunks = std::size_t((x1 - x0 + 15) / 16) * ((y1 - y0 + 15) / 16);
            if (!reserve(finalBytes, limits.maximumCandidateBytes, vertices, 14)
                || !reserve(finalBytes, limits.maximumCandidateBytes, chunks, 2 * sizeof(TerrainChunk))
                || !reserve(work, limits.maximumWork, vertices, 3)
                || !reserve(work, limits.maximumWork, area)) return budgetFailure();
        }
        if (blightCells && (!reserve(finalBytes, limits.maximumCandidateBytes, pathing_->size(), 2)
            || !reserve(work, limits.maximumWork, pathing_->size(), 2)
            || !reserve(work, limits.maximumWork, area + blightCells))) return budgetFailure();
        if (followDoodads && !changed.Empty()) {
            if (!reserve(finalBytes, limits.maximumCandidateBytes, doodadWireBytes, 54)
                || !reserve(work, limits.maximumWork, doodadWireBytes, 4)) return budgetFailure();
            for (const auto index : {unitIndex, Index(MapDataKind::Settings)})
                if (current_[index] && (!reserve(finalBytes, limits.maximumCandidateBytes, current_[index]->size(), 64)
                    || !reserve(work, limits.maximumWork, current_[index]->size(), 4))) return budgetFailure();
        }
        auto next = std::make_shared<TerrainGridSnapshot>(std::move(candidate.value->terrain));
        auto prepared = PrepareTerrainCandidate(std::move(next), changed, {}, followDoodads,
            true, true, false, &doodads, false, rules->maximumCliffLevel, limits.maximumWork - work);
        if (!prepared.Succeeded() || !prepared.value) return prepared;
        if (!reserve(work, limits.maximumWork, prepared.value->waterPreparationWork_)) return budgetFailure();
        // 新块意图必须完整typed持久化，不能像旧自由笔划一样因预算不足降级成raw编辑。
        const auto digestBytes = resources.WaterBrushDependencyBudgetBytes();
        if (!digestBytes || !reserve(finalBytes, limits.maximumCandidateBytes, digestBytes, 2)
            || !reserve(finalBytes, limits.maximumCandidateBytes, 65)
            || !reserve(work, limits.maximumWork, digestBytes, 3)) return budgetFailure();
        auto fingerprint = resources.WaterBrushDependencyFingerprint();
        if (!fingerprint.Succeeded() || !fingerprint.value) return {{}, std::move(fingerprint.diagnostics)};
        prepared.value->waterRulesSha256_ = std::move(*fingerprint.value);
        prepared.value->blockPaste_ = intent;
        prepared.value->waterResult_ = std::move(candidate.value->result);
        prepared.value->waterPreparationWork_ = work;
        prepared.value->waterPreparationBytes_ = std::max(peak, finalBytes);
        return prepared;
    } catch (const std::bad_alloc&) { return budgetFailure(); }
    catch (const std::length_error&) { return budgetFailure(); }
}

} // namespace War3Map::MapData
