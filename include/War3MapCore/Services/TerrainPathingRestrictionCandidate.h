#pragma once

#include "War3MapCore/MapData/TerrainEditInspection.h"
#include "War3MapCore/Services/MapSaveService.h"

namespace War3Map::Services {

struct TerrainPathingCandidateLimits {
    // 本单元的容器、inspection/proof临时工作与task替换副本预算，不是进程RSS帽。
    // 不计调用前已拥有的archive/edits/saved会话、allocator/CNG内部内存，以及上游
    // 归档reader内部表/解码scratch（继续受其既有OpenLimits/ReadFileBounded硬限）。只能收紧。
    std::size_t maxWorkingBytes = 128u * 1024u * 1024u;
    std::size_t maximumCaptureBytes = 64u * 1024u * 1024u;
    std::size_t maximumKnownMembers = 4096;
};

// 密封的离线WPM附加限制候选，不是普通地图保存许可。只消费FreezeArchive实际任务和真实会话，
// 不接caller的proof、WPM或SHA。宿主须串行排除edits写入，独立testCopyOnly目标不得已存在。
// 默认MapDataEditSession/TerrainSavePlan保存门保持拒绝；不接Qt/CLI/脚本或原地保存。
// inspection只验证五个地形相关成员；regions/cameras等其它未保存编辑不输出、不提交新基线，
// 不能把本结果解释成整会话已保存或验证。宿主仍须保留该会话及其全部待编辑/Undo历史。
// 含(attributes)/(signature)来源拒绝，避免既有普通writer的元数据清理越出零删除域。
class TerrainPathingRestrictionCandidate final {
public:
    [[nodiscard]] static Result<std::shared_ptr<const TerrainPathingRestrictionCandidate>> Prepare(
        const MapSaveTask& task, const MapData::MapDataEditSession& edits,
        const TerrainPathingCandidateLimits& limits = {});

    // 准备时列表必须为空；宿主随后只能append这一份WPM，不能重建或改写其它成员。
    [[nodiscard]] const MemberReplacement& Replacement() const noexcept { return replacement_; }
    [[nodiscard]] const MapData::PathingPreservationProof& Proof() const noexcept { return *proof_; }
    // W3E/WPM/DOO/Units/W3I/SHD/MMP/MapBLP，Present(empty亦然)/ExactMissing不混同。
    [[nodiscard]] const std::array<ArchiveExactMemberIdentity, 8>& SourceMembers() const noexcept { return members_; }
    [[nodiscard]] std::size_t WorkingReservationBytes() const noexcept { return workingReservationBytes_; }
    [[nodiscard]] static constexpr bool FormalSaveAuthorized() noexcept { return false; }
    [[nodiscard]] static constexpr bool NativeValidated() noexcept { return false; }

    [[nodiscard]] Status VerifyCurrent(const MapSaveTask& task, const MapData::MapDataEditSession& edits) const;
    [[nodiscard]] Status VerifyFrozenOutput(const MapSaveTask& task) const;
    // 可信宿主以现MapArchiveSession::SaveCandidate/AfterSave写出，再交同一目标的真实会话。
    // 复核八成员、全部已命名成员、Prefix；匿名块完整保留仍由原归档写出/回读链负责。
    // writer可重建(listfile)/压缩布局，保证业务成员原字，不能据此宣称容器bitwise相等。
    // 不接受任意SHA作为回读凭证，不切换编辑基线，不构造另一套归档保存器。
    [[nodiscard]] Status VerifyReadback(const MapSaveTask& task, const MapArchiveSession& saved) const;

private:
    TerrainPathingRestrictionCandidate() = default;
    [[nodiscard]] Status VerifyTask(const MapSaveTask& task, bool published) const;
    [[nodiscard]] std::size_t RetainedCapacityBytes() const;
    MemberReplacement replacement_;
    std::optional<MapData::PathingPreservationProof> proof_;
    std::array<ArchiveExactMemberIdentity, 8> members_;
    std::string inspectionSha256_;
    std::filesystem::path sourcePath_, targetPath_;
    const MapSaveTask* taskIdentity_ = nullptr;
    const MapArchiveSession* archiveIdentity_ = nullptr;
    TerrainPathingCandidateLimits limits_;
    std::size_t captureBytes_ = 0, inspectionReservationBytes_ = 0, workingReservationBytes_ = 0;
};

} // namespace War3Map::Services
