#include "MainWindow.h"
#include "MapSettingsModule.h"
#include "SoundsModule.h"
#include "TriggerModule.h"
#include "ProgramIssueReporter.h"
#include "../QtCommon/ApplicationPaths.h"
#include "../QtCommon/LocalConsentPreference.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Services/MapTestWorkspace.h"
#include "War3MapCore/Script/MapTestToolchain.h"

#include <QDir>
#include <QAction>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>
#include <QScopedValueRollback>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>

using War3Map::Result;
using War3Map::Sha256File;
namespace Services = War3Map::Services;
namespace Script = War3Map::Script;
namespace QtCommon = War3Map::QtCommon;
namespace QtFeedback = War3Map::QtFeedback;
namespace {
QString TestStage(std::string_view stage) {
    if (stage == "copy") return QObject::tr("正在准备独立测试副本…");
    if (stage == "slk") return QObject::tr("正在转换测试副本为 SLK…");
    if (stage == "freeze") return QObject::tr("正在冻结测试副本的文件身份…");
    if (stage == "read-scripts") return QObject::tr("正在读取地图脚本和地图信息…");
    if (stage == "check-script") return QObject::tr("正在检查已保存脚本的结构和调用…");
    if (stage == "check-runtime") return QObject::tr("正在检查所需游戏运行库…");
    if (stage == "check-lua") return QObject::tr("正在检查地图里的 Lua 文件…");
    if (stage == "launch") return QObject::tr("正在复制游戏测试地图并启动…");
    return QObject::tr("正在核对冻结的测试副本…");
}
}

