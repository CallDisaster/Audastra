#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainEditInspection.h"
#include "MapDataEditSessionTerrainInternal.h"
#include "TerrainPaletteEditInternal.h"
#include "War3MapCore/MapData/MapPathing.h"
#include "War3MapCore/MapData/PlacementElevationInvariant.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"
#include "War3MapCore/MapData/TerrainNativeUnitsProof.h"
#include "War3MapCore/MapData/TerrainNativeDoodadsProof.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"

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
// 原版六种地形通行查询在没有水的格上不读取 groundHeightRaw：边界只看XY，
// 悬崖/坡道只看固定层级与分类，材质属性也独立于高度。因此，只要下列
// 原始字段和对象占地保持不变，合法raw高度的任意净差均保留原WPM。
// 这不是坡度近似或幅度白名单；水与未知标志仍明确拒绝。
bool DryHeightPathingInvariant(const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
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
                || original.UnknownTextureFlags() != 0 || edited.UnknownTextureFlags() != 0
                || original.CliffLevel() != level || edited.CliffLevel() != level) return false;
        }
    }
    return true;
}

// 水深直接由原始高度计算时，不能夹带相邻坡道的派生高度变化。
bool PlainWaterNeighborhood(const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    std::size_t index) {
    const auto width = before.header.width, height = before.header.height;
    const auto x = static_cast<std::uint32_t>(index % width), y = static_cast<std::uint32_t>(index / width);
    if (x == 0 || y == 0 || x + 1 >= width || y + 1 >= height) return false;
    for (std::uint32_t ny = y - 1; ny <= y + 1; ++ny) for (std::uint32_t nx = x - 1; nx <= x + 1; ++nx) {
        for (const auto* grid : {&before, &after}) {
            const auto& vertex = grid->vertices[static_cast<std::size_t>(ny) * width + nx];
            if (vertex.IsRamp() || vertex.IsBoundary() || vertex.IsMapEdge()
                || vertex.UnknownWaterFlags() || vertex.UnknownTextureFlags()) return false;
        }
    }
    return true;
}

bool SameCliffPathingFootprint(const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    std::size_t index) {
    if (!PlainWaterNeighborhood(before, after, index)) return false;
    const auto width = before.header.width;
    const auto x = static_cast<std::uint32_t>(index % width), y = static_cast<std::uint32_t>(index / width);
    // 无坡道时原版 C 只取该地格四角层级是否不同；四个受影响地格均须不变。
    const auto blocked = [width](const TerrainGridSnapshot& grid, std::uint32_t tx, std::uint32_t ty) {
        const auto sw = static_cast<std::size_t>(ty) * width + tx;
        const auto level = grid.vertices[sw].CliffLevel();
        return grid.vertices[sw + 1].CliffLevel() != level || grid.vertices[sw + width].CliffLevel() != level
            || grid.vertices[sw + width + 1].CliffLevel() != level;
    };
    for (auto ty = y - 1; ty <= y; ++ty) for (auto tx = x - 1; tx <= x; ++tx)
        if (blocked(before, tx, ty) != blocked(after, tx, ty)) return false;
    return true;
}

bool PathingBlightDeltaMatchesTerrain(std::span<const std::byte> source, std::span<const std::byte> current,
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after, std::uint8_t separatelyVerified = 0) {
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
                if (((sourceCell ^ currentCell) & ~separatelyVerified) != 0) return false;
            } else if (((sourceCell & kBlightBit) != 0) != sourceVertex.IsBlight()
                || ((currentCell & kBlightBit) != 0) != currentVertex.IsBlight()
                || ((sourceCell ^ currentCell) & ~(kBlightBit | separatelyVerified)) != 0) return false;
        }
    }
    return true;
}

Status ValidatePathingOverlay(std::span<const std::byte> bytes, std::span<const std::byte> original,
    const TerrainGridSnapshot& originalTerrain, const TerrainGridSnapshot& nextTerrain,
    std::uint8_t separatelyVerified = 0) {
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
        if ((difference & ~(kBlightBit | separatelyVerified)) != 0)
            return Status::Failure("MapData.Edit", "MAP_PATHING_UNKNOWN_BITS", "通行性覆盖改动了未验证位，拒绝恢复");
    }
    if (!PathingBlightDeltaMatchesTerrain(original, bytes, originalTerrain, nextTerrain, separatelyVerified))
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
    // 一次预留覆盖块数，避免增长期间old/new向量同时存活，超出调用方已收取的区块预算。
    if (x0 < x1 && y0 < y1) chunks.reserve(
        std::size_t((x1 - 1) / kTerrainChunk - x0 / kTerrainChunk + 1)
        * ((y1 - 1) / kTerrainChunk - y0 / kTerrainChunk + 1));
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
    auto restrictions = std::make_shared<const std::vector<std::byte>>(bytes.size() - kPathingHeaderBytes);
    std::vector<std::byte> protectedBytes;
    protectedBytes.reserve(bytes.size() - kPathingHeaderBytes);
    for (std::size_t i = kPathingHeaderBytes; i < bytes.size(); ++i)
        protectedBytes.push_back(bytes[i] & std::byte{kPathingRestrictionBits});
    auto protectedSource = std::make_shared<const std::vector<std::byte>>(std::move(protectedBytes));
    pathingOriginal_ = pathing_ = parsed.value->OriginalBytes();
    pathingProtected_.swap(protectedSource);
    pathingRestrictions_.swap(restrictions);
    originalPathingRestrictions_ = pathingRestrictions_;
    ++terrainSourceRevision_;
    return Status::Ok();
}

