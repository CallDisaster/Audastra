#include "War3MapCore/MapData/TerrainClassicDoodads.h"

#include <algorithm>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.DoodadsCodec";
constexpr std::size_t kMaximumBytes = 64u * 1024u * 1024u;
constexpr std::size_t Ceil(std::size_t count, std::size_t size) { return count / size + (count % size != 0); }
// ROC普通记录最短42字节，嵌套集合/物品最少4/8，特殊记录16。
// 四份树涵盖parser、writer内部重新解析与完整原字往返；十份wire预留
// OriginalBytes副本、writer容量增长和散列，不能在解析后才检查峰值。
constexpr auto kExpansion = std::max({Ceil(sizeof(DoodadPlacement), 42),
    Ceil(sizeof(PlacementItemSet), 4), Ceil(sizeof(PlacementItemChoice), 8),
    Ceil(sizeof(SpecialDoodadPlacement), 16), std::size_t{1}});
constexpr auto kFactor = 4 * kExpansion + 10;
constexpr auto kFixedBytes = 4 * sizeof(DoodadPlacements) + 4096;
}

Result<std::size_t> TerrainClassicDoodadsWorkingBytes(
    std::size_t beforeBytes, std::size_t afterBytes, std::size_t maximumBytes) {
    if (maximumBytes > kMaximumBytes || beforeBytes > kMaximumBytes || afterBytes > kMaximumBytes - beforeBytes
        || maximumBytes < kFixedBytes || beforeBytes + afterBytes > (maximumBytes - kFixedBytes) / kFactor)
        return Result<std::size_t>::Failure(kSubsystem, "TERRAIN_NATIVE_DOODADS_BUDGET",
            "剩余地形预算不足以完整解析并逐字往返旧/新DOO。");
    return Result<std::size_t>::Success(kFixedBytes + (beforeBytes + afterBytes) * kFactor);
}

Result<DoodadPlacements> ReadTerrainClassicDoodadsRoundtrip(std::span<const std::byte> wire) {
    PlacementReadLimits limits; limits.maxBytes = wire.size();
    auto parsed = ParseDoodadPlacements(wire, limits);
    if (!parsed.Succeeded() || !parsed.value) return parsed;
    if (parsed.value->recordLayout != PlacementRecordLayout::Classic
        || !IsClassicPlacementVersion(parsed.value->version, parsed.value->subversion)
        || parsed.value->specialVersion != 0)
        return Result<DoodadPlacements>::Failure(kSubsystem, "TERRAIN_NATIVE_DOODADS_LAYOUT",
            "地形组合修改只接受完整经典无皮肤DOO成员。");
    auto encoded = parsed.value->Serialize(limits);
    if (!encoded.Succeeded() || !encoded.value) return {{}, std::move(encoded.diagnostics)};
    if (!std::equal(wire.begin(), wire.end(), encoded.value->begin(), encoded.value->end()))
        return Result<DoodadPlacements>::Failure(kSubsystem, "TERRAIN_NATIVE_DOODADS_ROUNDTRIP",
            "DOO不能由唯一经典codec逐字往返，未取得普通对象编辑资格。");
    return parsed;
}

Status VerifyTerrainOrdinaryDoodadsTransition(const DoodadPlacements& before, const DoodadPlacements& after) {
    if (before.version != after.version || before.subversion != after.subversion
        || before.recordLayout != PlacementRecordLayout::Classic || after.recordLayout != PlacementRecordLayout::Classic
        || before.specialVersion != 0 || after.specialVersion != 0)
        return Status::Failure(kSubsystem, "TERRAIN_JOURNAL_DOODADS_LAYOUT", "普通DOO记录不能转换头版本或记录布局。");
    if (before.specialDoodads.size() != after.specialDoodads.size())
        return Status::Failure(kSubsystem, "TERRAIN_JOURNAL_DOODADS_SPECIAL", "普通DOO记录不能更改特殊装饰物尾段。");
    const auto& a = before.OriginalBytes(); const auto& b = after.OriginalBytes();
    if (a.size() < 8 || b.size() < 8 || before.specialDoodads.size() > (a.size() - 8) / 16
        || after.specialDoodads.size() > (b.size() - 8) / 16)
        return Status::Failure(kSubsystem, "TERRAIN_JOURNAL_DOODADS_SPECIAL", "经典DOO特殊尾段的完整范围无效。");
    const auto tail = 8 + before.specialDoodads.size() * 16;
    if (!std::equal(a.end() - tail, a.end(), b.end() - tail, b.end()))
        return Status::Failure(kSubsystem, "TERRAIN_JOURNAL_DOODADS_SPECIAL", "普通DOO记录不能更改特殊装饰物尾段。");
    return Status::Ok();
}

} // namespace War3Map::MapData
