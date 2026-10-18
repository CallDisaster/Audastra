#pragma once

#include "War3MapCore/MapData/PathingRestrictionEditing.h"

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainPointQuery.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace War3Map::Services { class AuxiliaryEditing; }

namespace War3Map::MapData {
namespace detail { struct PlacementEditingAccess; struct TerrainInspectionAccess; struct MapDataFieldEditingAccess; }
class PreparedMapDataFields;
class PreparedMapDataPlacement;
class TerrainSavePlan;

// 仅已提交的摆放操作提供完整行谱系；空目标表示删除，未被指向的新行表示新增。
// 不能用rawcode、创建编号或坐标猜测映射。此值在EditReceipt中只读。
struct PlacementRowLineage {
    MapDataKind kind = MapDataKind::Units;
    std::uint32_t beforeCount = 0, afterCount = 0;
    std::vector<std::optional<std::uint32_t>> beforeToAfter;
};

// 七类公共地图成员的编辑覆盖。仅提交经过各类型序列化器检查的完整字节；
// 不触碰源归档，撤销/重做恢复覆盖状态，MapArchiveSession 负责候选发布。
class MapDataEditSession {
    friend class TerrainSavePlan;
    friend class Services::AuxiliaryEditing;
    struct Command;
public:
    // 不可变事务回执供外层统一撤销栈使用；包含所有联动成员，不重新推导逆操作。
    class EditReceipt {
    public:
        EditReceipt() = default;
        [[nodiscard]] std::uint64_t SessionIdentity() const noexcept;
        [[nodiscard]] std::uint64_t CommandIdentity() const noexcept;
        [[nodiscard]] std::uint64_t BeforeRevision() const noexcept { return beforeRevision_; }
        [[nodiscard]] std::uint64_t AfterRevision() const noexcept { return afterRevision_; }
        [[nodiscard]] bool Forward() const noexcept;
        [[nodiscard]] bool Restoring() const noexcept { return restoring_; } // Replay/Undo/Redo，不能当首次新增。
        [[nodiscard]] bool HasPlacementLineage() const noexcept;
        [[nodiscard]] bool ChangesPlacements() const noexcept;
        [[nodiscard]] std::span<const PlacementRowLineage> PlacementLineage() const noexcept;
        // 本次应用方向的原生成员字节；未变化成员返回空，span由回执持有的命令保活。
        [[nodiscard]] std::optional<std::span<const std::byte>> PlacementMemberBytes(MapDataKind kind, bool after) const noexcept;
    private:
        friend class MapDataEditSession;
        EditReceipt(std::shared_ptr<const Command> command, const MapDataEditSession* session,
            std::uint64_t before, std::uint64_t after, bool appliedForward = true, bool restoring = false)
            : command_(std::move(command)), session_(session), beforeRevision_(before), afterRevision_(after), appliedForward_(appliedForward), restoring_(restoring) {}
        std::shared_ptr<const Command> command_;
        const MapDataEditSession* session_ = nullptr;
        std::uint64_t beforeRevision_ = 0, afterRevision_ = 0;
        bool appliedForward_ = true;
        bool restoring_ = false;
    };
    // 纯准备得到的不可伪造候选；只能向原会话、原修订和原地形来源提交。
    class PreparedTerrainEdit {
    public:
        [[nodiscard]] const TerrainStrokeResult& Changed() const noexcept { return changed_; }
        [[nodiscard]] const Status& TerrainPathingGate() const noexcept { return terrainPathingGate_; }
        [[nodiscard]] bool HasChanges() const noexcept;
        [[nodiscard]] const TerrainBrush& Brush() const noexcept { return brush_; }
        [[nodiscard]] std::span<const TerrainStrokePoint> Points() const noexcept { return points_; }
        [[nodiscard]] bool FollowDoodads() const noexcept { return followDoodads_; }
    private:
        friend class MapDataEditSession;
        PreparedTerrainEdit() = default;
        std::shared_ptr<const Command> command_;
        std::shared_ptr<const TerrainGridSnapshot> before_;
        std::uint64_t sessionIdentity_ = 0, revision_ = 0, sourceRevision_ = 0;
        TerrainStrokeResult changed_;
        Status terrainPathingGate_;
        TerrainBrush brush_;
        std::vector<TerrainStrokePoint> points_;
        bool followDoodads_ = false;
    };
    explicit MapDataEditSession(const MapDocument& source);
    MapDataEditSession(const MapDataEditSession&) = delete; // 会话身份与回执地址不能被复制。
    MapDataEditSession& operator=(const MapDataEditSession&) = delete;