Status MapDataEditSession::SetPathing(std::span<const std::byte> bytes) {
    if (!terrain_ || !terrainOriginal_ || !pathing_ || !pathingOriginal_)
        return Status::Failure("MapData.Edit", "MAP_PATHING_UNAVAILABLE", "源地形或源通行性不可用，不能恢复通行性覆盖");
    if (const auto status = ValidatePathingOverlay(bytes, *pathingOriginal_, *terrainOriginal_, *terrain_);
        !status.Succeeded()) return status;
    const auto declaration = EncodePathingRestrictionDraft(*terrainOriginal_, *pathingOriginal_, bytes,
        *pathingProtected_, *originalPathingRestrictions_, *pathingRestrictions_);
    if (!declaration.Succeeded()) return {declaration.diagnostics};
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
    auto protectedSource = pathingProtected_;
    auto savedIntent = originalPathingRestrictions_;
    auto intent = savedIntent;
    if (!restrictionDraft.empty()) {
        auto checked = DecodePathingRestrictionLayers(*terrainOriginal_, *pathingOriginal_, pathingBytes, restrictionDraft);
        if (!checked.Succeeded() || !checked.value) return {checked.diagnostics};
        if (checked.value->protectedSource != *protectedSource)
            protectedSource = std::make_shared<const std::vector<std::byte>>(std::move(checked.value->protectedSource));
        if (checked.value->savedIntent != *savedIntent)
            savedIntent = std::make_shared<const std::vector<std::byte>>(std::move(checked.value->savedIntent));
        intent = checked.value->intent == *savedIntent ? savedIntent
            : std::make_shared<const std::vector<std::byte>>(std::move(checked.value->intent));
    }
    const bool restoreLayers = protectedSource != pathingProtected_ || savedIntent != originalPathingRestrictions_;
    if (restoreLayers) {
        // 工程分层只能在原始地形/通行会话中恢复。不能在旧历史下面偷换保护来源。
        const bool hasTerrainHistory = std::any_of(history_.begin(), history_.end(), [](const auto& command) {
            return !command->terrain.empty() || command->headerAfter || command->pathingAfter || command->restrictionAfter;
        });
        if (hasTerrainHistory || TerrainDirty() || pathing_ != pathingOriginal_
            || pathingRestrictions_ != originalPathingRestrictions_
            || terrainSourceRevision_ == std::numeric_limits<std::uint64_t>::max())
            return Status::Failure("MapData.Edit", "PATHING_DRAFT_REBIND_LATE", "须在地形或通行编辑前恢复工程来源分层。");
    }
    if (const auto status = ValidatePathingOverlay(pathingBytes, *pathingOriginal_, *terrainOriginal_, next,
            restrictionDraft.empty() ? 0 : kPathingRestrictionBits);
        !status.Succeeded()) return status;
    Command command;
    command.terrain = DiffChunks<TerrainChunk>(*terrain_, next, 0, 0, header.width, header.height);
    const auto beforeIntent = restoreLayers ? savedIntent : pathingRestrictions_;
    if (*intent != *beforeIntent) {
        command.restrictionBefore = beforeIntent;
        command.restrictionAfter = intent;
    }
    if (!SameHeader(header, next.header)) { command.headerBefore = header; command.headerAfter = next.header; }
    if (!std::equal(pathingBytes.begin(), pathingBytes.end(), pathing_->begin())) {
        command.pathingBefore = *pathing_;
        command.pathingAfter = std::vector<std::byte>(pathingBytes.begin(), pathingBytes.end());
    }
    const auto previousProtected = pathingProtected_, previousSaved = originalPathingRestrictions_, previousIntent = pathingRestrictions_;
    if (restoreLayers) {
        pathingProtected_ = protectedSource;
        originalPathingRestrictions_ = savedIntent;
        pathingRestrictions_ = savedIntent;
    }
    const auto rollback = [&] {
        pathingProtected_ = previousProtected;
        originalPathingRestrictions_ = previousSaved;
        pathingRestrictions_ = previousIntent;
    };
    try {
        const auto status = CommitCommand(std::move(command));
        if (!status.Succeeded()) { rollback(); return status; }
        if (restoreLayers) ++terrainSourceRevision_;
        return status;
    } catch (...) { rollback(); throw; }
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

Result<MapDataEditSession::TerrainJournalDraftCost> MapDataEditSession::MeasureTerrainJournalDraft(
    std::size_t maximumBytes, std::size_t maximumWork) const {
    using Output = Result<TerrainJournalDraftCost>;
    const auto fail = [] { return Output::Failure("Terrain.Journal", "TERRAIN_REPLAY_BUDGET", "完整raw草稿驻留或输出比较超过恢复预算。"); };
    TerrainJournalDraftCost cost;
    const auto add = [](std::size_t& used, std::size_t limit, std::size_t count, std::size_t element = 1) {
        if (count > (limit - used) / element) return false;
        used += count * element; return true;
    };
    const auto bytes = [&](const auto& value, bool compare = true) {
        return !value || (add(cost.retainedBytes, maximumBytes, value->capacity())
            && (!compare || add(cost.comparisonWork, maximumWork, value->size())));
    };
    const auto header = [&](const TerrainHeader& value) {
        for (const auto* table : {&value.groundTilesets, &value.cliffTilesets}) {
            if (!add(cost.retainedBytes, maximumBytes, table->capacity(), sizeof(std::string))) return false;
            for (const auto& id : *table) if (!add(cost.retainedBytes, maximumBytes, id.capacity())) return false;
        }
        return true;
    };
    const auto special = [&](const auto& value) {
        if (!value) return true;
        if (!add(cost.retainedBytes, maximumBytes, value->capacity(), sizeof(SpecialDoodadPlacement))) return false;
        for (const auto& item : *value) if (!add(cost.retainedBytes, maximumBytes, item.rawcode.capacity())) return false;
        return true;
    };
    const auto bounds = [&](const auto& value) {
        return !value || (add(cost.retainedBytes, maximumBytes, sizeof(TerrainBoundsState))
            && add(cost.retainedBytes, maximumBytes, value->mapEdges.capacity())
            && add(cost.comparisonWork, maximumWork, value->mapEdges.size()));
    };
    const auto starts = [&](const auto& value) {
        return !value || (add(cost.retainedBytes, maximumBytes, sizeof(TerrainStartLocationsState) + 64)
            && add(cost.retainedBytes, maximumBytes, value->unitsSha256.capacity() + 1));
    };
    const auto grid = [&](const auto& value, bool compare) {
        if (!value) return true;
        if (!add(cost.retainedBytes, maximumBytes, sizeof(TerrainGridSnapshot) + 64)
            || !add(cost.retainedBytes, maximumBytes, value->vertices.capacity(), sizeof(TerrainVertex))
            || !add(cost.retainedBytes, maximumBytes, value->tiles.capacity(), sizeof(TerrainTile))
            || !header(value->header)) return false;
        return !compare || add(cost.comparisonWork, maximumWork, value->vertices.size(), 8);
    };
    const auto resize = [&](const auto& value, bool compare) {
        return !value || (add(cost.retainedBytes, maximumBytes, sizeof(TerrainResizeState) + 128)
            && header(value->header) && grid(value->geometry, false)
            && add(cost.retainedBytes, maximumBytes, value->shadow.name.capacity() + value->rulesSha256.capacity() + 2)
            && add(cost.retainedBytes, maximumBytes, value->shadow.bytes.capacity())
            && (!compare || add(cost.comparisonWork, maximumWork, value->shadow.bytes.size()))
            && bytes(value->historyPathing, compare) && bytes(value->protectedPathing, compare) && bytes(value->savedRestrictions, compare));
    };
    if (!add(cost.comparisonWork, maximumWork, 1024)
        || !special(specialDoodads_) || !special(savedSpecialDoodads_)
        || !bounds(boundsState_) || !bounds(savedBoundsState_)
        || !starts(startLocations_) || !starts(savedStartLocations_)
        || !resize(resizeState_, true) || !resize(savedResizeState_, false) || !resize(resizeDraftState_, true)) return fail();
    for (std::size_t i = 0; i < original_.size(); ++i) {
        if (!bytes(original_[i]) || !bytes(current_[i])) return fail();
        if (preservedSources_[i]) {
            if (!add(cost.retainedBytes, maximumBytes, preservedSources_[i]->sources.capacity(), sizeof(MapArchiveMember))) return fail();
            for (const auto& member : preservedSources_[i]->sources)
                if (!add(cost.retainedBytes, maximumBytes, member.data.capacity())
                    || !add(cost.retainedBytes, maximumBytes, member.name.capacity())
                    || !add(cost.comparisonWork, maximumWork, member.data.size())) return fail();
        }
    }
    for (const auto& grid : {terrainOriginal_, terrain_}) if (grid) {
        if (!add(cost.retainedBytes, maximumBytes, grid->vertices.capacity(), sizeof(TerrainVertex))
            || !add(cost.retainedBytes, maximumBytes, grid->tiles.capacity(), sizeof(TerrainTile))
            || !header(grid->header) || !add(cost.comparisonWork, maximumWork, grid->vertices.size(), 8)) return fail();
        for (const auto& table : {&grid->header.groundTilesets, &grid->header.cliffTilesets})
            for (const auto& id : *table) if (!add(cost.comparisonWork, maximumWork, id.size())) return fail();
    }
    if (!bytes(pathingOriginal_) || !bytes(pathing_) || !bytes(pathingProtected_)
        || !bytes(originalPathingRestrictions_) || !bytes(pathingRestrictions_)) return fail();
    // expected可来自API宿主。已有raw历史也计驻留，不能先克隆巨型DOO/WTS再报不符。
    if (history_.size() > 4096) return fail();
    if (!add(cost.retainedBytes, maximumBytes, history_.capacity(), sizeof(std::shared_ptr<const Command>))) return fail();
    for (const auto& command : history_) {
        if (!add(cost.retainedBytes, maximumBytes, 1, sizeof(Command))
            || !add(cost.retainedBytes, maximumBytes, command->changes.capacity(), sizeof(MemberChange))
            || !add(cost.retainedBytes, maximumBytes, command->terrain.capacity(), sizeof(TerrainChunk))
            || !special(command->specialBefore) || !special(command->specialAfter)
            || !bounds(command->boundsBefore) || !bounds(command->boundsAfter)
            || !starts(command->startsBefore) || !starts(command->startsAfter)
            || !grid(command->fullTerrainBefore, false) || !grid(command->fullTerrainAfter, false)
            || !resize(command->resizeBefore, false) || !resize(command->resizeAfter, false)
            || !resize(command->resizeDraftBefore, false) || !resize(command->resizeDraftAfter, false)
            || !add(cost.retainedBytes, maximumBytes, command->boundsRulesSha256.capacity())
            || (command->headerBefore && !header(*command->headerBefore))
            || (command->headerAfter && !header(*command->headerAfter))) return fail();
        for (const auto& member : command->changes) if (!bytes(member.before, false) || !bytes(member.after, false)) return fail();
        for (const auto& chunk : command->terrain)
            if (!add(cost.retainedBytes, maximumBytes, chunk.before.capacity())
                || !add(cost.retainedBytes, maximumBytes, chunk.after.capacity())) return fail();
        if (!bytes(command->pathingBefore, false) || !bytes(command->pathingAfter, false)
            || !bytes(command->restrictionBefore, false) || !bytes(command->restrictionAfter, false)) return fail();
        if (command->terrainStroke) {
            const auto& stroke = *command->terrainStroke;
            if (!add(cost.retainedBytes, maximumBytes, stroke.intent.points.capacity(), sizeof(TerrainStrokePoint))
                || !add(cost.retainedBytes, maximumBytes, stroke.rulesSha256.capacity())) return fail();
            if (stroke.intent.palette && (!add(cost.retainedBytes, maximumBytes, stroke.intent.palette->expectedRawcode.capacity())
                || !add(cost.retainedBytes, maximumBytes, stroke.intent.palette->replacementRawcode.capacity()))) return fail();
            if (stroke.intent.blockPaste
                && !add(cost.retainedBytes, maximumBytes, stroke.intent.blockPaste->expectedBlockSha256.capacity() + 1)) return fail();
        }
        if (command->placements) {
            if (!add(cost.retainedBytes, maximumBytes, command->placements->capacity(), sizeof(PlacementRowLineage))) return fail();
            for (const auto& row : *command->placements)
                if (!add(cost.retainedBytes, maximumBytes, row.beforeToAfter.capacity(), sizeof(std::optional<std::uint32_t>))) return fail();
        }
    }
    return Output::Success(cost);
}

bool MapDataEditSession::MatchesTerrainJournalDraft(const MapDataEditSession& expected) const noexcept {
    const auto equalLayer = [](const auto& a, const auto& b) { return bool(a) == bool(b) && (!a || *a == *b); };
    if (states_ != expected.states_ || memberNames_ != expected.memberNames_
        || original_ != expected.original_ || current_ != expected.current_
        || pathingOriginal_ != expected.pathingOriginal_ || pathing_ != expected.pathing_
        || !equalLayer(pathingProtected_, expected.pathingProtected_)
        || !equalLayer(originalPathingRestrictions_, expected.originalPathingRestrictions_)
        || !equalLayer(pathingRestrictions_, expected.pathingRestrictions_)) return false;
    const auto aResize = resizeState_ ? resizeState_ : resizeDraftState_;
    const auto bResize = expected.resizeState_ ? expected.resizeState_ : expected.resizeDraftState_;
    // raw工程只提供待核输出，不提供来源资格；仍逐字比较SHD及各shape映射层。
    if (bool(aResize) != bool(bResize)) return false;
    const auto* aShadow = TerrainHistoryShadow(); const auto* bShadow = expected.TerrainHistoryShadow();
    if (aResize && (!aShadow || !bShadow || aShadow->name != bShadow->name
        || aShadow->present != bShadow->present || aShadow->bytes != bShadow->bytes
        || !equalLayer(TerrainHistoryProtected(), expected.TerrainHistoryProtected())
        || !equalLayer(TerrainHistorySavedRestrictions(), expected.TerrainHistorySavedRestrictions())
        || TerrainHistoryPathing() != expected.TerrainHistoryPathing())) return false;
    for (std::size_t i = 0; i < preservedSources_.size(); ++i) {
        const auto& a = preservedSources_[i]; const auto& b = expected.preservedSources_[i];
        if (bool(a) != bool(b)) return false;
        if (!a) continue;
        if (a->sources.size() != b->sources.size()) return false;
        for (std::size_t n = 0; n < a->sources.size(); ++n)
            if (a->sources[n].name != b->sources[n].name || a->sources[n].data != b->sources[n].data) return false;
    }
    const auto equalGrid = [](const auto& a, const auto& b) {
        if (bool(a) != bool(b)) return false;
        if (!a) return true;
        if (!SameHeader(a->header, b->header) || a->vertices.size() != b->vertices.size()) return false;
        return std::equal(a->vertices.begin(), a->vertices.end(), b->vertices.begin(), [](const auto& x, const auto& y) {
            return x.groundHeightRaw == y.groundHeightRaw && x.waterHeightAndFlagsRaw == y.waterHeightAndFlagsRaw
                && x.textureAndFlagsRaw == y.textureAndFlagsRaw && x.variationRaw == y.variationRaw
                && x.cliffAndLayerRaw == y.cliffAndLayerRaw && x.textureFlagsExtensionRaw == y.textureFlagsExtensionRaw
                && x.extendedTextureLayout == y.extendedTextureLayout;
        });
    };
    // 不比较私有special指针；raw候选只能以完整DOO字节参加最终输出核对。
    return equalGrid(terrainOriginal_, expected.terrainOriginal_) && equalGrid(terrain_, expected.terrain_);
}

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainEdit(const TerrainBrush& brush,
    std::span<const TerrainStrokePoint> points, bool followDoodads, std::size_t maximumLevelPropagationWork) const {
    return PrepareTerrainEditInternal(brush, points, followDoodads, true, maximumLevelPropagationWork);
}

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainEditInternal(const TerrainBrush& brush,
    std::span<const TerrainStrokePoint> points, bool followDoodads, bool inspectSaveGate,
    std::size_t maximumLevelPropagationWork) const {
    if (!terrain_) return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "MAP_TERRAIN_UNAVAILABLE", "地形不可编辑");
    if (terrainSourceRevision_ == std::numeric_limits<std::uint64_t>::max())
        return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "TERRAIN_SOURCE_REVISION_EXHAUSTED", "地形来源修订已用尽，请重新打开地图。");
    if (brush.kind == TerrainBrushKind::PathingRestriction)
        return PreparePathingRestrictionEdit(brush, points, followDoodads, inspectSaveGate);
    if (brush.kind == TerrainBrushKind::Blight && !pathing_)
        return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "MAP_PATHING_UNAVAILABLE",
            "荒芜笔刷需要唯一且可解析的源 WPM；不能让地形与通行性数据失配");
    auto stroke = PrepareTerrainStroke(*terrain_, brush, points, maximumLevelPropagationWork);
    if (!stroke.Succeeded() || !stroke.value) return {{}, std::move(stroke.diagnostics)};
    auto next = std::make_shared<TerrainGridSnapshot>(std::move(stroke.value->terrain));
    const bool shaping = brush.kind == TerrainBrushKind::Raise || brush.kind == TerrainBrushKind::Lower
        || brush.kind == TerrainBrushKind::Plateau || brush.kind == TerrainBrushKind::Smooth
        || brush.kind == TerrainBrushKind::CliffLevel || brush.kind == TerrainBrushKind::Noise
        || brush.kind == TerrainBrushKind::Ramp;
    auto prepared = PrepareTerrainCandidate(std::move(next), stroke.value->result, points, followDoodads,
        shaping, brush.kind == TerrainBrushKind::Blight, inspectSaveGate);
    if (prepared.value) prepared.value->brush_ = brush;
    return prepared;
}

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainCandidate(
    std::shared_ptr<const TerrainGridSnapshot> next, const TerrainStrokeResult& changed,
    std::span<const TerrainStrokePoint> points, bool followDoodads, bool shaping,
    bool synchronizeBlight, bool inspectSaveGate, DoodadPlacements* parsedDoodads, bool specialChanged,
    std::optional<std::uint8_t> nativeMaximumCliffLevel, std::size_t maximumNativeFollowWork) const {
    Command command;
    if (!changed.Empty()) command.terrain = DiffChunks<TerrainChunk>(*terrain_, *next, changed.x0, changed.y0, changed.x1, changed.y1);
    if (changed.headerChanged) { command.headerBefore = terrain_->header; command.headerAfter = next->header; }
    bool blightChanged = false;
    if (synchronizeBlight) {
        for (auto y = changed.y0; y < changed.y1 && !blightChanged; ++y)
            for (auto x = changed.x0; x < changed.x1 && !blightChanged; ++x) {
                const auto index = static_cast<std::size_t>(y) * terrain_->header.width + x;
                blightChanged = terrain_->vertices[index].IsBlight() != next->vertices[index].IsBlight();
            }
    }
    if (blightChanged) {
        if (!pathing_) return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "MAP_PATHING_UNAVAILABLE",
            "本笔改变了荒芜，但缺少可解析的源WPM，不能提交失配的地形。");
        auto patched = PatchBlightPathing(*terrain_, *next, changed, *pathing_);
        if (!patched.Succeeded() || !patched.value) {
            Result<PreparedTerrainEdit> failed;
            failed.diagnostics = std::move(patched.diagnostics);
            return failed;
        }
        // 已有真实blight净差，Patch又逐格确认旧位与before一致；成功后必有位改变，
        // 不必再扫描整份WPM比较一次来重新确认同一事实。
        command.pathingBefore = *pathing_;
        command.pathingAfter = std::move(*patched.value);
    }
    const auto& doodadBytes = current_[Index(MapDataKind::Doodads)];
    const auto& unitBytes = current_[Index(MapDataKind::Units)];
    std::size_t nativeFollowWork = 0;
    if (followDoodads && shaping && !command.terrain.empty()) {
        // 完整R/S分类还可读取第二圈；层级变化影响两格外的坡道及其插值地格。
        const double tile = TerrainGridSnapshot::tileSize;
        const double halo = nativeMaximumCliffLevel ? 3 : 2;
        const double left = next->header.offset[0] + (static_cast<double>(changed.x0) - halo) * tile;
        const double bottom = next->header.offset[1] + (static_cast<double>(changed.y0) - halo) * tile;
        const double right = next->header.offset[0] + (static_cast<double>(changed.x1) + halo - 1) * tile;
        const double top = next->header.offset[1] + (static_cast<double>(changed.y1) + halo - 1) * tile;
        bool unavailableHeight = false;
        // 保持与地面的相对高度：Z 加上该点地面高度的变化量，飞行高度等偏移不变。
        const auto follow = [&](auto& placements) -> Result<bool> {
            bool moved = false;
            for (auto& placement : placements) {
                const double x = placement.position[0], y = placement.position[1];
                if (x < left || x > right || y < bottom || y > top) continue;
                // 地图外放置不属于地表；扩展分类halo不能把既有外置物变成拒绝原因。
                if (x < next->header.offset[0] || y < next->header.offset[1]
                    || x > next->header.offset[0] + (next->header.width - 1) * tile
                    || y > next->header.offset[1] + (next->header.height - 1) * tile) continue;
                if (nativeMaximumCliffLevel) {
                    // 前后各四角，每次完整分类至多512操作；只对实际采样点提前收费。
                    constexpr std::size_t queryWork = 8 * 512;
                    if (queryWork > maximumNativeFollowWork - nativeFollowWork)
                        return Result<bool>::Failure("MapData.Edit", "TERRAIN_WATER_SESSION_BUDGET",
                            "地物完整坡道高度跟随超出本笔预算，尚未修改会话。");
                    nativeFollowWork += queryWork;
                }
                const auto before = SampleGroundHeight(*terrain_, x, y, TerrainHeightSource::RawWithRamps, nativeMaximumCliffLevel);
                const auto after = SampleGroundHeight(*next, x, y, TerrainHeightSource::RawWithRamps, nativeMaximumCliffLevel);
                if (!before || !after) { unavailableHeight |= nativeMaximumCliffLevel.has_value(); continue; }
                if (*before == *after) continue;
                placement.position[2] += *after - *before;
                moved = true;
            }
            return Result<bool>::Success(moved);
        };
        const auto fail = [](std::vector<Diagnostic> diagnostics) {
            Result<PreparedTerrainEdit> failed; failed.diagnostics = std::move(diagnostics); return failed;
        };
        // 随地面升降只改 Z，行身份全部原位保留；记录逐行谱系，世界视图据此增量更新，不整张重建。
        std::vector<PlacementRowLineage> lineage;
        const auto keepRows = [&lineage](MapDataKind kind, std::size_t count) {
            PlacementRowLineage map; map.kind = kind;
            map.beforeCount = map.afterCount = static_cast<std::uint32_t>(count);
            map.beforeToAfter.reserve(count);
            for (std::uint32_t row = 0; row < count; ++row) map.beforeToAfter.emplace_back(row);
            lineage.push_back(std::move(map));
        };
        if (doodadBytes) {
            std::optional<DoodadPlacements> ownedDoodads;
            if (!parsedDoodads) {
                auto parsed = ParseDoodadPlacements(*doodadBytes);
                if (!parsed.Succeeded() || !parsed.value)
                    return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "MAP_DOODADS_UNSAFE",
                        "装饰物放置无法解析，不能让装饰物随地面高度变化");
                ownedDoodads = std::move(*parsed.value);
                parsedDoodads = &*ownedDoodads;
            }
            const auto followed = follow(parsedDoodads->doodads);
            if (!followed.Succeeded() || !followed.value) return fail(followed.diagnostics);
            if (*followed.value) {
                if (const auto status = Prepare(MapDataKind::Doodads, parsedDoodads->Serialize(), command.changes); !status.Succeeded())
                    return fail(status.diagnostics);
                keepRows(MapDataKind::Doodads, parsedDoodads->doodads.size());
            }
        }
        // 单位与物品同在 war3mapUnits.doo；原版 WE 改地形时它们也随地面升降，不能被埋进地里或悬空。
        if (unitBytes) {
            auto units = ParseUnitPlacements(*unitBytes);
            if (!units.Succeeded() || !units.value)
                return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "MAP_UNITS_UNSAFE",
                    "单位放置无法解析，不能让单位随地面高度变化");
            const auto followed = follow(units.value->units);
            if (!followed.Succeeded() || !followed.value) return fail(followed.diagnostics);
            if (*followed.value) {
                if (const auto status = PrepareUnits(*units.value, command.changes, true, &command); !status.Succeeded())
                    return fail(status.diagnostics);
                keepRows(MapDataKind::Units, units.value->units.size());
            }
        }
        if (unavailableHeight)
            return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "TERRAIN_CLIFF_FOLLOW_HEIGHT",
                "实际跟随地物的坡道高度邻域无法完整验证，整笔未修改。");
        if (!lineage.empty()) command.placements = std::make_shared<const std::vector<PlacementRowLineage>>(std::move(lineage));
    }
    if (specialChanged) {
        if (!parsedDoodads || !specialDoodads_)
            return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "TERRAIN_SPECIAL_STATE_UNAVAILABLE", "特殊模型删除缺少可信来源状态。");
        // 普通Z联动可能已生成同一DOO替换；最终只保留一份完整原子成员。
        command.changes.erase(std::remove_if(command.changes.begin(), command.changes.end(),
            [](const auto& item) { return item.kind == MapDataKind::Doodads; }), command.changes.end());
        if (const auto status = Prepare(MapDataKind::Doodads, parsedDoodads->Serialize(), command.changes); !status.Succeeded())
            return {{}, status.diagnostics};
        command.specialBefore = specialDoodads_;
        command.specialAfter = std::make_shared<const std::vector<SpecialDoodadPlacement>>(parsedDoodads->specialDoodads);
        if (!command.placements) {
            PlacementRowLineage lineage; lineage.kind = MapDataKind::Doodads;
            lineage.beforeCount = lineage.afterCount = static_cast<std::uint32_t>(parsedDoodads->doodads.size());
            for (std::uint32_t i = 0; i < lineage.beforeCount; ++i) lineage.beforeToAfter.emplace_back(i);
            command.placements = std::make_shared<const std::vector<PlacementRowLineage>>(std::vector{std::move(lineage)});
        }
    }
    const auto doodadChange = std::find_if(command.changes.begin(), command.changes.end(),
        [](const auto& change) { return change.kind == MapDataKind::Doodads; });
    const auto unitChange = std::find_if(command.changes.begin(), command.changes.end(),
        [](const auto& change) { return change.kind == MapDataKind::Units; });
    const auto& candidateUnits = unitChange == command.changes.end() ? unitBytes : unitChange->after;
    PreparedTerrainEdit prepared;
    prepared.before_ = terrain_;
    prepared.after_ = next;
    prepared.sessionIdentity_ = sessionIdentity_; prepared.revision_ = revision_;
    prepared.sourceRevision_ = terrainSourceRevision_;
    prepared.changed_ = changed;
    // 交互落笔没有预览回执需要，沿用原路径的成本；显式 dry-run 才做完整来源净差检查。
    // 两者实际保存仍必经同一个 VerifyTerrainPathingForSave，未检查不能报成通过。
    prepared.terrainPathingGate_ = inspectSaveGate ? VerifyTerrainCandidateForSave(next,
        command.pathingAfter ? command.pathingAfter : pathing_,
        doodadChange == command.changes.end() ? doodadBytes : doodadChange->after, nullptr, nullptr, &candidateUnits)
        : Status::Failure("MapData.Edit", "TERRAIN_SAVE_GATE_NOT_INSPECTED", "此落笔尚未检查地形保存资格。");
    prepared.points_.assign(points.begin(), points.end());
    prepared.followDoodads_ = followDoodads;
    prepared.waterPreparationWork_ = nativeFollowWork;
    prepared.command_ = std::make_shared<const Command>(std::move(command));
    return Result<PreparedTerrainEdit>::Success(std::move(prepared));
}

