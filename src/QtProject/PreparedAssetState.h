#pragma once
#include "EditorProject.h"

namespace War3Map::QtProject {
// 仅工程准备服务能创建，调用方不能修改来源、指纹或候选修订。
class PreparedAssetStateEdit final {
public:
    [[nodiscard]] const AssetState& Candidate() const noexcept { return after_; }
private:
    friend class EditorProject;
    PreparedAssetStateEdit(EditorProject& owner, AssetState before, AssetState after, std::optional<PreparedEditObservation> observation,
        std::vector<std::pair<std::size_t, std::string>> diskFingerprints)
        : owner_(&owner), before_(std::move(before)), after_(std::move(after)), observation_(std::move(observation)),
          archive_(owner.archiveSession_.get()), directory_(owner.directorySnapshot_.get()),
          sourcePath_(owner.sourceArchivePath_), projectPath_(owner.projectFilePath_), tracked_(owner.editTrackingReady_),
          generation_(owner.editRevision_.Generation()), revision_(owner.editRevision_.Revision()), diskFingerprints_(std::move(diskFingerprints)) {}
    EditorProject* owner_ = nullptr;
    AssetState before_, after_;
    std::optional<PreparedEditObservation> observation_;
    const MapArchiveSession* archive_ = nullptr;
    const DirectoryWorkspace* directory_ = nullptr;
    std::filesystem::path sourcePath_, projectPath_;
    bool tracked_ = false, consumed_ = false;
    std::uint64_t generation_ = 0, revision_ = 0;
    std::vector<std::pair<std::size_t, std::string>> diskFingerprints_;
};
}
