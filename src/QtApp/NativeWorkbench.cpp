#include "MainWindow.h"
#include "ObjectModule.h"
#include "MapEntitiesModule.h"

#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

// 会话级游戏资源入口（命令行 --game-root 与测试使用）：不写本机设置。

bool MainWindow::OpenGameResources(const std::filesystem::path& path) {
    if (!project_->iconProvider_->SetGameRoot(path)) return false;
    project_->war3GameRoot_ = path;
    project_->RefreshWorldEditStrings();
    objectsModule_->RefreshStandardCatalog();
    entitiesModule_->Refresh();
    project_->RefreshUiIconMappings();
    return project_->standardCatalog_ != nullptr && !project_->standardCatalog_->Entries().empty();
}