bool MapDataEditSession::PreparedTerrainEdit::HasChanges() const noexcept {
    return command_ && (!command_->changes.empty() || !command_->terrain.empty()
        || command_->headerAfter || command_->pathingAfter || command_->restrictionAfter);
}

std::size_t MapDataEditSession::PreparedTerrainEdit::RetainedCommandBytes() const noexcept {
    if (!HasChanges()) return 0;
    auto bytes = sizeof(Command) + 64 + 2 * sizeof(std::shared_ptr<void>);
    const auto add = [&bytes](std::size_t count, std::size_t element = 1) {
        const auto maximum = std::numeric_limits<std::size_t>::max();
        bytes = element && count > (maximum - bytes) / element ? maximum : bytes + count * element;
    };
    const auto raw = [&add](const auto& value) { if (value) add(value->capacity()); };
    add(command_->terrain.capacity(), sizeof(TerrainChunk));
    // Apply只为有界且锚点可表达的笔划附加typed记录；预算先覆盖其最终保留容量。
    if (points_.size() <= 4096) {
        add(points_.size(), sizeof(TerrainStrokePoint));
        add(sizeof(TerrainJournalStroke) + brush_.groundTexture.capacity() + 65);
        if (blockPaste_) add(blockPaste_->expectedBlockSha256.capacity() + 1);
    }
    for (const auto& chunk : command_->terrain) { add(chunk.before.capacity()); add(chunk.after.capacity()); }
    add(command_->changes.capacity(), sizeof(MemberChange));
    for (const auto& change : command_->changes) { raw(change.before); raw(change.after); }
    raw(command_->pathingBefore); raw(command_->pathingAfter);
    raw(command_->restrictionBefore); raw(command_->restrictionAfter);
    for (const auto* restrictions : {&command_->restrictionBefore, &command_->restrictionAfter})
        if (*restrictions) add(sizeof(std::vector<std::byte>) + 64);
    for (const auto* header : {&command_->headerBefore, &command_->headerAfter}) if (*header)
        for (const auto* table : {&(*header)->groundTilesets, &(*header)->cliffTilesets}) {
            add(table->capacity(), sizeof(std::string));
            for (const auto& value : *table) add(value.capacity() + 1);
        }
    if (command_->terrainStroke) {
        add(command_->terrainStroke->rulesSha256.capacity() + 1);
        if (command_->terrainStroke->intent.palette) {
            add(command_->terrainStroke->intent.palette->expectedRawcode.capacity() + 1);
            add(command_->terrainStroke->intent.palette->replacementRawcode.capacity() + 1);
        }
        if (command_->terrainStroke->intent.blockPaste)
            add(command_->terrainStroke->intent.blockPaste->expectedBlockSha256.capacity() + 1);
    }
    if (command_->placements) {
        add(sizeof(std::vector<PlacementRowLineage>) + 64);
        add(command_->placements->capacity(), sizeof(PlacementRowLineage));
        for (const auto& rows : *command_->placements) add(rows.beforeToAfter.capacity(), sizeof(std::optional<std::uint32_t>));
    }
    for (const auto* specials : {&command_->specialBefore, &command_->specialAfter}) if (*specials) {
        add(sizeof(std::vector<SpecialDoodadPlacement>) + 64);
        add((*specials)->capacity(), sizeof(SpecialDoodadPlacement));
        for (const auto& special : **specials) add(special.rawcode.capacity() + 1);
    }
    for (const auto* starts : {&command_->startsBefore, &command_->startsAfter}) if (*starts) {
        add(sizeof(TerrainStartLocationsState) + 64);
        add((*starts)->unitsSha256.capacity() + 1);
    }
    return bytes;
}

