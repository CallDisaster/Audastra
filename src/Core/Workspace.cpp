#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Workspace.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/ProjectSaveControl.h"

#include <Windows.h>
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

struct MpqWorkspace::State {
    HANDLE lease = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity{};
    std::filesystem::path path;
    std::string sha256;
    std::vector<Mpq::MpqFileInfo> knownMembers;
    std::optional<Mpq::MpqArchive> archive;
    ~State() { if (lease != INVALID_HANDLE_VALUE) CloseHandle(lease); }
};

Result<MpqWorkspace> MpqWorkspace::OpenReadOnly(
    const std::filesystem::path& archive) {
    std::error_code error;
    auto state = std::make_unique<State>();
    state->path = std::filesystem::weakly_canonical(archive, error);
    if (error || !std::filesystem::is_regular_file(state->path, error) || error) {
        return Result<MpqWorkspace>::Failure(
            "Workspace",
            "MPQ_NOT_FOUND",
            "MPQ workspace does not exist: " + archive.string());
    }
    // 允许事务以 rename 替换路径，但后续读取必须再确认路径仍指向原文件对象。
    state->lease = CreateFileW(state->path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (state->lease == INVALID_HANDLE_VALUE || GetFileType(state->lease) != FILE_TYPE_DISK
        || !GetFileInformationByHandle(state->lease, &state->identity)
        || (state->identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)))
        return Result<MpqWorkspace>::Failure("Workspace", "MPQ_SOURCE_LEASE_FAILED",
            "无法为地图来源取得只读文件租约。");
    auto hash = Sha256Handle(state->lease);
    if (!hash.Succeeded()) return {std::nullopt, std::move(hash.diagnostics)};
    state->sha256 = std::move(*hash.value);
    auto opened = Mpq::MpqArchive::OpenReadOnlyHandle(state->lease, state->path);
    if (!opened.Succeeded() || !opened.value.has_value()) {
        Result<MpqWorkspace> result;
        result.diagnostics = std::move(opened.diagnostics);
        return result;
    }
    state->archive.emplace(std::move(*opened.value));
    for (const auto& name : state->archive->ListFiles())
        if (auto member = state->archive->GetFileInfo(name)) state->knownMembers.push_back(std::move(*member));
    MpqWorkspace workspace(std::move(state));
    const auto verified = workspace.VerifyUnchanged();
    if (!verified.Succeeded()) return {std::nullopt, verified.diagnostics};
    return Result<MpqWorkspace>::Success(std::move(workspace));
}

MpqWorkspace::MpqWorkspace(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
MpqWorkspace::MpqWorkspace(MpqWorkspace&&) noexcept = default;
MpqWorkspace& MpqWorkspace::operator=(MpqWorkspace&&) noexcept = default;
MpqWorkspace::~MpqWorkspace() = default;

const Mpq::MpqArchive& MpqWorkspace::Archive() const noexcept {
    return *state_->archive;
}

const std::filesystem::path& MpqWorkspace::SourcePath() const noexcept { return state_->path; }
const std::string& MpqWorkspace::SourceSha256() const noexcept { return state_->sha256; }
const std::vector<Mpq::MpqFileInfo>& MpqWorkspace::KnownMembers() const noexcept { return state_->knownMembers; }

Status MpqWorkspace::VerifyUnchanged() const {
    const auto handle = CreateFileW(state_->path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto result = VerifySourceHandle(handle);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    return result;
}

Status MpqWorkspace::VerifySourceHandle(void* handle) const {
    BY_HANDLE_FILE_INFORMATION current{};
    const bool valid = handle && handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &current);
    const auto& original = state_->identity;
    // 原租约从 SHA 捕获起始终拒绝写入；无需为每个成员再次散列整张大图。
    if (!valid || current.dwVolumeSerialNumber != original.dwVolumeSerialNumber
        || current.nFileIndexHigh != original.nFileIndexHigh || current.nFileIndexLow != original.nFileIndexLow
        || current.nFileSizeHigh != original.nFileSizeHigh || current.nFileSizeLow != original.nFileSizeLow
        || CompareFileTime(&current.ftLastWriteTime, &original.ftLastWriteTime) != 0)
        return Status::Failure("Workspace", "MPQ_SOURCE_CHANGED", "地图来源已替换、移走或变化，请重新打开。");
    return Status::Ok();
}

Result<std::vector<std::byte>> MpqWorkspace::ReadMember(std::string_view name, std::size_t maximumBytes) const {
    auto status = VerifyUnchanged();
    if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
    auto result = state_->archive->ReadFileBounded(name, maximumBytes);
    status = VerifyUnchanged();
    if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
    return result;
}

Status MpqWorkspace::ExtractMapData(const std::filesystem::path& outDir) const {
    const auto before = VerifyUnchanged();
    if (!before.Succeeded()) return before;
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
    auto result = state_->archive->ExtractTo(outDir / "map", kStandardFiles);
    const auto after = VerifyUnchanged();
    if (!after.Succeeded()) result.diagnostics.insert(result.diagnostics.end(), after.diagnostics.begin(), after.diagnostics.end());
    return result;
}

} // namespace War3Map
