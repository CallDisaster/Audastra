#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/PathingSavePolicy.h"

#include <array>
#include <memory>
#include <span>

namespace War3Map::MapData {

class MapDataEditSession;

// 只借用给定原字节，与当前会话绑定的 W3I 原始基线比较；不读归档、不解析、不授予来源资格。
// exists=false 时 bytes 必须为空；实际空成员与缺失不同。当前编辑字节须由调用方另行核对。
// 与捕获接口相同，宿主须串行排除会话写入。
[[nodiscard]] Status ValidateTerrainSettingsSourceBaseline(
    const MapDataEditSession& edits, bool exists, std::span<const std::byte> bytes);

struct TerrainInspectionLimits {
    // 捕获五成员原/现唯一拥有字节的累计预算，调用方只能收紧。
    // 未改成员共享同一份只读字节。此预算不是地图实例数量上限。
    std::size_t maximumOwnedBytes = 64u * 1024u * 1024u;
};

struct TerrainInspectionMember {
    std::string name;
    // null 表示经源读取或已验证的会话删除证明的缺失；不可读/歧义使整次捕获失败。
    // 非空但empty是实际空成员，不能与缺失混用。
    std::shared_ptr<const std::vector<std::byte>> original, current;
    std::optional<std::string> originalSha256, currentSha256;
};

class TerrainEditInspection;

// 可信宿主调用，协议不能传入原始成员替换。调用方需串行排除会话写入。
// 复核原生成员原字节、已附地形的完整raw快照及WPM原字节，不能只比尺寸。
// 任一来源/序列化/预算错误无部分结果；源与会话修订变化不能形成混合快照。
[[nodiscard]] Result<TerrainEditInspection> CaptureTerrainEditInspection(
    const MapArchiveSession& source, const MapDataEditSession& edits,
    const TerrainInspectionLimits& limits = {});

class TerrainEditInspection {
public:
    [[nodiscard]] const std::string& SourceSha256() const noexcept { return sourceSha256_; }
    [[nodiscard]] const std::string& IdentitySha256() const noexcept { return identitySha256_; }
    [[nodiscard]] std::uint64_t SessionIdentity() const noexcept { return sessionIdentity_; }
    [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }
    [[nodiscard]] std::uint64_t TerrainSourceRevision() const noexcept { return terrainSourceRevision_; }
    // 固定顺序：W3E、WPM、DOO、Units.doo、W3I；不依赖listfile是否列出。
    [[nodiscard]] const std::array<TerrainInspectionMember, 5>& Members() const noexcept { return members_; }
    [[nodiscard]] std::size_t OwnedBytes() const noexcept { return ownedBytes_; }
    [[nodiscard]] const TerrainInspectionLimits& Limits() const noexcept { return limits_; }
    [[nodiscard]] bool TerrainChanged() const noexcept { return terrainChanged_; }
    // 只复制同一冻结点的既有地形/通行门禁，不是整图保存许可；被拒仍可提供影响快照。
    [[nodiscard]] const Status& TerrainPathingGate() const noexcept { return terrainPathingGate_; }
    // 独立只读分类，绑定本次归档/会话冻结点；失败不使可检查快照失效，也不改变旧保存门。
    // proof 另按逻辑span累计输入并限制摘要；OwnedBytes仍只计五成员唯一拥有的原／现字节。
    [[nodiscard]] const Result<PathingPreservationProof>& PathingPreservation() const noexcept { return pathingPreservation_; }

private:
    TerrainEditInspection() = default;
    std::string sourceSha256_, identitySha256_;
    std::uint64_t sessionIdentity_ = 0, revision_ = 0, terrainSourceRevision_ = 0;
    std::size_t ownedBytes_ = 0;
    TerrainInspectionLimits limits_;
    std::array<TerrainInspectionMember, 5> members_;
    bool terrainChanged_ = false;
    Status terrainPathingGate_;
    Result<PathingPreservationProof> pathingPreservation_;
    friend Result<TerrainEditInspection> CaptureTerrainEditInspection(
        const MapArchiveSession&, const MapDataEditSession&, const TerrainInspectionLimits&);
};

} // namespace War3Map::MapData