Result<TerrainStrokeResult> MapDataEditSession::ApplyPreparedTerrainEdit(const PreparedTerrainEdit& plan,
    const std::function<Status(const EditReceipt&)>& prepublish) {
    if (!plan.command_ || plan.sessionIdentity_ != sessionIdentity_ || plan.revision_ != revision_
        || plan.sourceRevision_ != terrainSourceRevision_ || plan.before_ != terrain_)
        return Result<TerrainStrokeResult>::Failure("MapData.Edit", "TERRAIN_EDIT_STALE",
            "地形候选不属于当前会话、修订或来源，请重新预览。");
    auto command = *plan.command_;
    // 超出持久化域不影响原始编辑。不能截断/拆笔或持久化caller锚点冒充来源。
    bool expressible = plan.blockPaste_ || (!plan.points_.empty() && plan.points_.size() <= 4096);
    TerrainJournalStroke stroke;
    if (expressible) {
        stroke.intent.points = plan.points_; stroke.intent.followDoodads = plan.followDoodads_;
        if (plan.blockPaste_) {
            expressible = plan.points_.empty() && !plan.waterRulesSha256_.empty()
                && ValidateTerrainBlockPasteIntent(*plan.blockPaste_).Succeeded();
            stroke.intent.blockPaste = plan.blockPaste_; stroke.rulesSha256 = plan.waterRulesSha256_;
        } else if (plan.waterBrush_) {
            expressible = expressible && plan.waterIntentOptions_ && !plan.waterRulesSha256_.empty()
                && plan.waterIntentOptions_->algorithm == plan.waterBrush_->algorithm;
            if (plan.waterBrush_->algorithm == TerrainWaterAlgorithm::NativeRamps) {
                // 私有位只由已完成实际max/完整锚点复核的Core候选设置；来源、before指针与
                // revision仍由上面的统一CAS门限定，不在提交时重复未计费的坡道查询。
                expressible = expressible && plan.waterAnchorVerified_;
            } else {
                const auto anchor = CaptureTerrainWaterStrokeAnchor(*plan.before_, plan.points_.front());
                const auto& actual = plan.waterBrush_->anchor;
                expressible = expressible && plan.waterBrush_->algorithm == TerrainWaterAlgorithm::LegacyFlat
                    && anchor.Succeeded() && anchor.value && !actual.rampHeightOffset
                    && anchor.value->groundHeightRaw == actual.groundHeightRaw && anchor.value->waterHeightRaw == actual.waterHeightRaw
                    && anchor.value->cliffLevel == actual.cliffLevel && anchor.value->hasWater == actual.hasWater;
            }
            stroke.intent.waterBrush = plan.waterIntentOptions_; stroke.rulesSha256 = plan.waterRulesSha256_;
        } else {
            stroke.intent.brush = plan.brush_;
            if (plan.brush_.kind == TerrainBrushKind::CliffLevel) {
                stroke.rulesSha256 = plan.waterRulesSha256_;
                stroke.cliffEnforceWaterLimits = plan.cliffEnforceWaterLimits_;
                // 规则摘要缺失时可编辑，但不能伪装成旧算法并持久化成可重放记录。
                if (plan.waterResult_ && (!stroke.cliffEnforceWaterLimits || stroke.rulesSha256.empty())) expressible = false;
            }
            if (plan.brush_.kind == TerrainBrushKind::CornerRamp) {
                stroke.rulesSha256 = plan.waterRulesSha256_;
                stroke.intent.cornerRampAlgorithm = plan.cornerRampAlgorithm_;
                expressible = expressible && plan.points_.size() == 1 && plan.waterResult_ && !stroke.rulesSha256.empty();
            }
            if (plan.brush_.kind == TerrainBrushKind::StraightRamp) {
                stroke.rulesSha256 = plan.waterRulesSha256_;
                stroke.intent.straightRampAlgorithm = plan.straightRampAlgorithm_;
                expressible = expressible && plan.points_.size() == 1 && plan.waterResult_ && !stroke.rulesSha256.empty();
            }
            if (plan.brush_.anchor) {
                const auto anchor = CaptureTerrainStrokeAnchor(*plan.before_, plan.points_.front());
                expressible = expressible && anchor.Succeeded() && anchor.value
                    && anchor.value->groundHeightRaw == plan.brush_.anchor->groundHeightRaw
                    && anchor.value->cliffLevel == plan.brush_.anchor->cliffLevel;
            }
            stroke.intent.brush.anchor.reset();
        }
    }
    if (expressible) command.terrainStroke = std::move(stroke);
    const auto status = CommitCommand(std::move(command), prepublish);
    if (!status.Succeeded()) return {{}, status.diagnostics};
    return Result<TerrainStrokeResult>::Success(plan.changed_);
}

