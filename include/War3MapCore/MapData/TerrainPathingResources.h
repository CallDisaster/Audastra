#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Game/NativeDoodadCategoryDirectory.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"
#include "War3MapCore/MapData/TerrainNativeObjectPathing.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"
#include "War3MapCore/Workspace.h"

#include <functional>
#include <map>
#include <memory>

namespace War3Map::Services { class TerrainPathingSave; class TerrainResizeEditService; class TerrainSessionResources; class TerrainHistorySave; }

namespace War3Map::MapData {
class MapDataEditSession;

struct DoodadPlacement;
enum class TerrainNativeStage { Before, After };

// 实际读取的地形语义输入；只能由真实地图/游戏来源建立。
// 旧Capture证明等价修改；CaptureNative另捕获完整生成输入，仍由保存计划
// 比较旧WPM六位并保留0x11与显式工程意图，不从合成位猜作者来源。
// CaptureWaterBrushRules仅捕获编辑规则，不建立保存资格。
class TerrainPathingResources {
public:
    // 只读显示/查询适配：回调须读取宿主冻结的同一地图/会话覆盖，缺失项才回退gameRoot。
    // 复用原生profile加载与来源owner；不读取DOO/水表，不建立任何地图保存资格。
    using HeightSourceReader = std::function<Result<std::vector<ArchiveExactMember>>(
        std::span<const std::string>, const ArchiveExactMemberLimits&)>;
    [[nodiscard]] static Result<TerrainPathingResources> CaptureHeightRules(const HeightSourceReader& reader,
        std::uint8_t tileset, const std::filesystem::path& gameRoot, std::size_t maximumBytes);
    [[nodiscard]] std::optional<std::uint8_t> HeightMaximumCliffLevel() const noexcept {
        return heightRulesOnly_ ? nativeMaximumCliffLevel_ : std::nullopt;
    }
    [[nodiscard]] Status VerifyHeightRuleSources(const HeightSourceReader& reader, std::size_t maximumBytes) const;
    [[nodiscard]] Result<std::string> HeightRuleDependencyFingerprint() const;
    [[nodiscard]] static bool Needed(const TerrainGridSnapshot& before, const TerrainGridSnapshot& after);
    [[nodiscard]] static Result<TerrainPathingResources> Capture(const MapArchiveSession& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes);
    // maximumBytes约束累计实际来源读取，并限制每次W3E身份序列化缓冲；
    // 不是整个保存事务的峰值工作内存预算，调用者另计快照、G0/G1和输出容量。
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNative(const MapArchiveSession& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNative(const WorkspaceSnapshot& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes);
    // 编辑器采用的经典固定初始化规则链；完整捕获其map/coreMPQ winner或缺失。
    // sourceTerrain必须属于实际来源W3E；currentTerrain为本笔冻结的Session草稿，
    // 其palette可包含此前的贴图编辑。enforceWaterLimits是本笔选项，不读取注册表。
    // 仅提供编辑算法输入，不授予原版运行时配置或保存资格；保存仍须CaptureNative。
    [[nodiscard]] static Result<TerrainPathingResources> CaptureWaterBrushRules(const MapArchiveSession& source,
        const TerrainGridSnapshot& sourceTerrain, const TerrainGridSnapshot& currentTerrain,
        const std::filesystem::path& gameRoot, bool enforceWaterLimits, std::size_t maximumBytes);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureWaterBrushRules(const WorkspaceSnapshot& source,
        const TerrainGridSnapshot& sourceTerrain, const TerrainGridSnapshot& currentTerrain,
        const std::filesystem::path& gameRoot, bool enforceWaterLimits, std::size_t maximumBytes);
    // 未编辑来源的便利重载，转发sourceTerrain=currentTerrain。
    [[nodiscard]] static Result<TerrainPathingResources> CaptureWaterBrushRules(const MapArchiveSession& source,
        const TerrainGridSnapshot& before, const std::filesystem::path& gameRoot,
        bool enforceWaterLimits, std::size_t maximumBytes);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureWaterBrushRules(const WorkspaceSnapshot& source,
        const TerrainGridSnapshot& before, const std::filesystem::path& gameRoot,
        bool enforceWaterLimits, std::size_t maximumBytes);
    [[nodiscard]] const TerrainWaterLoadedRules* WaterBrushRules() const noexcept {
        return waterBrushRules_ ? &*waterBrushRules_ : nullptr;
    }
    // 调用方须将冻结current的canonical W3E身份与落笔前Session核对；不是after身份。
    [[nodiscard]] std::string_view WaterBrushInputSha256() const noexcept { return waterBrushInputSha_; }
    [[nodiscard]] std::size_t WaterBrushInputBytes() const noexcept { return waterBrushInputBytes_; }
    // 实际成功owner的完整依赖/winner/absence及本笔输入摘要；不能代替VerifyReadback。
    // v1 wire另保留旧mapMisc缺失的固定常量槽，不证明当前地图Misc存在或缺失。
    // 无分配的wire容量上界；0表示缺少身份或超出1MiB/4096依赖记录域。
    [[nodiscard]] std::size_t WaterBrushDependencyBudgetBytes() const noexcept;
    [[nodiscard]] Result<std::string> WaterBrushDependencyFingerprint() const;
    // 仅真实水刷规则owner可验证捕获时的source/current；分别比较canonical W3E身份。
    // 不读盘、不授予保存资格；宿主另复核实际资源来源与Session修订。
    [[nodiscard]] Status VerifyWaterBrushInput(const TerrainGridSnapshot& sourceTerrain,
        const TerrainGridSnapshot& currentTerrain) const;
    // 只允许本次捕获的旧/新W3E内容；输出不含头、源WPM及工程意图。
    // 身份缓冲在生成输出前释放；maximumBytes分别约束该缓冲和生成输出。
    [[nodiscard]] Result<std::vector<std::byte>> GenerateNative(const TerrainGridSnapshot& terrain,
        std::size_t maximumBytes) const;
    [[nodiscard]] Result<std::vector<std::byte>> GenerateNative(const TerrainGridSnapshot& terrain,
        TerrainNativeStage stage, std::size_t maximumBytes) const;
    [[nodiscard]] std::span<const TerrainWaterSpecialDoodadShape> WaterSpecialShapes() const noexcept { return waterSpecialShapes_; }
    [[nodiscard]] Status VerifyWaterBrushDoodads(const DoodadPlacements& doodads, std::span<const std::byte> source = {}) const;
    [[nodiscard]] bool EquivalentGround(std::string_view before, std::string_view after) const;
    // 无坡道顶点的原版 D=0/1/2 和严格覆水状态都必须相等；不使用显示网格高度。
    [[nodiscard]] bool EquivalentWater(const TerrainVertex& before, const TerrainVertex& after) const;
    [[nodiscard]] Status VerifyFrozenOutput(std::span<const MemberReplacement> replacements,
        std::span<const std::string> removals) const;
    // 水刷规则owner在此只复核原始来源W3E/规则；不能用编辑后的输出冒充原始来源。
    [[nodiscard]] Status VerifyReadback(const MapArchiveSession& saved, std::size_t maximumBytes) const;
    [[nodiscard]] Status VerifyReadback(const WorkspaceSnapshot& saved, std::size_t maximumBytes) const;
    // 编辑落笔前复核捕获时的真实旧来源；与保存后新W3E/W3I/DOO回读分开。
    // 所有地图依赖均核对原始SHA/存在性，游戏winner/absence及预算与回读共用。
    [[nodiscard]] Status VerifySourceInputs(const MapArchiveSession& source, std::size_t maximumBytes) const;
    [[nodiscard]] Status VerifySourceInputs(const WorkspaceSnapshot& source, std::size_t maximumBytes) const;
    [[nodiscard]] std::size_t SourceBytes() const noexcept {
        return sourceBytes_ + (nativeSourceResources_ ? nativeSourceResources_->SourceBytes() : 0);
    }
    // 单阶段仍按来源读取、解析和分类的完整保守额度校验。双阶段取各阶段
    // 峰值，并扣除另一侧真实仍活owner；已释放的codec/catalog不能重复累加。
    // 保存计划仍需另计快照、通行生成和输出缓冲。
    [[nodiscard]] std::size_t CaptureBudgetBytes() const noexcept {
        return pairCapturePeakBytes_.value_or(sourceBytes_ + objectWorkBytes_);
    }
    // 跨阶段保留来源读取账与实际owner容量；不含已经退出的物编解析临时预留。
    [[nodiscard]] std::size_t RetainedBudgetBytes() const noexcept { return SourceBytes() + OwnedCapacityBytes(); }
    [[nodiscard]] std::size_t OwnedCapacityBytes() const noexcept;
    [[nodiscard]] const std::array<float, 4>* NativeCameraMargins() const noexcept {
        return nativeCameraMargins_ ? &*nativeCameraMargins_ : nullptr;
    }
    [[nodiscard]] Status VerifyTerrainBoundsInput(const TerrainGridSnapshot& sourceTerrain,
        const TerrainGridSnapshot& currentTerrain, std::span<const std::byte> sourceSettings) const;
    // 无分配预收实际来源摘要wire；0表示实际owner无效或超过硬上限。
    [[nodiscard]] std::size_t TerrainBoundsDependencyBudgetBytes() const noexcept;
    [[nodiscard]] Result<std::string> TerrainBoundsDependencyFingerprint(
        std::size_t maximumBytes = 1024u * 1024u) const;

private:
    friend class TerrainResizeResources;
    friend class Services::TerrainPathingSave;
    friend class Services::TerrainResizeEditService;
    friend class Services::TerrainSessionResources;
    friend class Services::TerrainHistorySave;
    friend class TerrainSavePlan;
    TerrainPathingResources() = default;
    // 仅内部保存计划可把会话完整W3I的窄域说明修改绑定到最终身份。
    // 原始依赖仍保留真实来源SHA；外部不能自报最终SHA或验证布尔。
    [[nodiscard]] Status BindNativeSettingsOutput(const MapDataEditSession& edits);
    [[nodiscard]] Status BindNativeResizeSettingsOutput(const MapDataEditSession& edits);
    [[nodiscard]] Status BindNativeSettingsOutputImpl(const MapDataEditSession& edits, bool resize);
    // 只有Services可信适配器可提供解析器；输入只能是同一次实际捕获的字节。
    // 成功值仅描述同次实际来源中的对象语义及解析预留，不是外部资格布尔。
    using NativeObjectValidator = std::function<Result<TerrainNativeObjectPathing>(
        std::span<const std::string_view>, std::span<const std::string_view>, std::span<const ArchiveExactMember>, std::size_t)>;
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNativeWithObjects(const MapArchiveSession& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes, const NativeObjectValidator& validator, const std::vector<std::byte>* currentDoodads = nullptr);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNativeWithObjects(const WorkspaceSnapshot& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes, const NativeObjectValidator& validator, const std::vector<std::byte>* currentDoodads = nullptr);
    using MemberReader = HeightSourceReader;
    // 仅可信Services最终保存工厂可建立双阶段；旧来源与最终overlay分别捕获，
    // 不能由外部SHA、mask或资格布尔授权。返回体持G1，内部owner持真实G0。
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNativePairWithObjects(
        const MemberReader& sourceReader, const MemberReader& finalReader,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes,
        const NativeObjectValidator& validator);
    // 扩裁的两阶段分别解析各自真实shape；不放宽旧同shape捕获入口。
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNativeResizePairWithObjects(
        const MemberReader& sourceReader, const MemberReader& finalReader,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes,
        const NativeObjectValidator& validator);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNativePairImpl(
        const MemberReader& sourceReader, const MemberReader& finalReader,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes,
        const NativeObjectValidator& validator, bool resize);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureInputs(const MemberReader& reader,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes, bool native,
        const NativeObjectValidator* validator = nullptr, const std::vector<std::byte>* currentDoodads = nullptr,
        bool sourceObjectsOnly = false, bool resizeSpatial = false);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureWaterBrushInputs(const MemberReader& reader,
        const TerrainGridSnapshot& sourceTerrain, const TerrainGridSnapshot& currentTerrain,
        const std::filesystem::path& gameRoot, bool enforceWaterLimits, std::size_t maximumBytes);
    [[nodiscard]] static Result<std::vector<ArchiveExactMember>> ReadDirectoryMembers(const WorkspaceSnapshot& source,
        std::span<const std::string> names, const ArchiveExactMemberLimits& limits, bool allowSettingsLni = false);
    [[nodiscard]] Status CaptureWaterSpecialInputs(const MemberReader& reader,
        const TerrainGridSnapshot& terrain, const DoodadPlacements& doodads, const NativeObjectValidator& validator);
    // requiredCount之前的成员必须有实际winner；余项允许缺失并固定缺失身份。
    using ResourceVisitor = std::function<void(std::size_t, bool, std::vector<std::byte>&&)>;
    [[nodiscard]] Status CaptureResourceMembers(const MemberReader& reader,
        std::span<const std::string> names, std::size_t requiredCount, const ResourceVisitor& visitor);
    [[nodiscard]] Status CaptureNativeFootprints(const MemberReader& reader, const TerrainHeader& header,
        std::span<const DoodadPlacement> placements,
        std::optional<std::span<const DoodadPlacement>> afterPlacements,
        std::span<const TerrainNativeDoodadPathingType> types);
    [[nodiscard]] Status CaptureSpecialFootprints(const MemberReader& reader,
        const TerrainGridSnapshot& terrain,
        std::span<const SpecialDoodadPlacement> placements, std::span<const TerrainNativeDoodadPathingType> types,
        std::uint8_t maximumCliffLevel, std::size_t stage);
    [[nodiscard]] Result<std::uint8_t> CaptureNativeCliffLimit(const MemberReader& reader);
    [[nodiscard]] Status VerifyReadbackMembers(const MemberReader& reader, std::size_t maximumBytes,
        bool sourceInputs = false) const;
    [[nodiscard]] Status VerifyGameDependencies(std::size_t remainingBytes) const;
    struct Dependency {
        std::string name, sha256, gameArchive;
        std::size_t bytes = 0;
        bool mapPresent = false;
        bool mapOnly = false;
        bool gameAbsent = false;
    };
    std::vector<Dependency> dependencies_;
    std::shared_ptr<TerrainPathingResources> nativeSourceResources_;
    // 沿用同次严格validator已预收的输出，不从最终通行mask倒推对象类别。
    std::vector<TerrainNativeDoodadPathingType> nativeObjectTypes_, nativeSpecialTypes_;
    struct SpatialFootprint {
        std::string rawcode;
        std::int32_t creationId = 0;
        std::array<std::int64_t, 4> rectangle{};
    };
    bool resizeSpatial_ = false;
    std::vector<SpatialFootprint> spatialFootprints_;
    std::optional<Game::NativeDoodadCategoryDirectory> nativeCategoryDirectory_;
    std::map<std::string, std::array<bool, 3>, std::less<>> ground_;
    std::filesystem::path gameRoot_;
    std::uint8_t tileset_ = 0;
    std::size_t sourceBytes_ = 0;
    std::size_t objectWorkBytes_ = 0;
    std::optional<std::size_t> pairCapturePeakBytes_;
    std::size_t capturedMaximumBytes_ = 0;
    std::vector<TerrainNativeGroundRule> nativeGround_;
    std::optional<TerrainNativeWaterRules> waterRules_;
    std::optional<std::uint8_t> nativeMaximumCliffLevel_;
    bool heightRulesOnly_ = false;
    std::string heightRuleFingerprint_; // 捕获时计算一次；显示任务复用不重复散列来源正文。
    std::optional<TerrainWaterLoadedRules> waterBrushRules_;
    std::string waterBrushInputSha_;
    std::size_t waterBrushInputBytes_ = 0;
    std::optional<TerrainNativePlayableBounds> nativeBounds_;
    std::optional<TerrainNativePlayableBounds> nativeAfterBounds_;
    std::optional<std::array<float, 4>> nativeCameraMargins_;
    std::optional<std::string> nativeSettingsOutputSha_;
    std::size_t nativeSettingsOutputBytes_ = 0;
    std::vector<std::byte> nativeDoodads_, nativeAfterDoodads_;
    bool nativeDoodadsFrozen_ = false;
    std::string waterBrushDoodadsSha_;
    std::vector<TerrainWaterSpecialDoodadShape> waterSpecialShapes_;
    std::vector<std::uint8_t> nativeStaticAttributes_;
    // nullopt仅表示完整普通wire未变，可共享旧层；显式空的新集合仍有独立零层。
    std::optional<std::vector<std::uint8_t>> nativeAfterStaticAttributes_;
    std::array<std::vector<std::uint8_t>, 2> nativeSpecialAttributes_;
    std::array<std::size_t, 2> nativeSpecialCount_{};
    std::array<std::string, 2> nativeTerrainSha_;
    std::array<std::size_t, 2> nativeTerrainBytes_{};
};

} // namespace War3Map::MapData
