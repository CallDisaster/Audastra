#include "War3MapCore/MapData/TerrainDoodadsJournal.h"

#include "TerrainJournalDelta.h"
#include "War3MapCore/MapData/TerrainClassicDoodads.h"
#include "War3MapCore/MapData/TerrainStrokeIntent.h"
#include "War3MapCore/MapData/TerrainStrokeJournal.h"

#include <algorithm>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.Journal";
constexpr std::size_t kMaximumBytes = 64u * 1024u * 1024u;

}

bool IsTerrainDoodadsJournalPlaceholder(const TerrainStrokeIntent& intent) noexcept {
    const TerrainStrokeIntent neutral;
    return !intent.palette && !intent.bounds && !intent.waterBrush && !intent.blockPaste && !intent.resize && !intent.followDoodads
        && intent.points.empty() && EqualTerrainJournalIntent(intent, neutral);
}

bool EqualTerrainJournalIntent(const TerrainStrokeIntent& a, const TerrainStrokeIntent& b) noexcept {
    const auto& x = a.brush; const auto& y = b.brush;
    if (x.kind != y.kind || x.shape != y.shape || x.radius != y.radius || x.groundTexture != y.groundTexture
        || x.cliffTexture != y.cliffTexture || x.levelDelta != y.levelDelta || x.value != y.value || x.strength != y.strength
        || x.seed != y.seed || x.anchor || y.anchor || x.size != y.size || x.variation != y.variation
        || x.randomizeVariation != y.randomizeVariation || x.waterHeight != y.waterHeight || x.pathingRestrictions != y.pathingRestrictions
        || a.followDoodads != b.followDoodads || a.cornerRampAlgorithm != b.cornerRampAlgorithm
        || a.straightRampAlgorithm != b.straightRampAlgorithm || a.points.size() != b.points.size()
        || a.waterBrush.has_value() != b.waterBrush.has_value() || a.bounds.has_value() != b.bounds.has_value()
        || a.palette.has_value() != b.palette.has_value() || a.blockPaste.has_value() != b.blockPaste.has_value()
        || a.resize != b.resize) return false;
    for (std::size_t i = 0; i < a.points.size(); ++i)
        if (a.points[i].x != b.points[i].x || a.points[i].y != b.points[i].y) return false;
    if (a.waterBrush) {
        const auto& p = *a.waterBrush; const auto& q = *b.waterBrush;
        if (p.kind != q.kind || p.shape != q.shape || p.size != q.size || p.cliffTexture != q.cliffTexture
            || p.randomSeed != q.randomSeed || p.enforceWaterLimits != q.enforceWaterLimits || p.algorithm != q.algorithm) return false;
    }
    if (a.bounds) {
        const auto& p = *a.bounds; const auto& q = *b.bounds;
        if (p.algorithm != q.algorithm || p.playable.west != q.playable.west || p.playable.south != q.playable.south
            || p.playable.east != q.playable.east || p.playable.north != q.playable.north) return false;
    }
    if (a.palette) {
        const auto& p = *a.palette; const auto& q = *b.palette;
        if (p.algorithm != q.algorithm || p.table != q.table || p.slot != q.slot
            || p.expectedRawcode != q.expectedRawcode || p.replacementRawcode != q.replacementRawcode) return false;
    }
    if (a.blockPaste) {
        const auto& p = *a.blockPaste; const auto& q = *b.blockPaste;
        if (p.algorithm != q.algorithm || p.sourceCells.x0 != q.sourceCells.x0 || p.sourceCells.y0 != q.sourceCells.y0
            || p.sourceCells.x1 != q.sourceCells.x1 || p.sourceCells.y1 != q.sourceCells.y1
            || p.destinationVertex != q.destinationVertex || p.expectedBlockSha256 != q.expectedBlockSha256) return false;
    }
    return true;
}

Result<TerrainDoodadsJournalCost> MeasureTerrainDoodadsJournalEdit(
    std::size_t beforeBytes, std::size_t afterBytes, std::size_t maximumBytes, std::size_t maximumWork) {
    using Output = Result<TerrainDoodadsJournalCost>;
    const auto codec = TerrainClassicDoodadsWorkingBytes(beforeBytes, afterBytes, std::min(maximumBytes, kMaximumBytes));
    if (!codec.Succeeded() || !codec.value) return {{}, codec.diagnostics};
    // codec的四树/十wire峰值同时覆盖正常EditDoodads再次writer/解析以及
    // 原生事务两份历史wire；32次wire访问涵盖探测布局、往返、SHA和最小delta。
    if (maximumWork > kMaximumBytes || maximumWork < 4096 || beforeBytes > (maximumWork - 4096) / 32
        || afterBytes > (maximumWork - 4096) / 32 - beforeBytes)
        return Output::Failure(kSubsystem, "TERRAIN_JOURNAL_BUDGET", "普通DOO完整codec、散列和命令超过剩余累计工作预算。");
    return Output::Success({*codec.value, 4096 + beforeBytes + afterBytes, 4096 + (beforeBytes + afterBytes) * 32});
}

Status ValidateTerrainDoodadsJournalEdit(const TerrainDoodadsJournalEdit& edit) {
    return Detail::ValidatePlacementDelta(edit, 24, "TERRAIN_JOURNAL_DOODADS_DELTA");
}

Result<TerrainDoodadsJournalEdit> BuildTerrainDoodadsJournalEdit(
    std::span<const std::byte> before, std::span<const std::byte> after, std::size_t maximumInsertedBytes) {
    using Output = Result<TerrainDoodadsJournalEdit>;
    if (before.size() > kMaximumBytes || after.size() > kMaximumBytes)
        return Output::Failure(kSubsystem, "TERRAIN_JOURNAL_BUDGET", "普通DOO记录超过单成员预算。");
    auto a = ReadTerrainClassicDoodadsRoundtrip(before); if (!a.Succeeded() || !a.value) return {{}, std::move(a.diagnostics)};
    auto b = ReadTerrainClassicDoodadsRoundtrip(after); if (!b.Succeeded() || !b.value) return {{}, std::move(b.diagnostics)};
    const auto ordinary = VerifyTerrainOrdinaryDoodadsTransition(*a.value, *b.value);
    if (!ordinary.Succeeded()) return {{}, ordinary.diagnostics};
    return Detail::BuildPlacementDelta<TerrainDoodadsJournalEdit>(before, after, maximumInsertedBytes, 24,
        "TERRAIN_JOURNAL_DOODADS_DELTA");
}

Result<DoodadPlacements> ApplyTerrainDoodadsJournalEdit(
    std::span<const std::byte> current, const TerrainDoodadsJournalEdit& edit) {
    auto after = Detail::ApplyPlacementDelta(current, edit, 24, "TERRAIN_JOURNAL_DOODADS_DELTA",
        "TERRAIN_JOURNAL_DOODADS_STALE", "TERRAIN_JOURNAL_DOODADS_OUTPUT");
    if (!after.Succeeded() || !after.value) return {{}, std::move(after.diagnostics)};
    auto a = ReadTerrainClassicDoodadsRoundtrip(current); if (!a.Succeeded() || !a.value) return a;
    auto b = ReadTerrainClassicDoodadsRoundtrip(*after.value); if (!b.Succeeded() || !b.value) return b;
    const auto ordinary = VerifyTerrainOrdinaryDoodadsTransition(*a.value, *b.value);
    if (!ordinary.Succeeded()) return {{}, ordinary.diagnostics};
    return b;
}

} // namespace War3Map::MapData
