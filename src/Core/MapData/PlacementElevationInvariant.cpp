#include "War3MapCore/MapData/PlacementElevationInvariant.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <stdexcept>
#include <utility>

namespace War3Map::MapData {
namespace {

constexpr std::size_t kMaximumInputBytes = 64 * 1024 * 1024;

Status Reject(const char* code, const char* message) {
    return Status::Failure("Terrain.PlacementElevation", code, message);
}

template<class Parse, class Rows>
Status Verify(std::span<const std::byte> before, std::span<const std::byte> after,
    std::size_t maximumInputBytes, Parse parse, Rows rows) {
    if (maximumInputBytes > kMaximumInputBytes || before.size() > maximumInputBytes
        || after.size() > maximumInputBytes - before.size())
        return Reject("TERRAIN_PLACEMENT_ELEVATION_BUDGET", "摆放高度核对超过累计输入字节预算。");
    if (std::equal(before.begin(), before.end(), after.begin(), after.end())) return Status::Ok();
    if (before.size() != after.size())
        return Reject("TERRAIN_PLACEMENT_ELEVATION_FIELDS", "摆放成员长度改变，不是仅修改高度。");
    try {
        PlacementReadLimits limits;
        limits.maxBytes = maximumInputBytes;
        auto original = parse(before, limits);
        if (!original.Succeeded() || !original.value) return {std::move(original.diagnostics)};
        auto current = parse(after, limits);
        if (!current.Succeeded() || !current.value) return {std::move(current.diagnostics)};
        const auto& previousRows = rows(*original.value);
        auto& currentRows = rows(*current.value);
        if (previousRows.size() != currentRows.size())
            return Reject("TERRAIN_PLACEMENT_ELEVATION_FIELDS", "摆放数量改变，不是仅修改高度。");
        for (std::size_t i = 0; i < currentRows.size(); ++i) {
            const auto oldZ = previousRows[i].position[2];
            const auto newZ = currentRows[i].position[2];
            if (std::bit_cast<std::uint32_t>(oldZ) != std::bit_cast<std::uint32_t>(newZ)
                && (!std::isfinite(oldZ) || !std::isfinite(newZ) || std::abs(newZ) > 1'000'000.0f))
                return Reject("TERRAIN_PLACEMENT_ELEVATION_INVALID", "摆放高度必须是受支持范围内的有限数值。");
            currentRows[i].position[2] = oldZ;
        }
        // 将候选 Z 恢复为源值，再让唯一经典 writer 编码；比较的是全部源字节，
        // 因而以后增加字段也不会被手写的字段白名单遗漏。含特殊尾段的 DOO 同样核对。
        auto normalized = current.value->Serialize(limits);
        if (!normalized.Succeeded() || !normalized.value) return {std::move(normalized.diagnostics)};
        if (!std::equal(before.begin(), before.end(), normalized.value->begin(), normalized.value->end()))
            return Reject("TERRAIN_PLACEMENT_ELEVATION_FIELDS", "摆放修改包含高度之外的字段或不能原样往返的旧字节。");
        return Status::Ok();
    } catch (const std::bad_alloc&) {
        return Reject("TERRAIN_PLACEMENT_ELEVATION_ALLOCATION", "摆放高度核对分配失败，未放行部分结果。");
    } catch (const std::length_error&) {
        return Reject("TERRAIN_PLACEMENT_ELEVATION_ALLOCATION", "摆放高度核对超过容器容量，未放行部分结果。");
    }
}

} // namespace

Status VerifyUnitPlacementElevationOnly(std::span<const std::byte> before,
    std::span<const std::byte> after, std::size_t maximumInputBytes) {
    return Verify(before, after, maximumInputBytes, ParseUnitPlacements,
        [](auto& placements) -> auto& { return placements.units; });
}

Status VerifyDoodadPlacementElevationOnly(std::span<const std::byte> before,
    std::span<const std::byte> after, std::size_t maximumInputBytes) {
    return Verify(before, after, maximumInputBytes, ParseDoodadPlacements,
        [](auto& placements) -> auto& { return placements.doodads; });
}

} // namespace War3Map::MapData
