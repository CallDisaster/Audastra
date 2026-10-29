#pragma once
#include "EditorProject.h"

namespace War3Map::QtProject {
// 仅工程准备服务能创建，调用方不能修改来源、指纹或候选修订。
class PreparedAssetStateEdit final {
    friend class EditorProject;
    PreparedAssetStateEdit(EditorProject& owner, AssetState before, AssetState after, PreparedEditObservation observation)
        : owner_(&owner), before_(std::move(before)), after_(std::move(after)), observation_(std::move(observation)),
          archive_(owner.archiveSession_.get()), directory_(owner.directorySnapshot_.get()),
          sourcePath_(owner.sourceArchivePath_), projectPath_(owner.projectFilePath_) {}
    EditorProject* owner_ = nullptr;
    AssetState before_, after_;
    PreparedEditObservation observation_;
    const MapArchiveSession* archive_ = nullptr;
    const DirectoryWorkspace* directory_ = nullptr;
    std::filesystem::path sourcePath_, projectPath_;
};
}
