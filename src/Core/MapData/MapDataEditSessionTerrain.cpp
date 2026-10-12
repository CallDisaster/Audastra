#include "War3MapCore/MapData/MapDataEditSession.h"
#include "MapDataEditSessionTerrainInternal.h"
#include "War3MapCore/MapData/MapPathing.h"

#include <algorithm>
#include <array>
#include <bit>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace War3Map::MapData {
namespace detail {
void WriteChunk(TerrainGridSnapshot& terrain, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1,
    const std::vector<std::uint8_t>& raw) {
    std::size_t at = 0;
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x, at += 7) {
            auto& vertex = terrain.vertices[static_cast<std::size_t>(y) * terrain.header.width + x];
            vertex.groundHeightRaw = static_cast<std::uint16_t>(raw[at] | (raw[at + 1] << 8));
            vertex.waterHeightAndFlagsRaw = static_cast<std::uint16_t>(raw[at + 2] | (raw[at + 3] << 8));
            vertex.textureAndFlagsRaw = raw[at + 4];
            vertex.variationRaw = raw[at + 5];
            vertex.cliffAndLayerRaw = raw[at + 6];
        }
    }
}
} // namespace detail

namespace {
constexpr std::uint32_t kTerrainChunk = 16;
constexpr std::size_t kPathingHeaderBytes = 16;
constexpr std::uint8_t kBlightBit = 0x20;
// 原版单点实验中，连续六次降低（raw 8192 -> 8036）仍保留原 WPM。
// 更大的高度变化尚无原版对照，正式保存先限制在已取证的幅度内。
constexpr std::uint16_t kVerifiedHeightDeltaRaw = 156;

bool HeightNeighborhoodSafe(const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    std::size_t changedIndex) {
    const auto width = before.header.width, height = before.header.height;
    const auto x = static_cast<std::uint32_t>(changedIndex % width);
    const auto y = static_cast<std::uint32_t>(changedIndex / width);
    // 必须能检查完整的外扩一格；地图边缘的通行规则单独取证。
    if (x == 0 || y == 0 || x + 1 >= width || y + 1 >= height) return false;
    const auto level = before.vertices[changedIndex].CliffLevel();
    for (std::uint32_t ny = y - 1; ny <= y + 1; ++ny) {
        for (std::uint32_t nx = x - 1; nx <= x + 1; ++nx) {
            const auto index = static_cast<std::size_t>(ny) * width + nx;
            const auto& original = before.vertices[index];
            const auto& edited = after.vertices[index];
            if (original.HasWater() || edited.HasWater() || original.IsRamp() || edited.IsRamp()
                || original.IsBoundary() || edited.IsBoundary() || original.IsMapEdge() || edited.IsMapEdge()
                || original.UnknownWaterFlags() != 0 || edited.UnknownWaterFlags() != 0
                || original.CliffLevel() != level || edited.CliffLevel() != level) return false;
        }
    }
    return true;
}

bool PathingBlightDeltaMatchesTerrain(std::span<const std::byte> source, std::span<const std::byte> current,
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after) {
    if (before.header.width != after.header.width || before.header.height != after.header.height
        || before.vertices.size() != after.vertices.size()) return false;
    const auto width = (before.header.width - 1) * 4, height = (before.header.height - 1) * 4;
    const auto expectedBytes = kPathingHeaderBytes + static_cast<std::size_t>(width) * height;
    if (source.size() != expectedBytes || current.size() != expectedBytes
        || !std::equal(source.begin(), source.begin() + kPathingHeaderBytes, current.begin())) return false;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto vertexRow = static_cast<std::size_t>((y + 2) / 4) * before.header.width;
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto vertexIndex = vertexRow + (x + 2) / 4;
            const auto& sourceVertex = before.vertices[vertexIndex];
            const auto& currentVertex = after.vertices[vertexIndex];
            const auto index = kPathingHeaderBytes + static_cast<std::size_t>(y) * width + x;
            const auto sourceCell = std::to_integer<std::uint8_t>(source[index]);
            const auto currentCell = std::to_integer<std::uint8_t>(current[index]);
            if (sourceVertex.IsBlight() == currentVertex.IsBlight()) {
                if (sourceCell != currentCell) return false;
            } else if (((sourceCell & kBlightBit) != 0) != sourceVertex.IsBlight()
                || ((currentCell & kBlightBit) != 0) != currentVertex.IsBlight()
                || ((sourceCell ^ currentCell) & ~kBlightBit) != 0) return false;
        }
    }
    return true;
}

