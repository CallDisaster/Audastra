#include "War3MapCore/MapData/TerrainDoodadsJournal.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainClassicDoodads.h"
#include "War3MapCore/MapData/TerrainStrokeIntent.h"
#include "War3MapCore/MapData/TerrainStrokeJournal.h"

#include <algorithm>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.Journal";
constexpr std::size_t kMaximumBytes = 64u * 1024u * 1024u;
bool Sha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
}

bool IsTerrainDoodadsJournalPlaceholder(const TerrainStrokeIntent& intent) noexcept {
    const TerrainStrokeIntent neutral;
    return !intent.palette && !intent.bounds && !intent.waterBrush && !intent.followDoodads
        && intent.points.empty() && EqualTerrainJournalIntent(intent, neutral);
}

bool EqualTerrainJournalIntent(const TerrainStrokeIntent& a, const TerrainStrokeIntent& b) noexcept {
    const auto& x = a.brush; const auto& y = b.brush;
    if (x.kind != y.kind || x.shape != y.shape || x.radius != y.radius || x.groundTexture != y.groundTexture
        || x.cliffTexture != y.cliffTexture || x.levelDelta != y.levelDelta || x.value != y.value || x.strength != y.strength
        || x.seed != y.seed || x.anchor || y.anchor || x.size != y.size || x.variation != y.variation
        || x.randomizeVariation != y.randomizeVariation || x.waterHeight != y.waterHeight || x.pathingRestrictions != y.pathingRestrictions
        || a.followDoodads != b.followDoodads || a.points.size() != b.points.size()
        || a.waterBrush.has_value() != b.waterBrush.has_value() || a.bounds.has_value() != b.bounds.has_value()
        || a.palette.has_value() != b.palette.has_value()) return false;
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
    if (!Sha(edit.beforeSha256) || !Sha(edit.afterSha256) || edit.beforeSha256 == edit.afterSha256
        || edit.beforeBytes < 24 || edit.afterBytes < 24 || edit.beforeBytes > kMaximumBytes
        || edit.afterBytes > kMaximumBytes || edit.prefixBytes > edit.beforeBytes
        || edit.removedBytes > edit.beforeBytes - edit.prefixBytes || edit.inserted.size() > kMaximumBytes
        || edit.beforeBytes - edit.removedBytes > kMaximumBytes - edit.inserted.size()
        || edit.afterBytes != edit.beforeBytes - edit.removedBytes + edit.inserted.size()
        || (!edit.removedBytes && edit.inserted.empty()))
        return Status::Failure(kSubsystem, "TERRAIN_JOURNAL_DOODADS_DELTA", "普通DOO记录的完整身份、长度或有界替换范围无效。");
    return Status::Ok();
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
    const auto beforeSha = Sha256(before), afterSha = Sha256(after);
    if (!beforeSha.Succeeded() || !beforeSha.value) return {{}, beforeSha.diagnostics};
    if (!afterSha.Succeeded() || !afterSha.value) return {{}, afterSha.diagnostics};
    std::size_t prefix = 0, suffix = 0;
    while (prefix < std::min(before.size(), after.size()) && before[prefix] == after[prefix]) ++prefix;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix
        && before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) ++suffix;
    if (maximumInsertedBytes > kMaximumBytes || after.size() - prefix - suffix > maximumInsertedBytes)
        return Output::Failure(kSubsystem, "TERRAIN_JOURNAL_BUDGET", "普通DOO中段超过工程记录剩余预算，未分配或截断载荷。");
    TerrainDoodadsJournalEdit result;
    result.beforeSha256 = *beforeSha.value; result.afterSha256 = *afterSha.value;
    result.beforeBytes = static_cast<std::uint32_t>(before.size()); result.afterBytes = static_cast<std::uint32_t>(after.size());
    result.prefixBytes = static_cast<std::uint32_t>(prefix);
    result.removedBytes = static_cast<std::uint32_t>(before.size() - prefix - suffix);
    result.inserted.assign(after.begin() + prefix, after.end() - suffix);
    const auto valid = ValidateTerrainDoodadsJournalEdit(result);
    return valid.Succeeded() ? Output::Success(std::move(result)) : Output{{}, valid.diagnostics};
}

