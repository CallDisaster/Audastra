#include "MainWindow.h"
#include "MapSaveJob.h"
#include "TriggerModule.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Services/MapTestWorkspace.h"

#include <QAction>
#include <QEventLoop>
#include <QJsonObject>
#include <QProgressDialog>
#include <QScopedValueRollback>
#include <QThread>
#include <QTimer>

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

QJsonObject MainWindow::LuaTestReload(bool submit) {
    if (!luaReloadController_ || luaTestGeneration_ != project_->editRevision_.Generation())
        return Failure("LUA_RELOAD_NO_SESSION", "请先在当前地图启动启用热重载的 Lua 单人测试。");
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
    auto task = project_->PrepareMapPreflight();
    if (!task || !task->script || !task->script->luaTarget)
        return Failure("LUA_RELOAD_PROFILE_REQUIRED", "当前 Lua 保存档案不可用，差异没有发送。");
    task->testCopyOnly = true; task->script->luaTestOptions.testHotReload = true;
    QScopedValueRollback<bool> busy(project_->savingMap_, true);
    QProgressDialog progress(tr("正在检查触发器差异…"), QString(), 0, 0, this);
    progress.setWindowTitle(tr("Lua 热重载")); progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(0);
    War3Map::Result<Services::LuaReloadControllerSnapshot> result;
    const auto controller = luaReloadController_;
    const QPointer<MainWindow> window(this);
    auto worker = std::unique_ptr<QThread>(QThread::create([&task, &result, controller] {
        try {
            // PrepareMapPreflight 刚创建的独占空目录；先持有原句柄，成功后只清理本次拥有的文件。
            const auto workspace = Services::FreezeMapTestWorkspace(task->scriptWorkspace);
            if (!workspace.Succeeded() || !workspace.value) {
                result.diagnostics = workspace.diagnostics; return;
            }
            const auto checked = Services::MapSaveService::Preflight(*task);
            if (!checked.Succeeded()) { result.diagnostics = checked.diagnostics; return; }
            const auto captured = (*workspace.value)->Refresh();
            task->diagnostics.insert(task->diagnostics.end(), captured.diagnostics.begin(), captured.diagnostics.end());
            if (captured.Succeeded()) {
                const auto cleanup = (*workspace.value)->Cleanup(true);
                task->diagnostics.insert(task->diagnostics.end(), cleanup.diagnostics.begin(), cleanup.diagnostics.end());
            }
            if (!task->generatedLuaTestUnit || task->luaTestMapBinding.empty()) {
                result = War3Map::Result<Services::LuaReloadControllerSnapshot>::Failure("Lua.Test", "LUA_RELOAD_GENERATION_MISSING",
                    "没有本次实际生成的 Lua 测试单元，差异没有发送。"); return;
            }
            result = controller->Submit(std::move(*task->generatedLuaTestUnit), task->luaTestMapBinding);
        } catch (...) {
            result = War3Map::Result<Services::LuaReloadControllerSnapshot>::Failure("Lua.Test", "LUA_RELOAD_PREPARE_FAILED",
                "差异准备未完成，基线保留。");
        }
    }));
    QEventLoop loop; connect(worker.get(), &QThread::finished, &loop, &QEventLoop::quit);
    worker->start(QThread::LowPriority); progress.show(); loop.exec(); worker->wait(); progress.hide();
    if (!window) return Failure("LUA_RELOAD_WINDOW_CLOSED", "编辑器窗口已关闭。");
    project_->AddDiagnostics(task->diagnostics); project_->AddDiagnostics(result.diagnostics);
    // 预检没有地图发布点，不把差异准备标记为正式保存；失败目录保留诊断。
    if (!result.Succeeded() || !result.value) {
        const auto failure = Failure(result.diagnostics); ShowStatusMessage(failure.value("message").toString()); return failure;
    }
    ShowStatusMessage(QString::fromStdString(result.value->message));
    if (result.value->outcome == Services::LuaReloadOutcome::Pending) {
        if (!luaReloadPoll_) {
            luaReloadPoll_ = new QTimer(this); luaReloadPoll_->setInterval(100);
            connect(luaReloadPoll_, &QTimer::timeout, this, &MainWindow::PollLuaTestReload);
        }
        luaReloadPoll_->start();
    }
    return Snapshot(*result.value);
}

void MainWindow::PollLuaTestReload() {
    const auto result = LuaTestReload(false);
    if (!result.value("ok").toBool() || result.value("state").toString() == "stopped") {
        if (luaReloadPoll_) luaReloadPoll_->stop();
    }
    const auto diagnosticSequence = result.value("runtime_diagnostic_sequence").toString().toUInt();
    const bool freshDiagnostic = diagnosticSequence && diagnosticSequence != luaRuntimeDiagnosticSequence_;
    if (freshDiagnostic) {
        luaRuntimeDiagnosticSequence_ = diagnosticSequence;
        const auto message = result.value("runtime_message").toString();
        project_->AddDiagnostics({War3Map::Diagnostic{War3Map::DiagnosticSeverity::Error, "Lua.Test",
            "LUA_TEST_RUNTIME_ERROR", message.toStdString()}});
        ShowStatusMessage(message);
    }
    // 一局持续查询回调异常；相同ACK/等待提示不反复刷状态栏或跳转代码。
    const auto status = result.value("state").toString() + ":" + result.value("sequence").toString()
        + ":" + result.value("message").toString();
    if (status == luaReloadLastStatus_) return;
    luaReloadLastStatus_ = status;
    if (result.value("state").toString() == "pending") return;
    if (!freshDiagnostic)
        ShowStatusMessage(tr("%1 · %2 秒").arg(result.value("message").toString()).arg(result.value("seconds").toDouble(), 0, 'f', 2));
    const auto location = result.value("location").toObject();
    bool valid = false; const auto trigger = location.value("trigger_id").toString().toULongLong(&valid);
    if (valid && trigger) {
        const auto node = location.value("node_id").toString();
        Navigate({"triggers", node.isEmpty() || node == "0" || node == QString::number(trigger) ? "trigger" : "eca",
            node.isEmpty() || node == "0" || node == QString::number(trigger) ? QString::number(trigger) : node,
            {{"stableIdentity", true}}});
    }
}