Status ValidatePathingOverlay(std::span<const std::byte> bytes, std::span<const std::byte> original,
    const TerrainGridSnapshot& originalTerrain, const TerrainGridSnapshot& nextTerrain) {
    auto parsed = ParseMapPathing(bytes, &nextTerrain.header);
    if (!parsed.Succeeded() || !parsed.value) {
        Status status;
        status.diagnostics = std::move(parsed.diagnostics);
        return status;
    }
    if (bytes.size() != original.size()
        || !std::equal(bytes.begin(), bytes.begin() + kPathingHeaderBytes, original.begin()))
        return Status::Failure("MapData.Edit", "MAP_PATHING_HEADER_CHANGED", "通行性覆盖改变了源 WPM 头或长度，拒绝恢复");
    for (std::size_t index = kPathingHeaderBytes; index < bytes.size(); ++index) {
        const auto difference = std::to_integer<std::uint8_t>(bytes[index] ^ original[index]);
        if ((difference & ~kBlightBit) != 0)
            return Status::Failure("MapData.Edit", "MAP_PATHING_UNKNOWN_BITS", "通行性覆盖改动了未验证位，拒绝恢复");
    }
    if (!PathingBlightDeltaMatchesTerrain(original, bytes, originalTerrain, nextTerrain))
        return Status::Failure("MapData.Edit", "MAP_PATHING_BLIGHT_MISMATCH",
            "通行性覆盖未精确对应已改变的荒芜顶点，或触碰了未改变顶点的原始格");
    return Status::Ok();
}

Result<std::vector<std::byte>> PatchBlightPathing(const TerrainGridSnapshot& before,
    const TerrainGridSnapshot& after, const TerrainStrokeResult& changed, std::span<const std::byte> source) {
    const auto width = (before.header.width - 1) * 4, height = (before.header.height - 1) * 4;
    if (source.size() != kPathingHeaderBytes + static_cast<std::size_t>(width) * height)
        return Result<std::vector<std::byte>>::Failure("MapData.Edit", "MAP_PATHING_SHAPE_CHANGED",
            "通行性格子数量与当前地形不一致，不能联动荒芜笔刷");
    std::vector<std::byte> patched(source.begin(), source.end());
    for (std::uint32_t vy = changed.y0; vy < changed.y1; ++vy) {
        for (std::uint32_t vx = changed.x0; vx < changed.x1; ++vx) {
            const auto vertexIndex = static_cast<std::size_t>(vy) * before.header.width + vx;
            const bool oldBlight = before.vertices[vertexIndex].IsBlight();
            const bool newBlight = after.vertices[vertexIndex].IsBlight();
            if (oldBlight == newBlight) continue;
            const auto x0 = vx == 0 ? 0u : vx * 4 - 2;
            const auto y0 = vy == 0 ? 0u : vy * 4 - 2;
            const auto x1 = std::min(width, vx * 4 + 2);
            const auto y1 = std::min(height, vy * 4 + 2);
            for (std::uint32_t y = y0; y < y1; ++y) {
                for (std::uint32_t x = x0; x < x1; ++x) {
                    const auto index = kPathingHeaderBytes + static_cast<std::size_t>(y) * width + x;
                    const auto oldCell = std::to_integer<std::uint8_t>(source[index]);
                    if (((oldCell & kBlightBit) != 0) != oldBlight)
                        return Result<std::vector<std::byte>>::Failure("MapData.Edit", "MAP_PATHING_BLIGHT_MISMATCH",
                            "原通行性荒芜位与地形不一致，拒绝猜测联动结果");
                    patched[index] = static_cast<std::byte>((oldCell & ~kBlightBit) | (newBlight ? kBlightBit : 0));
                }
            }
        }
    }
    return Result<std::vector<std::byte>>::Success(std::move(patched));
}