Result<TerrainStrokeResult> MapDataEditSession::EditTerrain(const TerrainBrush& brush,
    std::span<const TerrainStrokePoint> points, bool followDoodads) {
    auto prepared = PrepareTerrainEditInternal(brush, points, followDoodads, false);
    if (!prepared.Succeeded() || !prepared.value) return {{}, std::move(prepared.diagnostics)};
    return ApplyPreparedTerrainEdit(*prepared.value);
}

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PrepareTerrainTextureSlot(TerrainTextureTable table,
    std::uint32_t slot, const std::string& rawcode, const TerrainPathingResources* resources) const {
    using Output = Result<PreparedTerrainEdit>;
    if (!terrain_) return Output::Failure("MapData.Edit", "MAP_TERRAIN_UNAVAILABLE", "地形不可编辑");
    auto next = std::make_shared<TerrainGridSnapshot>(*terrain_);
    auto result = ReplaceTerrainTextureSlot(*next, table, slot, rawcode);
    if (!result.Succeeded() || !result.value) return {{}, result.diagnostics};
    PreparedTerrainEdit plan;
    plan.before_ = terrain_; plan.after_ = next; plan.sessionIdentity_ = sessionIdentity_;
    plan.revision_ = revision_; plan.sourceRevision_ = terrainSourceRevision_; plan.changed_ = *result.value;
    Command command;
    if (!result.value->headerChanged) {
        plan.command_ = std::make_shared<const Command>(std::move(command));
        return Output::Success(std::move(plan));
    }
    command.headerBefore = terrain_->header;
    command.headerAfter = next->header;
    if (resources) {
        const auto& palette = table == TerrainTextureTable::Ground ? terrain_->header.groundTilesets : terrain_->header.cliffTilesets;
        const bool append = slot == palette.size() && slot < (table == TerrainTextureTable::Ground ? 16u : 15u);
        if (!terrainOriginal_ || (!append && slot >= palette.size())
            || terrain_->header.groundTilesets.empty() || terrain_->header.groundTilesets.size() > 16
            || terrain_->header.cliffTilesets.empty() || terrain_->header.cliffTilesets.size() > 15)
            return Output::Failure("Terrain.Journal", "TERRAIN_PALETTE_JOURNAL_DOMAIN",
                "可信贴图记录只支持既有槽替换、16槽以内的地面表尾追加或15槽以内的悬崖表尾追加。");
        if (resizeState_) {
            const auto shape = VerifyTerrainResizeForSave();
            if (!shape.Succeeded()) return {{}, shape.diagnostics};
        }
        const auto bound = resources->VerifyWaterBrushInput(resizeState_ ? *terrain_ : *terrainOriginal_, *next);
        if (!bound.Succeeded()) return {{}, bound.diagnostics};
        const auto index = Index(MapDataKind::Doodads);
        DoodadPlacements doodads;
        if (current_[index]) {
            auto parsed = ParseDoodadPlacements(*current_[index]);
            if (!parsed.Succeeded() || !parsed.value) return {{}, std::move(parsed.diagnostics)};
            doodads = std::move(*parsed.value);
        } else if (states_[index] != MapDataState::Missing)
            return Output::Failure("Terrain.Journal", "TERRAIN_PALETTE_DOODADS_UNAVAILABLE", "贴图记录需要完整当前DOO或明确缺失身份。");
        const auto& objectSource = resizeState_ ? current_[index] : original_[index];
        const auto objects = resources->VerifyWaterBrushDoodads(doodads,
            objectSource ? std::span<const std::byte>{*objectSource} : std::span<const std::byte>{});
        if (!objects.Succeeded()) return {{}, objects.diagnostics};
        const auto digest = resources->WaterBrushDependencyFingerprint();
        if (!digest.Succeeded() || !digest.value) return {{}, digest.diagnostics};
        TerrainJournalStroke stroke;
        stroke.intent.palette = TerrainPaletteEditIntent{table, slot, append ? std::string{} : palette[slot], rawcode,
            static_cast<std::uint8_t>(append ? (table == TerrainTextureTable::Ground ? 2 : 3) : 1)};
        stroke.rulesSha256 = *digest.value;
        command.terrainStroke = std::move(stroke);
    }
    plan.command_ = std::make_shared<const Command>(std::move(command));
    return Output::Success(std::move(plan));
}