Result<DoodadPlacements> ApplyTerrainDoodadsJournalEdit(
    std::span<const std::byte> current, const TerrainDoodadsJournalEdit& edit) {
    using Output = Result<DoodadPlacements>;
    const auto valid = ValidateTerrainDoodadsJournalEdit(edit);
    if (!valid.Succeeded()) return {{}, valid.diagnostics};
    if (current.size() != edit.beforeBytes)
        return Output::Failure(kSubsystem, "TERRAIN_JOURNAL_DOODADS_STALE", "普通DOO记录不属于当前完整成员长度。");
    const auto beforeSha = Sha256(current);
    if (!beforeSha.Succeeded() || !beforeSha.value) return {{}, beforeSha.diagnostics};
    if (*beforeSha.value != edit.beforeSha256)
        return Output::Failure(kSubsystem, "TERRAIN_JOURNAL_DOODADS_STALE", "普通DOO记录的完整旧成员SHA与当前会话不同。");
    std::vector<std::byte> after; after.reserve(edit.afterBytes);
    after.insert(after.end(), current.begin(), current.begin() + edit.prefixBytes);
    after.insert(after.end(), edit.inserted.begin(), edit.inserted.end());
    after.insert(after.end(), current.begin() + edit.prefixBytes + edit.removedBytes, current.end());
    const auto afterSha = Sha256(after);
    if (!afterSha.Succeeded() || !afterSha.value) return {{}, afterSha.diagnostics};
    if (after.size() != edit.afterBytes || *afterSha.value != edit.afterSha256)
        return Output::Failure(kSubsystem, "TERRAIN_JOURNAL_DOODADS_OUTPUT", "普通DOO替换结果不符合完整新成员身份。");
    std::size_t prefix = 0, suffix = 0;
    while (prefix < std::min(current.size(), after.size()) && current[prefix] == after[prefix]) ++prefix;
    while (suffix < current.size() - prefix && suffix < after.size() - prefix
        && current[current.size() - 1 - suffix] == after[after.size() - 1 - suffix]) ++suffix;
    if (prefix != edit.prefixBytes || current.size() - prefix - suffix != edit.removedBytes
        || after.size() - prefix - suffix != edit.inserted.size())
        return Output::Failure(kSubsystem, "TERRAIN_JOURNAL_DOODADS_DELTA", "普通DOO记录必须使用完整公共前后缀的唯一最小中段。");
    auto a = ReadTerrainClassicDoodadsRoundtrip(current); if (!a.Succeeded() || !a.value) return a;
    auto b = ReadTerrainClassicDoodadsRoundtrip(after); if (!b.Succeeded() || !b.value) return b;
    const auto ordinary = VerifyTerrainOrdinaryDoodadsTransition(*a.value, *b.value);
    if (!ordinary.Succeeded()) return {{}, ordinary.diagnostics};
    return b;
}

Status MaterializeTerrainJournalDoodads(TerrainStrokeJournal& journal) {
    const auto fail = [] { return Status::Failure(kSubsystem, "TERRAIN_JOURNAL_BUDGET",
        "普通DOO记录的冻结快照、完整codec、累计扫描或中段载荷超过工程预算，raw草稿仍可保留。"); };
    if (journal.strokes.size() > 128 || journal.baselinePathingLayers.size() > kMaximumBytes - 128u * 1024u) return fail();
    std::size_t available = kMaximumBytes - 128u * 1024u - journal.baselinePathingLayers.size();
    std::size_t frozenBytes = 0, remainingWork = kMaximumBytes;
    // 所有尺寸和累计work先收齐；不能在解析了前半条大历史后才发现总量不成立。
    for (const auto& stroke : journal.strokes) {
        if (stroke.frozenDoodads) {
            if (stroke.ordinaryDoodads || !IsTerrainDoodadsJournalPlaceholder(stroke.intent)
                || !stroke.rulesSha256.empty() || stroke.cliffEnforceWaterLimits) return fail();
            const auto& frozen = *stroke.frozenDoodads;
            if (!frozen.RetainedBytes() || frozen.Before().empty() || frozen.After().empty()
                || frozen.RetainedBytes() > kMaximumBytes - frozenBytes) return fail();
            frozenBytes += frozen.RetainedBytes();
            const auto cost = MeasureTerrainDoodadsJournalEdit(frozen.Before().size(), frozen.After().size(), kMaximumBytes, remainingWork);
            if (!cost.Succeeded() || !cost.value) return fail();
            remainingWork -= cost.value->work;
        } else if (stroke.ordinaryDoodads) {
            const auto valid = ValidateTerrainDoodadsJournalEdit(*stroke.ordinaryDoodads);
            if (!valid.Succeeded()) return valid;
            if (stroke.ordinaryDoodads->inserted.size() > available) return fail();
            available -= stroke.ordinaryDoodads->inserted.size();
        }
    }
    // 冻结command连同wire/摆放谱系最多64MiB、codec峰值64MiB、最终wire最多64MiB和baseline最多64MiB；
    // 连同128项固定元数据仍低于工程512MiB候选帽。冻结别名逐项释放，不回到UI读活动历史。
    for (auto& stroke : journal.strokes) {
        if (!stroke.frozenDoodads) continue;
        const auto& frozen = *stroke.frozenDoodads;
        auto materialized = BuildTerrainDoodadsJournalEdit(frozen.Before(), frozen.After(), available);
        if (!materialized.Succeeded() || !materialized.value) return {materialized.diagnostics};
        available -= materialized.value->inserted.size();
        stroke.ordinaryDoodads = std::move(*materialized.value); stroke.frozenDoodads.reset();
    }
    return Status::Ok();
}

} // namespace War3Map::MapData