std::vector<std::uint8_t> ReadChunk(const TerrainGridSnapshot& terrain, std::uint32_t x0, std::uint32_t y0,
    std::uint32_t x1, std::uint32_t y1) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(x1 - x0) * (y1 - y0) * 7);
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const auto& vertex = terrain.vertices[static_cast<std::size_t>(y) * terrain.header.width + x];
            raw.insert(raw.end(), {static_cast<std::uint8_t>(vertex.groundHeightRaw & 0xff),
                static_cast<std::uint8_t>(vertex.groundHeightRaw >> 8),
                static_cast<std::uint8_t>(vertex.waterHeightAndFlagsRaw & 0xff),
                static_cast<std::uint8_t>(vertex.waterHeightAndFlagsRaw >> 8),
                vertex.textureAndFlagsRaw, vertex.variationRaw, vertex.cliffAndLayerRaw});
        }
    }
    return raw;
}

bool SameHeader(const TerrainHeader& a, const TerrainHeader& b) {
    return a.version == b.version && a.mainTileset == b.mainTileset && a.usesCustomTileset == b.usesCustomTileset
        && a.groundTilesets == b.groundTilesets && a.cliffTilesets == b.cliffTilesets && a.width == b.width
        && a.height == b.height && std::bit_cast<std::uint64_t>(a.offset) == std::bit_cast<std::uint64_t>(b.offset);
}

bool TerrainDiffers(const std::shared_ptr<const TerrainGridSnapshot>& terrain,
    const std::shared_ptr<const TerrainGridSnapshot>& original) noexcept {
    if (terrain == original) return false;
    if (!terrain || !original) return true;
    if (!SameHeader(terrain->header, original->header)) return true;
    return !std::equal(terrain->vertices.begin(), terrain->vertices.end(),
        original->vertices.begin(), original->vertices.end(), [](const TerrainVertex& a, const TerrainVertex& b) {
            return a.groundHeightRaw == b.groundHeightRaw && a.waterHeightAndFlagsRaw == b.waterHeightAndFlagsRaw
                && a.textureAndFlagsRaw == b.textureAndFlagsRaw && a.variationRaw == b.variationRaw
                && a.cliffAndLayerRaw == b.cliffAndLayerRaw;
        });
}

// 比较 [x0,x1)×[y0,y1) 覆盖的区块，只记录有差异的区块。
template <typename Chunk>
std::vector<Chunk> DiffChunks(const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    std::vector<Chunk> chunks;
    const auto width = before.header.width, height = before.header.height;
    for (std::uint32_t cy = y0 / kTerrainChunk * kTerrainChunk; cy < y1; cy += kTerrainChunk) {
        for (std::uint32_t cx = x0 / kTerrainChunk * kTerrainChunk; cx < x1; cx += kTerrainChunk) {
            const auto ex = std::min(cx + kTerrainChunk, width), ey = std::min(cy + kTerrainChunk, height);
            auto first = ReadChunk(before, cx, cy, ex, ey);
            auto second = ReadChunk(after, cx, cy, ex, ey);
            if (first != second) chunks.push_back({cx, cy, ex, ey, std::move(first), std::move(second)});
        }
    }
    return chunks;
}

} // namespace

Status MapDataEditSession::AttachTerrain(std::span<const std::byte> bytes) {
    if (terrainSourceRevision_ == std::numeric_limits<std::uint64_t>::max())
        return Status::Failure("MapData.Edit", "TERRAIN_SOURCE_REVISION_EXHAUSTED", "地形来源修订已用尽，请重新打开地图。");
    if (!history_.empty() || terrain_)
        return Status::Failure("MapData.Edit", "MAP_TERRAIN_ATTACH_LATE", "只能在编辑前附上一次地形来源");
    auto parsed = ParseMapTerrain(bytes);
    if (!parsed.Succeeded() || !parsed.value) {
        Status status;
        status.diagnostics = std::move(parsed.diagnostics);
        return status;
    }
    if (parsed.value->Snapshot()->header.version != 11)
        return Status::Failure("MapData.Edit", "MAP_TERRAIN_READ_ONLY", "新版地形只读，不能附入经典地形编辑事务");
    terrainOriginal_ = terrain_ = parsed.value->Snapshot();
    ++terrainSourceRevision_;
    return Status::Ok();
}

