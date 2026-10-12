#pragma once

#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "War3MapCore/MapData/PlacementEditing.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace War3Map::MapData {
namespace detail { struct MapDataPlacementEditingAccess; }

// 来自同一修订的完整来源定位；保存意图另绑定原来源，不能接受协议成员 bytes。
struct MapDataPlacementSource {
    PlacementId id;
    std::string expectedRowKey;
    std::string expectedMemberSha256;
};
struct MapDataPlacementRotation {
    MapDataPlacementSource source;
    float facingRadians = 0; // 绝对朝向，输入为 [0, 2π) 弧度；保留请求 float，不从结果反推。
};
struct MapDataPlacementCopy {
    std::vector<MapDataPlacementSource> sources;
    std::array<float, 3> offset{}; // 游戏世界坐标 XYZ，同一批次同类别、共用偏移。
};
struct MapDataPlacementCreate {
    std::string expectedMemberSha256;
    PlacementCreationRecords records;
};
struct MapDataPlacementDelete {
    std::vector<MapDataPlacementSource> sources;
};
using MapDataPlacementIntent = std::variant<MapDataPlacementRotation, MapDataPlacementCopy,
    MapDataPlacementCreate, MapDataPlacementDelete>;
[[nodiscard]] PlacementFile PlacementIntentFile(const MapDataPlacementIntent& intent);

struct PlacementCommandLimits {
    std::size_t maximumSources = 32;
    std::size_t maximumWireBytes = 64 * 1024 * 1024; // 展开后受影响 native wire 总额；不是 RSS 限额。
};

// 私有构造的原来源重放证明。恢复至原始 bytes 时 Members 为空，保存须报告无净变化。
class FrozenMapDataPlacement {
public:
    [[nodiscard]] const MapDataPlacementIntent& Intent() const noexcept { return intent_; }
    [[nodiscard]] std::span<const MapArchiveMember> Members() const noexcept { return members_; }
    [[nodiscard]] bool HasChanges() const noexcept { return !members_.empty(); }
private:
    friend struct detail::MapDataPlacementEditingAccess;
    FrozenMapDataPlacement(MapDataPlacementIntent intent, std::vector<MapArchiveMember> members)
        : intent_(std::move(intent)), members_(std::move(members)) {}
    MapDataPlacementIntent intent_;
    std::vector<MapArchiveMember> members_;
};

// 不可变且自有的领域候选；唯一 kernel 产生真实 native、谱系与新行身份。
class PreparedMapDataPlacement {
public:
    [[nodiscard]] MapDataKind Kind() const noexcept { return kind_; }
    [[nodiscard]] const MapDocument& Original() const noexcept { return *original_; }
    [[nodiscard]] const MapDocument& Before() const noexcept { return *before_; }
    [[nodiscard]] const MapDocument& After() const noexcept { return *after_; }
    [[nodiscard]] std::shared_ptr<const MapDocument> OwnedBefore() const noexcept { return before_; }
    [[nodiscard]] std::shared_ptr<const MapDocument> OwnedAfter() const noexcept { return after_; }
    [[nodiscard]] const MapDataPlacementIntent& Intent() const noexcept { return intent_; }
    // GUI 作者先前新增/删行可使原定位不可用；独立宿主必须非空并通过完整原来源重放。
    [[nodiscard]] const std::optional<MapDataPlacementIntent>& OriginalReplayIntent() const noexcept { return originalIntent_; }
    [[nodiscard]] std::span<const PlacementRowLineage> Lineage() const noexcept { return lineage_; }
    [[nodiscard]] std::span<const PlacementId> NewIds() const noexcept { return newIds_; }
    [[nodiscard]] std::span<const std::byte> BeforeMemberBytes() const noexcept;
    [[nodiscard]] std::span<const std::byte> AfterMemberBytes() const noexcept;
    [[nodiscard]] const std::string& BeforeMemberSha256() const noexcept { return beforeSha_; }
    [[nodiscard]] const std::string& AfterMemberSha256() const noexcept { return afterSha_; }
private:
    friend struct detail::MapDataPlacementEditingAccess;
    PreparedMapDataPlacement(MapDataKind kind, std::shared_ptr<const MapDocument> original,
        std::shared_ptr<const MapDocument> before, std::shared_ptr<const MapDocument> after,
        MapDataPlacementIntent intent, std::optional<MapDataPlacementIntent> originalIntent,
        std::vector<PlacementRowLineage> lineage, std::vector<PlacementId> newIds,
        std::string beforeSha, std::string afterSha)
        : kind_(kind), original_(std::move(original)), before_(std::move(before)), after_(std::move(after)),
          intent_(std::move(intent)), originalIntent_(std::move(originalIntent)), lineage_(std::move(lineage)),
          newIds_(std::move(newIds)), beforeSha_(std::move(beforeSha)), afterSha_(std::move(afterSha)) {}
    MapDataKind kind_;
    std::shared_ptr<const MapDocument> original_, before_, after_;
    MapDataPlacementIntent intent_;
    std::optional<MapDataPlacementIntent> originalIntent_;
    std::vector<PlacementRowLineage> lineage_;
    std::vector<PlacementId> newIds_;
    std::string beforeSha_, afterSha_;
};

[[nodiscard]] Result<PreparedMapDataPlacement> PrepareMapDataPlacement(const MapDocument& original,
    const MapDocument& current, const MapDataPlacementIntent& intent, PlacementCommandLimits limits = {});
// 独立宿主保存/发布时调用；全体成员状态、presence 与 native 必须等于原意图重放。
// GUI 原完整保存不调用本冻结器，不能据此拒绝作者先前其他编辑。
[[nodiscard]] Result<FrozenMapDataPlacement> FreezeOriginalMapDataPlacement(const MapDocument& original,
    const MapDocument& current, const MapDataPlacementIntent& originalIntent, PlacementCommandLimits limits = {});

} // namespace War3Map::MapData
