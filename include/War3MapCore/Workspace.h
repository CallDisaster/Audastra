#pragma once

#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace War3Map {

struct WorkspaceFile {
    std::filesystem::path relativePath;
    std::uintmax_t size = 0;
    std::int64_t lastWriteTicks = 0;
    std::string sha256;
};

struct WorkspaceSnapshot {
    std::filesystem::path root;
    std::vector<WorkspaceFile> files;
    std::string aggregateSha256;
};

class DirectoryWorkspace {
public:
    [[nodiscard]] static Result<DirectoryWorkspace> Open(
        const std::filesystem::path& root);

    [[nodiscard]] const WorkspaceSnapshot& Snapshot() const noexcept;
    [[nodiscard]] Result<WorkspaceSnapshot> Rescan(const ProjectSaveControl* control = nullptr) const;
    [[nodiscard]] Status VerifyUnchanged(const ProjectSaveControl* control = nullptr) const;

private:
    explicit DirectoryWorkspace(WorkspaceSnapshot snapshot);

    WorkspaceSnapshot snapshot_;
};

// 只读打开 w3x/w3m（MPQ）地图，并把标准地图数据文件提取到工作目录，
// 供既有的目录式编辑管线消费。源归档始终只读。
class MpqWorkspace {
public:
    MpqWorkspace(MpqWorkspace&&) noexcept;
    MpqWorkspace& operator=(MpqWorkspace&&) noexcept;
    MpqWorkspace(const MpqWorkspace&) = delete;
    MpqWorkspace& operator=(const MpqWorkspace&) = delete;
    ~MpqWorkspace();

    [[nodiscard]] static Result<MpqWorkspace> OpenReadOnly(
        const std::filesystem::path& archive);

    // 提取触发器（wtg/wct/wts）、物体数据（w3u/w3t/w3a/w3b/w3d/w3h/w3q）、
    // 地图信息（w3i）等标准文件到 outDir/map/ 下，保持原始文件名。
    // 缺失的可选文件只记录诊断，不视为失败。
    [[nodiscard]] Status ExtractMapData(const std::filesystem::path& outDir) const;

    [[nodiscard]] const Mpq::MpqArchive& Archive() const noexcept;

private:
    explicit MpqWorkspace(std::unique_ptr<Mpq::MpqArchive> archive);

    std::unique_ptr<Mpq::MpqArchive> archive_;
};

} // namespace War3Map
