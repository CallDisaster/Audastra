#pragma once

#include "EditorProject.h"

#include <QUndoCommand>

namespace War3Map::QtProject {

// 资源与 Lua 文件共享工程栈。状态在私有候选上规划；收据只重放精确的当前状态与来源。
class AssetStateCommand final : public QUndoCommand {
public:
    AssetStateCommand(EditorProject& project, AssetState before, AssetState after, QString label);
    void undo() override { Apply(false); }
    void redo() override { Apply(true); }

private:
    void Apply(bool forward);
    EditorProject& project_;
    AssetState before_, after_;
    const MapArchiveSession* archive_ = nullptr;
    const DirectoryWorkspace* directory_ = nullptr;
    std::string sourceSha256_;
    std::filesystem::path sourcePath_, projectPath_;
    std::uint64_t generation_ = 0;
};

} // namespace War3Map::QtProject