void MainWindow::TestMap() {
    if (project_->savingMap_) return;
    if (mapTestLease_ && mapTestLease_->IsActive()) {
        ShowStatusMessage(tr("上一轮测试游戏仍在运行，请先退出游戏再测试。"));
        return;
    }
    if (!project_->archiveSession_ || project_->sourceArchivePath_.empty()) {
        ShowStatusMessage(tr("请先打开一张地图，再运行测试。"));
        return;
    }
    QSettings settings;
    const bool ownedLua = project_->luaMapTarget_
        && project_->luaMapTarget_->dialect == Script::LuaMapSaveDialect::Classic127YDWE;
    const bool testHotReload = ownedLua && settings.value("mapTest/hotReloadEnabled", true).toBool();
    luaTestGenerated_.reset(); luaTestMapBinding_.clear(); luaReloadController_.reset();
    luaTestInstalledBindings_.reset();
    luaRuntimeMailbox_.reset();
    luaFileReload_ = false; luaFileQueue_.clear(); luaFileBaseline_.clear();
    luaRuntimeDiagnosticSequence_ = 0; luaReloadLastStatus_.clear();
    if (luaReloadPoll_) luaReloadPoll_->stop();
    const auto gameRoot = settings.value(QStringLiteral("war3/gameRoot")).toString();
    const auto runtimeRoot = settings.value(QStringLiteral("mapTest/runtimeRoot")).toString().trimmed();
    const auto runtimeYdweRoot = settings.value(QStringLiteral("ui/dataRoot")).toString().trimmed();
    if (gameRoot.isEmpty()) {
        QMessageBox::information(this, tr("地图测试"), tr("请在应用设置中选择 Warcraft III 安装目录。"));
        return;
    }
    const QDir testRoot(QDir(QtCommon::InstallationRoot()).filePath(QStringLiteral("test-maps")));
    const auto runName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto runDirectory = testRoot.filePath(runName);
    std::error_code directoryError;
    if (!QDir().mkpath(testRoot.absolutePath())
        || !std::filesystem::create_directory(std::filesystem::path(runDirectory.toStdWString()), directoryError)) {
        QMessageBox::warning(this, tr("地图测试"), tr("无法创建独立测试目录：%1").arg(runDirectory));
        return;
    }
    auto workspace = Services::FreezeMapTestWorkspace(std::filesystem::path(runDirectory.toStdWString()));
    if (!workspace.Succeeded()) {
        project_->AddDiagnostics(workspace.diagnostics);
        QMessageBox::warning(this, tr("地图测试"), tr("无法保护本次测试目录，请查看诊断。"));
        return;
    }
    const auto cleanupUnstarted = [this, files = *workspace.value](bool refresh) {
        const auto result = std::make_shared<War3Map::Status>();
        auto* worker = QThread::create([files, result, refresh] {
            try {
                if (refresh) *result = files->Refresh();
                if (result->Succeeded()) *result = files->Cleanup(true);
            } catch (...) {
                *result = War3Map::Status::Failure("Map.Test", "MAP_TEST_CLEANUP_EXCEPTION", "未启动的测试目录清理失败，未确认内容已保留。");
            }
        });
        connect(worker, &QThread::finished, this, [this, result] { project_->AddDiagnostics(result->diagnostics); });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start(QThread::LowPriority);
    };
    // 草稿进入当前编辑事务，但绝不提交为已保存。无修改时直接使用已保存地图的冻结输入。
    if (!triggersModule_->CommitPendingEdits()) {
        cleanupUnstarted(false);
        QMessageBox::warning(this, tr("地图测试未启动"), tr("代码草稿未能提交，请查看触发器模块中的诊断。原图和当前编辑均保留。"));
        return;
    }
    const bool snapshot = project_->HasUnsavedMapChanges() || project_->luaMapTarget_
        || settingsModule_->HasPendingDraft() || soundsModule_->HasPendingDraft();
    auto input = project_->sourceArchivePath_;
    if (snapshot) {
        input = std::filesystem::path(QDir(runDirectory).filePath(QStringLiteral("input.w3x")).toStdWString());
        if (!RunMapSave(input, false, true, true, testHotReload)) { cleanupUnstarted(true); return; }
    }
    Services::MapTestRequest request;
    request.inputMap = input;
    request.sourceMap = project_->sourceArchivePath_;
    request.gameRoot = std::filesystem::path(gameRoot.toStdWString());
    request.runtimeRoot = std::filesystem::path(runtimeRoot.toStdWString());
    request.runtimeYdweRoot = std::filesystem::path(runtimeYdweRoot.toStdWString());
    const QDir program(QCoreApplication::applicationDirPath());
    request.runtimeBridge = std::filesystem::path(program.filePath(QStringLiteral("audastra-warcraft-launcher.exe")).toStdWString());
    request.runtimeBootstrap = std::filesystem::path(program.filePath(QStringLiteral("audastra-warcraft-bootstrap.dll")).toStdWString());
    request.workspace = std::filesystem::path(QDir(runDirectory).filePath(QStringLiteral("prepared")).toStdWString());
    request.optimizeSlk = QtCommon::ReadExplicitLocalConsent(settings, QStringLiteral("mapTest/slkEnabled"));
    request.useOwnedLuaRuntime = ownedLua;
    request.enableLuaHotReload = testHotReload;
    // 作者手写 Lua（exec-lua 入口）由预检识别；这里只传设置中的运行环境偏好。
    request.handwrittenLuaEngine = settings.value("mapTest/luaEngine").toString() == QLatin1String("ydwe")
        ? Services::MapTestLuaEngine::Ydwe : Services::MapTestLuaEngine::Owned;
    request.handwrittenLuaHotReload = settings.value("mapTest/hotReloadEnabled", true).toBool();
    if (!snapshot) request.expectedInputSha256 = project_->archiveSession_->SourceSha256();

    QProgressDialog dialog(tr("正在准备测试地图…"), QString(), 0, 0, this);
    dialog.setWindowTitle(tr("地图测试"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setCancelButton(nullptr);
    dialog.setMinimumDuration(0);
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    dialog.setMinimumWidth(420);
    QScopedValueRollback<bool> saving(project_->savingMap_, true);
    Result<Services::MapTestArtifact> artifact;
    Result<Script::MapTestLaunchReceipt> launched;
    auto workspaceReady = War3Map::Status::Failure("Map.Test", "MAP_TEST_WORKSPACE_UNFROZEN", "测试目录内容尚未冻结，目录保留。");
    auto* progress = &dialog;
    const auto update = [progress](std::string_view stage) {
        const auto text = TestStage(stage);
        QMetaObject::invokeMethod(progress, [progress, text] { progress->setLabelText(text); }, Qt::QueuedConnection);
    };
    auto worker = std::unique_ptr<QThread>(QThread::create([&, reporter = programIssueReporter_] {
        try {
            if (request.expectedInputSha256.empty()) {
                const auto sha = Sha256File(request.inputMap);
                if (!sha.Succeeded()) artifact.diagnostics = sha.diagnostics;
                else request.expectedInputSha256 = *sha.value;
            }
            if (!request.expectedInputSha256.empty())
                artifact = Services::PrepareMapTest(request, Script::OptimizeMapTestSlk, update);
            update("freeze");
            workspaceReady = (*workspace.value)->Refresh();
            if (artifact.Succeeded() && workspaceReady.Succeeded()) {
                launched = Script::LaunchMapTest(*artifact.value, update);
            }
        } catch (const std::exception&) {
            if (reporter) (void)reporter->UnhandledException(QtFeedback::ExceptionBoundary::SaveWorker, true);
            artifact = Result<Services::MapTestArtifact>::Failure("Map.Test", "MAP_TEST_CPP_EXCEPTION", "测试准备未能完成；原图和当前编辑均保留。");
        } catch (...) {
            if (reporter) (void)reporter->UnhandledException(QtFeedback::ExceptionBoundary::SaveWorker, false);
            artifact = Result<Services::MapTestArtifact>::Failure("Map.Test", "MAP_TEST_UNKNOWN_EXCEPTION", "测试准备未能完成；原图和当前编辑均保留。");
        }
    }));
    QEventLoop loop;
    connect(worker.get(), &QThread::finished, &loop, &QEventLoop::quit);
    worker->start(QThread::LowPriority);
    dialog.show();
    loop.exec();
    worker->wait();
    dialog.hide();
    project_->AddDiagnostics(artifact.diagnostics);
    project_->AddDiagnostics(workspaceReady.diagnostics);
    project_->AddDiagnostics(launched.diagnostics);
    if (!artifact.Succeeded() || !workspaceReady.Succeeded() || !launched.Succeeded()) {
        const auto& diagnostics = !artifact.Succeeded() ? artifact.diagnostics
            : !workspaceReady.Succeeded() ? workspaceReady.diagnostics : launched.diagnostics;
        const auto detail = diagnostics.empty() ? tr("测试准备未通过，请查看诊断。")
            : QString::fromStdString(diagnostics.back().message);
        QMessageBox::warning(this, tr("地图测试未启动"), detail + tr("\n原图和当前编辑均保留。"));
        if (workspaceReady.Succeeded()) cleanupUnstarted(false);
        return;
    }
    // 仅用户点击测试时发起游戏；预检、SHA与复制均在低优先级worker完成。
    mapTestLease_ = launched.value->lease;
    if (launched.value->luaMailbox) {
        luaRuntimeMailbox_ = launched.value->luaMailbox;
        luaTestGeneration_ = project_->editRevision_.Generation();
        luaTestSource_ = project_->sourceArchivePath_;
    }
    const bool handwrittenLua = !ownedLua && launched.value->luaMailbox && !launched.value->luaEntryModules.empty();
    if (handwrittenLua && launched.value->luaHotReload) {
        luaFileReload_ = true;
        SnapshotLuaFileBaseline();
    }
    if (testHotReload && launched.value->luaMailbox && luaTestGenerated_ && !luaTestMapBinding_.empty()) {
        luaReloadController_ = std::make_shared<Services::LuaTestReloadController>(launched.value->luaMailbox,
            std::move(*luaTestGenerated_), std::move(luaTestMapBinding_), std::move(luaTestInstalledBindings_));
        luaTestGenerated_.reset(); luaTestGeneration_ = project_->editRevision_.Generation();
    }
    if (luaRuntimeMailbox_) {
        if (!luaReloadPoll_) {
            luaReloadPoll_ = new QTimer(this); luaReloadPoll_->setInterval(100);
            connect(luaReloadPoll_, &QTimer::timeout, this, &MainWindow::PollLuaTestReload);
        }
        luaReloadPoll_->start();
    }
    // 游戏已启动，准备阶段到此结束：先解除准备锁再刷新按钮，否则“测试”“热重载”会一直灰着。
    project_->savingMap_ = false;
    UpdateExportAvailability();
    const auto luaNote = handwrittenLua ? (luaFileReload_ ? tr(" · 自带 Lua 环境，可热重载") : tr(" · 自带 Lua 环境"))
        : !launched.value->luaEntryModules.empty() ? tr(" · YDWE Lua 引擎") : QString{};
    ShowStatusMessage((request.optimizeSlk ? tr("已启动地图测试 · SLK 测试副本 · 原图与当前编辑保留")
        : tr("已启动地图测试 · 独立副本 · 原图与当前编辑保留")) + luaNote);
    auto* cleanup = new QTimer(this);
    cleanup->setInterval(2000);
    const auto lease = mapTestLease_;
    const auto files = *workspace.value;
    connect(cleanup, &QTimer::timeout, this, [this, cleanup, lease, files] {
        if (lease && lease->IsActive()) return;
        cleanup->stop();
        // 清理绑定创建时的目录/文件身份，新增或变化的内容保留，不按路径递归删除。
        // 大地图的身份/SHA 核验不能在 GUI 线程执行；关闭窗口时工作线程仍只持有自己的租约。
        const auto result = std::make_shared<War3Map::Status>();
        auto* worker = QThread::create([files, lease, result] {
            try {
                *result = lease ? lease->Cleanup() : War3Map::Status::Ok();
                const bool unknownTree = std::any_of(result->diagnostics.begin(), result->diagnostics.end(), [](const auto& diagnostic) {
                    return diagnostic.code == "MAP_TEST_LIFETIME_UNCONFIRMED";
                });
                if (!unknownTree) {
                    const auto ownFiles = files->Cleanup(true);
                    result->diagnostics.insert(result->diagnostics.end(), ownFiles.diagnostics.begin(), ownFiles.diagnostics.end());
                }
            } catch (...) {
                *result = War3Map::Status::Failure("Map.Test", "MAP_TEST_CLEANUP_EXCEPTION",
                    "测试副本清理未能完成，未确认的文件已保留。");
            }
        });
        connect(worker, &QThread::finished, this, [this, result] {
            project_->AddDiagnostics(result->diagnostics);
            for (const auto& diagnostic : result->diagnostics) {
                if (diagnostic.code == "MAP_TEST_PROCESS_EXIT") {
                    QMessageBox::warning(this, tr("地图测试已退出"), QString::fromStdString(diagnostic.message));
                    break;
                }
            }
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        if (mapTestLease_ == lease) {
            // Starting时尚无game PID；启动失败也须停止本局轮询，不能留下永久初始化提示。
            if (luaRuntimeMailbox_) PollLuaTestReload();
            if (luaReloadPoll_) luaReloadPoll_->stop();
            luaRuntimeMailbox_.reset(); luaReloadController_.reset();
            luaFileReload_ = false; luaFileQueue_.clear();
            mapTestLease_.reset();
            if (luaReloadAction_) luaReloadAction_->setEnabled(false);
        }
        // 末次诊断查询先完成，随后才允许清理持有生命周期锁核验大图。
        // 否则后台清理先获锁时，GUI上的邮箱归属查询会等待整图SHA。
        worker->start(QThread::LowPriority);
        cleanup->deleteLater();
    });
    cleanup->start();
}