    [[nodiscard]] Status EditSettings(const MapSettings& value);
    [[nodiscard]] Status EditUnits(const UnitPlacements& value);
    [[nodiscard]] Status EditDoodads(const DoodadPlacements& value);
    // 单位与装饰物放置作为一次事务提交（空指针表示该成员不变）；单位部分同样联动起始位置。
    [[nodiscard]] Status EditPlacements(const UnitPlacements* units, const DoodadPlacements* doodads);
    [[nodiscard]] Status EditRegions(const RegionsDocument& value);
    [[nodiscard]] Status EditCameras(const CamerasDocument& value);
    [[nodiscard]] Status EditSounds(const SoundsDocument& value);
    [[nodiscard]] Status EditStrings(const StringTableDocument& value);
    // 共用字段 planner 的密封候选；原会话回执与已有撤销历史保持同一身份。
    [[nodiscard]] Status ApplyPreparedFields(const PreparedMapDataFields& plan,
        const std::function<Status(const MapDocument&, const MapDocument&, const EditReceipt&)>& validator = {});
    // 摆放共用纯候选只向原会话提交，完整回执在历史/身份/字节发布前验收。
    [[nodiscard]] Status ApplyPreparedPlacement(const PreparedMapDataPlacement& plan,
        const std::function<Status(const MapDocument&, const MapDocument&, const EditReceipt&)>& validator = {});
    [[nodiscard]] Status EditSerialized(MapDataKind kind, std::span<const std::byte> bytes);
    // 工程恢复预检专用：原生字节必须先通过对应类型解析与写出检查。
    // 精确恢复已保存成员，不重新推导起始位置联动，以保留原有不一致来源。
    [[nodiscard]] Status SetSerialized(MapDataKind kind, std::span<const std::byte> bytes);
    [[nodiscard]] Status Remove(MapDataKind kind);

