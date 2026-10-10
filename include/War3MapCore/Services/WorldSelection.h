#pragma once

#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/Services/ProjectSearch.h"

#include <functional>
#include <variant>

namespace War3Map::Services {

using WorldObjectId = std::variant<MapData::PlacementId, MapData::RegionId, MapData::CameraId>;
enum class WorldCategory : std::uint8_t { Unit, Item, Destructable, Doodad, Region, Camera, StartLocation, Unknown };
enum class WorldTypeState : std::uint8_t { Known, Missing, Ambiguous, Unavailable, Partial };
enum class SelectionMode : std::uint8_t { Replace, Add, Subtract };

struct WorldSelectionVersion {
    MapDataQueryBinding document;
    std::uint64_t catalogRevision = 0, editorRevision = 0;
    [[nodiscard]] bool operator==(const WorldSelectionVersion&) const = default;
};
struct WorldEditorFlags {
    bool visible = true, locked = false;
    [[nodiscard]] bool operator==(const WorldEditorFlags&) const = default;
};
struct WorldEditorFlagEdit { WorldObjectId id; WorldEditorFlags value; };
struct WorldSelectionSnapshot {
    WorldSelectionVersion version;
    std::vector<WorldObjectId> selected;
    std::optional<WorldObjectId> primary;
};
// 宿主在文档线程借用同一当前 overlay 的 typed 文档与冻结查询快照；服务不保留任何借用指针。
// objects 每种类型只传当前生效的 native/LNI；catalogRevision 覆盖目录、物编、字符串和资源层。
struct WorldSelectionContext {
    const MapDocument* document = nullptr;
    const MapDataQuerySnapshot* mapData = nullptr;
    MapDataQueryBinding binding;
    std::uint64_t catalogRevision = 0;
    std::span<const ProjectSearchObjectSource> objects;
    const ObjectData::StandardObjectCatalog* standardObjects = nullptr;
    ProjectSearchState standardState = ProjectSearchState::Unavailable;
    // 宿主复用现有物编父链/WTS 展示；在筛选前调用，不保留回调或在 Core 再实现父链。
    std::function<Result<MapDataQueryText>(const ObjectData::ObjectKey&)> objectNameReader;
};
struct WorldObjectRow {
    // specialDoodads 等未有 typed 编辑身份的记录为空；仍返回可核查的只读行和原因。
    std::optional<WorldObjectId> id;
    std::string rowKey;
    MapDataKind member = MapDataKind::Units;
    std::uint64_t sourceIndex = 0;
    WorldCategory category = WorldCategory::Unknown;
    WorldTypeState typeState = WorldTypeState::Unavailable;
    std::optional<std::int32_t> owner;
    MapDataQueryText name;
    std::optional<std::array<float, 3>> positionWorld;
    WorldEditorFlags flags;
    bool selected = false, primary = false, textTruncated = false;
    std::string reason;
};
struct WorldObjectFilter {
    std::string searchUtf8;
    std::optional<WorldCategory> category;
    std::optional<std::int32_t> owner;
    [[nodiscard]] bool operator==(const WorldObjectFilter&) const = default;
};
struct WorldObjectCursor {
    WorldSelectionVersion version;
    MapDataQueryCursor source;
    WorldObjectFilter filter;
};
struct WorldObjectQueryOptions {
    std::size_t limit = 64; // 1..128
    std::size_t maxExamined = 4096; // 1..4096，每次查询扫描预算；next 可以继续。
    std::size_t maxLogicalBytes = 256 * 1024; // 1..256 KiB
};
struct WorldObjectPage {
    WorldSelectionVersion version;
    MapDataQueryMember member;
    std::vector<WorldObjectRow> rows;
    std::optional<WorldObjectCursor> next;
    std::size_t examined = 0, logicalBytes = 0;
    bool partial = false;
    std::string reason;
};

[[nodiscard]] std::string_view WorldCategoryName(WorldCategory category) noexcept;

// 唯一共享编辑器状态，不编辑地图、不建 Undo、不写文件。宿主必须串行调用。
class WorldSelection final {
public:
    static constexpr std::size_t MaxSelection = 4096, MaxFlagOverrides = 16384;
    [[nodiscard]] Status Bind(const WorldSelectionContext& context);
    [[nodiscard]] Result<WorldSelectionSnapshot> Snapshot(const WorldSelectionContext& context,
        const WorldSelectionVersion& expected) const;
    // 选择仍可查看部分来源；编辑须同一当前身份、完整成员且未隐藏/锁定。
    // 多个辅助对象或辅助对象与摆放混选没有共同事务，不能拆成多次 Undo。
    [[nodiscard]] Status EditStatus(const WorldSelectionContext& context,
        const WorldSelectionVersion& expected) const;
    [[nodiscard]] Result<WorldSelectionSnapshot> Select(const WorldSelectionContext& context,
        const WorldSelectionVersion& expected, SelectionMode mode, std::span<const WorldObjectId> ids);
    [[nodiscard]] Result<WorldSelectionSnapshot> SetEditorFlags(const WorldSelectionContext& context,
        const WorldSelectionVersion& expected, std::span<const WorldEditorFlagEdit> edits);
    [[nodiscard]] Result<WorldObjectPage> QueryObjects(const WorldSelectionContext& context,
        const WorldSelectionVersion& expected, MapDataKind kind, const WorldObjectFilter& filter = {},
        const std::optional<WorldObjectCursor>& cursor = std::nullopt, const WorldObjectQueryOptions& options = {}) const;
    // Version 是宿主获取刚 Bind/编辑后的值，不代替 expected 校验；Clear 后为空。
    [[nodiscard]] const std::optional<WorldSelectionVersion>& Version() const noexcept { return version_; }
    [[nodiscard]] WorldEditorFlags Flags(const WorldObjectId& id) const noexcept;
    void Clear() noexcept;

private:
    [[nodiscard]] Status Check(const WorldSelectionContext& context, const WorldSelectionVersion& expected) const;
    std::optional<WorldSelectionVersion> version_;
    std::uint64_t retiredRevision_ = 0; // Clear 后再次绑定同一文档也不能复用旧 editorRevision。
    std::vector<WorldObjectId> selected_;
    std::optional<WorldObjectId> primary_;
    std::vector<WorldEditorFlagEdit> flags_;
};

} // namespace War3Map::Services