Status MapDataEditSession::AttachPathing(std::span<const std::byte> bytes) {
    if (terrainSourceRevision_ == std::numeric_limits<std::uint64_t>::max())
        return Status::Failure("MapData.Edit", "TERRAIN_SOURCE_REVISION_EXHAUSTED", "地形来源修订已用尽，请重新打开地图。");
    if (!terrain_) return Status::Failure("MapData.Edit", "MAP_TERRAIN_UNAVAILABLE", "须先附上源地形才能验证通行性尺寸");
    if (!history_.empty() || pathing_)
        return Status::Failure("MapData.Edit", "MAP_PATHING_ATTACH_LATE", "只能在编辑前附上一次通行性来源");
    auto parsed = ParseMapPathing(bytes, &terrain_->header);
    if (!parsed.Succeeded() || !parsed.value) {
        Status status;
        status.diagnostics = std::move(parsed.diagnostics);
        return status;
    }
    pathingOriginal_ = pathing_ = parsed.value->OriginalBytes();
    ++terrainSourceRevision_;
    return Status::Ok();
}

Status MapDataEditSession::SetPathing(std::span<const std::byte> bytes) {
    if (!terrain_ || !terrainOriginal_ || !pathing_ || !pathingOriginal_)
        return Status::Failure("MapData.Edit", "MAP_PATHING_UNAVAILABLE", "源地形或源通行性不可用，不能恢复通行性覆盖");
    if (const auto status = ValidatePathingOverlay(bytes, *pathingOriginal_, *terrainOriginal_, *terrain_);
        !status.Succeeded()) return status;
    if (std::equal(bytes.begin(), bytes.end(), pathing_->begin())) return Status::Ok();
    Command command;
    command.pathingBefore = *pathing_;
    command.pathingAfter = std::vector<std::byte>(bytes.begin(), bytes.end());
    return CommitCommand(std::move(command));
}

Status MapDataEditSession::SetTerrainAndPathing(std::span<const std::byte> terrainBytes,
    std::span<const std::byte> pathingBytes) {
    return SetTerrainAndPathingInternal(terrainBytes, pathingBytes, {});
}

Status MapDataEditSession::SetTerrainAndPathingDraft(std::span<const std::byte> terrainBytes,
    std::span<const std::byte> pathingBytes, std::span<const std::byte> restrictionDraft) {
    if (restrictionDraft.empty())
        return Status::Failure("MapData.Edit", "PATHING_DRAFT_MISSING", "附加通行限制缺少工程声明，拒绝恢复。");
    return SetTerrainAndPathingInternal(terrainBytes, pathingBytes, restrictionDraft);
}

