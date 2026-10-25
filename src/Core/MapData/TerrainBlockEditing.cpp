#include "War3MapCore/MapData/TerrainBlockEditing.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, const char* message) {
    throw Failure{Status::Failure("Terrain.Block", code, message).diagnostics};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
void Check(Status status) { if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)}; }
struct Budget {
    const TerrainWaterBrushLimits& limits;
    std::size_t work = 0, peak = 0;
    void Work(std::size_t count = 1) {
        if (count > limits.maximumWork - work) Fail("TERRAIN_BLOCK_WORK_BUDGET", "地形块处理超过工作量预算，尚未修改来源。");
        work += count;
    }
    void Peak(std::size_t bytes) {
        if (bytes > limits.maximumCandidateBytes) Fail("TERRAIN_BLOCK_MEMORY_BUDGET", "地形块候选超过字节预算，尚未修改来源。");
        peak = std::max(peak, bytes);
    }
    void Add(std::size_t& bytes, std::size_t count, std::size_t element = 1) {
        if (element && count > (limits.maximumCandidateBytes - bytes) / element)
            Fail("TERRAIN_BLOCK_MEMORY_BUDGET", "地形块候选超过字节预算，尚未修改来源。");
        bytes += count * element;
    }
};
void Limits(const TerrainWaterBrushLimits& limits) {
    const TerrainWaterBrushLimits hard;
    if (!limits.maximumCandidateBytes || limits.maximumCandidateBytes > hard.maximumCandidateBytes
        || !limits.maximumWork || limits.maximumWork > hard.maximumWork
        || !limits.maximumPoints || limits.maximumPoints > hard.maximumPoints
        || !limits.maximumRecursionDepth || limits.maximumRecursionDepth > hard.maximumRecursionDepth)
        Fail("TERRAIN_BLOCK_LIMITS", "地形块预算只能收紧硬上限，不能为零。");
}
bool Rectangle(const TerrainCellRect& cells) {
    return cells.x0 < cells.x1 && cells.y0 < cells.y1 && cells.x1 <= 2048 && cells.y1 <= 2048
        && cells.x1 - cells.x0 <= TerrainBlockMaximumSide && cells.y1 - cells.y0 <= TerrainBlockMaximumSide;
}
void Grid(const TerrainGridSnapshot& grid, Budget& budget) {
    const auto& header = grid.header;
    if (header.version != 11 || header.width < 2 || header.height < 2 || header.width > 2049 || header.height > 2049
        || grid.vertices.size() != std::size_t(header.width) * header.height
        || (!grid.tiles.empty() && grid.tiles.size() != std::size_t(header.width - 1) * (header.height - 1)))
        Fail("TERRAIN_BLOCK_LAYOUT", "地形块需要完整经典W3E v11网格。");
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const double first = header.offset[axis];
        const double last = first + 128.0 * ((axis ? header.height : header.width) - 1);
        if (!std::isfinite(first) || std::fmod(first, 128.0) != 0 || first < -32768 || last > 32640)
            Fail("TERRAIN_BLOCK_COORDINATE", "地形块需要128对齐且在原版九位坐标范围内的网格。");
    }
    for (const auto* table : {&header.groundTilesets, &header.cliffTilesets}) {
        if (table->size() > 256) Fail("TERRAIN_BLOCK_PALETTE", "地形块贴图表超过经典预算。");
        for (const auto& id : *table) {
            budget.Work();
            if (id.size() != 4 || std::any_of(id.begin(), id.end(), [](unsigned char c) { return c >= 128; }))
                Fail("TERRAIN_BLOCK_PALETTE", "地形块贴图表不是已知四字节rawcode。");
        }
    }
}
TerrainCellRect Destination(const TerrainBlockPasteIntent& intent) {
    return {intent.destinationVertex[0], intent.destinationVertex[1],
        intent.destinationVertex[0] + intent.sourceCells.x1 - intent.sourceCells.x0,
        intent.destinationVertex[1] + intent.sourceCells.y1 - intent.sourceCells.y0};
}
TerrainCellRect Halo(const TerrainGridSnapshot& grid, const TerrainCellRect& cells) {
    if (!Rectangle(cells) || cells.x0 < 2 || cells.y0 < 2
        || std::uint64_t(cells.x1) + 2 >= grid.header.width || std::uint64_t(cells.y1) + 2 >= grid.header.height)
        Fail("TERRAIN_BLOCK_NEIGHBORHOOD", "地形块与目标均须保留完整两圈邻域，不能裁掉越界部分。");
    return {cells.x0 - 2, cells.y0 - 2, cells.x1 + 2, cells.y1 + 2};
}
const TerrainVertex& At(const TerrainGridSnapshot& grid, std::uint32_t x, std::uint32_t y) {
    return grid.vertices[std::size_t(y) * grid.header.width + x];
}
void Domain(const TerrainGridSnapshot& grid, const TerrainCellRect& halo, Budget& budget,
    std::uint8_t maximum, const TerrainWaterLoadedRules* rules = nullptr) {
    for (auto y = halo.y0; y <= halo.y1; ++y) for (auto x = halo.x0; x <= halo.x1; ++x) {
        budget.Work(); const auto& vertex = At(grid, x, y);
        if (vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw || vertex.groundHeightRaw > 0x3fff
            || vertex.UnknownWaterFlags() || vertex.IsRamp() || vertex.IsBoundary() || vertex.IsMapEdge()
            || vertex.GroundVariation() > 17 || vertex.CliffVariation() > 2
            || vertex.CliffLevel() > maximum || vertex.GroundTextureIndex() >= grid.header.groundTilesets.size()
            || (vertex.CliffTextureIndex() != 15 && vertex.CliffTextureIndex() >= grid.header.cliffTilesets.size()))
            Fail("TERRAIN_BLOCK_DOMAIN", "地形块完整邻域含未知字段、坡道、边界、外缘或无效贴图/层级。");
        if (rules) {
            bool found = false;
            for (const auto& ground : rules->groundRules) {
                budget.Work(); if (ground.rawcode == grid.header.groundTilesets[vertex.GroundTextureIndex()]) { found = true; break; }
            }
            if (!found) Fail("TERRAIN_BLOCK_GROUND_RESOURCE", "地形块实际地面槽缺少本次加载的规则。");
        }
    }
    // 无R/S时原版按四角实际层级决定悬崖。只接受普通一级接缝，不能靠
    // 调低层级、换材质或向块外传播来修好一个不合法的粘贴结果。
    for (auto y = halo.y0; y < halo.y1; ++y) for (auto x = halo.x0; x < halo.x1; ++x) {
        unsigned low = 15, high = 0; std::optional<std::uint8_t> palette; bool mixedPalette = false;
        for (unsigned dy = 0; dy < 2; ++dy) for (unsigned dx = 0; dx < 2; ++dx) {
            budget.Work(); const auto& vertex = At(grid, x + dx, y + dy);
            low = std::min(low, unsigned(vertex.CliffLevel())); high = std::max(high, unsigned(vertex.CliffLevel()));
            const auto current = vertex.CliffTextureIndex();
            if (current != 15) {
                mixedPalette |= palette && *palette != current;
                palette = current;
            }
        }
        if (high == low) continue; // 平地的cliff15是合法未选材质哨兵。
        if (high - low != 1 || !palette || mixedPalette)
            Fail("TERRAIN_BLOCK_CLIFF_SEAM", "粘贴会产生多级接缝或缺少实际悬崖材质，整笔未修改。");
        if (rules) {
            bool found = false;
            for (const auto& id : rules->availableCliffTilesets) {
                budget.Work(); if (id == grid.header.cliffTilesets[*palette]) { found = true; break; }
            }
            if (!found) Fail("TERRAIN_BLOCK_CLIFF_RESOURCE", "地形块接缝的实际悬崖材质尚未加载。");
        }
    }
}
std::string Fingerprint(const TerrainGridSnapshot& grid, const TerrainCellRect& cells, Budget& budget) {
    constexpr std::string_view tag = "Audastra.TerrainBlock.v1";
    // 逐字段流式SHA，不再为块载荷建立两份完整wire。这里只计C++实际拥有的
    // State/CNG对象缓冲和结果；provider系统内部内存不冒充可控RSS预算。
    std::size_t bytes = tag.size() + 1 + 16 + 4 + 2 + 16 + 8;
    // Rectangle和Grid已将两轴、表项数量限定在经典硬域；这是输入字节数，
    // 不是分配量，不能把不存在的完整wire再次收进内存峰值。
    bytes += 4 * (grid.header.groundTilesets.size() + grid.header.cliffTilesets.size())
        + 7 * std::size_t(cells.x1 - cells.x0 + 1) * (cells.y1 - cells.y0 + 1);
    budget.Peak(Sha256Stream::kBufferBytes);
    budget.Work(bytes * 3);
    Sha256Stream hash(bytes); Check(hash.Update({}));
    std::size_t scratch = sizeof(hash); budget.Add(scratch, hash.ScratchBytes()); budget.Add(scratch, 65);
    budget.Peak(scratch);
    const auto Put = [&](std::uint32_t value, unsigned count = 4) {
        std::array<std::byte, 4> raw{};
        for (unsigned i = 0; i < count; ++i) raw[i] = std::byte((value >> (i * 8)) & 255);
        Check(hash.Update(std::span(raw.data(), count)));
    };
    Check(hash.Update(std::as_bytes(std::span(tag.data(), tag.size()))));
    Put(1, 1); Put(cells.x0); Put(cells.y0); Put(cells.x1 - cells.x0); Put(cells.y1 - cells.y0);
    Put(std::uint32_t(grid.header.version)); Put(grid.header.mainTileset, 1);
    Put(grid.header.usesCustomTileset ? 1 : 0, 1); Put(grid.header.width); Put(grid.header.height);
    for (const auto offset : grid.header.offset) Put(std::bit_cast<std::uint32_t>(offset));
    for (const auto* table : {&grid.header.groundTilesets, &grid.header.cliffTilesets}) {
        Put(std::uint32_t(table->size()));
        for (const auto& id : *table) Check(hash.Update(std::as_bytes(std::span(id.data(), id.size()))));
    }
    for (auto y = cells.y0; y <= cells.y1; ++y) for (auto x = cells.x0; x <= cells.x1; ++x) {
        const auto& vertex = At(grid, x, y); Put(vertex.groundHeightRaw, 2); Put(vertex.waterHeightAndFlagsRaw, 2);
        Put(vertex.textureAndFlagsRaw, 1); Put(vertex.variationRaw, 1); Put(vertex.cliffAndLayerRaw, 1);
    }
    return Take(hash.Finish());
}
bool Intersects(const TerrainCellRect& a, const TerrainCellRect& b) {
    // 这里是闭顶点占地，接触最后一行/列仍共享真实special顶点。
    return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}
