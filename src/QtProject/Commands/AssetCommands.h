#pragma once

#include "EditorProject.h"

#include <QUndoCommand>

namespace War3Map::QtProject {

// 资源与 Lua 文件共享工程栈。状态在私有候选上规划；收据只重放精确的当前状态与来源。
class AssetStateCommand final : public QUndoCommand {
public:
    AssetStateCommand(EditorProject& project, AssetState before, AssetState after, QString label,
        std::shared_ptr<PreparedAssetStateEdit> firstPrepared = {}, std::shared_ptr<Status> firstStatus = {});
    void undo() override { Apply(false); }
    void redo() override { if (skipNextRedo_) skipNextRedo_ = false; else Apply(true); }
    // 首次发布在入栈前完成；Qt push即使redo拒绝也会删除旧重做分支。
    void PublishBeforePush() { Apply(true); skipNextRedo_ = !isObsolete(); }

private:
    void Apply(bool forward);
    EditorProject& project_;
    AssetState before_, after_;
    const MapArchiveSession* archive_ = nullptr;
    const DirectoryWorkspace* directory_ = nullptr;
    std::string sourceSha256_;
    std::filesystem::path sourcePath_, projectPath_;
    std::uint64_t generation_ = 0;
    std::shared_ptr<PreparedAssetStateEdit> firstPrepared_;
    std::shared_ptr<Status> firstStatus_;
    bool skipNextRedo_ = false;
};

} // namespace War3Map::QtProject
