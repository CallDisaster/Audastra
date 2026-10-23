#pragma once

#include "EditorProject.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/TerrainStrokeJournal.h"
#include "War3MapCore/Workspace.h"

#include <QJsonObject>

#include <atomic>

namespace War3Map::QtProject {

// Prepare 只在界面线程冻结编辑载荷；Run 只访问任务自己拥有的输入，
// Commit 回到界面线程确认会话归属。恢复快照不持手动工程保存票据。
// 调用方必须等工作线程结束后再读取 Outcome/Completed 或执行 Commit。
class ProjectSaveJob final {
public:
    ~ProjectSaveJob();
    ProjectSaveJob(const ProjectSaveJob&) = delete;
    ProjectSaveJob& operator=(const ProjectSaveJob&) = delete;

    [[nodiscard]] std::uint64_t Generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }
    [[nodiscard]] const EditScopeSnapshot& Scopes() const noexcept { return scopes_; }
    [[nodiscard]] const std::filesystem::path& SourceArchivePath() const noexcept { return sourceArchivePath_; }
    [[nodiscard]] const std::filesystem::path& SourceRoot() const noexcept { return sourceRoot_; }
    [[nodiscard]] const std::string& SourceSha256() const noexcept { return sourceSha256_; }
    [[nodiscard]] const std::filesystem::path& Target() const noexcept { return target_; }
    [[nodiscard]] const std::optional<std::string>& ExpectedTargetSha256() const noexcept { return options_.expectedTargetSha256; }
    [[nodiscard]] bool Recovery() const noexcept { return recovery_; }
    [[nodiscard]] bool RequestCancellation() noexcept {
        return recovery_ && options_.control && options_.control->RequestCancellation();
    }
    [[nodiscard]] bool PublicationStarted() const noexcept {
        return options_.control && options_.control->PublicationStarted();
    }
    [[nodiscard]] bool Cancelled() const noexcept { return phase_.load(std::memory_order_acquire) == Phase::Cancelled; }
    [[nodiscard]] bool Completed() const noexcept {
        const auto phase = phase_.load(std::memory_order_acquire);
        return phase == Phase::Completed || phase == Phase::Cancelled;
    }
    [[nodiscard]] const Result<ProjectSaveResult>& Outcome() const noexcept { return outcome_; }

private:
    friend class EditorProject;
    ProjectSaveJob();

    enum class Phase { Prepared, Running, Completed, Cancelling, Cancelled };

    struct Import {
        PendingImport entry;
        std::string member;
        std::string sha256;
    };
    struct NativeBaseline {
        std::string member;
        // Directory 来源从 Run 读入的完整基线成员取原字节，避免重新读取作者文件。
        std::optional<std::string> directoryMember;
        std::string directorySha256;
        bool archiveMemberPresent = false;
        std::vector<std::byte> expected;
    };
    struct SourceLease;
    void ReleaseRunLeases() noexcept;
    void ReleaseCommitLeases() noexcept;

    std::uint64_t generation_ = 0;
    std::uint64_t revision_ = 0;
    EditScopeSnapshot scopes_;
    std::string fingerprint_;
    std::filesystem::path sourceRoot_;
    std::filesystem::path sourceArchivePath_;
    std::string sourceSha256_;
    std::filesystem::path target_;
    std::string uiSha256_;
    bool recovery_ = false;
    std::atomic<Phase> phase_{Phase::Prepared};
    bool committed_ = false;
    std::optional<EditSaveTicket> ticket_;
    ProjectSaveOptions options_;
    QJsonObject manifest_;
    std::vector<Diagnostic> warnings_;
    std::optional<MapData::TerrainStrokeJournal> terrainJournalPrefix_;
    std::shared_ptr<const MapData::MapDataEditSession> terrainJournalBaseline_;
    std::vector<std::byte> terrainJournalPayload_;
    std::vector<MapArchiveMember> overlays_;
    std::vector<Import> imports_;
    std::vector<NativeBaseline> nativeBaselines_;
    std::shared_ptr<const MapArchiveSession> archive_;
    // worker 释放旧快照的大载荷后，仍可用 control block 身份拒绝同路径重新绑定。
    std::weak_ptr<const MapArchiveSession> archiveOwner_;
    std::optional<DirectoryWorkspace> directory_;
    std::shared_ptr<QTemporaryDir> restoredFiles_;
    std::shared_ptr<QTemporaryDir> mapWorkspace_;
    std::unique_ptr<SourceLease> sourceLease_;
    Result<ProjectSaveResult> outcome_;
    Result<ProjectSaveResult> cancelledOutcome_; // Prepare 预分配，取消终态不再依赖内存分配。
};

} // namespace War3Map::QtProject
