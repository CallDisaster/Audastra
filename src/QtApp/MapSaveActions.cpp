#include "MainWindow.h"
#include "CampaignModule.h"

#include "MapSaveJob.h"
#include "ProgramIssueReporter.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Services/LuaTestReloadController.h"
#include "War3MapCore/Services/TerrainMinimapSave.h"
#include "ObjectModule.h"
#include "TriggerModule.h"
#include "MapSettingsModule.h"
#include "SoundsModule.h"

#include <QEventLoop>
#include <QFileDialog>
#include <QMetaObject>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPointer>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QThread>
#include <QTimer>

#include <memory>
#include <algorithm>

// 地图保存（WE 式）：Ctrl+S 写回打开的地图（原图移入备份目录），“另存地图为”写出新文件。
// 重建脚本与打包在工作线程执行，期间显示窗口模态的进度框；工程在此期间不接受修改。
using War3Map::QtProject::EditorProject;
using War3Map::QtProject::MapSaveJob;

namespace {
QStringList SaveDiagnostics(const EditorProject& project, int first, const QString& severity) {
    QStringList lines;
    for (int row = first; row < project.diagnosticsModel_->rowCount() && lines.size() < 24; ++row) {
        if (project.diagnosticsModel_->index(row, 0).data().toString() != severity) continue;
        QString line = project.diagnosticsModel_->index(row, 2).data().toString() + " · "
            + project.diagnosticsModel_->index(row, 3).data().toString();
        // 说明中保留编译器已经给出的来源、行号/原文；不猜测别的阶段行号。
        lines << line.left(4096);
    }
    return lines;
}
void SaveNotice(QMainWindow& window, bool interactive, bool success, const QString& summary, const QStringList& details,
    const QStringList& warnings = {}) {
    window.statusBar()->showMessage(summary); // 保留结果，不能一秒后悄然消失。
    if (!interactive) return;
    if (auto* previous = window.findChild<QMessageBox*>(QStringLiteral("mapSaveResult"))) previous->close();
    auto* result = new QMessageBox(success ? QMessageBox::Information : QMessageBox::Warning,
        QObject::tr("地图保存结果"), summary, QMessageBox::Ok, &window);
    result->setObjectName(QStringLiteral("mapSaveResult"));
    result->setAttribute(Qt::WA_DeleteOnClose);
    result->setTextFormat(Qt::PlainText);
    result->setInformativeText(details.join(QLatin1Char('\n')));
    if (!warnings.isEmpty()) result->setDetailedText(QObject::tr("警告（%1 条）\n").arg(warnings.size()) + warnings.join(QLatin1Char('\n')));
    result->setWindowModality(Qt::NonModal);
    result->show();
}
QString ChangedMembers(const MapSaveJob& job) {
    const std::pair<const char*, const char*> known[] = {
        {"war3map.w3u", "单位物编"}, {"war3map.w3t", "物品物编"}, {"war3map.w3b", "可破坏物物编"},
        {"war3map.w3d", "装饰物物编"}, {"war3map.w3a", "技能物编"}, {"war3map.w3h", "魔法效果物编"},
        {"war3map.w3q", "升级物编"}, {"war3map.wtg", "触发器与变量"}, {"war3map.wct", "自定义脚本"},
        {"war3map.w3i", "地图设置"}, {"war3map.w3s", "声音"}, {"war3map.w3r", "区域"},
        {"war3map.w3c", "镜头"}, {"war3mapUnits.doo", "单位与物品摆放"}, {"war3map.doo", "地物摆放"},
        {"war3map.w3e", "地形"}, {"war3map.wpm", "通行性"}, {"war3map.wts", "字符串表"},
        {"war3mapMap.blp", "地形小地图"}, {"war3map.shd", "阴影"}, {"war3map.mmp", "小地图图标"}};
    QStringList labels;
    for (const auto& [member, label] : known)
        if (std::any_of(job.replacements.begin(), job.replacements.end(), [member](const auto& r) { return r.name == member; }))
            labels << QObject::tr(label);
    if (labels.isEmpty()) labels << QObject::tr("地图资源与归档内容");
    if (!job.removals.empty()) labels << QObject::tr("删除 %1 个归档成员").arg(job.removals.size());
    return labels.join(QStringLiteral("、"));
}
}

