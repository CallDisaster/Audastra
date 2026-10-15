#include "EditorProject.h"
#include "MapSaveJob.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"

#include <QDateTime>
#include <QUndoStack>

#include <algorithm>
#include "War3MapCore/ObjectData/LniObjectCodec.h"

// 地图保存的执行与提交阶段。执行阶段只读任务里的输入（可在工作线程运行，期间工程不得修改）；
// 提交阶段在界面线程写入诊断并切换到新基线。准备阶段见 MapCopySave.cpp。
namespace War3Map::QtProject {
namespace {

QString StageText(std::string_view stage) {
    if (stage == "generate") return EditorProject::tr("正在生成游戏脚本…");
    if (stage == "inject") return EditorProject::tr("正在注入函数库代码…");
    if (stage == "preprocess") return EditorProject::tr("正在预处理脚本…");
    if (stage == "wave") return EditorProject::tr("正在预处理脚本（Wave）…");
    if (stage == "template") return EditorProject::tr("正在展开脚本模板…");
    if (stage == "vjass") return EditorProject::tr("正在翻译 vJass…");
    if (stage == "jasshelper") return EditorProject::tr("正在编译并检查语法（jasshelper / pjass）…");
    if (stage == "check") return EditorProject::tr("正在检查语法…");
    if (stage == "pjass") return EditorProject::tr("正在检查语法（pjass）…");
    if (stage == "archive-in-place") return EditorProject::tr("正在打包并写回地图（原图移入备份）…");
    if (stage == "archive-candidate") return EditorProject::tr("正在打包地图…");
    if (stage == "archive-reopen") return EditorProject::tr("正在切换到保存结果…");
    return EditorProject::tr("正在构建游戏脚本…");
}

} // namespace

void EditorProject::RunMapSave(MapSaveJob& job) {
    Services::MapSaveService::Run(job, [&job](std::string_view stage) { job.Report(StageText(stage)); });
}

std::optional<MapCopySaveResult> EditorProject::CommitMapSave(MapSaveJob& job) {
    AddDiagnostics(job.diagnostics);
    if (!job.succeeded || !job.nextSession) return std::nullopt;

    const auto rejectStaleCommit = [this, &job](const QString& code, const QString& reason)
        -> std::optional<MapCopySaveResult> {
        if (job.inPlace) {
            // 原路径已由保存服务写回；旧会话的文件身份失效，必须显式提示重新打开。
            sourceChangeSuspected_ = true;
            sourceChangeConfirmed_ = true;
            RefreshProjectWatch();
            AddDiagnostic(tr("错误"), tr("地图"), code,
                tr("地图已写回 %1（备份：%2），但%3；当前编辑未标记为已保存，必须重新打开地图。")
                    .arg(QString::fromStdWString(job.written.wstring()))
                    .arg(QString::fromStdWString(job.backup.wstring())).arg(reason));
        } else {
            AddDiagnostic(tr("错误"), tr("地图"), code,
                tr("候选地图已写出至 %1，但%2；当前编辑未标记为已保存，保存基线保持不变。")
                    .arg(QString::fromStdWString(job.written.wstring())).arg(reason));
        }
        return std::nullopt;
    };
    if (!job.preparedRevision) {
        return rejectStaleCommit(tr("SAVE_REVISION_BINDING_MISSING"), tr("保存任务缺少冻结的编辑修订身份"));
    }
    if (!ObserveEditState("地图保存提交前复核")) {
        return rejectStaleCommit(tr("SAVE_REVISION_UNAVAILABLE"), tr("无法复核当前编辑修订"));
    }
    const auto& frozen = *job.preparedRevision;
    if (editRevision_.Generation() != frozen.generation || editRevision_.Revision() != frozen.revision
        || editRevision_.Current() != frozen.scopes || luaMapTargetGeneration_ != frozen.luaTargetGeneration) {
        return rejectStaleCommit(tr("SAVE_REVISION_CHANGED"), tr("保存期间工程编辑状态发生变化"));
    }

    // 唯一提交点。后续保存以新文件为来源，包含此前已保存的物编及导入资源。
    MapCopySaveResult result;
    result.target = job.written;
    result.memberCount = job.memberCount;
    result.savedTriggerChanges = job.triggerChanges && !job.scriptRebuilt;
    result.scriptRebuilt = job.scriptRebuilt;
    result.inPlace = job.inPlace;
    result.backup = job.backup;
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - job.started).count();
    if (job.inPlace) {
        // 写回替换了被监视的源文件；这是自己的操作，不提示“来源已变化”。
        sourceWatchQuietUntil_ = QDateTime::currentMSecsSinceEpoch() + 3000;
        sourceChangeSuspected_ = false;
        sourceChangeConfirmed_ = false;
    }
    archiveSession_ = std::move(job.nextSession);
    readOnlyArchive_.reset();
    sourceArchivePath_ = job.written;
    if (luaMapTarget_) luaMapTargetSource_ = sourceArchivePath_;
    const bool generatedObjects = std::any_of(job.generatedObjectBaselines.begin(), job.generatedObjectBaselines.end(),
        [](const auto& document) { return document != nullptr; });
    if (generatedObjects) emit ObjectDocumentsAboutToReplace();
    for (std::size_t i = 0; i < kObjectTypeCount; ++i) {
        if (!job.generatedObjectBaselines[i]) continue;
        // 旧撤销命令保存旧文档地址；只清理被生成结果替换的类别，不能留悬空命令。
        if (objectUndoStacks_[i]) objectUndoStacks_[i]->clear();
        objectDocuments_[i] = std::move(job.generatedObjectBaselines[i]);
    }
    nativeObjectDocuments_ = std::move(job.nextBaselines);
    for (std::size_t i = 0; i < kObjectTypeCount; ++i) {
        if (!objectDocuments_[i] || objectDocuments_[i]->CloneSources().empty()) continue;
        // 新 native 基线已经包含这些克隆；旧撤销快照仍绑定旧基线，不能再恢复临时来源。
        if (objectUndoStacks_[i]) objectUndoStacks_[i]->clear();
        objectDocuments_[i]->ClearCloneSources();
    }
    // MapDocumentChanged 的名称/实例观察者也必须看到本次最终物编，而不是旧对象表。
    LoadMapDocument(true);
    restoredTriggerOverlay_ = false;
    triggerDirty_ = false;
    objectDirty_.fill(false);
    pendingImports_.clear();
    assetEdits_.clear();
    NotifyImportsChanged();
    if (triggerUndoStack_) triggerUndoStack_->setClean();
    for (auto* stack : objectUndoStacks_) {
        if (stack) stack->setClean();
    }
    CommitMapEditBaseline();
    if (generatedObjects) emit ObjectDocumentsReplaced();
    RefreshProjectWatch();
    const auto diagnosticCount = job.diagnostics.size();
    Services::FinalizeScriptWorkspace(job);
    if (job.diagnostics.size() > diagnosticCount) {
        AddDiagnostics(std::vector<Diagnostic>(job.diagnostics.begin() + diagnosticCount, job.diagnostics.end()));
    }
    if (iconProvider_ && !iconProvider_->SetMapArchive(sourceArchivePath_)) {
        AddDiagnostic(tr("警告"), tr("资源"), tr("SAVED_ICON_LAYER_UNAVAILABLE"), tr("地图已保存，但地图图标层未能重新挂载。"));
    }
    const QString target = QString::fromStdWString(job.written.wstring());
    if (job.inPlace) {
        AddDiagnostic(tr("信息"), tr("地图"), tr("MAP_SAVED"),
            tr("地图已保存：%1（%2 秒）；原图备份为 %3%4").arg(target).arg(result.seconds, 0, 'f', 1)
                .arg(QString::fromStdWString(job.backup.wstring()))
                .arg(job.prunedBackups > 0 ? tr("；已清理 %1 个旧备份").arg(job.prunedBackups) : QString{}));
    } else {
        AddDiagnostic(tr("信息"), tr("地图"), tr("REPACK_SAVED"),
            tr("地图已另存为：%1（%2 秒）").arg(target).arg(result.seconds, 0, 'f', 1));
    }
    if (result.savedTriggerChanges) {
        AddDiagnostic(tr("警告"), tr("构建"), tr("EDIT_COPY_SCRIPT_NOT_REBUILT"),
            tr("触发器编辑数据已保存；游戏脚本仍为源地图版本，须配置 YDWE 编译器后重新保存"));
    }
    return result;
}

std::optional<MapCopySaveResult> EditorProject::SaveMapCopy(const std::filesystem::path& outputPath) {
    auto job = PrepareMapSave(outputPath, false);
    if (!job) return std::nullopt;
    savingMap_ = true;
    RunMapSave(*job);
    savingMap_ = false;
    return CommitMapSave(*job);
}

std::optional<MapCopySaveResult> EditorProject::SaveMapInPlace() {
    auto job = PrepareMapSave({}, true);
    if (!job) return std::nullopt;
    savingMap_ = true;
    RunMapSave(*job);
    savingMap_ = false;
    return CommitMapSave(*job);
}

} // namespace War3Map::QtProject
