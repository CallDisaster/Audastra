#include "MainWindow.h"
#include "MapSaveJob.h"
#include "TriggerModule.h"
#include "War3MapCore/Script/MapTestToolchain.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Services/MapTestWorkspace.h"

#include <QAction>
#include <QEventLoop>
#include <QJsonObject>
#include <QProgressDialog>
#include <QScopedValueRollback>
#include <QThread>
#include <QTimer>

#include <limits>

namespace {
namespace Services = War3Map::Services;
QJsonObject Failure(std::string code, std::string message) {
    return {{"ok", false}, {"code", QString::fromStdString(code)}, {"message", QString::fromStdString(message)}};
}
QJsonObject Failure(const std::vector<War3Map::Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return Failure("LUA_RELOAD_INCOMPLETE", "热重载未完成，基线保留。");
    const auto& last = diagnostics.back(); return Failure(last.code, last.message);
}
QJsonObject Snapshot(const Services::LuaReloadControllerSnapshot& snapshot) {
    constexpr const char* names[]{"ready", "no_changes", "pending", "succeeded", "failed", "restart_required", "stopped"};
    QJsonObject result{{"ok", true}, {"state", names[static_cast<unsigned>(snapshot.outcome)]},
        {"sequence", QString::number(snapshot.sequence)}, {"message", QString::fromStdString(snapshot.message)},
        {"baseline_script_sha256", QString::fromStdString(snapshot.baselineScriptSha256)}, {"seconds", snapshot.seconds}};
    if (snapshot.location) result.insert("location", QJsonObject{{"trigger_id", QString::number(snapshot.location->triggerId)},
        {"node_id", QString::number(snapshot.location->nodeId)}, {"source_line", QString::number(snapshot.location->sourceLine)}});
    result.insert("runtime_diagnostic_sequence", QString::number(snapshot.runtimeDiagnosticSequence));
    result.insert("runtime_diagnostic_code", static_cast<int>(snapshot.runtimeDiagnosticCode));
    result.insert("runtime_message", QString::fromStdString(snapshot.runtimeMessage));
    result.insert("runtime_diagnostic_truncated", snapshot.runtimeDiagnosticTruncated);
    return result;
}
}

War3Map::Status MainWindow::RunLuaTestPreflight(War3Map::QtProject::MapSaveJob& task, const QString& label,
    const std::function<void()>& thenInWorker) {
    task.testCopyOnly = true; task.script->luaTestOptions.testHotReload = true;
    QScopedValueRollback<bool> busy(project_->savingMap_, true);
    QProgressDialog progress(label, QString(), 0, 0, this);
    progress.setWindowTitle(tr("Lua 测试")); progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(0);
    auto status = War3Map::Status::Ok();
    const QPointer<MainWindow> window(this);
    auto worker = std::unique_ptr<QThread>(QThread::create([&task, &status, &thenInWorker] {
        try {
            // PrepareMapPreflight 刚创建的独占空目录；先持有原句柄，成功后只清理本次拥有的文件。
            const auto workspace = Services::FreezeMapTestWorkspace(task.scriptWorkspace);
            if (!workspace.Succeeded() || !workspace.value) { status.diagnostics = workspace.diagnostics; return; }
            const auto checked = Services::MapSaveService::Preflight(task);
            if (!checked.Succeeded()) { status.diagnostics = checked.diagnostics; return; }
            const auto captured = (*workspace.value)->Refresh();
            task.diagnostics.insert(task.diagnostics.end(), captured.diagnostics.begin(), captured.diagnostics.end());
            if (captured.Succeeded()) {
                const auto cleanup = (*workspace.value)->Cleanup(true);
                task.diagnostics.insert(task.diagnostics.end(), cleanup.diagnostics.begin(), cleanup.diagnostics.end());
            }
            if (!task.generatedLuaTestUnit || task.luaTestMapBinding.empty()) {
                status = War3Map::Status::Failure("Lua.Test", "LUA_TEST_GENERATION_MISSING", "没有本次实际生成的 Lua 测试单元，代码没有发送。");
                return;
            }
            if (thenInWorker) thenInWorker();
        } catch (...) {
            status = War3Map::Status::Failure("Lua.Test", "LUA_TEST_PREPARE_FAILED", "测试代码准备未完成，没有发送。");
        }
    }));
    QEventLoop loop; connect(worker.get(), &QThread::finished, &loop, &QEventLoop::quit);
    worker->start(QThread::LowPriority); progress.show(); loop.exec(); worker->wait();
    if (!window) return War3Map::Status::Failure("Lua.Test", "LUA_TEST_WINDOW_CLOSED", "编辑器窗口已关闭。");
    progress.hide();
    project_->AddDiagnostics(task.diagnostics);
    return status;
}