Status MapDataEditSession::SetTerrainAndPathingInternal(std::span<const std::byte> terrainBytes,
    std::span<const std::byte> pathingBytes, std::span<const std::byte> restrictionDraft) {
    if (!terrain_ || !terrainOriginal_ || !pathing_ || !pathingOriginal_)
        return Status::Failure("MapData.Edit", "MAP_PATHING_UNAVAILABLE",
            "须先附上唯一的源地形与源通行性，才能联合恢复两种覆盖");
    auto parsed = ParseMapTerrain(terrainBytes);
    if (!parsed.Succeeded() || !parsed.value) {
        Status status;
        status.diagnostics = std::move(parsed.diagnostics);
        return status;
    }
    const auto& next = *parsed.value->Snapshot();
    if (next.header.version != 11)
        return Status::Failure("MapData.Edit", "MAP_TERRAIN_READ_ONLY", "新版地形只读，不能恢复为经典地形覆盖");
    const auto& header = terrain_->header;
    if (next.header.width != header.width || next.header.height != header.height
        || std::bit_cast<std::uint64_t>(next.header.offset) != std::bit_cast<std::uint64_t>(header.offset))
        return Status::Failure("MapData.Edit", "MAP_TERRAIN_SHAPE_CHANGED",
            "地形覆盖的尺寸或原点与源地形不同，不能联合恢复");
    std::vector<std::byte> withoutRestrictions;
    if (!restrictionDraft.empty()) {
        auto checked = StripPathingRestrictionDraft(*terrainOriginal_, *pathingOriginal_, pathingBytes, restrictionDraft);
        if (!checked.Succeeded() || !checked.value) return {checked.diagnostics};
        withoutRestrictions = std::move(*checked.value);
    }
    const auto rawOverlay = restrictionDraft.empty() ? pathingBytes : std::span<const std::byte>(withoutRestrictions);
    if (const auto status = ValidatePathingOverlay(rawOverlay, *pathingOriginal_, *terrainOriginal_, next);
        !status.Succeeded()) return status;
    Command command;
    command.terrain = DiffChunks<TerrainChunk>(*terrain_, next, 0, 0, header.width, header.height);
    if (!SameHeader(header, next.header)) { command.headerBefore = header; command.headerAfter = next.header; }
    if (!std::equal(pathingBytes.begin(), pathingBytes.end(), pathing_->begin())) {
        command.pathingBefore = *pathing_;
        command.pathingAfter = std::vector<std::byte>(pathingBytes.begin(), pathingBytes.end());
    }
    return CommitCommand(std::move(command));
}