void Specials(const TerrainGridSnapshot& grid, std::span<const TerrainWaterSpecialDoodadShape> shapes,
    const TerrainCellRect& source, const TerrainCellRect& target, Budget& budget) {
    for (const auto& shape : shapes) {
        budget.Work();
        if (!shape.hasFootprint) {
            if (!shape.relativeCliffLevels.empty()) Fail("TERRAIN_BLOCK_SPECIAL_SHAPE", "无占地特殊模型带有不一致的形状。");
            continue;
        }
        const TerrainCellRect footprint{shape.x0, shape.y0, shape.x1, shape.y1};
        if (shape.rawcode.size() != 4 || shape.x0 >= shape.x1 || shape.y0 >= shape.y1
            || shape.x1 >= grid.header.width || shape.y1 >= grid.header.height
            || shape.relativeCliffLevels.size() != std::uint64_t(shape.x1 - shape.x0 + 1) * (shape.y1 - shape.y0 + 1))
            Fail("TERRAIN_BLOCK_SPECIAL_SHAPE", "实际特殊模型形状不完整或超出地图。");
        const int base = int(At(grid, shape.x0, shape.y0).CliffLevel()) - shape.relativeCliffLevels.front();
        for (auto y = shape.y0; y <= shape.y1; ++y) for (auto x = shape.x0; x <= shape.x1; ++x) {
            budget.Work(); const int level = shape.relativeCliffLevels[std::size_t(y - shape.y0) * (shape.x1 - shape.x0 + 1) + x - shape.x0];
            if (level < 0 || level > 2 || At(grid, x, y).CliffLevel() != base + level)
                Fail("TERRAIN_BLOCK_SPECIAL_SHAPE", "实际特殊模型与当前地形层级已失配。");
        }
        if (Intersects(footprint, source) || Intersects(footprint, target))
            Fail("TERRAIN_BLOCK_SPECIAL", "地形块源或目标邻域与实际特殊模型相交，不能移动或删除它。");
    }
}
bool Same(const TerrainVertex& a, const TerrainVertex& b) {
    return a.groundHeightRaw == b.groundHeightRaw && a.waterHeightAndFlagsRaw == b.waterHeightAndFlagsRaw
        && a.textureAndFlagsRaw == b.textureAndFlagsRaw && a.variationRaw == b.variationRaw && a.cliffAndLayerRaw == b.cliffAndLayerRaw;
}
template<class T, class Function> Result<T> Guard(Function function) {
    try { return Result<T>::Success(function()); }
    catch (Failure& failed) { return {{}, std::move(failed.diagnostics)}; }
    catch (const std::bad_alloc&) { return Result<T>::Failure("Terrain.Block", "TERRAIN_BLOCK_MEMORY_BUDGET", "地形块内存不足，尚未修改来源。"); }
    catch (const std::length_error&) { return Result<T>::Failure("Terrain.Block", "TERRAIN_BLOCK_MEMORY_BUDGET", "地形块容量超限，尚未修改来源。"); }
}
} // namespace

