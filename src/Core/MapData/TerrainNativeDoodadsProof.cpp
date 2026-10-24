#include "War3MapCore/MapData/TerrainNativeDoodadsProof.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapPlacements.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.NativeDoodads";
constexpr std::size_t kMaximumBytes = 64u * 1024u * 1024u;
constexpr std::size_t Ceil(std::size_t count, std::size_t size) { return count / size + (count % size != 0); }
// ROC普通记录最短42字节，嵌套集合/物品最少4/8，特殊记录16。
// 四份树涵盖parser、writer内部重新解析与完整原字往返；十份wire预留
// OriginalBytes副本、writer容量增长和散列。实际解析前由保存总预算预收。
constexpr auto kExpansion = std::max({Ceil(sizeof(DoodadPlacement), 42),
    Ceil(sizeof(PlacementItemSet), 4), Ceil(sizeof(PlacementItemChoice), 8),
    Ceil(sizeof(SpecialDoodadPlacement), 16), std::size_t{1}});
constexpr auto kFactor = 4 * kExpansion + 10;
constexpr auto kFixedBytes = 4 * sizeof(DoodadPlacements) + 4096;

Status Fail(const char* code, std::string message) { return Status::Failure(kSubsystem, code, std::move(message)); }
bool ValidSha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
Result<DoodadPlacements> Roundtrip(std::span<const std::byte> wire) {
    PlacementReadLimits limits; limits.maxBytes = wire.size();
    auto parsed = ParseDoodadPlacements(wire, limits);
    if (!parsed.Succeeded() || !parsed.value) return parsed;
    if (parsed.value->recordLayout != PlacementRecordLayout::Classic
        || !IsClassicPlacementVersion(parsed.value->version, parsed.value->subversion)
        || parsed.value->specialVersion != 0)
        return Result<DoodadPlacements>::Failure(kSubsystem, "TERRAIN_NATIVE_DOODADS_LAYOUT",
            "地形组合保存只接受完整经典无皮肤DOO成员。");
    auto encoded = parsed.value->Serialize(limits);
    if (!encoded.Succeeded() || !encoded.value) return {{}, std::move(encoded.diagnostics)};
    if (!std::equal(wire.begin(), wire.end(), encoded.value->begin(), encoded.value->end()))
        return Result<DoodadPlacements>::Failure(kSubsystem, "TERRAIN_NATIVE_DOODADS_ROUNDTRIP",
            "DOO不能由唯一经典codec逐字往返，未取得普通对象编辑资格。");
    return parsed;
}
}

Result<std::size_t> TerrainNativeDoodadsProof::RequiredWorkingBytes(
    std::size_t beforeBytes, std::size_t afterBytes, std::size_t remainingBytes) {
    using Output = Result<std::size_t>;
    if (remainingBytes > kMaximumBytes || beforeBytes > kMaximumBytes || afterBytes > kMaximumBytes - beforeBytes
        || remainingBytes < kFixedBytes || beforeBytes + afterBytes > (remainingBytes - kFixedBytes) / kFactor)
        return Output::Failure(kSubsystem, "TERRAIN_NATIVE_DOODADS_BUDGET",
            "剩余地形保存预算不足以完整解析并逐字往返旧/新DOO。");
    return Output::Success(kFixedBytes + (beforeBytes + afterBytes) * kFactor);
}

