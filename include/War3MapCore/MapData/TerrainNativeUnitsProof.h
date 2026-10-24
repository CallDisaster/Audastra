#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace War3Map::MapData {

class MapDataEditSession;
class TerrainSavePlan;

// 原版Units管理器仅写动态层4/8，不写保存WPM使用的静态层2。
// 本证明只核对已存在的完整经典Units成员；它本身不授予WPM生成或保存资格。
// 唯一工厂由完整native G0/G1保存链持有，普通通行不变门不能构造此证明。
class TerrainNativeUnitsProof {
public:
    TerrainNativeUnitsProof(const TerrainNativeUnitsProof&) = default;
    TerrainNativeUnitsProof(TerrainNativeUnitsProof&&) noexcept = default;
    TerrainNativeUnitsProof& operator=(const TerrainNativeUnitsProof&) = default;
    TerrainNativeUnitsProof& operator=(TerrainNativeUnitsProof&&) noexcept = default;

private:
    friend class MapDataEditSession;
    friend class TerrainSavePlan;
    TerrainNativeUnitsProof() = default;

    // 在任何parser/writer分配前按外层剩余额度预收峰值工作量；不得单独使用默认预算。
    [[nodiscard]] static Result<std::size_t> RequiredWorkingBytes(
        std::size_t beforeBytes, std::size_t afterBytes, std::size_t remainingBytes);
    [[nodiscard]] static Result<TerrainNativeUnitsProof> Prepare(
        std::span<const std::byte> before, std::span<const std::byte> after,
        std::string_view sourceSha256, std::uint64_t sessionIdentity, std::uint64_t revision,
        std::uint64_t terrainSourceRevision, std::size_t prepaidBytes);
    [[nodiscard]] Status Verify(std::span<const std::byte> before,
        std::span<const std::byte> after, std::uint64_t sessionIdentity,
        std::uint64_t revision, std::uint64_t terrainSourceRevision) const;

    std::string sourceSha256_, beforeSha256_, afterSha256_;
    std::size_t beforeBytes_ = 0, afterBytes_ = 0;
    std::uint64_t sessionIdentity_ = 0, revision_ = 0, terrainSourceRevision_ = 0;
};

} // namespace War3Map::MapData