Result<TerrainStrokeResult> MapDataEditSession::EditTerrainTextureSlot(TerrainTextureTable table,
    std::uint32_t slot, const std::string& rawcode, const TerrainPathingResources* resources) {
    auto prepared = PrepareTerrainTextureSlot(table, slot, rawcode, resources);
    if (!prepared.Succeeded() || !prepared.value) return {{}, prepared.diagnostics};
    return ApplyPreparedTerrainEdit(*prepared.value);
}

bool MapDataEditSession::TerrainMatches(const Command& command, bool forward) const {
    if (command.fullTerrainAfter) {
        const auto expected = forward ? command.fullTerrainBefore : command.fullTerrainAfter;
        if (!terrain_ || !expected || !SameHeader(terrain_->header, expected->header)
            || terrain_->vertices.size() != expected->vertices.size()) return false;
        for (std::size_t i = 0; i < terrain_->vertices.size(); ++i) {
            const auto& a = terrain_->vertices[i]; const auto& b = expected->vertices[i];
            if (a.groundHeightRaw != b.groundHeightRaw || a.waterHeightAndFlagsRaw != b.waterHeightAndFlagsRaw
                || a.textureAndFlagsRaw != b.textureAndFlagsRaw || a.variationRaw != b.variationRaw
                || a.cliffAndLayerRaw != b.cliffAndLayerRaw || a.extendedTextureLayout != b.extendedTextureLayout
                || a.textureFlagsExtensionRaw != b.textureFlagsExtensionRaw) return false;
        }
        return true;
    }
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

Status MapDataEditSession::VerifyTerrainSourceBaseline(std::span<const std::byte> sourceBytes) const {
    if (!terrainOriginal_ || !terrain_)
        return Status::Failure("MapData.Edit", "TERRAIN_EDIT_INSPECTION_SOURCE_UNSAFE", "地形会话缺少原始或当前 W3E 快照。");
    const auto serialized = SerializeMapTerrain(*terrainOriginal_);
    if (!serialized.Succeeded() || !serialized.value)
        return Status::Failure("MapData.Edit", "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH", "无法序列化地形原始基线进行来源核对。");
    if (*serialized.value != std::vector<std::byte>(sourceBytes.begin(), sourceBytes.end()))
        return Status::Failure("MapData.Edit", "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH", "W3E原始字节与编辑会话基线不同。");
    return Status::Ok();
}

Status MapDataEditSession::VerifyTerrainDoodadChange(std::span<const std::byte> before,
    std::span<const std::byte> after) const {
    if (std::equal(before.begin(), before.end(), after.begin(), after.end())) return Status::Ok();
    auto original = ParseDoodadPlacements(before), current = ParseDoodadPlacements(after);
    if (!original.Succeeded() || !original.value) return {original.diagnostics};
    if (!current.Succeeded() || !current.value) return {current.diagnostics};
    const auto provenance = VerifyTerrainSpecialDoodadProvenance(*original.value, *current.value);
    if (!provenance.Succeeded()) return provenance;
    // 旧仅Z保存域不变。新普通DOO变换/增删必须由native双阶段保存链另持私有证明。
    current.value->specialDoodads = original.value->specialDoodads;
    const auto normalized = current.value->Serialize();
    if (!normalized.Succeeded() || !normalized.value) return {normalized.diagnostics};
    return VerifyDoodadPlacementElevationOnly(before, *normalized.value);
}

Status MapDataEditSession::VerifyTerrainSpecialDoodadChange(std::span<const std::byte> before,
    std::span<const std::byte> after) const {
    if (std::equal(before.begin(), before.end(), after.begin(), after.end())) return Status::Ok();
    auto original = ParseDoodadPlacements(before), current = ParseDoodadPlacements(after);
    if (!original.Succeeded() || !original.value) return {original.diagnostics};
    if (!current.Succeeded() || !current.value) return {current.diagnostics};
    return VerifyTerrainSpecialDoodadProvenance(*original.value, *current.value);
}

Status MapDataEditSession::VerifyTerrainSpecialDoodadProvenance(
    const DoodadPlacements& before, const DoodadPlacements& after) const {
    const auto same = [](const auto& a, const auto& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
            return x.rawcode == y.rawcode && x.variation == y.variation && x.position == y.position;
        });
    };
    if (!specialDoodads_ || !savedSpecialDoodads_
        || !same(before.specialDoodads, *savedSpecialDoodads_)
        || !same(after.specialDoodads, *specialDoodads_))
        return Status::Failure("MapData.Edit", "TERRAIN_SPECIAL_PROVENANCE_REQUIRED", "特殊模型集合未由本会话可信水刷事务生成或恢复；请撤销该摆放修改，或从实际地图来源重放水刷。");
    return Status::Ok();
}