bool MainWindow::RunMapSave(const std::filesystem::path& target, bool inPlace, bool background, bool testSnapshot, bool testHotReload,
    War3Map::Services::TerrainMinimapSaveChoice minimapChoice, bool rebuildTerrainShadow) {
    if (!CheckPendingTerrainCommit(testSnapshot ? tr("测试地图") : tr("保存地图"))) return false;
    // 测试快照绝不能进入原地保存分支，也不能把当前工程切换到测试副本。
    if ((testSnapshot && inPlace) || (testHotReload && !testSnapshot)) return false;
    if (rebuildTerrainShadow && (testSnapshot || testHotReload
        || (CurrentModule() == "campaign" && campaignModule_->Session()))) return false;
    if (project_->savingMap_) return false;
    if (writingRecovery_ && recoveryWorker_) {
        // 恢复 Store 持有来源保护句柄，Ctrl+S 要等它释放后才可替换原图。
        // 继续处理界面事件，并在等待结束后重新捕获草稿；不能提前回报保存成功。
        const auto generation = project_->editRevision_.Generation();
        const QPointer<MainWindow> window(this);
        project_->savingMap_ = true;
        statusBar()->showMessage(tr("正在等待后台恢复完成，然后保存地图…"));
        QEventLoop loop;
        QTimer poll;
        poll.setInterval(25);
        connect(&poll, &QTimer::timeout, &loop, [window, &loop] {
            if (!window || !window->writingRecovery_) loop.quit();
        });
        poll.start();
        loop.exec();
        if (!window) return false;
        project_->savingMap_ = false;
        if (generation != project_->editRevision_.Generation()) {
            AddDiagnostic(tr("警告"), tr("地图"), tr("MAP_SAVE_SESSION_CHANGED"), tr("等待恢复期间会话已切换，地图保存已取消"));
            return false;
        }
    }
    const auto firstDiagnostic = project_->diagnosticsModel_->rowCount();
    const auto reportFailure = [&](const QString& summary, const MapSaveJob* job = nullptr) {
        ReportProgramSaveFailures(firstDiagnostic);
        const auto errors = SaveDiagnostics(*project_, firstDiagnostic, tr("错误"));
        const auto warnings = SaveDiagnostics(*project_, firstDiagnostic, tr("警告"));
        QStringList details{tr("目标：%1").arg(QString::fromStdWString((inPlace ? project_->sourceArchivePath_ : target).wstring()))};
        if (job && !job->written.empty()) {
            details << tr("已写出文件但未完成当前编辑的提交：%1").arg(QString::fromStdWString(job->written.wstring()));
            if (!job->backup.empty()) details << tr("原文件备份：%1").arg(QString::fromStdWString(job->backup.wstring()));
        }
        details.append(errors);
        if (details.size() == 1) details << tr("请查看当前模块中标出的无效值；编辑内容仍然保留。");
        // 标题取最后一条错误：它是中止保存的结论，前面的通常是不致命的来源提示。
        const auto failureSummary = job && !job->written.empty()
            ? tr("文件已写出，但保存结果未被接受；当前编辑仍未保存")
            : errors.isEmpty() ? summary : tr("地图未保存：%1").arg(errors.back().section(QLatin1Char('\n'), 0, 0));
        SaveNotice(*this, background, false, failureSummary, details, warnings);
    };
    if (minimapChoice != War3Map::Services::TerrainMinimapSaveChoice::Preserve
        && minimapChoice != War3Map::Services::TerrainMinimapSaveChoice::Automatic) {
        if ((minimapChoice != War3Map::Services::TerrainMinimapSaveChoice::GenerateMissingGroundOnly
                && minimapChoice != War3Map::Services::TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly)
            || inPlace || testSnapshot || testHotReload || (CurrentModule() == "campaign" && campaignModule_->Session())) {
            project_->AddDiagnostic(tr("错误"), tr("地图"), tr("MINIMAP_SAVE_TARGET"),
                tr("补建缺失的地形小地图仅用于本次正式另存，不支持原地、测试或战役保存。"));
            reportFailure(tr("小地图补建范围不支持")); return false;
        }
        if (settingsModule_->HasPendingDraft() || soundsModule_->HasPendingDraft() || triggersModule_->HasPendingCodeDraft()) {
            project_->AddDiagnostic(tr("错误"), tr("地图"), tr("MINIMAP_SAVE_DOMAIN"),
                tr("请先处理地图设置、声音或代码草稿，再单独另存纯高度修改。"));
            reportFailure(tr("小地图补建不能混入其它草稿")); return false;
        }
    }
    if (settingsModule_->HasPendingDraft() && !settingsModule_->ApplyDraft()) {
        reportFailure(tr("地图设置未通过校验，保存已取消"));
        return false;
    }
    if (soundsModule_->HasPendingDraft() && !soundsModule_->ApplyPendingDraft()) {
        reportFailure(tr("声音定义未通过校验，保存已取消"));
        return false;
    }
    if (!triggersModule_->CommitPendingEdits()) {
        reportFailure(tr("代码草稿未能提交，保存已取消；当前代码仍然保留"));
        return false;
    }
    const auto sourceBeforeSave = project_->sourceArchivePath_;
    auto job = project_->PrepareMapSave(target, inPlace, minimapChoice);
    if (!job) {
        UpdateEditDirtyFlags();
        reportFailure(tr("地图未保存：保存准备未通过"));
        return false;
    }
    job->rebuildTerrainShadow = rebuildTerrainShadow;
    if (testHotReload) {
        if (!job->script || !job->script->luaTarget) {
            project_->AddDiagnostic(tr("错误"), tr("测试"), QStringLiteral("LUA_RELOAD_PROFILE_REQUIRED"),
                tr("热重载需要当前地图明确的 Lua 保存档案，请先选择档案再测试。"));
            return false;
        }
        job->testCopyOnly = true;
        job->script->luaTestOptions.testHotReload = true;
    }
    project_->savingMap_ = true;
    UpdateExportAvailability();
    const auto execute = [reporter = programIssueReporter_](MapSaveJob& task) {
        const auto failed = [&](bool standardException) {
            task.succeeded = false;
            if (reporter) (void)reporter->UnhandledException(War3Map::QtFeedback::ExceptionBoundary::SaveWorker, standardException);
            try {
                task.diagnostics.push_back({War3Map::DiagnosticSeverity::Error, "地图",
                    standardException ? "SAVE_WORKER_CPP_EXCEPTION" : "SAVE_WORKER_NONSTANDARD_EXCEPTION",
                    "保存工作线程未能完成；当前编辑没有标记为保存成功，请核对目标文件状态。"});
            } catch (...) {
            }
        };
        try { EditorProject::RunMapSave(task); }
        catch (const std::exception&) { failed(true); }
        catch (...) { failed(false); }
    };
    if (background) {
        QProgressDialog dialog(tr("正在准备保存…"), QString(), 0, 0, this);
        dialog.setWindowTitle(testSnapshot ? tr("准备测试地图") : inPlace ? tr("保存地图") : tr("另存地图"));
        dialog.setWindowModality(Qt::WindowModal);
        dialog.setCancelButton(nullptr);
        dialog.setMinimumDuration(0);
        dialog.setAutoClose(false);
        dialog.setAutoReset(false);
        dialog.setMinimumWidth(420);
        // 进度回调在工作线程调用：排队到界面线程更新。对话框在工作线程结束后才销毁，
        // 销毁时尚未处理的排队事件随之丢弃。
        auto* label = &dialog;
        job->progress = [label](const QString& text) {
            QMetaObject::invokeMethod(label, [label, text] { label->setLabelText(text); }, Qt::QueuedConnection);
        };
        auto* raw = job.get();
        std::unique_ptr<QThread> worker(QThread::create([raw, execute] { execute(*raw); }));
        QEventLoop loop;
        connect(worker.get(), &QThread::finished, &loop, &QEventLoop::quit);
        worker->start(QThread::LowPriority);
        dialog.show();
        loop.exec();
        worker->wait();
        job->progress = nullptr;
    } else {
        execute(*job);
    }
    project_->savingMap_ = false;
    UpdateExportAvailability();
    if (testSnapshot) {
        project_->AddDiagnostics(job->diagnostics);
        const auto diagnosticCount = job->diagnostics.size();
        if (job->succeeded) War3Map::Services::FinalizeScriptWorkspace(*job);
        if (job->diagnostics.size() > diagnosticCount)
            project_->AddDiagnostics(std::vector<War3Map::Diagnostic>(job->diagnostics.begin() + diagnosticCount, job->diagnostics.end()));
        UpdateEditDirtyFlags();
        if (!job->succeeded) {
            reportFailure(tr("测试副本未能生成"), job.get());
            return false;
        }
        const bool initializationChanged = job->triggerChanges || std::any_of(job->replacements.begin(), job->replacements.end(), [](const auto& member) {
            // 地图设置、声音与预放对象参与游戏初始化。不能测试旧脚本对应的旧摆放。
            const auto name = QString::fromStdString(member.name).toLower();
            return name == QStringLiteral("war3map.w3i") || name == QStringLiteral("war3map.w3s")
                || name == QStringLiteral("war3map.w3r") || name == QStringLiteral("war3map.w3c")
                || name == QStringLiteral("war3mapunits.doo") || name == QStringLiteral("war3map.doo");
        });
        if (initializationChanged && !job->scriptRebuilt) {
            project_->AddDiagnostic(tr("错误"), tr("测试"), QStringLiteral("MAP_TEST_SCRIPT_NOT_REBUILT"),
                tr("触发器或地图初始化内容已修改，但游戏脚本未能重建。请配置编译工具链后重试；当前编辑和原图均保留。"));
            reportFailure(tr("测试已取消：游戏脚本未重建"), job.get());
            return false;
        }
        // 不调用 CommitMapSave：不改变 sourceArchivePath、编辑基线、dirty 或撤销栈。
        if (testHotReload) {
            luaTestGenerated_ = std::move(job->generatedLuaTestUnit);
            luaTestInstalledBindings_ = std::move(job->luaTestInstalledBindings);
            luaTestMapBinding_ = std::move(job->luaTestMapBinding);
        }
        return true;
    }
    const auto saved = project_->CommitMapSave(*job);
    UpdateEditDirtyFlags();
    if (!saved) {
        reportFailure(tr("地图保存未完成"), job.get());
        return false;
    }
    // 先登记章节来源迁移，再显示可能进入嵌套事件循环的保存提示。
    campaignModule_->NoteChapterSaved(sourceBeforeSave, saved->target);
    mapDataAction_->setEnabled(project_->mapDocument_ != nullptr);
    objectsModule_->RefreshIcons();
    objectsModule_->UpdateObjectReferenceEntries();
    UpdateStatusBar();
    UpdateExportAvailability();
    const QString name = QString::fromStdWString(saved->target.wstring());
    const QString script = saved->scriptPreserved ? tr("改动不影响脚本，沿用原地图脚本")
        : saved->scriptRebuilt ? tr("脚本已重建并通过语法检查")
        : saved->savedTriggerChanges ? tr("脚本未重建（工具链未就绪，详见诊断）") : tr("脚本未变");
    QStringList details{tr("文件：%1").arg(name), tr("已写入：%1").arg(ChangedMembers(*job)), script};
    if (job->minimapSave && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded()) {
        const auto actualRange = job->minimapSave->Manifest().rangePolicy;
        details << (job->minimapSave->ClassicManifest()
            ? tr("已按当前地图重建小地图，并核对保存后的图像文件。")
            : actualRange == War3Map::Render::TerrainMinimapRangePolicy::DeclaredW3iMarginsExteriorMapEdge
            ? tr("已按源图声明边距补建缺失的地形小地图；阴影和标记没有重建，仍需原版编辑器或游戏验证。")
            : tr("已补建源图缺失的地形小地图；阴影和标记没有重建，仍需原版编辑器或游戏验证。"));
    }
    if (!saved->backup.empty()) details << (job->inPlaceOptions.keepBackups == 0
        ? tr("原文件备份：%1（不限保留数量）").arg(QString::fromStdWString(saved->backup.wstring()))
        : tr("原文件备份：%1（每张源图保留最近 %2 份）").arg(QString::fromStdWString(saved->backup.wstring()))
            .arg(static_cast<qulonglong>(job->inPlaceOptions.keepBackups)));
    const auto summary = (saved->inPlace ? tr("地图已保存 · %1 秒") : tr("地图已另存为 · %1 秒")).arg(saved->seconds, 0, 'f', 1);
    project_->AddDiagnostic(tr("信息"), tr("地图"), QStringLiteral("MAP_SAVE_COMPLETED"), details.join(QLatin1Char('\n')));
    SaveNotice(*this, background, true, summary, details);
    if ((luaReloadController_ || luaFileReload_) && luaAutoReloadAction_ && luaAutoReloadAction_->isChecked())
        QTimer::singleShot(0, this, [this] { (void)LuaTestReload(true); });
    return true;
}

