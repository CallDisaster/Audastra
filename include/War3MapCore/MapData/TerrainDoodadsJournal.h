#pragma once

#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/TerrainPlacementJournalDelta.h"

#include <memory>

namespace War3Map::MapData {

// FTJ7仅表示完整DOO的一次普通段改变，不表示来源授权或任意成员补丁。
// 旧完整SHA是CAS；新完整SHA与两个长度约束单段替换的完整结果。
struct TerrainDoodadsJournalEdit : TerrainPlacementJournalDelta {};

class MapDataEditSession;
struct TerrainStrokeIntent;

// 只由会话从真实不可变Command别名冻结；UI导出不扫描或复制DOO。
// 后台把两份实际wire物化为FTJ7，持久化不能编码此内存快照。
class FrozenTerrainDoodadsJournalEdit {
public:
    [[nodiscard]] std::span<const std::byte> Before() const noexcept {
        return before_ ? std::span<const std::byte>(*before_) : std::span<const std::byte>{};
    }
    [[nodiscard]] std::span<const std::byte> After() const noexcept {
        return after_ ? std::span<const std::byte>(*after_) : std::span<const std::byte>{};
    }
    [[nodiscard]] std::uint64_t SessionIdentity() const noexcept { return sessionIdentity_; }
    [[nodiscard]] std::uint64_t LogicalIdentity() const noexcept { return logicalIdentity_; }
    [[nodiscard]] std::size_t RetainedBytes() const noexcept { return retainedBytes_; }

private:
    friend class MapDataEditSession;
    FrozenTerrainDoodadsJournalEdit() = default;
    std::shared_ptr<const std::vector<std::byte>> before_, after_;
    std::uint64_t sessionIdentity_ = 0, logicalIdentity_ = 0;
    std::size_t retainedBytes_ = 0;
};

[[nodiscard]] Status ValidateTerrainDoodadsJournalEdit(const TerrainDoodadsJournalEdit& edit);
[[nodiscard]] bool IsTerrainDoodadsJournalPlaceholder(const TerrainStrokeIntent& intent) noexcept;
[[nodiscard]] bool EqualTerrainJournalIntent(const TerrainStrokeIntent& a, const TerrainStrokeIntent& b) noexcept;
struct TerrainDoodadsJournalCost {
    std::size_t workingBytes = 0, retainedBytes = 0, work = 0;
};
[[nodiscard]] Result<TerrainDoodadsJournalCost> MeasureTerrainDoodadsJournalEdit(
    std::size_t beforeBytes, std::size_t afterBytes, std::size_t maximumBytes, std::size_t maximumWork);
// 两个函数均须在后台使用；调用方先累计预收整对codec和历史预算。
[[nodiscard]] Result<TerrainDoodadsJournalEdit> BuildTerrainDoodadsJournalEdit(
    std::span<const std::byte> before, std::span<const std::byte> after,
    std::size_t maximumInsertedBytes = 64u * 1024u * 1024u);
[[nodiscard]] Result<DoodadPlacements> ApplyTerrainDoodadsJournalEdit(
    std::span<const std::byte> current, const TerrainDoodadsJournalEdit& edit);

} // namespace War3Map::MapData