bool MainWindow::LuaTestSessionCurrent() const {
    if (luaFileReload_) return !luaTestSource_.empty() && luaTestSource_ == project_->sourceArchivePath_;
    return luaTestGeneration_ == project_->editRevision_.Generation();
}

QJsonObject MainWindow::LuaTestReload(bool submit) {
    if (!LuaTestSessionCurrent())
        return Failure("LUA_RELOAD_NO_SESSION", "请先在当前地图启动启用热重载的 Lua 单人测试。");
    if (luaFileReload_) {
        if (submit && project_->savingMap_) return Failure("LUA_RELOAD_BUSY", "保存或测试准备正在进行，请稍后重试。");
        const auto result = LuaFileReload(submit);
        if (submit) {
            ShowStatusMessage(result.value("message").toString());
            if (luaReloadPoll_ && result.value("state").toString() == "pending") luaReloadPoll_->start();
        }
        return result;
    }
    if (!luaReloadController_) {
        if (submit || !luaRuntimeMailbox_)
            return Failure("LUA_RELOAD_NO_SESSION", "请先在当前地图启动启用热重载的 Lua 单人测试。");
        // 关闭热重载只关闭差异发送；地图入口和异步回调错误仍必须回传。
        const auto result = Services::QueryLuaTestReload(*luaRuntimeMailbox_);
        if (!result.Succeeded() || !result.value) return Failure(result.diagnostics);
        Services::LuaReloadControllerSnapshot snapshot;
        const bool starting = result.value->state == Services::LuaTestReloadState::Starting;
        snapshot.outcome = starting || (result.value->processActive && result.value->state != Services::LuaTestReloadState::Stopped)
            ? Services::LuaReloadOutcome::Ready : Services::LuaReloadOutcome::Stopped;
        snapshot.message = snapshot.outcome == Services::LuaReloadOutcome::Stopped
            ? "本次测试游戏已退出。" : starting ? "测试运行时正在初始化。" : "本次Lua测试未开启热重载。";
        snapshot.runtimeDiagnosticSequence = result.value->runtimeDiagnosticSequence;
        snapshot.runtimeDiagnosticCode = result.value->runtimeDiagnosticCode;
        snapshot.runtimeMessage = result.value->runtimeMessage;
        snapshot.runtimeDiagnosticTruncated = result.value->runtimeDiagnosticTruncated;
        return Snapshot(snapshot);
    }
    if (!submit) {
        const auto result = luaReloadController_->Poll();
        return result.Succeeded() && result.value ? Snapshot(*result.value) : Failure(result.diagnostics);
    }
    if (project_->savingMap_) return Failure("LUA_RELOAD_BUSY", "保存或测试准备正在进行，请稍后重试。");
    if (!triggersModule_->CommitPendingEdits()) return Failure("LUA_RELOAD_DRAFT_INVALID", "代码草稿未能提交，差异没有发送。");
    const auto current = luaReloadController_->Poll();
    if (!current.Succeeded() || !current.value) return Failure(current.diagnostics);
    if (current.value->outcome == Services::LuaReloadOutcome::Pending)
        return Failure("LUA_RELOAD_PENDING", "上一份差异尚未收到游戏确认。");
    if (current.value->outcome == Services::LuaReloadOutcome::Stopped
        || current.value->outcome == Services::LuaReloadOutcome::RestartRequired)
        return Failure("LUA_RELOAD_RESTART_REQUIRED", current.value->message);
    const std::array<std::uint64_t, 2> submittedRevision{
        project_->editRevision_.Generation(), project_->editRevision_.Revision()};
    auto task = project_->PrepareMapPreflight();
    if (!task || !task->script || !task->script->luaTarget)
        return Failure("LUA_RELOAD_PROFILE_REQUIRED", "当前 Lua 保存档案不可用，差异没有发送。");
    War3Map::Result<Services::LuaReloadControllerSnapshot> result;
    const auto controller = luaReloadController_;
    const QPointer<MainWindow> window(this);
    const auto prepared = RunLuaTestPreflight(*task, tr("正在检查触发器差异…"), [&task, &result, controller] {
        result = controller->Submit(std::move(*task->generatedLuaTestUnit), task->luaTestMapBinding);
    });
    if (!window) return Failure("LUA_RELOAD_WINDOW_CLOSED", "编辑器窗口已关闭。");
    if (!prepared.Succeeded()) result.diagnostics = prepared.diagnostics;
    project_->AddDiagnostics(result.diagnostics);
    // 预检没有地图发布点，不把差异准备标记为正式保存；失败目录保留诊断。
    if (!result.Succeeded() || !result.value) {
        const auto failure = Failure(result.diagnostics); ShowStatusMessage(failure.value("message").toString()); return failure;
    }
    ShowStatusMessage(QString::fromStdString(result.value->message));
    if (result.value->outcome == Services::LuaReloadOutcome::Pending) {
        luaReloadSubmittedRevision_ = submittedRevision;
        if (!luaReloadPoll_) {
            luaReloadPoll_ = new QTimer(this); luaReloadPoll_->setInterval(100);
            connect(luaReloadPoll_, &QTimer::timeout, this, &MainWindow::PollLuaTestReload);
        }
        luaReloadPoll_->start();
    }
    return Snapshot(*result.value);
}