Status ValidateTerrainBlockPasteIntent(const TerrainBlockPasteIntent& intent) {
    if (intent.algorithm != 1 || !Rectangle(intent.sourceCells)
        || intent.destinationVertex[0] > 2048 - (intent.sourceCells.x1 - intent.sourceCells.x0)
        || intent.destinationVertex[1] > 2048 - (intent.sourceCells.y1 - intent.sourceCells.y0)
        || intent.expectedBlockSha256.size() != 64
        || std::any_of(intent.expectedBlockSha256.begin(), intent.expectedBlockSha256.end(), [](char c) {
            return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        })) return Status::Failure("Terrain.Block", "TERRAIN_BLOCK_INTENT", "地形块意图需要算法1、有效有界矩形、目标及小写SHA-256。");
    return Status::Ok();
}

Result<std::string> CaptureTerrainBlockFingerprint(const TerrainGridSnapshot& terrain,
    const TerrainCellRect& cells, const TerrainWaterBrushLimits& limits) {
    return Guard<std::string>([&] {
        Limits(limits); Budget budget{limits}; Grid(terrain, budget);
        const auto halo = Halo(terrain, cells); Domain(terrain, halo, budget, 14);
        return Fingerprint(terrain, cells, budget);
    });
}

Result<TerrainBlockStrokeCandidate> PrepareTerrainBlockPasteStroke(const TerrainGridSnapshot& source,
    const TerrainWaterLoadedRules& rules, const TerrainBlockPasteIntent& intent,
    std::span<const TerrainWaterSpecialDoodadShape> specialShapes, const TerrainWaterBrushLimits& limits) {
    return Guard<TerrainBlockStrokeCandidate>([&] {
        Limits(limits); Check(ValidateTerrainBlockPasteIntent(intent)); Budget budget{limits}; Grid(source, budget);
        if (rules.maximumCliffLevel > 14 || rules.groundRules.size() > 256 || rules.availableCliffTilesets.size() > 256)
            Fail("TERRAIN_BLOCK_RULES", "地形块需要实际有限地形规则。");
        const auto target = Destination(intent), sourceHalo = Halo(source, intent.sourceCells), targetHalo = Halo(source, target);
        Domain(source, sourceHalo, budget, rules.maximumCliffLevel, &rules);
        Domain(source, targetHalo, budget, rules.maximumCliffLevel, &rules);
        Specials(source, specialShapes, sourceHalo, targetHalo, budget);
        if (Fingerprint(source, intent.sourceCells, budget) != intent.expectedBlockSha256)
            Fail("TERRAIN_BLOCK_SOURCE_STALE", "复制后的源地形块或有序贴图表已变化，请重新复制。");
        std::size_t bytes = sizeof(TerrainBlockStrokeCandidate);
        budget.Peak(bytes); budget.Add(bytes, source.vertices.size(), sizeof(TerrainVertex));
        budget.Add(bytes, source.tiles.size(), sizeof(TerrainTile));
        budget.Add(bytes, source.header.groundTilesets.size() + source.header.cliffTilesets.size(), sizeof(std::string) + 16);
        budget.Peak(bytes); budget.Work(source.vertices.size() + source.tiles.size());
        TerrainBlockStrokeCandidate candidate; candidate.terrain = source;
        const auto width = intent.sourceCells.x1 - intent.sourceCells.x0, height = intent.sourceCells.y1 - intent.sourceCells.y0;
        for (std::uint32_t y = 0; y <= height; ++y) for (std::uint32_t x = 0; x <= width; ++x) {
            budget.Work(2); const auto dx = target.x0 + x, dy = target.y0 + y;
            // 始终从冻结before读取；相邻行和同图重叠不会读到已经写过的目标。
            const auto& original = At(source, intent.sourceCells.x0 + x, intent.sourceCells.y0 + y);
            auto& next = candidate.terrain.vertices[std::size_t(dy) * source.header.width + dx];
            if (Same(original, next)) continue;
            next.groundHeightRaw = original.groundHeightRaw;
            next.waterHeightAndFlagsRaw = std::uint16_t((next.waterHeightAndFlagsRaw & 0x4000) | (original.waterHeightAndFlagsRaw & 0x3fff));
            next.textureAndFlagsRaw = std::uint8_t((next.textureAndFlagsRaw & 0x80) | (original.textureAndFlagsRaw & 0x7f));
            next.variationRaw = original.variationRaw; next.cliffAndLayerRaw = original.cliffAndLayerRaw;
            auto& changed = candidate.result.changed;
            if (changed.Empty()) changed = {dx, dy, dx + 1, dy + 1, false};
            else { changed.x0 = std::min(changed.x0, dx); changed.y0 = std::min(changed.y0, dy);
                changed.x1 = std::max(changed.x1, dx + 1); changed.y1 = std::max(changed.y1, dy + 1); }
            ++candidate.result.changedVertices;
        }
        Domain(candidate.terrain, targetHalo, budget, rules.maximumCliffLevel, &rules);
        for (auto y = target.y0; y <= target.y1; ++y) for (auto x = target.x0; x <= target.x1; ++x) {
            budget.Work(); const auto index = std::size_t(y) * source.header.width + x;
            const auto oldSlot = source.vertices[index].CliffTextureIndex(), newSlot = candidate.terrain.vertices[index].CliffTextureIndex();
            if (oldSlot == newSlot || (oldSlot < 15 && newSlot < 15)) continue;
            // 与正式G0/G1捕获及保存门共享同一哨兵资格；不能先接受交互粘贴，
            // 再让保存才发现slot15一侧并不平坦。完整3x3查询提前收取双侧工作。
            budget.Work(32);
            if (!IsNativeTerrainCliffSentinelTransition(source, candidate.terrain, index))
                Fail("TERRAIN_BLOCK_CLIFF_SENTINEL", "粘贴的未选悬崖材质过渡不在原版保存已支持的完整平坦邻域内，整笔未修改。");
        }
        if (!candidate.result.changed.Empty()) {
            budget.Work(candidate.terrain.vertices.size() + std::size_t(candidate.result.changed.x1 - candidate.result.changed.x0)
                * (candidate.result.changed.y1 - candidate.result.changed.y0));
            RefreshTerrainDerived(candidate.terrain, candidate.result.changed.x0, candidate.result.changed.y0,
                candidate.result.changed.x1, candidate.result.changed.y1);
            candidate.result.requiresPathingRebuild = candidate.result.requiresObjectValidityRefresh = true;
        }
        candidate.result.processed = {std::min(sourceHalo.x0, targetHalo.x0), std::min(sourceHalo.y0, targetHalo.y0),
            std::max(sourceHalo.x1, targetHalo.x1) + 1, std::max(sourceHalo.y1, targetHalo.y1) + 1, false};
        candidate.result.work = budget.work; candidate.peakBytes = budget.peak;
        return candidate;
    });
}
} // namespace War3Map::MapData
