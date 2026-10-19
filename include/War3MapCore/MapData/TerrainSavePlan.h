#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Workspace.h"

#include <array>
#include <span>

namespace War3Map::Services { class TerrainMinimapSave; }

namespace War3Map::MapData {

class MapDataEditSession;
class TerrainPathingResources;

enum class TerrainSaveSourceState { Present, Missing, Unknown };
enum class TerrainSaveMemberAction { Preserve, WriteEdited, Reject };

struct TerrainSaveMemberPlan {
    std::string name;
    TerrainSaveSourceState sourceState = TerrainSaveSourceState::Unknown;
    std::optional<std::size_t> sourceBytes, outputBytes;
    std::optional<std::string> sourceSha256, outputSha256;
    TerrainSaveMemberAction action = TerrainSaveMemberAction::Preserve;
    std::string reasonCode, reason;
};

struct TerrainSavePlanLimits {
    // 五个地形成员、十个范围/摆放/物编依赖、基线序列化核对与W3E/WPM
    // 输出的累计工作字节预算；只能收紧。
    std::size_t maximumBytes = 64u * 1024u * 1024u;
};

// 计划只接受原版通行查询已证不变的修改及荒芜净变化；PrepareForSave还会
// 从实际来源验证等属性地面替换与同通行水深。它不重建WPM，
// 不清除源图的静态对象、手画或未知通行位。
// 默认SHD、MMP、小地图原样保留或已证明缺失；只有Services的密封missing-only
// 准备单元能在原干地height门内增加MapBLP，普通计划与任意调用方不能授予其写入。
// 归档 Prepare 在同一已核来源的只读租约内按五个明确名字捕获，不依赖 listfile。
// 目录 Prepare 只使用已冻结快照；实际保存仍须执行 MapSaveService 的源租约检查。
// 调用方须串行排除会话写入。Result 成功表示计划完整，SaveGate 才表示地形门通过。
class TerrainSavePlan {
public:
    TerrainSavePlan(const TerrainSavePlan&) = default;
    TerrainSavePlan(TerrainSavePlan&&) noexcept = default;
    TerrainSavePlan& operator=(const TerrainSavePlan&) = default;
    TerrainSavePlan& operator=(TerrainSavePlan&&) noexcept = default;

    [[nodiscard]] static bool HasTerrainChanges(const MapDataEditSession& edits) noexcept;
    [[nodiscard]] static Result<TerrainSavePlan> Prepare(const MapArchiveSession& source,
        const MapDataEditSession& edits, const TerrainSavePlanLimits& limits = {});
    // 正常保存使用实际地图/游戏表补充通行等价判断；缺少或改变的来源仍拒绝。
    [[nodiscard]] static Result<TerrainSavePlan> PrepareForSave(const MapArchiveSession& source,
        const MapDataEditSession& edits, const std::filesystem::path& gameRoot,
        const TerrainSavePlanLimits& limits = {});
    // 目录来源使用已捕获的完整快照；不读取或信任快照之外的磁盘内容。
    [[nodiscard]] static Result<TerrainSavePlan> Prepare(const WorkspaceSnapshot& source,
        const MapDataEditSession& edits, const TerrainSavePlanLimits& limits = {});

    [[nodiscard]] const std::string& SourceSha256() const noexcept { return sourceSha256_; }
    [[nodiscard]] std::uint64_t SessionIdentity() const noexcept { return sessionIdentity_; }
    [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }
    [[nodiscard]] std::uint64_t TerrainSourceRevision() const noexcept { return terrainSourceRevision_; }
    // 固定顺序 W3E / WPM / SHD / MMP / war3mapMap.blp；Missing 与 Unknown 不混用。
    [[nodiscard]] const std::array<TerrainSaveMemberPlan, 5>& Members() const noexcept { return members_; }
    [[nodiscard]] std::span<const MemberReplacement> Replacements() const noexcept { return replacements_; }
    [[nodiscard]] const Status& SaveGate() const noexcept { return saveGate_; }
    [[nodiscard]] const std::vector<Diagnostic>& Diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] bool RequiresNativeVerification() const noexcept { return terrainChanged_; }

    // 准备阶段检查原会话、修订和来源。最终列表可包含其它领域成员，但五个地形成员
    // 必须精确符合本计划，不能在脚本模板、导入或删除阶段夹带派生数据覆盖。
    [[nodiscard]] Status VerifyForSave(const MapDataEditSession& edits, std::string_view sourceSha256,
        std::span<const MemberReplacement> replacements, std::span<const std::string> removals = {}) const;
    // 后台执行只消费冻结身份，不借用活跃编辑会话；应在最终成员列表形成后、发布前调用。
    [[nodiscard]] Status VerifyFrozenOutput(std::string_view sourceSha256,
        std::span<const MemberReplacement> replacements, std::span<const std::string> removals = {}) const;
    // 按明确名字独立回读五个成员的存在性和原字身份，包括未列在 listfile 中的成员。
    // limits仍是五成员累计读取字节帽，默认64MiB保持旧行为；共享保存单元可显式
    // 收紧并在其外层预收3×读取容量及固定散列/元数据工作。不表示native验证。
    [[nodiscard]] Status VerifyReadback(const MapArchiveSession& saved,
        const TerrainSavePlanLimits& limits = {}) const;

private:
    friend class Services::TerrainMinimapSave;
    TerrainSavePlan() = default;
    [[nodiscard]] static Result<TerrainSavePlan> PrepareArchive(const MapArchiveSession& source,
        const MapDataEditSession& edits, const TerrainSavePlanLimits& limits,
        const std::filesystem::path* gameRoot);
    [[nodiscard]] Status VerifyMissingMinimapEligibility(const MapDataEditSession& edits) const;
    [[nodiscard]] Status AdmitMissingMinimap(const MapDataEditSession& edits, std::vector<std::byte> generatedBytes);
    // 正常保存的自动小地图重建：仍受同一正式地形保存门和会话绑定约束，
    // 但允许源图已有非空 BLP，输出会精确替换该成员；不改变 WPM/SHD/MMP 资格。
    [[nodiscard]] Status AdmitGeneratedMinimap(const MapDataEditSession& edits, std::vector<std::byte> generatedBytes);
    [[nodiscard]] std::size_t OwnedCapacityBytes() const noexcept;
    [[nodiscard]] static Result<TerrainSavePlan> Complete(TerrainSavePlan plan,
        const MapDataEditSession& edits, const TerrainSavePlanLimits& limits, std::size_t sourceBytes,
        const MapArchiveSession* source = nullptr, const std::filesystem::path* gameRoot = nullptr);
    std::string sourceSha256_;
    std::uint64_t sessionIdentity_ = 0, revision_ = 0, terrainSourceRevision_ = 0;
    bool terrainChanged_ = false;
    bool pathingInvariant_ = false;
    std::shared_ptr<const TerrainPathingResources> pathingResources_;
    std::array<TerrainSaveMemberPlan, 5> members_;
    // 地形只保存不变通行时冻结：W3I、两DOO、七类native物编。它们不新增
    // 写出能力；最终列表与回读必须沿用源身份，防止混入占地/范围变化。
    std::array<TerrainSaveMemberPlan, 10> invariantMembers_;
    std::vector<MemberReplacement> replacements_;
    Status saveGate_;
    std::vector<Diagnostic> diagnostics_;
    std::size_t cumulativeBytes_ = 0, maximumBytes_ = 0;
};

} // namespace War3Map::MapData
