#pragma once

#include "War3MapCore/MapData/PathingRestrictionEditing.h"

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainPointQuery.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"
#include "War3MapCore/MapData/TerrainBlockEditing.h"
#include "War3MapCore/MapData/TerrainStrokeJournal.h"
#include "War3MapCore/MapData/TerrainBoundsEditing.h"
#include "War3MapCore/MapData/TerrainResizeEditing.h"

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
class TerrainPathingResources;
class TerrainNativeUnitsProof;
class TerrainNativeDoodadsProof;
class TerrainResizeResources;

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
    friend class TerrainNativeDoodadsProof;
    friend class Services::AuxiliaryEditing;
    struct Command;
    struct TerrainBoundsState;
    struct TerrainStartLocationsState;
    struct TerrainHistoryState;
    struct TerrainResizeState;
    friend Status MaterializeTerrainJournalHistory(TerrainStrokeJournal& journal);
public:
    // 仅真实会话、真实修订能产生的可选历史冻结；不持reader、wire资格或旧私有地址。
    class TerrainHistorySaveLease {
    public:
        TerrainHistorySaveLease() = default;
        [[nodiscard]] std::size_t RetainedBytes() const noexcept { return retainedBytes_; }
        [[nodiscard]] std::size_t PreparationBytes() const noexcept { return preparationBytes_; }
    private:
        friend class MapDataEditSession;
        std::shared_ptr<const TerrainHistoryState> state_;
        std::shared_ptr<const TerrainHistoryState> previousEpoch_;
        std::shared_ptr<const MapDataEditSession> checkpoint_;
        std::uint64_t sessionIdentity_ = 0, revision_ = 0, sourceRevision_ = 0;
        std::size_t retainedBytes_ = 0, preparationBytes_ = 0;
    };
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
        // 只用于普通笔刷；WaterBrush非空时必须读取水刷描述，不用此兼容字段重放。
        [[nodiscard]] const TerrainBrush& Brush() const noexcept { return brush_; }
        // 水刷使用自己的完整锚点与随机序列，不能当成普通高度笔刷重放。
        [[nodiscard]] const TerrainWaterBrush* WaterBrush() const noexcept { return waterBrush_ ? &*waterBrush_ : nullptr; }
        [[nodiscard]] const TerrainWaterBrushResult* WaterResult() const noexcept { return waterResult_ ? &*waterResult_ : nullptr; }
        [[nodiscard]] const TerrainBlockPasteIntent* BlockPasteIntent() const noexcept { return blockPaste_ ? &*blockPaste_ : nullptr; }
        // 协议宿主须保留本次实际准备的算法，不能按当前默认值重建历史。
        [[nodiscard]] TerrainCornerRampAlgorithm CornerRampAlgorithm() const noexcept { return cornerRampAlgorithm_; }
        [[nodiscard]] TerrainStraightRampAlgorithm StraightRampAlgorithm() const noexcept { return straightRampAlgorithm_; }
        // 水刷/原版悬崖来源检查、算法及事务准备的预算结算；其他笔刷为0，算法计数在WaterResult中。
        [[nodiscard]] std::size_t WaterPreparationWork() const noexcept { return waterPreparationWork_; }
        // 本笔已预收的临时候选/撤销/联动峰值；不能把已释放临时网格累计当成历史。
        [[nodiscard]] std::size_t WaterPreparationBytes() const noexcept { return waterPreparationBytes_; }
        // 真实命令在历史中保留的容量上界；共享前态保守重复计，溢出时饱和。
        [[nodiscard]] std::size_t RetainedCommandBytes() const noexcept;
        [[nodiscard]] std::span<const TerrainStrokePoint> Points() const noexcept { return points_; }
        [[nodiscard]] bool FollowDoodads() const noexcept { return followDoodads_; }
        // 同一密封候选的只读地形；预览不消费事务或授予保存资格。
        [[nodiscard]] std::shared_ptr<const TerrainGridSnapshot> TerrainPreview() const noexcept { return after_; }
    private:
        friend class MapDataEditSession;
        PreparedTerrainEdit() = default;
        std::shared_ptr<const Command> command_;
        std::shared_ptr<const TerrainGridSnapshot> before_;
        std::shared_ptr<const TerrainGridSnapshot> after_;
        std::uint64_t sessionIdentity_ = 0, revision_ = 0, sourceRevision_ = 0;
        TerrainStrokeResult changed_;
        Status terrainPathingGate_;
        TerrainBrush brush_;
        std::optional<TerrainWaterBrush> waterBrush_;
        std::optional<TerrainWaterBrushResult> waterResult_;
        std::optional<TerrainWaterStrokeOptions> waterIntentOptions_;
        bool waterAnchorVerified_ = false; // v2纯候选已按实际规则完整核过不可变起点。
        std::string waterRulesSha256_;
        std::optional<bool> cliffEnforceWaterLimits_;
        TerrainCornerRampAlgorithm cornerRampAlgorithm_ = TerrainCornerRampAlgorithm::DryOnly;
        TerrainStraightRampAlgorithm straightRampAlgorithm_ = TerrainStraightRampAlgorithm::Independent;
        std::optional<TerrainBlockPasteIntent> blockPaste_;
        std::size_t waterPreparationWork_ = 0;
        std::size_t waterPreparationBytes_ = 0;
        std::vector<TerrainStrokePoint> points_;
        bool followDoodads_ = false;
    };
    class PreparedTerrainBoundsEdit {
    public:
        [[nodiscard]] bool HasChanges() const noexcept { return static_cast<bool>(command_); }
        [[nodiscard]] const TerrainBoundsEditIntent& Intent() const noexcept { return intent_; }
        [[nodiscard]] std::size_t ChangedMapEdges() const noexcept { return changedEdges_; }
        [[nodiscard]] std::size_t PreparationBytes() const noexcept { return preparationBytes_; }
        [[nodiscard]] std::size_t PreparationWork() const noexcept { return preparationWork_; }
        [[nodiscard]] std::size_t RetainedCommandBytes() const noexcept;
        [[nodiscard]] const std::array<float, 8>& CameraBounds() const noexcept { return cameraBounds_; }
    private:
        friend class MapDataEditSession;
        PreparedTerrainBoundsEdit() = default;
        std::shared_ptr<const Command> command_;
        std::shared_ptr<const TerrainGridSnapshot> before_;
        std::shared_ptr<const TerrainBoundsState> beforeBounds_;
        std::uint64_t sessionIdentity_ = 0, revision_ = 0, sourceRevision_ = 0;
        TerrainBoundsEditIntent intent_;
        std::array<float, 8> cameraBounds_{};
        std::size_t changedEdges_ = 0, preparationBytes_ = 0, preparationWork_ = 0;
    };
    // 独立外矩形命令；完整网格与裁去材料由同一不可伪造候选保活。
    class PreparedTerrainResizeEdit {
    public:
        [[nodiscard]] bool HasChanges() const noexcept { return static_cast<bool>(command_); }
        [[nodiscard]] const TerrainResizeIntent& Intent() const noexcept { return intent_; }
        [[nodiscard]] const TerrainHeader& BeforeHeader() const noexcept { return before_->header; }
        [[nodiscard]] const TerrainHeader& AfterHeader() const noexcept { return after_->header; }
        [[nodiscard]] std::shared_ptr<const TerrainGridSnapshot> TerrainPreview() const noexcept { return after_; }
        [[nodiscard]] std::size_t PreparationBytes() const noexcept { return preparationBytes_; }
        [[nodiscard]] std::size_t PreparationWork() const noexcept { return preparationWork_; }
        [[nodiscard]] std::size_t RetainedCommandBytes() const noexcept;
    private:
        friend class MapDataEditSession;
        PreparedTerrainResizeEdit() = default;
        std::shared_ptr<const Command> command_;
        std::shared_ptr<const TerrainGridSnapshot> before_, after_;
        std::shared_ptr<const TerrainResizeState> beforeResize_;
        std::uint64_t sessionIdentity_ = 0, revision_ = 0, sourceRevision_ = 0;
        TerrainResizeIntent intent_;
        std::size_t preparationBytes_ = 0, preparationWork_ = 0;
    };
    explicit MapDataEditSession(const MapDocument& source);
    MapDataEditSession(const MapDataEditSession&) = delete; // 会话身份与回执地址不能被复制。
    // 只冻结真实会话的保存输入；不提供可编辑副本，不从持久化字节恢复私有事务身份。
    [[nodiscard]] std::shared_ptr<const MapDataEditSession> FreezeForSave() const;
    // 在分配前预收快照实际复制的成员/WPM/诊断，地形与私有special仍共享只读所有权。
    [[nodiscard]] Result<std::shared_ptr<const MapDataEditSession>> FreezeForSave(std::size_t maximumBytes) const;
    // 贴图槽worker只保留共享地形及必要DOO/W3I；不复制其它原生成员、WPM或历史。
    [[nodiscard]] Result<std::shared_ptr<const MapDataEditSession>> FreezeForTerrainTextureSlot(
        std::size_t maximumBytes, bool includeDoodads = true) const;
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
    // 只导出真实命令的基线后前向前缀。来源成员身份由Services从实际来源补齐；
    // 返回值本身不授予special谱系，恢复必须经owned replay工厂。
    [[nodiscard]] Result<TerrainStrokeJournal> ExportTerrainStrokeJournal() const;
    [[nodiscard]] Result<TerrainHistorySaveLease> PrepareTerrainHistoryForSave(
        std::size_t maximumBytes = TerrainJournalMaximumBytes) const;
    // 仅真实保存回读/CAS及CommitSaved成功之后安装；失败不能反向否定已完成的地图保存。
    [[nodiscard]] Status CommitTerrainHistorySave(const TerrainHistorySaveLease& prepared);
    [[nodiscard]] Result<TerrainStrokeJournal> ExportTerrainHistoryJournal(
        std::size_t maximumBytes = TerrainJournalMaximumBytes) const;
    [[nodiscard]] Status VerifyTerrainJournalBaseline(std::span<const ArchiveExactMember> actual) const;
    struct TerrainJournalDraftCost { std::size_t retainedBytes = 0, comparisonWork = 0; };
    // 不复制raw成员/APR；先量驻留和比较成本，再由Services从整个恢复预算预收。
    [[nodiscard]] Result<TerrainJournalDraftCost> MeasureTerrainJournalDraft(
        std::size_t maximumBytes, std::size_t maximumWork) const;
    [[nodiscard]] bool MatchesTerrainJournalDraft(const MapDataEditSession& expected) const noexcept;
    // 类型化恢复后只合并raw草稿中已知不改变通行的说明/环境/玩家字段；几何仍须类型化命令。
    // 宿主仍须以MatchesTerrainJournalDraft逐字比较全部输出后才能安装候选。
    [[nodiscard]] Status RestoreTerrainJournalDescriptions(const MapDataEditSession& expected,
        std::size_t maximumBytes, std::size_t maximumWork);
    // owned journal工厂的普通DOO窄域步骤；完整CAS及同特殊尾后走正常EditDoodads。
    // 不恢复raw尾段来源，不接受任意成员或special改动。
    [[nodiscard]] Status ReplayTerrainJournalDoodads(const TerrainDoodadsJournalEdit& edit,
        std::size_t maximumBytes, std::size_t maximumWork);
    // 仅完整经典Units单成员；PrepareUnits若要求W3I起始位置联动则拒绝，不安装部分命令。
    [[nodiscard]] Status ReplayTerrainJournalUnits(const TerrainUnitsJournalEdit& edit,
        std::size_t maximumBytes, std::size_t maximumWork);
    [[nodiscard]] Status ReplayTerrainJournalStartLocations(const TerrainStartLocationsJournalEdit& edit,
        std::size_t maximumBytes, std::size_t maximumWork);
    [[nodiscard]] bool RequiresTerrainStrokeJournal() const noexcept { return specialDoodads_ != savedSpecialDoodads_ || HasTerrainBoundsChanges(); }
    // 工程CAS专用：包含可表达状态及完整typed前缀，不读取外部来源。
    [[nodiscard]] std::string TerrainJournalStateFingerprint() const;
    [[nodiscard]] Result<PathingRestrictionPreview> QueryPathingRestrictions(std::size_t maximumCells = 4096) const;
    [[nodiscard]] std::shared_ptr<const TerrainGridSnapshot> Terrain() const noexcept { return terrain_; }
    [[nodiscard]] bool HasTerrainPathing() const noexcept { return pathing_.has_value(); }
    // 附上来源或保存为新基线会变化；这些操作不消耗正常编辑/撤销 Revision。
    [[nodiscard]] std::uint64_t TerrainSourceRevision() const noexcept { return terrainSourceRevision_; }
    // actualMaximumCliffLevel由宿主同地图实际规则owner提供；缺少时坡道高度明确报告为近似。
    [[nodiscard]] Result<TerrainPointSample> InspectTerrainPoint(const TerrainStrokePoint& point,
        std::optional<std::uint8_t> actualMaximumCliffLevel = {}) const;
    [[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainEdit(const TerrainBrush& brush,
        std::span<const TerrainStrokePoint> points, bool followDoodads = true,
        std::size_t maximumLevelPropagationWork = TerrainLevelPropagationMaximumWork) const;
    // 实际捕获的规则必须同时匹配原始来源和当前草稿。W3E、荒芜WPM位与
    // 两份DOO的Z跟随一起准备；本笔不搬移XY、不删除对象，也不生成用户通行限制。
    // limits同时约束水刷候选和事务额外内存；保存资格仍由原生G0/G1计划决定。
    [[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainWaterEdit(const TerrainPathingResources& resources,
        const TerrainWaterBrush& brush, std::span<const TerrainStrokePoint> points,
        bool followDoodads = true, const TerrainWaterBrushLimits& limits = {}) const;
    // 普通悬崖使用真实规则和与水刷共用的原版邻接/清理事务；仍记录普通CliffLevel意图。
    [[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainCliffEdit(const TerrainPathingResources& resources,
        const TerrainBrush& brush, std::span<const TerrainStrokePoint> points,
        bool followDoodads = true, const TerrainWaterBrushLimits& limits = {}) const;
    [[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainCornerRampEdit(const TerrainPathingResources& resources,
        const TerrainBrush& brush, std::span<const TerrainStrokePoint> points,
        bool followDoodads = true, const TerrainWaterBrushLimits& limits = {},
        TerrainCornerRampAlgorithm algorithm = TerrainCornerRampAlgorithm::NativeCorners) const;
    [[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainStraightRampEdit(const TerrainPathingResources& resources,
        const TerrainBrush& brush, std::span<const TerrainStrokePoint> points, bool followDoodads = false,
        const TerrainWaterBrushLimits& limits = {},
        TerrainStraightRampAlgorithm algorithm = TerrainStraightRampAlgorithm::Independent) const;
    [[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainBlockPasteEdit(const TerrainPathingResources& resources,
        const TerrainBlockPasteIntent& intent, bool followDoodads = false,
        const TerrainWaterBrushLimits& limits = {}) const;
    [[nodiscard]] Result<TerrainStrokeResult> ApplyPreparedTerrainEdit(const PreparedTerrainEdit& plan,
        const std::function<Status(const EditReceipt&)>& prepublish = {});
    // 输入实际native资源owner；不能用任意W3I覆写或自报规则授予范围保存资格。
    [[nodiscard]] Result<PreparedTerrainBoundsEdit> PrepareTerrainBoundsEdit(const TerrainPathingResources& resources,
        const TerrainBoundsEditIntent& intent, std::size_t maximumBytes = 512u * 1024u * 1024u,
        std::size_t maximumWork = 64u * 1024u * 1024u) const;
    [[nodiscard]] Status ApplyPreparedTerrainBoundsEdit(const PreparedTerrainBoundsEdit& plan,
        const std::function<Status(const EditReceipt&)>& prepublish = {});
    [[nodiscard]] Result<PreparedTerrainResizeEdit> PrepareTerrainResizeEdit(const TerrainResizeResources& resources,
        const TerrainResizeIntent& intent, const TerrainResizeLimits& limits = {}) const;
    [[nodiscard]] Status ApplyPreparedTerrainResizeEdit(const PreparedTerrainResizeEdit& plan,
        const std::function<Status(const EditReceipt&)>& prepublish = {});
    [[nodiscard]] bool HasTerrainResizeChanges() const noexcept;
    [[nodiscard]] bool HasTerrainBoundsChanges() const noexcept;
    // 只接受密封范围状态和四说明字段的组合净差，其余W3I字节仍须完全一致。
    [[nodiscard]] Status VerifyTerrainSettingsForSave() const;
    // 一笔地形笔刷为一次事务，撤销按 16×16 顶点区块保存前后原始字段。
    // followDoodads 为真时，改动范围内的装饰物、单位与物品随地面高度变化（同一事务）。
    [[nodiscard]] Result<TerrainStrokeResult> EditTerrain(const TerrainBrush& brush,
        std::span<const TerrainStrokePoint> points, bool followDoodads = true);
    // 贴图槽替换/追加也进入同一撤销、工程恢复和原生地图保存门。
    [[nodiscard]] Result<TerrainStrokeResult> EditTerrainTextureSlot(TerrainTextureTable table,
        std::uint32_t slot, const std::string& rawcode, const TerrainPathingResources* resources = nullptr);
    // 与笔刷共用同一密封候选；worker可用冻结会话准备，前台只按原修订提交。
    [[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainTextureSlot(TerrainTextureTable table,
        std::uint32_t slot, const std::string& rawcode, const TerrainPathingResources* resources = nullptr) const;
    // 正式地图保存门：干地高度按原版六模式查询不变条件保留WPM；荒芜位与
    // WPM bit5逐格同步。涉水高度、材质通行属性与其它派生数据未知时仍拒绝。
    [[nodiscard]] Status VerifyTerrainPathingForSave() const;
    // 只核对附入会话的原始 W3E 与外部冻结来源，不读取其它地图成员。
    [[nodiscard]] Status VerifyTerrainSourceBaseline(std::span<const std::byte> sourceBytes) const;
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
    friend class TerrainPathingResources;
    struct TerrainBoundsState {
        std::array<float, 8> cameraBounds{};
        std::array<std::int32_t, 4> cameraMargins{};
        std::int32_t playableWidth = 0, playableHeight = 0;
        std::uint32_t flags = 0, width = 0, height = 0;
        std::vector<std::byte> mapEdges;
        [[nodiscard]] static std::shared_ptr<const TerrainBoundsState> Capture(const MapSettings&, const TerrainGridSnapshot&);
        [[nodiscard]] bool Matches(const MapSettings&, const TerrainGridSnapshot&) const noexcept;
        [[nodiscard]] bool MatchesSettings(const MapSettings&) const noexcept;
        [[nodiscard]] bool MatchesSettingsBytes(std::span<const std::byte>) const noexcept;
        [[nodiscard]] bool Equivalent(const TerrainBoundsState&) const noexcept;
        void RestoreGeometry(MapSettings&) const noexcept;
    };
    struct TerrainStartLocationsState {
        struct Player { std::int32_t number = 0; std::uint32_t x = 0, y = 0; };
        std::array<Player, 16> players{};
        std::size_t count = 0, unitsBytes = 0;
        std::string unitsSha256;
        [[nodiscard]] static Result<std::shared_ptr<const TerrainStartLocationsState>> Capture(
            std::span<const std::byte>, const UnitPlacements&, const MapSettings&);
        [[nodiscard]] bool Matches(std::span<const std::byte>, const MapSettings&) const;
        [[nodiscard]] bool MatchesUnits(std::span<const std::byte>) const;
        [[nodiscard]] bool MatchesPlayers(const MapSettings&) const noexcept;
        void RestorePlayers(MapSettings&) const noexcept;
    };
    // 不同shape的历史基线必须随typed命令保活；物理来源仍留在terrainOriginal_/pathingOriginal_。
    // 保存后Undo不能把新尺寸SHD/WPM误用到旧尺寸，也不从raw草稿恢复此资格。
    struct TerrainResizeState {
        TerrainHeader header;
        ArchiveExactMember shadow;
        std::shared_ptr<const std::vector<std::byte>> historyPathing;
        std::shared_ptr<const std::vector<std::byte>> protectedPathing;
        std::shared_ptr<const std::vector<std::byte>> savedRestrictions;
        std::string rulesSha256;
    };
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
        std::shared_ptr<const std::vector<std::byte>> restrictionBefore, restrictionAfter;
        std::uint64_t sessionIdentity = 0, logicalIdentity = 0;
        std::uint64_t beforeRevision = 0, afterRevision = 0;
        bool logicalForward = true;
        bool replayed = false;
        std::shared_ptr<const std::vector<PlacementRowLineage>> placements;
        std::shared_ptr<const std::vector<SpecialDoodadPlacement>> specialBefore, specialAfter;
        std::optional<TerrainJournalStroke> terrainStroke;
        std::shared_ptr<const TerrainBoundsState> boundsBefore, boundsAfter;
        std::optional<TerrainBoundsEditIntent> boundsIntent;
        std::string boundsRulesSha256;
        std::shared_ptr<const TerrainStartLocationsState> startsBefore, startsAfter;
        std::shared_ptr<const TerrainGridSnapshot> fullTerrainBefore, fullTerrainAfter;
        std::shared_ptr<const TerrainResizeState> resizeBefore, resizeAfter;
    };
    struct TerrainHistoryState {
        std::shared_ptr<const MapDataEditSession> baseline;
        std::vector<std::shared_ptr<const Command>> savedCommands;
        std::size_t savedIndex = 0, retainedBytes = 0;
        std::uint64_t savedSourceRevision = 0;
    };
    struct TerrainHistoryLine {
        std::vector<std::shared_ptr<const Command>> commands;
        std::size_t currentIndex = 0;
    };
    [[nodiscard]] Result<TerrainHistoryLine> CaptureTerrainHistoryLine(std::size_t maximumBytes) const;
    [[nodiscard]] std::string TerrainHistoryStateFingerprint() const;
    [[nodiscard]] Result<TerrainStrokeJournal> ExportTerrainJournalCommands(
        std::span<const std::shared_ptr<const Command>> commands, bool baselineLayers) const;
    [[nodiscard]] Result<std::vector<ArchiveExactMember>> TerrainHistoryBaselineMembers(std::size_t maximumBytes) const;
    [[nodiscard]] Status Apply(MapDataKind kind, Result<std::vector<std::byte>> serialized);
    [[nodiscard]] Status Prepare(MapDataKind kind, Result<std::vector<std::byte>> serialized,
        std::vector<MemberChange>& changes, bool ownedBounds = false) const;
    [[nodiscard]] Status Commit(std::vector<MemberChange> changes);
    [[nodiscard]] Status CommitCommand(Command command,
        const std::function<Status(const EditReceipt&)>& prepublish = {});
    [[nodiscard]] Status EditPlacementRows(const MapDocument& source, const UnitPlacements* units, const DoodadPlacements* doodads,
        std::vector<PlacementRowLineage> lineage,
        const std::function<Status(const EditReceipt&)>& prepublish = {});
    [[nodiscard]] bool TerrainMatches(const Command& command, bool forward) const;
    [[nodiscard]] bool TerrainDirty() const noexcept;
    [[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainEditInternal(const TerrainBrush& brush,
        std::span<const TerrainStrokePoint> points, bool followDoodads, bool inspectSaveGate,
        std::size_t maximumLevelPropagationWork = TerrainLevelPropagationMaximumWork) const;
    [[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainStructuralEdit(const TerrainPathingResources& resources,
        const TerrainWaterBrush* water, const TerrainBrush* cliff, std::span<const TerrainStrokePoint> points,
        bool followDoodads, const TerrainWaterBrushLimits& limits,
        TerrainCornerRampAlgorithm cornerAlgorithm = TerrainCornerRampAlgorithm::DryOnly,
        TerrainStraightRampAlgorithm straightAlgorithm = TerrainStraightRampAlgorithm::Independent) const;
    [[nodiscard]] Result<PreparedTerrainEdit> PreparePathingRestrictionEdit(const TerrainBrush& brush,
        std::span<const TerrainStrokePoint> points, bool followDoodads, bool inspectSaveGate) const;
    [[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainCandidate(
        std::shared_ptr<const TerrainGridSnapshot> next, const TerrainStrokeResult& changed,
        std::span<const TerrainStrokePoint> points, bool followDoodads, bool shaping,
        bool synchronizeBlight, bool inspectSaveGate, DoodadPlacements* parsedDoodads = nullptr, bool specialChanged = false,
        std::optional<std::uint8_t> nativeMaximumCliffLevel = {},
        std::size_t maximumNativeFollowWork = std::numeric_limits<std::size_t>::max()) const;
    [[nodiscard]] Status SetTerrainAndPathingInternal(std::span<const std::byte> terrainBytes,
        std::span<const std::byte> pathingBytes, std::span<const std::byte> restrictionDraft);
    [[nodiscard]] Status VerifyTerrainCandidateForSave(
        const std::shared_ptr<const TerrainGridSnapshot>& terrain,
        const std::optional<std::vector<std::byte>>& pathing,
        const std::optional<std::vector<std::byte>>& doodads,
        const TerrainPathingResources* resources = nullptr,
        const std::vector<std::byte>* restrictions = nullptr,
        const std::optional<std::vector<std::byte>>* units = nullptr,
        bool nativeRebuild = false, const TerrainNativeUnitsProof* nativeUnits = nullptr,
        const TerrainNativeDoodadsProof* nativeDoodads = nullptr) const;
    [[nodiscard]] Status VerifyTerrainDoodadChange(std::span<const std::byte> before,
        std::span<const std::byte> after) const;
    [[nodiscard]] Status VerifyTerrainSpecialDoodadChange(std::span<const std::byte> before,
        std::span<const std::byte> after) const;
    [[nodiscard]] Status VerifyTerrainSpecialDoodadProvenance(
        const DoodadPlacements& before, const DoodadPlacements& after) const;
    [[nodiscard]] Result<TerrainStrokeJournal> ExportTerrainStrokeJournalInternal(bool baselineLayers) const;
    [[nodiscard]] Status PrepareUnits(const UnitPlacements& value, std::vector<MemberChange>& changes,
        bool allowLinkedSettings = true, Command* linkedOwner = nullptr) const;
    void InitializeTerrainStartLocations(const MapDocument&);
    [[nodiscard]] Status PrepareTerrainStartLocations(Command&, const UnitPlacements&,
        const UnitPlacements&, const std::vector<MemberChange>&) const;
    [[nodiscard]] Status NormalizeTerrainStartLocationsForSave(MapSettings& current, const MapSettings& original) const;
    [[nodiscard]] Status DeserializeAndApply(MapDataKind kind, std::span<const std::byte> bytes,
        bool linkStartLocations);
    void Restore(const Command& command, bool forward);
    [[nodiscard]] const std::vector<std::byte>& TerrainHistoryPathing(const TerrainResizeState* shape = nullptr) const;
    [[nodiscard]] std::shared_ptr<const std::vector<std::byte>> TerrainHistoryProtected(const TerrainResizeState* shape = nullptr) const;
    // 先准备全部新基线，再无异常交换；供已回读的通行重建输出原子提交。
    void CommitSavedBaseline(const MapDocument& saved,
        std::optional<std::vector<std::byte>> pathing,
        std::shared_ptr<const std::vector<std::byte>> protectedPathing);
    [[nodiscard]] static std::size_t Index(MapDataKind kind) noexcept;
    struct SaveSnapshotTag {};
    MapDataEditSession(const MapDataEditSession& source, SaveSnapshotTag);
    struct TextureSlotSnapshotTag {};
    MapDataEditSession(const MapDataEditSession& source, TextureSlotSnapshotTag, bool includeDoodads);

    std::array<MapDataState, 7> states_{};
    std::array<std::string, 7> memberNames_{};
    std::array<std::optional<std::vector<std::byte>>, 7> original_{};
    std::array<std::optional<std::vector<std::byte>>, 7> current_{};
    // 无法建立可写原生基线的成员仍属于来源；快照不能把损坏、歧义或只读成员改称缺失。
    std::array<std::optional<MapMemberState>, 7> preservedSources_{};
    std::vector<std::shared_ptr<const Command>> history_;
    // 真保存边界的历史前缀身份。Qt Replay追加inverse，不能只比较cursor数字。
    std::shared_ptr<const Command> terrainJournalSavedBoundary_;
    std::shared_ptr<const TerrainHistoryState> terrainHistoryEpoch_;
    std::size_t terrainJournalSavedCursor_ = 0;
    std::size_t cursor_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t sessionIdentity_ = 0, nextCommandIdentity_ = 1;
    std::uint64_t terrainSourceRevision_ = 0;
    EditReceipt lastApplied_;
    // 只由真实来源构造、可信水刷更新；与Command一起撤销，保存只移动基线，不丢历史身份。
    std::shared_ptr<const std::vector<SpecialDoodadPlacement>> specialDoodads_, savedSpecialDoodads_;
    std::shared_ptr<const TerrainBoundsState> boundsState_, savedBoundsState_;
    std::shared_ptr<const TerrainStartLocationsState> startLocations_, savedStartLocations_;
    std::shared_ptr<const TerrainResizeState> resizeState_, savedResizeState_;
    std::shared_ptr<const TerrainGridSnapshot> terrainOriginal_;
    std::shared_ptr<const TerrainGridSnapshot> terrain_;
    std::optional<std::vector<std::byte>> pathingOriginal_;
    std::optional<std::vector<std::byte>> pathing_;
    // 首次从真实 WPM 捕获；地图保存不会把本工程新增限制升级为受保护来源。
    std::shared_ptr<const std::vector<std::byte>> pathingProtected_;
    std::shared_ptr<const std::vector<std::byte>> pathingRestrictions_, originalPathingRestrictions_;
};

} // namespace War3Map::MapData