    // 地形（war3map.w3e）不属于 MapDocument：打开后、编辑前由调用方附上源字节。解析失败时地形不可编辑。
    [[nodiscard]] Status AttachTerrain(std::span<const std::byte> bytes);
    // 通行性（war3map.wpm）须在源地形之后、编辑之前附上唯一原生来源；缺失时荒芜笔刷拒绝写入。
    [[nodiscard]] Status AttachPathing(std::span<const std::byte> bytes);
    // 工程恢复：以已保存的地形覆盖作为一次修改；宽高与原点必须与源地形一致。
    [[nodiscard]] Status SetTerrain(std::span<const std::byte> bytes);
    // 工程恢复：只接受与源 WPM 头和非荒芜位完全相同、荒芜位与当前 W3E 一致的覆盖。
    [[nodiscard]] Status SetPathing(std::span<const std::byte> bytes);
    // 工程恢复：两种覆盖一起预检并作为一条撤销命令提交；任一失败时两者都不改。
    [[nodiscard]] Status SetTerrainAndPathing(std::span<const std::byte> terrainBytes,
        std::span<const std::byte> pathingBytes);
    // 新增通行限制只能携带来源绑定的工程声明恢复；原有 raw 覆盖入口不放宽。
    [[nodiscard]] Status SetTerrainAndPathingDraft(std::span<const std::byte> terrainBytes,
        std::span<const std::byte> pathingBytes, std::span<const std::byte> restrictionDraft);
    [[nodiscard]] Result<std::vector<std::byte>> SerializePathingRestrictionDraft() const;
    [[nodiscard]] Result<PathingRestrictionPreview> QueryPathingRestrictions(std::size_t maximumCells = 4096) const;
    [[nodiscard]] std::shared_ptr<const TerrainGridSnapshot> Terrain() const noexcept { return terrain_; }
    [[nodiscard]] bool HasTerrainPathing() const noexcept { return pathing_.has_value(); }
    // 附上来源或保存为新基线会变化；这些操作不消耗正常编辑/撤销 Revision。
    [[nodiscard]] std::uint64_t TerrainSourceRevision() const noexcept { return terrainSourceRevision_; }
    [[nodiscard]] Result<TerrainPointSample> InspectTerrainPoint(const TerrainStrokePoint& point) const;
    [[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainEdit(const TerrainBrush& brush,
        std::span<const TerrainStrokePoint> points, bool followDoodads = true) const;
    [[nodiscard]] Result<TerrainStrokeResult> ApplyPreparedTerrainEdit(const PreparedTerrainEdit& plan,
        const std::function<Status(const EditReceipt&)>& prepublish = {});
    // 一笔地形笔刷为一次事务，撤销按 16×16 顶点区块保存前后原始字段。
    // followDoodads 为真时，改动范围内的装饰物、单位与物品随地面高度变化（同一事务）。
    [[nodiscard]] Result<TerrainStrokeResult> EditTerrain(const TerrainBrush& brush,
        std::span<const TerrainStrokePoint> points, bool followDoodads = true);
    // 贴图槽替换/追加也进入同一撤销、工程恢复和原生地图保存门。
    [[nodiscard]] Result<TerrainStrokeResult> EditTerrainTextureSlot(TerrainTextureTable table,
        std::uint32_t slot, const std::string& rawcode);
    // 正式地图保存门禁：仅证实原地形荒芜位与 WPM bit5 的逐格同步；其它地形改动拒绝保存。
    [[nodiscard]] Status VerifyTerrainPathingForSave() const;
    [[nodiscard]] EditReceipt LastEdit() const;
    // 最近一次真实应用（提交/Replay/Undo/Redo）；LastEdit仍供原统一撤销栈取得命令。
    [[nodiscard]] EditReceipt LastAppliedEdit() const { return lastApplied_; }
    [[nodiscard]] std::uint64_t SessionIdentity() const noexcept { return sessionIdentity_; }
    [[nodiscard]] Status Replay(const EditReceipt& receipt, bool forward);

    [[nodiscard]] bool CanUndo() const noexcept { return cursor_ != 0; }
    [[nodiscard]] bool CanRedo() const noexcept { return cursor_ < history_.size(); }
    [[nodiscard]] bool Undo();
    [[nodiscard]] bool Redo();
    [[nodiscard]] bool IsDirty() const noexcept;
    [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }
    [[nodiscard]] std::vector<MapArchiveMember> Replacements() const;
    [[nodiscard]] std::vector<std::string> Removals() const;
    // 仅在候选已通过回读并成为新的归档基线后调用；保留撤销历史。
    void CommitSaved(const MapDocument& saved);
    [[nodiscard]] MapDocument Snapshot() const;
    // 与有界Snapshot相同的成员/W3I选择，只计wire字节，不复制正文或解析。
    // 来源不可读/歧义仍失败；供调用者在对象解析前预收实际容量。
    [[nodiscard]] Result<std::size_t> SnapshotByteSize(MapDataKind kind) const;
    // 有界只读快照：只复制所需成员；摆放成员额外保留 W3I 布局上下文。
    // 已知缺失与来源不可读/歧义分别报告，不把无法确定的来源当作空表。
    [[nodiscard]] Result<MapDocument> Snapshot(MapDataKind kind,
        std::size_t maximumBytes = std::numeric_limits<std::size_t>::max()) const;

private:
    friend class TerrainPatchCodec;
    friend struct detail::PlacementEditingAccess;
    friend struct detail::TerrainInspectionAccess;
    friend struct detail::MapDataFieldEditingAccess;
    struct MemberChange {
        MapDataKind kind;
        std::optional<std::vector<std::byte>> before;
        std::optional<std::vector<std::byte>> after;
    };
    struct TerrainChunk {
        std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // [x0,x1)×[y0,y1)
        std::vector<std::uint8_t> before, after;      // 每顶点 7 字节原始字段
    };
    struct Command {
        std::vector<MemberChange> changes;
        std::vector<TerrainChunk> terrain;
        std::optional<TerrainHeader> headerBefore, headerAfter;
        std::optional<std::vector<std::byte>> pathingBefore, pathingAfter;
        std::uint64_t sessionIdentity = 0, logicalIdentity = 0;
        std::uint64_t beforeRevision = 0, afterRevision = 0;
        bool logicalForward = true;
        bool replayed = false;
        std::shared_ptr<const std::vector<PlacementRowLineage>> placements;
    };
    [[nodiscard]] Status Apply(MapDataKind kind, Result<std::vector<std::byte>> serialized);
    [[nodiscard]] Status Prepare(MapDataKind kind, Result<std::vector<std::byte>> serialized,
        std::vector<MemberChange>& changes) const;
    [[nodiscard]] Status Commit(std::vector<MemberChange> changes);
    [[nodiscard]] Status CommitCommand(Command command,
        const std::function<Status(const EditReceipt&)>& prepublish = {});
    [[nodiscard]] Status EditPlacementRows(const MapDocument& source, const UnitPlacements* units, const DoodadPlacements* doodads,
        std::vector<PlacementRowLineage> lineage,
        const std::function<Status(const EditReceipt&)>& prepublish = {});
    [[nodiscard]] bool TerrainMatches(const Command& command, bool forward) const;
    [[nodiscard]] bool TerrainDirty() const noexcept;
    [[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainEditInternal(const TerrainBrush& brush,
        std::span<const TerrainStrokePoint> points, bool followDoodads, bool inspectSaveGate) const;
    [[nodiscard]] Result<PreparedTerrainEdit> PreparePathingRestrictionEdit(const TerrainBrush& brush,
        std::span<const TerrainStrokePoint> points, bool followDoodads, bool inspectSaveGate) const;
    [[nodiscard]] Status SetTerrainAndPathingInternal(std::span<const std::byte> terrainBytes,
        std::span<const std::byte> pathingBytes, std::span<const std::byte> restrictionDraft);
    [[nodiscard]] Status VerifyTerrainCandidateForSave(
        const std::shared_ptr<const TerrainGridSnapshot>& terrain,
        const std::optional<std::vector<std::byte>>& pathing,
        const std::optional<std::vector<std::byte>>& doodads) const;
    [[nodiscard]] Status PrepareUnits(const UnitPlacements& value, std::vector<MemberChange>& changes) const;
    [[nodiscard]] Status DeserializeAndApply(MapDataKind kind, std::span<const std::byte> bytes,
        bool linkStartLocations);
    void Restore(const Command& command, bool forward);
    [[nodiscard]] static std::size_t Index(MapDataKind kind) noexcept;

    std::array<MapDataState, 7> states_{};
    std::array<std::string, 7> memberNames_{};
    std::array<std::optional<std::vector<std::byte>>, 7> original_{};
    std::array<std::optional<std::vector<std::byte>>, 7> current_{};
    // 无法建立可写原生基线的成员仍属于来源；快照不能把损坏、歧义或只读成员改称缺失。
    std::array<std::optional<MapMemberState>, 7> preservedSources_{};
    std::vector<std::shared_ptr<const Command>> history_;
    std::size_t cursor_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t sessionIdentity_ = 0, nextCommandIdentity_ = 1;
    std::uint64_t terrainSourceRevision_ = 0;
    EditReceipt lastApplied_;
    std::shared_ptr<const TerrainGridSnapshot> terrainOriginal_;
    std::shared_ptr<const TerrainGridSnapshot> terrain_;
    std::optional<std::vector<std::byte>> pathingOriginal_;
    std::optional<std::vector<std::byte>> pathing_;
};

} // namespace War3Map::MapData