Status MapDataEditSession::SetTerrain(std::span<const std::byte> bytes) {
    if (!terrain_) return Status::Failure("MapData.Edit", "MAP_TERRAIN_UNAVAILABLE", "源地形不可用，不能恢复地形覆盖");
    auto parsed = ParseMapTerrain(bytes);
    if (!parsed.Succeeded() || !parsed.value) {
        Status status;
        status.diagnostics = std::move(parsed.diagnostics);
        return status;
    }
    const auto& next = *parsed.value->Snapshot();
    if (next.header.version != 11)
        return Status::Failure("MapData.Edit", "MAP_TERRAIN_READ_ONLY", "新版地形只读，不能恢复为经典地形覆盖");
    const auto& header = terrain_->header;
    if (next.header.width != header.width || next.header.height != header.height
        || std::bit_cast<std::uint64_t>(next.header.offset) != std::bit_cast<std::uint64_t>(header.offset))
        return Status::Failure("MapData.Edit", "MAP_TERRAIN_SHAPE_CHANGED", "地形覆盖的尺寸或原点与源地形不同，不能恢复");
    Command command;
    command.terrain = DiffChunks<TerrainChunk>(*terrain_, next, 0, 0, header.width, header.height);
    if (!SameHeader(header, next.header)) { command.headerBefore = header; command.headerAfter = next.header; }
    return CommitCommand(std::move(command));
}

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainEdit(const TerrainBrush& brush,
    std::span<const TerrainStrokePoint> points, bool followDoodads) const {
    return PrepareTerrainEditInternal(brush, points, followDoodads, true);
}

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainEditInternal(const TerrainBrush& brush,
    std::span<const TerrainStrokePoint> points, bool followDoodads, bool inspectSaveGate) const {
    if (!terrain_) return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "MAP_TERRAIN_UNAVAILABLE", "地形不可编辑");
    if (terrainSourceRevision_ == std::numeric_limits<std::uint64_t>::max())
        return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "TERRAIN_SOURCE_REVISION_EXHAUSTED", "地形来源修订已用尽，请重新打开地图。");
    if (brush.kind == TerrainBrushKind::PathingRestriction)
        return PreparePathingRestrictionEdit(brush, points, followDoodads, inspectSaveGate);
    if (brush.kind == TerrainBrushKind::Blight && !pathing_)
        return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "MAP_PATHING_UNAVAILABLE",
            "荒芜笔刷需要唯一且可解析的源 WPM；不能让地形与通行性数据失配");
    auto next = std::make_shared<TerrainGridSnapshot>(*terrain_);
    auto stroke = ApplyTerrainStroke(*next, brush, points);
    if (!stroke.Succeeded() || !stroke.value) return {{}, std::move(stroke.diagnostics)};
    const auto& changed = *stroke.value;
    Command command;
    if (!changed.Empty()) command.terrain = DiffChunks<TerrainChunk>(*terrain_, *next, changed.x0, changed.y0, changed.x1, changed.y1);
    if (changed.headerChanged) { command.headerBefore = terrain_->header; command.headerAfter = next->header; }
    if (brush.kind == TerrainBrushKind::Blight && !changed.Empty()) {
        auto patched = PatchBlightPathing(*terrain_, *next, changed, *pathing_);
        if (!patched.Succeeded() || !patched.value) {
            Result<PreparedTerrainEdit> failed;
            failed.diagnostics = std::move(patched.diagnostics);
            return failed;
        }
        if (*patched.value != *pathing_) {
            command.pathingBefore = *pathing_;
            command.pathingAfter = std::move(*patched.value);
        }
    }
    const bool shaping = brush.kind == TerrainBrushKind::Raise || brush.kind == TerrainBrushKind::Lower
        || brush.kind == TerrainBrushKind::Plateau || brush.kind == TerrainBrushKind::Smooth
        || brush.kind == TerrainBrushKind::CliffLevel || brush.kind == TerrainBrushKind::Noise
        || brush.kind == TerrainBrushKind::Ramp;
    const auto& doodadBytes = current_[Index(MapDataKind::Doodads)];
    const auto& unitBytes = current_[Index(MapDataKind::Units)];
    if (followDoodads && shaping && !command.terrain.empty()) {
        // 层级变化还可能改变相邻坡道中点；再外扩一格覆盖这些中点周围的地面插值。
        const double tile = TerrainGridSnapshot::tileSize;
        const double left = next->header.offset[0] + (static_cast<double>(changed.x0) - 2) * tile;
        const double bottom = next->header.offset[1] + (static_cast<double>(changed.y0) - 2) * tile;
        const double right = next->header.offset[0] + (static_cast<double>(changed.x1) + 1) * tile;
        const double top = next->header.offset[1] + (static_cast<double>(changed.y1) + 1) * tile;
        // 保持与地面的相对高度：Z 加上该点地面高度的变化量，飞行高度等偏移不变。
        const auto follow = [&](auto& placements) {
            bool moved = false;
            for (auto& placement : placements) {
                const double x = placement.position[0], y = placement.position[1];
                if (x < left || x > right || y < bottom || y > top) continue;
                const auto before = SampleGroundHeight(*terrain_, x, y, TerrainHeightSource::RawWithRamps);
                const auto after = SampleGroundHeight(*next, x, y, TerrainHeightSource::RawWithRamps);
                if (!before || !after || *before == *after) continue;
                placement.position[2] += *after - *before;
                moved = true;
            }
            return moved;
        };
        const auto fail = [](std::vector<Diagnostic> diagnostics) {
            Result<PreparedTerrainEdit> failed; failed.diagnostics = std::move(diagnostics); return failed;
        };
        if (doodadBytes) {
            auto doodads = ParseDoodadPlacements(*doodadBytes);
            if (!doodads.Succeeded() || !doodads.value)
                return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "MAP_DOODADS_UNSAFE",
                    "装饰物放置无法解析，不能让装饰物随地面高度变化");
            if (follow(doodads.value->doodads))
                if (const auto status = Prepare(MapDataKind::Doodads, doodads.value->Serialize(), command.changes); !status.Succeeded())
                    return fail(status.diagnostics);
        }
        // 单位与物品同在 war3mapUnits.doo；原版 WE 改地形时它们也随地面升降，不能被埋进地里或悬空。
        if (unitBytes) {
            auto units = ParseUnitPlacements(*unitBytes);
            if (!units.Succeeded() || !units.value)
                return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "MAP_UNITS_UNSAFE",
                    "单位放置无法解析，不能让单位随地面高度变化");
            if (follow(units.value->units))
                if (const auto status = Prepare(MapDataKind::Units, units.value->Serialize(), command.changes); !status.Succeeded())
                    return fail(status.diagnostics);
        }
    }
    const auto doodadChange = std::find_if(command.changes.begin(), command.changes.end(),
        [](const auto& change) { return change.kind == MapDataKind::Doodads; });
    PreparedTerrainEdit prepared;
    prepared.before_ = terrain_;
    prepared.sessionIdentity_ = sessionIdentity_; prepared.revision_ = revision_;
    prepared.sourceRevision_ = terrainSourceRevision_;
    prepared.changed_ = changed;
    // 交互落笔没有预览回执需要，沿用原路径的成本；显式 dry-run 才做完整来源净差检查。
    // 两者实际保存仍必经同一个 VerifyTerrainPathingForSave，未检查不能报成通过。
    prepared.terrainPathingGate_ = inspectSaveGate ? VerifyTerrainCandidateForSave(next,
        command.pathingAfter ? command.pathingAfter : pathing_,
        doodadChange == command.changes.end() ? doodadBytes : doodadChange->after)
        : Status::Failure("MapData.Edit", "TERRAIN_SAVE_GATE_NOT_INSPECTED", "此落笔尚未检查地形保存资格。");
    prepared.brush_ = brush;
    prepared.points_.assign(points.begin(), points.end());
    prepared.followDoodads_ = followDoodads;
    prepared.command_ = std::make_shared<const Command>(std::move(command));
    return Result<PreparedTerrainEdit>::Success(std::move(prepared));
}

