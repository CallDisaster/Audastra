#include "War3MapCore/MapData/TerrainClassicUnits.h"

#include <algorithm>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.NativeUnits";
constexpr std::size_t kMaximumBytes = 64u * 1024u * 1024u;
constexpr std::size_t Ceil(std::size_t value, std::size_t divisor) {
    return value / divisor + (value % divisor != 0);
}
// 最短经典单位91字节，嵌套元素至少4/8/8/12字节。四树与八份wire
// 覆盖布局试读、writer再解析、完整成员副本、正常事务及散列工作。
constexpr auto kTreeExpansion = std::max({Ceil(sizeof(UnitPlacement), 91),
    Ceil(sizeof(PlacementItemSet), 4), Ceil(sizeof(PlacementItemChoice), 8),
    Ceil(sizeof(PlacementInventoryItem), 8), Ceil(sizeof(PlacementAbility), 12), std::size_t{1}});
constexpr auto kWorkingFactor = 4 * kTreeExpansion + 8;
constexpr auto kFixedWorkingBytes = 4 * sizeof(UnitPlacements) + 4096;
}

Result<std::size_t> TerrainClassicUnitsWorkingBytes(
    std::size_t beforeBytes, std::size_t afterBytes, std::size_t maximumBytes) {
    if (maximumBytes > kMaximumBytes || beforeBytes > kMaximumBytes || afterBytes > kMaximumBytes - beforeBytes
        || maximumBytes < kFixedWorkingBytes
        || beforeBytes + afterBytes > (maximumBytes - kFixedWorkingBytes) / kWorkingFactor)
        return Result<std::size_t>::Failure(kSubsystem, "TERRAIN_NATIVE_UNITS_BUDGET",
            "剩余地形保存预算不足以完整解析并往返旧/新Units成员。");
    return Result<std::size_t>::Success(kFixedWorkingBytes + (beforeBytes + afterBytes) * kWorkingFactor);
}

Result<UnitPlacements> ReadTerrainClassicUnitsRoundtrip(std::span<const std::byte> wire) {
    PlacementReadLimits limits; limits.maxBytes = wire.size();
    auto parsed = ParseUnitPlacements(wire, limits);
    if (!parsed.Succeeded() || !parsed.value) return parsed;
    if (parsed.value->recordLayout != PlacementRecordLayout::Classic
        || !IsClassicPlacementVersion(parsed.value->version, parsed.value->subversion))
        return Result<UnitPlacements>::Failure(kSubsystem, "TERRAIN_NATIVE_UNITS_LAYOUT",
            "地形与单位组合保存只支持完整经典无皮肤Units成员。");
    auto encoded = parsed.value->Serialize(limits);
    if (!encoded.Succeeded() || !encoded.value) return {{}, std::move(encoded.diagnostics)};
    if (!std::equal(wire.begin(), wire.end(), encoded.value->begin(), encoded.value->end()))
        return Result<UnitPlacements>::Failure(kSubsystem, "TERRAIN_NATIVE_UNITS_ROUNDTRIP",
            "Units成员不能由唯一经典codec逐字往返，保留来源和草稿。");
    return parsed;
}

} // namespace War3Map::MapData