void MainWindow::PollLuaTestReload() {
    const auto lease = mapTestLease_;
    const auto result = LuaTestReload(false);
    // 游戏可能先发布就绪、Job观察器后捕获原句柄。只等待本局归属确认，
    // 不读取未确认回执、不放行发送；实际终态仍交同一租约的清理流程报告。
    if (!result.value("ok").toBool() && result.value("code").toString() == QLatin1String("LUA_TEST_PROCESS_UNCONFIRMED")
        && lease && mapTestLease_ == lease && LuaTestSessionCurrent()
        && lease->IsActive()) return;
    if (!result.value("ok").toBool() || result.value("state").toString() == "stopped") {
        if (luaReloadPoll_) luaReloadPoll_->stop();
    }
    const auto diagnosticSequence = result.value("runtime_diagnostic_sequence").toString().toUInt();
    const bool freshDiagnostic = diagnosticSequence && diagnosticSequence != luaRuntimeDiagnosticSequence_;
    if (freshDiagnostic) {
        luaRuntimeDiagnosticSequence_ = diagnosticSequence;
        const auto message = result.value("runtime_message").toString();
        // 运行时转发的普通打印（结果码为成功）只作信息，不算错误。
        const bool printed = result.value("runtime_diagnostic_code").toInt() == static_cast<int>(Audastra::Runtime32::LuaReloadResult::Success);
        project_->AddDiagnostics({War3Map::Diagnostic{printed ? War3Map::DiagnosticSeverity::Info : War3Map::DiagnosticSeverity::Error,
            "Lua.Test", printed ? "LUA_TEST_OUTPUT" : "LUA_TEST_RUNTIME_ERROR", message.toStdString()}});
        ShowStatusMessage(message);
    }
    // 一局持续查询回调异常；相同ACK/等待提示不反复刷状态栏或跳转代码。
    const auto status = result.value("state").toString() + ":" + result.value("sequence").toString()
        + ":" + result.value("message").toString();
    if (status == luaReloadLastStatus_) return;
    luaReloadLastStatus_ = status;
    if (result.value("state").toString() == "pending") return;
    if (!freshDiagnostic) {
        const auto seconds = result.value("seconds").toDouble();
        ShowStatusMessage(seconds > 0 ? tr("%1 · %2 秒").arg(result.value("message").toString()).arg(seconds, 0, 'f', 2)
            : result.value("message").toString());
    }
    const auto location = result.value("location").toObject();
    bool valid = false; const auto trigger = location.value("trigger_id").toString().toULongLong(&valid);
    if (valid && trigger) {
        // 回执位置属于已发送正文；只在当前触发器模块定位，不切换模块提交其它草稿。
        if (CurrentModule() != "triggers" || !luaReloadSubmittedRevision_
            || !project_->editTrackingReady_ || project_->editTrackingFailed_
            || triggersModule_->HasPendingNavigationDraft()) return;
        const auto& identity = *luaReloadSubmittedRevision_;
        if (project_->editRevision_.Generation() != identity[0]
            || project_->editRevision_.Revision() != identity[1]) return;
        bool lineValid = false;
        const auto line = location.value("source_line").toString().toULongLong(&lineValid);
        if (lineValid && line) {
            if (line > static_cast<qulonglong>((std::numeric_limits<int>::max)())) return;
            Navigate({"triggers", "runtime-code", QString::number(trigger),
                {{"generation", QString::number(identity[0])}, {"revision", QString::number(identity[1])},
                    {"line", static_cast<int>(line)}}});
            return;
        }
        const auto node = location.value("node_id").toString();
        Navigate({"triggers", node.isEmpty() || node == "0" || node == QString::number(trigger) ? "trigger" : "eca",
            node.isEmpty() || node == "0" || node == QString::number(trigger) ? QString::number(trigger) : node,
            {{"stableIdentity", true}}});
    }
}