void MainWindow::SaveMapInPlace() {
    if (CurrentModule() == "campaign" && campaignModule_->Session()) { (void)campaignModule_->Save(); return; }
    if (!CheckPendingTerrainCommit(tr("保存地图"))) return;
    if (project_->archiveSession_ == nullptr) {
        // 从展开目录打开时没有地图归档，Ctrl+S 保存工程。
        if (!project_->projectRoot_.empty()) SaveEditorProject();
        else SaveNotice(*this, true, false, tr("没有可保存的地图"), {tr("请先打开地图，或使用另存为指定输出路径。")});
        return;
    }
    if (project_->MapSourceIsRecoveryCopy()) {
        SaveNotice(*this, true, false, tr("原地图已变化或找不到，不能直接写回"),
            {tr("当前编辑来自恢复工程，打开的是临时副本。请在接下来的窗口中选择保存位置；当前编辑保持不变。")});
        (void)SaveAsMapArchive();
        return;
    }
    // 草稿验证和失败反馈统一交给 RunMapSave，不能在这里静默返回。
    if (settingsModule_->HasPendingDraft() || soundsModule_->HasPendingDraft() || triggersModule_->HasPendingCodeDraft()) {
        (void)RunMapSave({}, true, true); return;
    }
    if (!triggersModule_->CommitPendingEdits()) {
        SaveNotice(*this, true, false, tr("代码草稿未能提交，地图未保存"), {tr("请查看触发器模块中的诊断；当前编辑仍然保留。")});
        return;
    }
    if (!project_->HasUnsavedMapChanges() && !project_->luaMapTarget_) {
        statusBar()->showMessage(tr("没有需要保存的地图修改"), 5000);
        return;
    }
    (void)RunMapSave({}, true, true);
}

