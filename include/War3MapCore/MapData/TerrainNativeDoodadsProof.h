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

// 仅证明同一真实会话的完整经典普通DOO修改及saved/current私有特殊模型谱系。
// 特殊集合可随可信水刷或其Undo/Redo改变；原始尾段覆写不能取得该谱系。
// 实际对象类别、占地、旧WPM解释和G0/G1仍由保存链另行完成。
// 此值不能由外部SHA或“已经验证”的布尔构造，也不放宽旧仅Z保存域。
class TerrainNativeDoodadsProof {
public:
    TerrainNativeDoodadsProof(const TerrainNativeDoodadsProof&) = default;
    TerrainNativeDoodadsProof(TerrainNativeDoodadsProof&&) noexcept = default;
    TerrainNativeDoodadsProof& operator=(const TerrainNativeDoodadsProof&) = default;
    TerrainNativeDoodadsProof& operator=(TerrainNativeDoodadsProof&&) noexcept = default;

private:
    friend class MapDataEditSession;
    friend class TerrainSavePlan;
    TerrainNativeDoodadsProof() = default;
    [[nodiscard]] static Result<std::size_t> RequiredWorkingBytes(
        std::size_t beforeBytes, std::size_t afterBytes, std::size_t remainingBytes);
    [[nodiscard]] static Result<TerrainNativeDoodadsProof> Prepare(
        const MapDataEditSession& session, std::string_view sourceSha256, std::size_t prepaidBytes);
    [[nodiscard]] Status Verify(const MapDataEditSession& session,
        std::span<const std::byte> before, std::span<const std::byte> after) const;

    std::string sourceSha256_, beforeSha256_, afterSha256_;
    std::size_t beforeBytes_ = 0, afterBytes_ = 0;
    std::uint64_t sessionIdentity_ = 0, revision_ = 0, terrainSourceRevision_ = 0;
};

} // namespace War3Map::MapData
