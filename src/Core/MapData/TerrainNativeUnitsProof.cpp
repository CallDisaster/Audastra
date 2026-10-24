#include "War3MapCore/MapData/TerrainNativeUnitsProof.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainClassicUnits.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.NativeUnits";

Status Fail(const char* code, std::string message) {
    return Status::Failure(kSubsystem, code, std::move(message));
}
bool ValidSha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

}

Result<std::size_t> TerrainNativeUnitsProof::RequiredWorkingBytes(
    std::size_t beforeBytes, std::size_t afterBytes, std::size_t remainingBytes) {
    return TerrainClassicUnitsWorkingBytes(beforeBytes, afterBytes, remainingBytes);
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
        const auto original = ReadTerrainClassicUnitsRoundtrip(before);
        if (!original.Succeeded()) return {{}, original.diagnostics};
        const auto current = ReadTerrainClassicUnitsRoundtrip(after);
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
