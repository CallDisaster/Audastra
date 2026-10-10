#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Workspace.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/ProjectSaveControl.h"

#include <algorithm>
#include <sstream>
#include <system_error>

namespace War3Map {
namespace {

bool ShouldSkip(const std::filesystem::path& relativePath) {
    if (relativePath.empty()) {
        return false;
    }
    const auto first = relativePath.begin()->wstring();
    return first == L".git" ||
           first == L"build" ||
           first.rfind(L"build-", 0) == 0 ||
           first.rfind(L".war3me-", 0) == 0;
}



Result<WorkspaceSnapshot> Scan(const std::filesystem::path& requestedRoot, const ProjectSaveControl* control = nullptr) {
    if (control && !control->Checkpoint(ProjectSaveStage::Verify)) return {{}, ProjectSaveCancelledStatus().diagnostics};
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(requestedRoot, error);
    if (error || !std::filesystem::is_directory(root)) {
        return Result<WorkspaceSnapshot>::Failure(
            "Workspace",
            "WORKSPACE_NOT_DIRECTORY",
            "Workspace root is not an accessible directory: " + requestedRoot.string());
    }

    WorkspaceSnapshot snapshot;
    snapshot.root = root;

    std::filesystem::recursive_directory_iterator iterator(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return Result<WorkspaceSnapshot>::Failure(
            "Workspace",
            "WORKSPACE_ENUMERATION_FAILED",
            "Unable to enumerate workspace: " + error.message());
    }

    for (; iterator != end; iterator.increment(error)) {
        if (control && !control->Checkpoint(ProjectSaveStage::Verify)) return {{}, ProjectSaveCancelledStatus().diagnostics};
        if (error) {
            return Result<WorkspaceSnapshot>::Failure(
                "Workspace",
                "WORKSPACE_ENUMERATION_FAILED",
                "Unable to enumerate workspace: " + error.message());
        }

        const auto relative = iterator->path().lexically_relative(root);
        if (ShouldSkip(relative)) {
            if (iterator->is_directory(error)) {
                iterator.disable_recursion_pending();
            }
            continue;
        }
        if (iterator->is_symlink(error)) {
            return Result<WorkspaceSnapshot>::Failure(
                "Workspace",
                "WORKSPACE_SYMLINK_REJECTED",
                "Workspace contains a symbolic link or junction: " + relative.string());
        }
        if (!iterator->is_regular_file(error)) {
            continue;
        }
        if (error) {
            return Result<WorkspaceSnapshot>::Failure(
                "Workspace",
                "WORKSPACE_FILE_STATUS_FAILED",
                "Unable to read workspace entry status: " + relative.string());
        }

        auto hash = Sha256File(iterator->path(), control);
        if (!hash.Succeeded()) {
            Result<WorkspaceSnapshot> result;
            result.diagnostics = std::move(hash.diagnostics);
            return result;
        }

        WorkspaceFile file;
        file.relativePath = relative;
        file.size = iterator->file_size(error);
        if (error) {
            return Result<WorkspaceSnapshot>::Failure(
                "Workspace",
                "WORKSPACE_FILE_SIZE_FAILED",
                "Unable to read file size: " + relative.string());
        }
        file.lastWriteTicks = iterator->last_write_time(error).time_since_epoch().count();
        if (error) {
            return Result<WorkspaceSnapshot>::Failure(
                "Workspace",
                "WORKSPACE_FILE_TIME_FAILED",
                "Unable to read file timestamp: " + relative.string());
        }
        file.sha256 = std::move(*hash.value);
        snapshot.files.push_back(std::move(file));
    }

    std::sort(
        snapshot.files.begin(),
        snapshot.files.end(),
        [](const WorkspaceFile& left, const WorkspaceFile& right) {
            return IO::GenericPathUtf8(left.relativePath) < IO::GenericPathUtf8(right.relativePath);
        });

    std::ostringstream aggregate;
    for (const auto& file : snapshot.files) {
        aggregate << IO::GenericPathUtf8(file.relativePath) << '\0'
                  << file.size << '\0'
                  << file.sha256 << '\n';
    }
    const auto aggregateText = aggregate.str();
    auto aggregateHash = Sha256(std::as_bytes(std::span(aggregateText)), control);
    if (!aggregateHash.Succeeded()) {
        Result<WorkspaceSnapshot> result;
        result.diagnostics = std::move(aggregateHash.diagnostics);
        return result;
    }
    snapshot.aggregateSha256 = std::move(*aggregateHash.value);
    return Result<WorkspaceSnapshot>::Success(std::move(snapshot));
}

} // namespace

DirectoryWorkspace::DirectoryWorkspace(WorkspaceSnapshot snapshot)
    : snapshot_(std::move(snapshot)) {
}

Result<DirectoryWorkspace> DirectoryWorkspace::Open(
    const std::filesystem::path& root) {
    auto snapshot = Scan(root);
    if (!snapshot.Succeeded()) {
        Result<DirectoryWorkspace> result;
        result.diagnostics = std::move(snapshot.diagnostics);
        return result;
    }
    return Result<DirectoryWorkspace>::Success(
        DirectoryWorkspace(std::move(*snapshot.value)));
}

const WorkspaceSnapshot& DirectoryWorkspace::Snapshot() const noexcept {
    return snapshot_;
}

Result<WorkspaceSnapshot> DirectoryWorkspace::Rescan(const ProjectSaveControl* control) const {
    return Scan(snapshot_.root, control);
}

Status DirectoryWorkspace::VerifyUnchanged(const ProjectSaveControl* control) const {
    auto current = Rescan(control);
    if (!current.Succeeded()) {
        Status status;
        status.diagnostics = std::move(current.diagnostics);
        return status;
    }
    if (current.value->aggregateSha256 != snapshot_.aggregateSha256) {
        return Status::Failure(
            "Workspace",
            "SOURCE_CHANGED",
            "Source workspace changed after it was opened. Reopen before saving.");
    }
    return Status::Ok();
}

Result<MpqWorkspace> MpqWorkspace::OpenReadOnly(
    const std::filesystem::path& archive) {
    if (!std::filesystem::is_regular_file(archive)) {
        return Result<MpqWorkspace>::Failure(
            "Workspace",
            "MPQ_NOT_FOUND",
            "MPQ workspace does not exist: " + archive.string());
    }
    auto opened = Mpq::MpqArchive::OpenReadOnly(archive);
    if (!opened.Succeeded() || !opened.value.has_value()) {
        Result<MpqWorkspace> result;
        result.diagnostics = std::move(opened.diagnostics);
        return result;
    }
    auto held = std::make_unique<Mpq::MpqArchive>(std::move(*opened.value));
    return Result<MpqWorkspace>::Success(MpqWorkspace(std::move(held)));
}

MpqWorkspace::MpqWorkspace(std::unique_ptr<Mpq::MpqArchive> archive)
    : archive_(std::move(archive)) {}
MpqWorkspace::MpqWorkspace(MpqWorkspace&&) noexcept = default;
MpqWorkspace& MpqWorkspace::operator=(MpqWorkspace&&) noexcept = default;
MpqWorkspace::~MpqWorkspace() = default;

const Mpq::MpqArchive& MpqWorkspace::Archive() const noexcept {
    return *archive_;
}

Status MpqWorkspace::ExtractMapData(const std::filesystem::path& outDir) const {
    // WC3 World Editor / YDWE 标准内部文件（归档根目录，无子路径）。
    static const std::vector<std::string> kStandardFiles{
        "war3map.w3i",  // 地图信息
        "war3map.wtg",  // 触发器
        "war3map.wct",  // 自定义文本
        "war3map.wts",  // 触发器字符串表
        "war3map.w3u",  // 单位
        "war3map.w3t",  // 物品
        "war3map.w3a",  // 技能
        "war3map.w3b",  // 可破坏物
        "war3map.w3d",  // 装饰物
        "war3map.w3h",  // Buff
        "war3map.w3q",  // 科技
        "war3mapunits.doo", // 预放置单位
        "(listfile)",
    };
    return archive_->ExtractTo(outDir / "map", kStandardFiles);
}

} // namespace War3Map