Result<TerrainNativeDoodadsProof> TerrainNativeDoodadsProof::Prepare(
    const MapDataEditSession& session, std::string_view sourceSha256, std::size_t prepaidBytes) {
    using Output = Result<TerrainNativeDoodadsProof>;
    try {
        const auto index = MapDataEditSession::Index(MapDataKind::Doodads);
        const auto& before = session.original_[index]; const auto& after = session.current_[index];
        if (!before || !after || !session.SessionIdentity() || !ValidSha(sourceSha256))
            return Output::Failure(kSubsystem, "TERRAIN_NATIVE_DOODADS_IDENTITY",
                "普通DOO证明必须来自同一实际会话的完整旧/新成员，不能新增或删除整个成员。");
        const auto required = RequiredWorkingBytes(before->size(), after->size(), prepaidBytes);
        if (!required.Succeeded() || !required.value) return {{}, required.diagnostics};
        if (*required.value != prepaidBytes)
            return Output::Failure(kSubsystem, "TERRAIN_NATIVE_DOODADS_BUDGET", "DOO证明须消费保存计划精确预收的工作额度。");
        auto old = Roundtrip(*before); if (!old.Succeeded() || !old.value) return {{}, std::move(old.diagnostics)};
        auto current = Roundtrip(*after); if (!current.Succeeded() || !current.value) return {{}, std::move(current.diagnostics)};
        const auto provenance = session.VerifyTerrainSpecialDoodadProvenance(*old.value, *current.value);
        if (!provenance.Succeeded()) return {{}, provenance.diagnostics};
        if (old.value->version != current.value->version || old.value->subversion != current.value->subversion)
            return Output::Failure(kSubsystem, "TERRAIN_NATIVE_DOODADS_LAYOUT_CHANGED",
                "普通对象组合修改须保持原DOO头版本，不能借特殊模型谱系转换记录格式。");
        // 两份special分别匹配同一会话的saved/current私有集合；这允许水刷的
        // 删除与Undo/Redo恢复和普通XY修改同存，不从相同尾段字节重建授权。
        // 原版owner仍分别生成普通/special的G0/G1，并独立冻结桥的完整普通段。
        auto oldSha = Sha256(*before), currentSha = Sha256(*after);
        if (!oldSha.Succeeded() || !oldSha.value) return {{}, std::move(oldSha.diagnostics)};
        if (!currentSha.Succeeded() || !currentSha.value) return {{}, std::move(currentSha.diagnostics)};
        TerrainNativeDoodadsProof proof;
        proof.sourceSha256_ = sourceSha256; proof.beforeSha256_ = std::move(*oldSha.value);
        proof.afterSha256_ = std::move(*currentSha.value); proof.beforeBytes_ = before->size(); proof.afterBytes_ = after->size();
        proof.sessionIdentity_ = session.SessionIdentity(); proof.revision_ = session.Revision();
        proof.terrainSourceRevision_ = session.TerrainSourceRevision();
        return Output::Success(std::move(proof));
    } catch (const std::bad_alloc&) {
        return Output::Failure(kSubsystem, "TERRAIN_NATIVE_DOODADS_ALLOCATION", "普通DOO证明分配失败，未放行部分结果。");
    } catch (const std::length_error&) {
        return Output::Failure(kSubsystem, "TERRAIN_NATIVE_DOODADS_ALLOCATION", "普通DOO证明超过容器容量，未放行部分结果。");
    }
}

Status TerrainNativeDoodadsProof::Verify(const MapDataEditSession& session,
    std::span<const std::byte> before, std::span<const std::byte> after) const {
    if (!ValidSha(sourceSha256_) || session.SessionIdentity() != sessionIdentity_ || session.Revision() != revision_
        || session.TerrainSourceRevision() != terrainSourceRevision_ || before.size() != beforeBytes_ || after.size() != afterBytes_)
        return Fail("TERRAIN_NATIVE_DOODADS_STALE", "普通DOO证明的完整成员、会话或修订已改变。");
    const auto a = Sha256(before), b = Sha256(after);
    if (!a.Succeeded() || !a.value) return {a.diagnostics};
    if (!b.Succeeded() || !b.value) return {b.diagnostics};
    if (*a.value != beforeSha256_ || *b.value != afterSha256_)
        return Fail("TERRAIN_NATIVE_DOODADS_STALE", "普通DOO证明不属于本次完整旧/新成员。");
    return Status::Ok();
}

} // namespace War3Map::MapData
