#include "War3MapCore/MapData/TerrainNativeUnitsProof.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapPlacements.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.NativeUnits";
constexpr std::size_t kMaximumBytes = 64u * 1024u * 1024u;
constexpr std::size_t Ceil(std::size_t value, std::size_t divisor) {
    return value / divisor + (value % divisor != 0);
}
// 最短经典单位为91字节；各嵌套元素分别至少消耗4/8/8/12字节。
// 每一段wire只属于一种元素。四份树覆盖classic+skin歧义试读，以及
// writer内部再次完整parse的同时存活树；另8份wire覆盖原字副本、输出
// 容量增长、相等时的返回副本和散列工作。这里计工作/容器容量，不声称RSS。
constexpr auto kTreeExpansion = std::max({Ceil(sizeof(UnitPlacement), 91),
    Ceil(sizeof(PlacementItemSet), 4), Ceil(sizeof(PlacementItemChoice), 8),
    Ceil(sizeof(PlacementInventoryItem), 8), Ceil(sizeof(PlacementAbility), 12), std::size_t{1}});
constexpr auto kWorkingFactor = 4 * kTreeExpansion + 8;
constexpr auto kFixedWorkingBytes = 4 * sizeof(UnitPlacements) + 4096;

Status Fail(const char* code, std::string message) {
    return Status::Failure(kSubsystem, code, std::move(message));
}
bool ValidSha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
Status Roundtrip(std::span<const std::byte> wire) {
    PlacementReadLimits limits;
    limits.maxBytes = wire.size();
    // 不因保存扩展codec硬上限；字节预算已在最外层预收，计数仍取原上限。
    auto parsed = ParseUnitPlacements(wire, limits);
    if (!parsed.Succeeded() || !parsed.value) return {std::move(parsed.diagnostics)};
    if (parsed.value->recordLayout != PlacementRecordLayout::Classic
        || !IsClassicPlacementVersion(parsed.value->version, parsed.value->subversion))
        return Fail("TERRAIN_NATIVE_UNITS_LAYOUT", "地形与单位组合保存只支持完整经典无皮肤Units成员。");
    auto encoded = parsed.value->Serialize(limits);
    if (!encoded.Succeeded() || !encoded.value) return {std::move(encoded.diagnostics)};
    if (!std::equal(wire.begin(), wire.end(), encoded.value->begin(), encoded.value->end()))
        return Fail("TERRAIN_NATIVE_UNITS_ROUNDTRIP", "Units成员不能由唯一经典codec逐字往返，保留来源和草稿。");
    return Status::Ok();
}
}

Result<std::size_t> TerrainNativeUnitsProof::RequiredWorkingBytes(
    std::size_t beforeBytes, std::size_t afterBytes, std::size_t remainingBytes) {
    using Output = Result<std::size_t>;
    if (remainingBytes > kMaximumBytes || beforeBytes > kMaximumBytes
        || afterBytes > kMaximumBytes - beforeBytes)
        return Output::Failure(kSubsystem, "TERRAIN_NATIVE_UNITS_BUDGET", "Units核对超过地形保存的累计输入预算。");
    const auto wireBytes = beforeBytes + afterBytes;
    if (remainingBytes < kFixedWorkingBytes
        || wireBytes > (remainingBytes - kFixedWorkingBytes) / kWorkingFactor)
        return Output::Failure(kSubsystem, "TERRAIN_NATIVE_UNITS_BUDGET", "剩余地形保存预算不足以完整解析并往返旧/新Units成员。");
    return Output::Success(kFixedWorkingBytes + wireBytes * kWorkingFactor);
}

Result<TerrainNativeUnitsProof> TerrainNativeUnitsProof::Prepare(
    std::span<const std::byte> before, std::span<const std::byte> after,
    std::string_view sourceSha256, std::uint64_t sessionIdentity, std::uint64_t revision,
    std::uint64_t terrainSourceRevision, std::size_t prepaidBytes) {
    using Output = Result<TerrainNativeUnitsProof>;
    try {
        if (!ValidSha(sourceSha256) || !sessionIdentity)
            return Output::Failure(kSubsystem, "TERRAIN_NATIVE_UNITS_IDENTITY", "Units证明缺少实际地图来源或会话身份。");
        auto charge = RequiredWorkingBytes(before.size(), after.size(), prepaidBytes);
        if (!charge.Succeeded() || !charge.value) return {{}, std::move(charge.diagnostics)};
        if (*charge.value != prepaidBytes)
            return Output::Failure(kSubsystem, "TERRAIN_NATIVE_UNITS_BUDGET", "Units证明必须消费同次地形计划精确预收的额度。");
        const auto original = Roundtrip(before);
        if (!original.Succeeded()) return {{}, original.diagnostics};
        const auto current = Roundtrip(after);
        if (!current.Succeeded()) return {{}, current.diagnostics};
        auto beforeSha = Sha256(before), afterSha = Sha256(after);
        if (!beforeSha.Succeeded() || !beforeSha.value) return {{}, std::move(beforeSha.diagnostics)};
        if (!afterSha.Succeeded() || !afterSha.value) return {{}, std::move(afterSha.diagnostics)};
        TerrainNativeUnitsProof proof;
        proof.sourceSha256_ = sourceSha256; proof.beforeSha256_ = std::move(*beforeSha.value);
        proof.afterSha256_ = std::move(*afterSha.value);
        proof.beforeBytes_ = before.size(); proof.afterBytes_ = after.size();
        proof.sessionIdentity_ = sessionIdentity; proof.revision_ = revision;
        proof.terrainSourceRevision_ = terrainSourceRevision;
        return Output::Success(std::move(proof));
    } catch (const std::bad_alloc&) {
        return Output::Failure(kSubsystem, "TERRAIN_NATIVE_UNITS_ALLOCATION", "Units证明内存分配失败，未放行部分结果。");
    } catch (const std::length_error&) {
        return Output::Failure(kSubsystem, "TERRAIN_NATIVE_UNITS_ALLOCATION", "Units证明超过容器容量，未放行部分结果。");
    }
}

Status TerrainNativeUnitsProof::Verify(std::span<const std::byte> before,
    std::span<const std::byte> after, std::uint64_t sessionIdentity,
    std::uint64_t revision, std::uint64_t terrainSourceRevision) const {
    if (!ValidSha(sourceSha256_) || sessionIdentity != sessionIdentity_ || revision != revision_
        || terrainSourceRevision != terrainSourceRevision_
        || before.size() != beforeBytes_ || after.size() != afterBytes_)
        return Fail("TERRAIN_NATIVE_UNITS_STALE", "Units证明的来源、会话、修订或完整成员长度已经改变。");
    const auto oldSha = Sha256(before), newSha = Sha256(after);
    if (!oldSha.Succeeded() || !oldSha.value) return {oldSha.diagnostics};
    if (!newSha.Succeeded() || !newSha.value) return {newSha.diagnostics};
    if (*oldSha.value != beforeSha256_ || *newSha.value != afterSha256_)
        return Fail("TERRAIN_NATIVE_UNITS_STALE", "Units证明不属于当前完整旧/新成员，拒绝保存。");
    return Status::Ok();
}

} // namespace War3Map::MapData