bool MapDataEditSession::PreparedTerrainEdit::HasChanges() const noexcept {
    return command_ && (!command_->changes.empty() || !command_->terrain.empty()
        || command_->headerAfter || command_->pathingAfter);
}

Result<TerrainStrokeResult> MapDataEditSession::ApplyPreparedTerrainEdit(const PreparedTerrainEdit& plan,
    const std::function<Status(const EditReceipt&)>& prepublish) {
    if (!plan.command_ || plan.sessionIdentity_ != sessionIdentity_ || plan.revision_ != revision_
        || plan.sourceRevision_ != terrainSourceRevision_ || plan.before_ != terrain_)
        return Result<TerrainStrokeResult>::Failure("MapData.Edit", "TERRAIN_EDIT_STALE",
            "地形候选不属于当前会话、修订或来源，请重新预览。");
    const auto status = CommitCommand(*plan.command_, prepublish);
    if (!status.Succeeded()) return {{}, status.diagnostics};
    return Result<TerrainStrokeResult>::Success(plan.changed_);
}

Result<TerrainStrokeResult> MapDataEditSession::EditTerrain(const TerrainBrush& brush,
    std::span<const TerrainStrokePoint> points, bool followDoodads) {
    auto prepared = PrepareTerrainEditInternal(brush, points, followDoodads, false);
    if (!prepared.Succeeded() || !prepared.value) return {{}, std::move(prepared.diagnostics)};
    return ApplyPreparedTerrainEdit(*prepared.value);
}

Result<TerrainStrokeResult> MapDataEditSession::EditTerrainTextureSlot(TerrainTextureTable table,
    std::uint32_t slot, const std::string& rawcode) {
    if (!terrain_) return Result<TerrainStrokeResult>::Failure("MapData.Edit", "MAP_TERRAIN_UNAVAILABLE", "地形不可编辑");
    auto next = std::make_shared<TerrainGridSnapshot>(*terrain_);
    auto result = ReplaceTerrainTextureSlot(*next, table, slot, rawcode);
    if (!result.Succeeded() || !result.value || !result.value->headerChanged) return result;
    Command command;
    command.headerBefore = terrain_->header;
    command.headerAfter = next->header;
    if (const auto status = CommitCommand(std::move(command)); !status.Succeeded()) {
        Result<TerrainStrokeResult> failed;
        failed.diagnostics = status.diagnostics;
        return failed;
    }
    return result;
}

bool MapDataEditSession::TerrainMatches(const Command& command, bool forward) const {
    if (command.terrain.empty() && !command.headerAfter) return true;
    if (!terrain_) return false;
    if (command.headerAfter && !SameHeader(terrain_->header, forward ? *command.headerBefore : *command.headerAfter)) return false;
    for (const auto& chunk : command.terrain)
        if (ReadChunk(*terrain_, chunk.x0, chunk.y0, chunk.x1, chunk.y1) != (forward ? chunk.before : chunk.after)) return false;
    return true;
}

