#include "EditorProject.h"
#include "ProjectObjectCatalog.h"

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QTemporaryDir>
#include <QUndoStack>

#include "War3MapCore/EditorSession.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Workspace.h"

namespace War3Map::QtProject {

EditorProject::EditorProject()
    : projectWatch_(new QFileSystemWatcher(this)),
      iconProvider_(std::make_unique<QtApp::IconProvider>()) {
    mapDataUndoStack_ = new QUndoStack(this);
    mapDataUndoStack_->setUndoLimit(128);
    assetUndoStack_ = new QUndoStack(this);
    assetUndoStack_->setUndoLimit(128);
    projectWatch_->setObjectName("projectSourceWatcher");
    InitializeDiagnostics();
    InstallProjectObjectCatalogInvalidation(*this);
    const auto changed = [this](const QString&) {
        // 回调中不散列或重载大地图。最终保存仍执行完整来源验证。
        if (projectRoot_.empty() && sourceArchivePath_.empty() && projectFilePath_.empty()) return;
        // 保存地图期间与写回后短时间内的事件来自自己的替换操作。
        if (savingMap_ || QDateTime::currentMSecsSinceEpoch() < sourceWatchQuietUntil_) return;
        if (!sourceChangeSuspected_) projectNoticeDismissed_ = false;
        sourceChangeSuspected_ = true;
        emit SourceStateChanged();
    };
    connect(projectWatch_, &QFileSystemWatcher::fileChanged, this, changed);
    connect(projectWatch_, &QFileSystemWatcher::directoryChanged, this, changed);
}

EditorProject::~EditorProject() {
    delete assetUndoStack_; assetUndoStack_ = nullptr;
    for (const auto& stack : contentCacheStacks_) if (stack) disconnect(stack.data(), nullptr, this, nullptr);
    // 在文档成员销毁之前停止外部来源事件；QObject 随后负责销毁子监视器。
    projectWatch_->blockSignals(true);
    disconnect(projectWatch_, nullptr, this, nullptr);
}

void EditorProject::ReplaceTriggerDocument(std::unique_ptr<Trigger::TriggerDocument> document) {
    emit TriggerDocumentAboutToChange();
    guiConversionSnapshots_.Clear();
    triggerDocument_ = std::move(document);
    emit TriggerDocumentChanged();
}

} // namespace War3Map::QtProject
