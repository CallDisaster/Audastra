#include "MainWindow.h"
#include "RecoveryPreferences.h"
#include "MapSettingsModule.h"
#include "SoundsModule.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "ObjectModule.h"
#include "TriggerModule.h"
#include "ProjectFileIO.h"
#include "ProjectSaveJob.h"
#include "RecoveryBackupCleanup.h"
#include "RecoveryExitDialog.h"
#include "ProjectOpen.h"
#include "TriggerModels.h"
#include "Icons/IconProvider.h"
#include "War3MapCore/CapabilityRegistry.h"
#include "War3MapCore/EditorSession.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ProjectStore.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Workspace.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Trigger/ScriptBuildService.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QPointer>
#include <QScopeGuard>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QUndoStack>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace War3Map;
namespace {
QString ProjectText(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QString Path(const std::filesystem::path& path) { return QString::fromStdWString(path.wstring()); }

}

std::string MainWindow::ProjectFingerprint() const {
    return project_->ProjectFingerprint();
}

bool MainWindow::SaveEditorProjectToPath(const std::filesystem::path& target, bool recovery) {
    if (target.empty() || project_->projectRoot_.empty()) return false;
    if (settingsModule_->HasPendingDraft() && !settingsModule_->ApplyDraft()) return false;
    if (soundsModule_->HasPendingDraft() && !soundsModule_->ApplyPendingDraft()) return false;
    if (!triggersModule_->CommitPendingEdits()) return false;
    const auto written = project_->SaveProjectToPath(target, recovery);
    for (const auto& diagnostic : written.diagnostics) {
        const auto level = diagnostic.severity == DiagnosticSeverity::Error ? tr("错误")
            : diagnostic.severity == DiagnosticSeverity::Warning ? tr("警告") : tr("信息");
        AddDiagnostic(level, tr("工程"), ProjectText(diagnostic.code), ProjectText(diagnostic.message));
        if (diagnostic.origin && !diagnostic.origin->source.empty())
            AddDiagnostic(tr("信息"), tr("工程"), tr("PROJECT_RECOVERABLE_FILE"), Path(diagnostic.origin->source));
    }
    // 工程层负责内容与基线，界面只提交草稿和显示结果。
    UpdateEditDirtyFlags();
    if (!written.Succeeded()) return false;
    if (written.value->backupPath) AddDiagnostic(tr("信息"), tr("工程"), tr("PROJECT_BACKUP_SAVED"),
        tr("上一版工程：%1").arg(Path(*written.value->backupPath)));
    statusBar()->showMessage(recovery ? tr("已保存恢复点")
        : tr("工程已保存 · %1").arg(Path(target.filename())), 5000);
    return true;
}

bool MainWindow::OpenEditorProjectPath(const std::filesystem::path& path) {
    auto prepared = War3Map::QtProject::PrepareProjectOpen(path);
    if (!prepared.Succeeded()) {
        for (const auto& diagnostic : prepared.diagnostics)
            AddDiagnostic(tr("错误"), tr("工程"), ProjectText(diagnostic.code), ProjectText(diagnostic.message));
        return false;
    }
    auto& candidate = **prepared.value;
    const bool map = candidate.map;
    auto& objects = candidate.objects;
    auto& triggers = candidate.triggers;
    auto& catalog = candidate.catalog;
    auto& imports = candidate.imports;
    // 完成全部解析后才切换工作区，任何预检错误不会丢掉当前编辑。
    // 工程的私有基线属于恢复实现，不是作者打开的地图；保持外部入口，不登记最近地图。
    // 作者原图仍与基线逐字节相同时直接打开原图，“保存地图”写回原位置；否则只能另存为。
    const auto mapSource = map && candidate.sourceMapMatchesBaseline ? candidate.sourceMapPath : candidate.BaselinePath();
    if (!(map ? OpenMapArchivePath(mapSource) : OpenWorkspacePath(candidate.BaselinePath()))) return false;
    project_->recordedSourceMapPath_ = candidate.sourceMapPath;
    const auto originalMapScopes = project_->editRevision_.Current();
    project_->restoredFiles_ = std::move(candidate.extraction); // 会话结束后自动回收提取目录。
    project_->mapEdits_ = std::move(candidate.mapDataEdits);
    objectsModule_->DetachDocuments();
    for (int i = 0; i < 7; ++i) {
        if (!objects[i]) continue;
        if (map && !project_->nativeObjectDocuments_[i]) {
            auto empty = ObjectData::NativeObjectDocument::CreateEmpty(static_cast<ObjectData::ObjectType>(i));
            project_->nativeObjectDocuments_[i] = std::make_unique<ObjectData::NativeObjectDocument>(std::move(*empty.value));
        }
        project_->objectDocuments_[i] = std::move(objects[i]); project_->objectDirty_[i] = true;
    }
    if (triggers) {
        project_->activeUiDataRoot_ = candidate.uiRoot;
        project_->compilerDataRoot_ = candidate.compilerRoot;
        // 内嵌定义用于无损恢复；本机包的 JASS includes 仍从原工具根目录发现。
        if (!candidate.compilerRoot.empty()) {
            const auto restoredRoot = project_->activeUiDataRoot_;
            project_->activeUiDataRoot_ = candidate.compilerRoot;
            const auto localCatalog = project_->LoadEffectiveUiCatalog(candidate.compilerRoot);
            project_->AddDiagnostics(localCatalog.diagnostics);
            project_->activeUiDataRoot_ = restoredRoot;
        }
        project_->ReplaceTriggerWorkspace(std::move(catalog), std::move(triggers));
        project_->triggerDirty_ = true; project_->restoredTriggerOverlay_ = true;
        triggersModule_->RefreshEditor(); RefreshVariablesPage();
    }
    project_->pendingImports_ = std::move(imports);
    project_->assetEdits_ = std::move(candidate.assetEdits);
    objectsModule_->UpdateObjectReferenceEntries(); objectsModule_->ShowObjectType(0); project_->NotifyImportsChanged(); UpdateExportAvailability(); UpdateStatusBar();
    const bool recoveryProject = candidate.recovery;
    project_->lastProjectFingerprint_ = recoveryProject ? std::string{} : ProjectFingerprint();
    project_->restoredProjectJournalJson_ = candidate.journalJson;
    const auto restoredScopes = CaptureEditScopes();
    if (restoredScopes) {
        project_->editTrackingReady_ = project_->editRevision_.Reset(*restoredScopes,
            recoveryProject ? originalMapScopes : *restoredScopes, originalMapScopes).Succeeded();
        project_->editTrackingFailed_ = !project_->editTrackingReady_;
        UpdateEditDirtyFlags();
    }
    if (!recoveryProject) {
        project_->projectFilePath_ = candidate.sourcePath;
        project_->projectFileSha256_ = candidate.sourceSha256;
    }
    RefreshProjectWatch();
    UpdateStatusBar();
    statusBar()->showMessage(tr("工程已恢复 · %1").arg(Path(path.filename())), 5000);
    return true;
}

void MainWindow::SaveEditorProject() {
    if (!project_->projectFilePath_.empty()) {
        (void)SaveEditorProjectToPath(project_->projectFilePath_);
        return;
    }
    SaveEditorProjectAs();
}

void MainWindow::SaveEditorProjectAs() {
    const auto suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(
        "Audastra-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + ".forge");
    const auto path = QFileDialog::getSaveFileName(this, tr("另存编辑工程"), suggested, tr("Audastra 工程 (*.forge)"));
    if (!path.isEmpty()) SaveEditorProjectToPath(std::filesystem::path(path.toStdWString()));
}
void MainWindow::OpenEditorProject() {
    const auto path = QFileDialog::getOpenFileName(this, tr("打开编辑工程"), {}, tr("Audastra 工程 (*.forge)"));
    if (!path.isEmpty()) OpenEditorProjectPath(std::filesystem::path(path.toStdWString()));
}
void MainWindow::ScheduleRecovery() {
    UpdateWindowTitle();
    if (writingRecovery_) { recoveryRequested_ = true; return; }
    // 首次编辑启动周期；持续输入不重置倒计时，避免恢复被无限推迟。
    if (startupMode_ == StartupMode::Normal && autoRecoveryEnabled_ && !writingRecovery_ && project_->recoveryTimer_
        && !project_->projectRoot_.empty() && !project_->recoveryTimer_->isActive()) project_->recoveryTimer_->start();
}
void MainWindow::ApplyRecoveryPreferences(const War3Map::Agent::RecoveryPreferences& preferences) {
    autoRecoveryEnabled_ = preferences.enabled;
    if (!project_->recoveryTimer_) return;
    project_->recoveryTimer_->stop();
    const int minutes = War3Map::Agent::IsRecoveryIntervalValid(preferences.intervalMinutes)
        ? preferences.intervalMinutes : 5;
    project_->recoveryTimer_->setInterval(minutes * 60 * 1000);
    if (autoRecoveryEnabled_ && (project_->HasUnsavedMapChanges()
        || (triggersModule_ && triggersModule_->HasPendingCodeDraft()))) ScheduleRecovery();
}
void MainWindow::WriteRecovery() {
    if (!autoRecoveryEnabled_ || writingRecovery_ || project_->projectRoot_.empty()) return;
    if (project_->savingMap_) {
        project_->recoveryTimer_->start(); // 保存地图期间不写恢复点，稍后重试
        return;
    }
    // 提交待应用草稿会同步触发编辑通知；恢复本身不排重复的下一份快照。
    writingRecovery_ = true;
    bool startupHandedOff = false;
    // 只防启动前准备异常留下latch；运行后的任务仍由原完成/退出链拥有。
    const auto resetUnpublished = qScopeGuard([this, &startupHandedOff] {
        if (!startupHandedOff) { writingRecovery_ = false; recoveryRequested_ = false; }
    });
    const auto retry = [this] {
        writingRecovery_ = false;
        recoveryRequested_ = false;
        if (startupMode_ == StartupMode::Normal && autoRecoveryEnabled_ && project_->recoveryTimer_)
            project_->recoveryTimer_->start();
    };
    if ((settingsModule_->HasPendingDraft() && !settingsModule_->ApplyDraft())
        || (soundsModule_->HasPendingDraft() && !soundsModule_->ApplyPendingDraft())
        || !triggersModule_->CommitPendingEdits()) { retry(); return; }
    if (!project_->HasUnsavedMapChanges()) { writingRecovery_ = false; recoveryRequested_ = false; return; }
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/recovery";
    if (!QDir().mkpath(dir)) { retry(); return; }
    auto target = project_->recoveryPath_;
    std::optional<std::string> expected;
    if (!target.empty() && !project_->recoveryFileSha256_.empty()) expected = project_->recoveryFileSha256_;
    // 没有本会话可信回执的既存文件不能覆盖；SHA 的核验由 Store 在工作线程执行。
    if (target.empty() || (!expected && QFileInfo::exists(Path(target))))
        target = std::filesystem::path((dir + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".forge").toStdWString());
    auto prepared = project_->PrepareProjectSave(target, true, expected);
    if (!prepared.Succeeded() || !prepared.value) {
        project_->AddDiagnostics(prepared.diagnostics); retry(); return;
    }
    std::shared_ptr<War3Map::QtProject::ProjectSaveJob> job;
    std::unique_ptr<QThread> workerOwner;
    QThread* worker = nullptr;
    std::array<QMetaObject::Connection, 4> ownedConnections{};
    std::size_t connectionCount = 0;
    bool startAttempted = false;
    try {
        job = std::shared_ptr<War3Map::QtProject::ProjectSaveJob>(std::move(*prepared.value));
        const auto cleanup = std::make_shared<Status>(Status::Ok());
        const auto handled = std::make_shared<bool>(false); // 仅在界面线程读写。
        const auto quitWaiting = std::make_shared<bool>(false);
        const QPointer<MainWindow> window(this);
        const auto startupStage = [this](int stage) {
            if (startupMode_ == StartupMode::HeadlessProbe && recoveryStartupStageHook_)
                recoveryStartupStageHook_(stage); // 自有回归只注入启动前异常，不运行作者代码。
        };
        startupStage(1);
        workerOwner.reset(QThread::create([job, cleanup] {
#ifdef _WIN32
            const bool backgroundMode = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
            const auto restorePriority = qScopeGuard([backgroundMode] {
                if (backgroundMode) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
            });
#endif
            War3Map::QtProject::EditorProject::RunProjectSave(*job);
            if (job->Outcome().Succeeded() && job->ExpectedTargetSha256()) {
                try { *cleanup = War3Map::QtProject::CleanupConfirmedRecoveryBackup(
                    *job->Outcome().value, *job->ExpectedTargetSha256()); }
                catch (...) { *cleanup = Status::Failure("恢复", "RECOVERY_BACKUP_KEPT", "恢复已写出，上一恢复点因清理异常而保留。"); }
            }
        }));
        if (!workerOwner) throw std::runtime_error("Recovery worker allocation failed");
        worker = workerOwner.get();
        const auto keepConnection = [&](QMetaObject::Connection connection) {
            if (!connection || connectionCount >= ownedConnections.size()) {
                QObject::disconnect(connection);
                throw std::runtime_error("Recovery worker connection failed");
            }
            ownedConnections[connectionCount++] = connection;
            return connection;
        };
        const auto finish = [window, worker, job, cleanup, handled] {
            if (*handled) return;
            *handled = true;
            if (!window) return;
            bool changed = true;
            bool succeeded = false;
            const auto settle = qScopeGuard([&] {
                if (!window) return;
                if (window->recoveryWorker_ == worker) window->recoveryWorker_ = nullptr;
                if (window->recoveryJob_ == job) window->recoveryJob_.reset();
                window->writingRecovery_ = false;
                window->recoveryRequested_ = false;
                // 失败采用完整周期重试；持续编辑不使正在写出的冻结修订失效。
                if ((changed || !succeeded) && window->autoRecoveryEnabled_)
                    try { window->ScheduleRecovery(); } catch (...) {}
            });
            try {
                changed = window->project_->editRevision_.Generation() != job->Generation()
                    || window->project_->editRevision_.Revision() != job->Revision()
                    || window->project_->editRevision_.Current() != job->Scopes()
                    || window->recoveryRequested_;
                const auto written = window->project_->CommitProjectSave(*job);
                if (job->Cancelled()) {
                    auto diagnostics = written.diagnostics;
                    QString retainedPath;
                    for (auto& diagnostic : diagnostics) {
                        if (diagnostic.code == "PROJECT_CANDIDATE_RETAINED") {
                            diagnostic.severity = DiagnosticSeverity::Warning;
                            if (diagnostic.origin) retainedPath = Path(diagnostic.origin->source);
                        } else if (diagnostic.code == War3Map::kProjectSaveCancelledCode) {
                            diagnostic.severity = DiagnosticSeverity::Info;
                        }
                    }
                    window->project_->AddDiagnostics(diagnostics);
                    window->statusBar()->showMessage(retainedPath.isEmpty()
                        ? tr("已取消本次恢复写入 · 上一份恢复点保留")
                        : tr("已取消本次恢复写入 · 上一份恢复点保留；临时候选保留：%1").arg(retainedPath), 8000);
                    return;
                }
                window->project_->AddDiagnostics(written.diagnostics);
                if (written.Succeeded()) {
                    window->project_->recoveryPath_ = written.value->path;
                    window->project_->recoveryFileSha256_ = written.value->sha256;
                    QSettings{}.setValue("project/latestRecovery", Path(written.value->path));
                    window->statusBar()->showMessage(tr("已保存恢复点"), 5000);
                    succeeded = true;
                }
                for (auto diagnostic : cleanup->diagnostics) {
                    diagnostic.severity = DiagnosticSeverity::Warning;
                    window->project_->AddDiagnostics({diagnostic});
                }
            } catch (...) {
                try { window->AddDiagnostic(tr("警告"), tr("恢复"), tr("RECOVERY_RECEIPT_FAILED"),
                    tr("恢复回执发布异常，当前编辑仍保留；已写文件可从恢复目录核对")); } catch (...) {}
            }
        };
        keepConnection(connect(worker, &QThread::finished, QCoreApplication::instance(), finish));
        keepConnection(connect(worker, &QThread::finished, worker, &QObject::deleteLater));
        startupStage(2);
        const QPointer<QThread> ownedWorker(worker);
        const auto quitting = keepConnection(connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            QCoreApplication::instance(), [window, ownedWorker, job, finish, quitWaiting] {
                if (*quitWaiting || (window && window->recoveryExitWaiting_)) return;
                *quitWaiting = true;
                if (window) window->recoveryExitWaiting_ = true;
                const auto settleWait = qScopeGuard([&] {
                    *quitWaiting = false;
                    if (window) window->recoveryExitWaiting_ = false;
                });
                if (window && ownedWorker) {
                    (void)War3Map::QtApp::WaitForRecoveryExit(ownedWorker, [job] { return job->RequestCancellation(); }, window, false);
                }
                if (ownedWorker) ownedWorker->wait();
                finish();
            }));
        keepConnection(connect(worker, &QThread::finished, QCoreApplication::instance(), [quitting] { QObject::disconnect(quitting); }));
        QTimer::singleShot(5000, worker, [window, worker, job, finish, quitting] {
            if (worker->isRunning() || job->Completed()) return;
            if (!window) { QObject::disconnect(quitting); worker->deleteLater(); return; }
            if (!window->project_->CancelPreparedProjectSave(*job).Succeeded()) return;
            QObject::disconnect(quitting);
            finish();
            worker->deleteLater();
        });
        startupStage(3);
        // 所有可失败分配/接线先完成，只有就绪任务才发布到当前窗口。
        recoveryRequested_ = false;
        recoveryWorker_ = worker;
        recoveryJob_ = job;
        startupHandedOff = true;
        startAttempted = true;
        (void)workerOwner.release(); // start可能已被OS接受，后续异常不得删除活线程。
        worker->start(QThread::LowPriority);
        if (worker->isFinished() && !job->Completed()
            && project_->CancelPreparedProjectSave(*job).Succeeded()) {
            QObject::disconnect(quitting);
            finish();
            worker->deleteLater();
        }
    } catch (...) {
        if (startAttempted) {
            // 完成/退出/5秒Prepared确认连接均已就绪；保活原任务，不能强杀或虚报取消。
            try { AddDiagnostic(tr("警告"), tr("恢复"), tr("RECOVERY_WORKER_START_RECEIPT_FAILED"),
                tr("后台恢复启动回执发生异常，请等待任务完成，当前编辑仍保留")); } catch (...) {}
            return;
        }
        for (std::size_t index = 0; index < connectionCount; ++index)
            QObject::disconnect(ownedConnections[index]);
        auto* pending = job ? job.get() : prepared.value->get();
        if (pending) try { (void)project_->CancelPreparedProjectSave(*pending); } catch (...) {}
        if (recoveryWorker_ == worker) recoveryWorker_ = nullptr;
        if (recoveryJob_ == job) recoveryJob_.reset();
        workerOwner.reset(); // 只释放本次从未启动的线程，同时回收其singleShot。
        try { retry(); } catch (...) { writingRecovery_ = false; recoveryRequested_ = false; }
        try { AddDiagnostic(tr("警告"), tr("恢复"), tr("RECOVERY_WORKER_START_FAILED"),
            tr("未能启动后台恢复，当前编辑与上一恢复点保留")); } catch (...) {}
    }
}
bool MainWindow::WaitForRecoveryExit() {
    if (!writingRecovery_ || !recoveryWorker_) return true;
    if (recoveryExitWaiting_) return false;
    recoveryExitWaiting_ = true;
    const QPointer<MainWindow> window(this);
    const auto settleWait = qScopeGuard([window] { if (window) window->recoveryExitWaiting_ = false; });
    const auto job = recoveryJob_;
    return War3Map::QtApp::WaitForRecoveryExit(recoveryWorker_, [job] {
        return job && job->RequestCancellation();
    }, this);
}
void MainWindow::RestoreRecovery() {
    const auto recent = QSettings{}.value("project/latestRecovery").toString();
    const auto path = QFileDialog::getOpenFileName(this, tr("恢复编辑"), recent, tr("Audastra 恢复工程 (*.forge)"));
    if (!path.isEmpty()) OpenEditorProjectPath(std::filesystem::path(path.toStdWString()));
}

void MainWindow::CheckGameBuild() {
    if (!project_->triggerDocument_ || !project_->uiCatalog_) {
        AddDiagnostic(tr("错误"), tr("构建"), tr("BUILD_TRIGGER_INPUT_MISSING"), tr("未载入触发器文档及匹配定义，无法重建地图脚本"));
    } else {
        const auto result = Trigger::ScriptBuildService::CheckGameBuild(*project_->triggerDocument_, *project_->uiCatalog_, Trigger::ScriptBackend::Jass, project_->mapDocument_.get());
        for (const auto& diagnostic : result.diagnostics)
            AddDiagnostic(diagnostic.severity == DiagnosticSeverity::Error ? tr("错误") : tr("警告"), tr("构建"), ProjectText(diagnostic.code), ProjectText(diagnostic.message));
    }
    if (auto* dock = findChild<QDockWidget*>("diagnosticsDock")) dock->show();
    statusBar()->showMessage(tr("构建未产出地图 · 查看具体缺项；可先保存工程"), 8000);
}
void MainWindow::ShowCapabilities() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("功能状态")); dialog.resize(1020, 520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* table = new QTableWidget(static_cast<int>(CapabilityRegistry::Modules().size()), 8);
    table->setHorizontalHeaderLabels({tr("模块"), tr("读取"), tr("编辑"), tr("往返"), tr("编译"), tr("原版验收"), tr("运行验收"), tr("待完成")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->verticalHeader()->hide();
    const auto state = [](CapabilityLevel level) { return level == CapabilityLevel::Supported ? QObject::tr("已支持") : level == CapabilityLevel::Partial ? QObject::tr("部分") : QObject::tr("未接通"); };
    int row = 0;
    for (const auto& module : CapabilityRegistry::Modules()) {
        const QStringList values{ProjectText(module.name), state(module.read), state(module.edit), state(module.roundtrip), state(module.compile), tr("未验收"), tr("未验收"), ProjectText(module.remaining)};
        for (int col = 0; col < values.size(); ++col) table->setItem(row, col, new QTableWidgetItem(values[col]));
        ++row;
    }
    table->resizeColumnsToContents(); table->horizontalHeader()->setStretchLastSection(true); layout->addWidget(table);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(tr("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons); dialog.exec();
}