bool MainWindow::SaveAsMapArchive(War3Map::Services::TerrainMinimapSaveChoice minimapChoice) {
    if (CurrentModule() == "campaign" && campaignModule_->Session()) {
        if (minimapChoice != War3Map::Services::TerrainMinimapSaveChoice::Preserve
            && minimapChoice != War3Map::Services::TerrainMinimapSaveChoice::Automatic) return false;
        return campaignModule_->Save(true);
    }
    if (!CheckPendingTerrainCommit(tr("另存地图"))) return false;
    // 先完成路径选择；取消时不读取、回灌或修改任何会话文档。
    // 恢复副本不提示临时目录；放在作者原图旁边，文件名加“恢复”避免与原图重名（另存为不覆盖已有文件）。
    auto suggested = project_->sourceArchivePath_;
    if (project_->MapSourceIsRecoveryCopy() && !project_->recordedSourceMapPath_.empty()) {
        const auto& original = project_->recordedSourceMapPath_;
        suggested = original.parent_path() / (original.stem().wstring() + L" (恢复)" + original.extension().wstring());
    }
    const QString target = QFileDialog::getSaveFileName(
        this, tr("另存地图为"), QString::fromStdWString(suggested.wstring()),
        tr("Warcraft III 地图 (*.w3x);;所有文件 (*.*)"));
    return !target.isEmpty() && RunMapSave(std::filesystem::path(target.toStdWString()), false, true, false, false, minimapChoice);
}

bool MainWindow::SaveMapArchiveToPath(const std::filesystem::path& outputPath,
    War3Map::Services::TerrainMinimapSaveChoice minimapChoice) {
    // 同步执行（无界面探针与测试使用），不显示模态窗口。
    return RunMapSave(outputPath, false, false, false, false, minimapChoice);
}