Status MapDataEditSession::VerifyTerrainCandidateForSave(
    const std::shared_ptr<const TerrainGridSnapshot>& terrain,
    const std::optional<std::vector<std::byte>>& pathing,
    const std::optional<std::vector<std::byte>>& doodads,
    const TerrainPathingResources* resources, const std::vector<std::byte>* restrictions,
    const std::optional<std::vector<std::byte>>* units, bool nativeRebuild,
    const TerrainNativeUnitsProof* nativeUnits, const TerrainNativeDoodadsProof* nativeDoodads) const {
    const auto reject = [](const char* reason) {
        return Status::Failure("MapData.Edit", "MAP_TERRAIN_PATHING_UNVERIFIED", reason);
    };
    if (resizeDraftState_) return reject("扩裁raw inspection不持有typed来源资格；必须从实际来源完整重放后才能保存。");
    // 即使可信范围已经保存且W3E/WPM无净差，也不能让随后raw设置或草稿覆写
    // 跳过密封geometry/MapEdge复核。
    if (boundsState_) {
        const auto settings = VerifyTerrainSettingsForSave();
        if (!settings.Succeeded()) return settings;
    }
    if (!TerrainDiffers(terrain, terrainOriginal_) && pathing == pathingOriginal_
        && specialDoodads_ == savedSpecialDoodads_ && !HasTerrainBoundsChanges()
        && (!terrain || doodads == original_[Index(MapDataKind::Doodads)])) return Status::Ok();
    if (!terrain || !terrainOriginal_ || !pathing || !pathingOriginal_)
        return reject("已修改地形但缺少源 W3E 或唯一可验证的 WPM，拒绝正式地图保存");
    if (!pathingProtected_ || !originalPathingRestrictions_ || !pathingRestrictions_)
        return reject("通行修改缺少受保护来源和保存前后的独立意图，拒绝正式地图保存");
    const bool resize = resizeState_ && (!SameHeader(terrainOriginal_->header, terrain->header) || HasTerrainResizeChanges());
    if (!resize) {
        const auto layers = VerifyPathingRestrictionLayers(terrainOriginal_->header, *pathingOriginal_, *pathing,
            *pathingProtected_, *originalPathingRestrictions_, restrictions ? *restrictions : *pathingRestrictions_);
        if (!layers.Succeeded()) return layers;
    }
    const auto checkPlacements = [&]() -> Status {
        const auto settings = VerifyTerrainSettingsForSave();
        if (!settings.Succeeded()) return settings;
        const auto check = [&](MapDataKind kind, const auto& current, auto verify) -> Status {
            const auto& before = original_[Index(kind)];
            if (before == current) return Status::Ok();
            if (!before || !current) return reject("地形通行不变保存不能新增或删除摆放成员");
            const auto status = verify(*before, *current);
            if (status.Succeeded()) return status;
            auto result = reject("摆放高度变化未通过核对，或同时改变了XY、朝向、缩放等其它字段");
            result.diagnostics.insert(result.diagnostics.end(), status.diagnostics.begin(), status.diagnostics.end());
            return result;
        };
        const auto doo = check(MapDataKind::Doodads, doodads, [&](const auto& before, const auto& after) {
            if (nativeRebuild && nativeDoodads) {
                // 私有证明在预收预算后核过完整codec和特殊谱系；此处只复核
                // 同一session/revision及完整成员身份，不再无预算解析第二遍。
                return nativeDoodads->Verify(*this, before, after);
            }
            return nativeRebuild ? VerifyTerrainDoodadChange(before, after) : VerifyDoodadPlacementElevationOnly(before, after);
        });
        if (!doo.Succeeded()) return doo;
        return check(MapDataKind::Units, units ? *units : current_[Index(MapDataKind::Units)],
            [&](const auto& before, const auto& after) {
                // 证明只能由完整native保存链按剩余额度构造；普通不变性检查
                // 保持仅Z规则。此处不把Units动态占地错误烘焙到静态WPM。
                return nativeRebuild && nativeUnits
                    ? nativeUnits->Verify(before, after, SessionIdentity(), Revision(), TerrainSourceRevision())
                    : VerifyUnitPlacementElevationOnly(before, after);
            });
    };
    if (resize) {
        if (!nativeRebuild || terrain != terrain_ || pathing != pathing_
            || (restrictions && *restrictions != *pathingRestrictions_))
            return reject("外矩形扩裁必须由同一会话的完整原版G0/G1保存链重建，不能保留或替换任意WPM。");
        const auto shape = VerifyTerrainResizeForSave();
        return shape.Succeeded() ? checkPlacements() : shape;
    }
    if (specialDoodads_ != savedSpecialDoodads_ && !nativeRebuild)
        return reject("可信水刷改变了特殊模型集合，必须分别重算旧/新DOO的完整通行贡献。");
    if (HasTerrainBoundsChanges() && !nativeRebuild)
        return reject("地图范围已变，必须分别按旧/新W3I重算完整通行贡献。");
    if (!TerrainDiffers(terrain, terrainOriginal_)) {
        if (const auto checked = checkPlacements(); !checked.Succeeded()) return checked;
        return PathingBlightDeltaMatchesTerrain(*pathingOriginal_, *pathing, *terrainOriginal_, *terrain, kPathingRestrictionBits)
            ? Status::Ok() : reject("W3E 未变，WPM 只能写入已声明的本工程通行限制变化");
    }
    const auto& oldHeader = terrainOriginal_->header;
    const auto& newHeader = terrain->header;
    const bool resourceHeader = (resources || nativeRebuild) && oldHeader.version == newHeader.version
        && oldHeader.version == 11 && oldHeader.mainTileset == newHeader.mainTileset
        && oldHeader.width == newHeader.width && oldHeader.height == newHeader.height
        && std::bit_cast<std::uint64_t>(oldHeader.offset) == std::bit_cast<std::uint64_t>(newHeader.offset)
        // 悬崖表替换或严格前缀型末尾增减仍须实际旧/新行绑定及完整G0/G1。
        // 撤回末尾槽供保存后Undo；不放行中段删除、重排或“保留原通行”路径。
        && (oldHeader.cliffTilesets == newHeader.cliffTilesets
            || (nativeRebuild && TerrainPaletteEditDetail::SupportsCliffTableDelta(oldHeader.cliffTilesets, newHeader.cliffTilesets)))
        && !newHeader.groundTilesets.empty()
        && newHeader.groundTilesets.size() <= 16;
    if ((!SameHeader(newHeader, oldHeader) && !resourceHeader)
        || terrain->vertices.size() != terrainOriginal_->vertices.size())
        return reject("W3E 头部、贴图表或顶点数量已改变，尚无对应 WPM 生成规则");
    // 按源图到当前 W3E 的最终原始净差判定；工程恢复没有可靠的笔刷来源身份，
    // 不能只检查 Raise/Lower/Smooth 枚举来保护 UI 与 Agent 共用的保存路径。
    bool invariantChanged = !SameHeader(newHeader, oldHeader)
        || doodads != original_[Index(MapDataKind::Doodads)];
    // 上方已完整核对私有范围状态绑定的旧/新W3I与全部MapEdge位。
    // 仅原版G0/G1重建可消费该可信0x4000变化，未知0x8000及无owner草稿仍拒绝。
    const auto protectedWaterFlags = nativeRebuild && boundsState_ ? 0x8000u : 0xc000u;
    for (std::size_t index = 0; index < terrain->vertices.size(); ++index) {
        const auto& before = terrainOriginal_->vertices[index];
        const auto& after = terrain->vertices[index];
        const bool waterChanged = before.waterHeightAndFlagsRaw != after.waterHeightAndFlagsRaw;
        const bool cliffChanged = before.CliffLevel() != after.CliffLevel();
        const bool cliffVariationChanged = before.CliffVariation() != after.CliffVariation();
        const bool cliffTextureChanged = before.CliffTextureIndex() != after.CliffTextureIndex();
        if ((waterChanged && ((!resources && !nativeRebuild) || ((before.waterHeightAndFlagsRaw ^ after.waterHeightAndFlagsRaw) & protectedWaterFlags) != 0))
            || (cliffVariationChanged && !nativeRebuild)
            || (before.cliffAndLayerRaw != after.cliffAndLayerRaw && ((!resources && !nativeRebuild)
                || (cliffTextureChanged && !nativeRebuild)))
            || ((before.textureAndFlagsRaw ^ after.textureAndFlagsRaw)
                // Boundary仅在完整原版G0/G1路径放行；原WPM必须逐格解释，
                // 新边界按所属地格SW记录重算，不能沿用“通行不变”的输出。
                & ~(kBlightBit | ((resources || nativeRebuild) ? 0x0f : 0) | (nativeRebuild ? 0xd0 : 0))) != 0)
            return reject("地形修改包含尚未验证的悬崖、坡道、水标志或边界变化，不能保留原 WPM");
        if (before.GroundVariation() != after.GroundVariation() && after.GroundVariation() > 17)
            return reject("新地面变体不在已支持的 0 到 17 范围内，未知变体只能原样保留");
        const auto ground = [](const TerrainHeader& header, const TerrainVertex& vertex) -> std::string_view {
            return vertex.GroundTextureIndex() < header.groundTilesets.size()
                ? std::string_view(header.groundTilesets[vertex.GroundTextureIndex()]) : std::string_view{};
        };
        const auto oldGround = ground(oldHeader, before), newGround = ground(newHeader, after);
        if (before.GroundTextureIndex() != after.GroundTextureIndex() && (oldGround.empty() || newGround.empty()))
            return reject("地面贴图索引越界，无法确认修改前后的通行属性；未修改的历史索引可原样保留");
        if (nativeRebuild) {
            // 此内部路径仅跳过“原通行必须不变”的比较；正式计划仍须用真实规则
            // 对完整旧/新输入重算，并逐格核对旧WPM。普通公开保存门不走此分支。
            // 水刷会从既有悬崖表选槽并生成0..2变体；平地slot15可选材质并在撤销时复原。
            // 此例外只使用共享的完整邻域检查，未知旧变体不能借覆盖清除后获准保存。
            // 资源owner另核对CliffTypes的真实旧/新行，不能单凭槽位合法授予规则来源。
            if ((cliffChanged || cliffTextureChanged || cliffVariationChanged)
                && (before.CliffVariation() > 2 || after.CliffVariation() > 2
                    || ((before.CliffTextureIndex() >= oldHeader.cliffTilesets.size()
                        || after.CliffTextureIndex() >= newHeader.cliffTilesets.size()
                        || before.CliffTextureIndex() >= 15 || after.CliffTextureIndex() >= 15)
                        && !IsNativeTerrainCliffSentinelTransition(*terrainOriginal_, *terrain, index))))
                return reject("悬崖修改需要有效槽和0到2变体；未选材质哨兵只允许无坡道、边界或未知字段的平地完整邻域互转");
            if (before.groundHeightRaw != after.groundHeightRaw
                && ((before.groundHeightRaw | after.groundHeightRaw) & 0xc000) != 0)
                return reject("高度修改超出现有14位可写范围，未知wire高度位必须原样保留");
            invariantChanged = true;
            continue;
        }
        if (oldGround != newGround && (!resources || !resources->EquivalentGround(oldGround, newGround)))
            return reject("旧/新地面贴图的实际 walkable、flyable、buildable 属性不同或缺少可信来源，不能保留原 WPM");
        invariantChanged = invariantChanged || before.variationRaw != after.variationRaw
            || oldGround != newGround || waterChanged || cliffChanged;
        if (cliffChanged && (!SameCliffPathingFootprint(*terrainOriginal_, *terrain, index)
            || !resources->EquivalentWater(before, after)))
            return reject("悬崖修改改变了通行阻断轮廓或水深分类，或涉及坡道/边界；尚需重建通行性");
        if (waterChanged && (!PlainWaterNeighborhood(*terrainOriginal_, *terrain, index)
            || !resources->EquivalentWater(before, after)))
            return reject("水面修改跨过浅深水或严格覆水阈值，或涉及坡道/边界；尚需重建通行性");
        if (before.groundHeightRaw != after.groundHeightRaw) {
            invariantChanged = true;
            // 不扩大原笔刷的可写高度范围。wire bit14/15即使parser保留，
            // 也不能借“通行不变”把未知高度语义变成新保存能力。
            if ((before.groundHeightRaw & 0xc000) != 0 || (after.groundHeightRaw & 0xc000) != 0)
                return reject("高度修改超出现有14位可写范围，未知wire高度位必须原样保留");
            if (!DryHeightPathingInvariant(*terrainOriginal_, *terrain, index)
                && (!resources || !PlainWaterNeighborhood(*terrainOriginal_, *terrain, index)
                    || !resources->EquivalentWater(before, after)))
                return reject("高度变化周边一格存在水面、坡道、悬崖边界或未识别标志");
        }
    }
    // 源限制和地形贡献各自已验证；只改变普通摆放Z不会改变对象的XY通行占地。
    // 荒芜与本工程限制合并时，同样不能混入其它摆放或范围变化。
    for (std::size_t i = kPathingHeaderBytes; !invariantChanged && i < pathing->size(); ++i)
        invariantChanged = (((*pathing)[i] ^ (*pathingOriginal_)[i]) & std::byte{kPathingRestrictionBits}) != std::byte{0};
    if (invariantChanged) {
        if (const auto checked = checkPlacements(); !checked.Succeeded()) return checked;
    }
    const auto width = (terrain->header.width - 1) * 4, height = (terrain->header.height - 1) * 4;
    const auto expectedBytes = kPathingHeaderBytes + static_cast<std::size_t>(width) * height;
    if (pathing->size() != expectedBytes || pathingOriginal_->size() != expectedBytes
        || !std::equal(pathing->begin(), pathing->begin() + kPathingHeaderBytes, pathingOriginal_->begin()))
        return reject("WPM 头部或格子数量与原成员不同");
    if (!PathingBlightDeltaMatchesTerrain(*pathingOriginal_, *pathing, *terrainOriginal_, *terrain, kPathingRestrictionBits))
        return reject("WPM 改动未逐格对应 W3E 荒芜位，或原始通行位发生变化");
    return Status::Ok();
}

} // namespace War3Map::MapData