bool MapDataEditSession::TerrainDirty() const noexcept {
    return TerrainDiffers(terrain_, terrainOriginal_);
}

Status MapDataEditSession::VerifyTerrainPathingForSave() const {
    return VerifyTerrainCandidateForSave(terrain_, pathing_, current_[Index(MapDataKind::Doodads)]);
}

Status MapDataEditSession::VerifyTerrainCandidateForSave(
    const std::shared_ptr<const TerrainGridSnapshot>& terrain,
    const std::optional<std::vector<std::byte>>& pathing,
    const std::optional<std::vector<std::byte>>& doodads) const {
    const auto reject = [](const char* reason) {
        return Status::Failure("MapData.Edit", "MAP_TERRAIN_PATHING_UNVERIFIED", reason);
    };
    if (!TerrainDiffers(terrain, terrainOriginal_)) {
        return pathing == pathingOriginal_ ? Status::Ok()
            : reject("W3E 与基线相同但 WPM 已改变，拒绝无地形对应的通行性覆盖");
    }
    if (!terrain || !terrainOriginal_ || !pathing || !pathingOriginal_)
        return reject("已修改地形但缺少源 W3E 或唯一可验证的 WPM，拒绝正式地图保存");
    if (!SameHeader(terrain->header, terrainOriginal_->header)
        || terrain->vertices.size() != terrainOriginal_->vertices.size())
        return reject("W3E 头部、贴图表或顶点数量已改变，尚无对应 WPM 生成规则");
    // 按源图到当前 W3E 的最终原始净差判定；工程恢复没有可靠的笔刷来源身份，
    // 不能只检查 Raise/Lower/Smooth 枚举来保护 UI 与 Agent 共用的保存路径。
    bool heightChanged = false;
    for (std::size_t index = 0; index < terrain->vertices.size(); ++index) {
        const auto& before = terrainOriginal_->vertices[index];
        const auto& after = terrain->vertices[index];
        if (before.waterHeightAndFlagsRaw != after.waterHeightAndFlagsRaw
            || before.variationRaw != after.variationRaw
            || before.cliffAndLayerRaw != after.cliffAndLayerRaw
            || ((before.textureAndFlagsRaw ^ after.textureAndFlagsRaw) & ~kBlightBit) != 0)
            return reject("W3E 修改了高度或荒芜位以外的字段，尚无安全的 WPM 联动规则");
        if (before.groundHeightRaw != after.groundHeightRaw) {
            heightChanged = true;
            const auto delta = before.groundHeightRaw > after.groundHeightRaw
                ? before.groundHeightRaw - after.groundHeightRaw : after.groundHeightRaw - before.groundHeightRaw;
            if (delta > kVerifiedHeightDeltaRaw)
                return reject("高度变化超过原版真保存已取证的每顶点 raw 净差 156（39 世界单位）");
            if (!HeightNeighborhoodSafe(*terrainOriginal_, *terrain, index))
                return reject("高度变化周边一格存在水面、坡道、悬崖边界或未识别标志");
        }
    }
    if (heightChanged && doodads != original_[Index(MapDataKind::Doodads)])
        return reject("高度变化联动了装饰物成员；装饰物通行性尚未取证");
    const auto width = (terrain->header.width - 1) * 4, height = (terrain->header.height - 1) * 4;
    const auto expectedBytes = kPathingHeaderBytes + static_cast<std::size_t>(width) * height;
    if (pathing->size() != expectedBytes || pathingOriginal_->size() != expectedBytes
        || !std::equal(pathing->begin(), pathing->begin() + kPathingHeaderBytes, pathingOriginal_->begin()))
        return reject("WPM 头部或格子数量与原成员不同");
    if (!PathingBlightDeltaMatchesTerrain(*pathingOriginal_, *pathing, *terrainOriginal_, *terrain))
        return reject("WPM 改动未逐格对应 W3E 荒芜位，或原始通行位发生变化");
    return Status::Ok();
}

} // namespace War3Map::MapData
