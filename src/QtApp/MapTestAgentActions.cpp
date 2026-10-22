#include "MainWindow.h"
#include "MapSaveJob.h"
#include "War3MapCore/Script/MapTestToolchain.h"
#include "War3MapCore/Services/LuaTestEval.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include <algorithm>
#include <optional>

// Agent 进游戏测试：map_test_start / map_test_stop / map_test_eval 的宿主实现。
// 测试步骤只接受紧凑 T，经与热重载相同的生成链变成 Lua 后送进本局测试游戏。
namespace {
namespace Services = War3Map::Services;
using Runtime32Result = Audastra::Runtime32::LuaReloadResult;
QJsonObject Failure(const char* code, const QString& message) {
    return {{"ok", false}, {"code", QString::fromLatin1(code)}, {"message", message}};
}
QJsonObject Failure(const std::vector<War3Map::Diagnostic>& diagnostics, const char* fallback) {
    QJsonArray rows;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity != War3Map::DiagnosticSeverity::Error || rows.size() >= 32) continue;
        rows.append(QJsonObject{{"code", QString::fromStdString(diagnostic.code)}, {"message", QString::fromStdString(diagnostic.message)}});
    }
    if (rows.isEmpty()) return Failure(fallback, QObject::tr("测试步骤没有执行。"));
    auto result = rows.first().toObject();
    result.insert("ok", false); result.insert("diagnostics", rows);
    return result;
}
const char* ResultName(Runtime32Result code) {
    constexpr const char* names[]{"success", "syntax_error", "runtime_error", "budget_exceeded", "invalid_request",
        "identity_mismatch", "not_ready", "cancelled", "host_failure"};
    const auto index = static_cast<unsigned>(code);
    return index < std::size(names) ? names[index] : "unknown";
}
// 在界面线程等待，期间照常处理事件（游戏是独立进程）。
void Pause(int milliseconds) {
    QEventLoop loop; QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit); loop.exec();
}
}

QJsonObject MainWindow::MapTestTool(const QString& name, const QJsonObject& arguments) {
    if (name == QLatin1String("map_test_start")) {
        const auto rejected = StartMapTest(false);
        if (!rejected.isEmpty()) return Failure("MAP_TEST_REJECTED", rejected);
        return {{"ok", true}, {"started", true}, {"game_verified", false},
            {"steps_supported", static_cast<bool>(luaReloadController_)}};
    }
    if (name == QLatin1String("map_test_stop")) {
        if (!mapTestLease_ || !mapTestLease_->IsActive())
            return {{"ok", true}, {"stopped", false}, {"message", tr("没有正在运行的测试游戏。")}};
        const auto stopped = mapTestLease_->Stop();
        if (!stopped.Succeeded()) return Failure(stopped.diagnostics, "MAP_TEST_STOP_FAILED");
        return {{"ok", true}, {"stopped", true}};
    }
    return MapTestSteps(arguments);
}

QJsonObject MainWindow::MapTestSteps(const QJsonObject& arguments) {
    if (!mapTestLease_ || !mapTestLease_->IsActive() || !luaRuntimeMailbox_)
        return Failure("MAP_TEST_NOT_RUNNING", tr("请先用 map_test_start 启动本图测试。"));
    if (!project_->luaMapTarget_ || !luaReloadController_ || !LuaTestSessionCurrent())
        return Failure("MAP_TEST_STEPS_UNSUPPORTED", tr("测试步骤目前只支持脚本语言为 Lua、开启热重载的本图测试；JASS 地图还不能在游戏里执行步骤。"));
    if (project_->savingMap_) return Failure("MAP_TEST_BUSY", tr("保存或测试准备正在进行，请稍后再试。"));
    const int wait = std::clamp(arguments.value("wait_ms").toInt(10000), 0, 60000);
    QElapsedTimer clock; clock.start();
    // 游戏载入期间运行时还不接收代码；在等待时间内等它就绪。
    for (;;) {
        const auto current = Services::QueryLuaTestReload(*luaRuntimeMailbox_);
        if (!current.Succeeded() || !current.value) return Failure(current.diagnostics, "MAP_TEST_STEPS_FAILED");
        if (current.value->state == Services::LuaTestReloadState::Stopped || !current.value->processActive)
            return Failure("MAP_TEST_NOT_RUNNING", tr("本局测试游戏已退出。"));
        if (current.value->state == Services::LuaTestReloadState::Idle
            || current.value->state == Services::LuaTestReloadState::Complete) break;
        if (current.value->state != Services::LuaTestReloadState::Starting)
            return Failure("MAP_TEST_BUSY", tr("上一段代码还没有执行完。"));
        if (clock.elapsed() >= wait) return Failure("MAP_TEST_NOT_READY", tr("游戏还在载入，测试运行时尚未就绪；稍后再试或加大 wait_ms。"));
        Pause(100);
    }
    Services::LuaTestEvalRequest request;
    request.actions = arguments.value("actions").toString().toStdString();
    for (const auto& read : arguments.value("reads").toArray()) request.reads.push_back(read.toString().toStdString());

    auto task = project_->PrepareMapPreflight();
    if (!task || !task->script || !task->script->luaTarget || !task->script->triggers || !task->script->catalog)
        return Failure("MAP_TEST_STEPS_PROFILE", tr("当前 Lua 保存档案不可用，测试步骤没有发送。"));
    const auto mapObjects = project_->CaptureTriggerMapObjectReferences();
    const auto plan = Services::AddLuaTestEvalTrigger(*task->script->triggers, *task->script->catalog, request,
        mapObjects.Succeeded() && mapObjects.value ? mapObjects.value->get() : nullptr);
    if (!plan.Succeeded() || !plan.value) return Failure(plan.diagnostics, "MAP_TEST_STEPS_INVALID");
    const auto generated = RunLuaTestPreflight(*task, tr("正在生成测试步骤…"));
    if (!generated.Succeeded()) return Failure(generated.diagnostics, "MAP_TEST_STEPS_GENERATION");
    const auto chunk = Services::BuildLuaTestEvalChunk(*task->generatedLuaTestUnit, *plan.value);
    if (!chunk.Succeeded() || !chunk.value) return Failure(chunk.diagnostics, "MAP_TEST_STEPS_GENERATION");

    const auto before = Services::QueryLuaTestReload(*luaRuntimeMailbox_);
    const auto diagnosticBefore = before.Succeeded() && before.value ? before.value->runtimeDiagnosticSequence : 0;
    const auto submitted = Services::SubmitLuaTestReload(*luaRuntimeMailbox_, {"audastra-agent-steps", *chunk.value});
    if (!submitted.Succeeded() || !submitted.value) return Failure(submitted.diagnostics, "MAP_TEST_STEPS_SUBMIT");
    const auto sequence = *submitted.value;
    std::optional<Services::LuaTestReloadSnapshot> done;
    for (;;) {
        const auto polled = Services::QueryLuaTestReload(*luaRuntimeMailbox_, sequence);
        if (!polled.Succeeded() || !polled.value) return Failure(polled.diagnostics, "MAP_TEST_STEPS_FAILED");
        if (polled.value->state == Services::LuaTestReloadState::Complete && polled.value->sequence == sequence) { done = *polled.value; break; }
        if (polled.value->state == Services::LuaTestReloadState::Stopped)
            return Failure("MAP_TEST_NOT_RUNNING", tr("测试游戏在执行步骤时退出。"));
        if (clock.elapsed() >= wait) {
            // 只撤回尚未开始执行的请求；已开始的副作用未知，如实说明。
            const bool cancelled = Services::CancelLuaTestReload(*luaRuntimeMailbox_, sequence).Succeeded();
            return Failure("MAP_TEST_STEPS_TIMEOUT", cancelled ? tr("等待超时，步骤尚未执行，已撤回。")
                : tr("等待超时；步骤可能正在执行或已执行，结果未知。"));
        }
        Pause(20);
    }
    // 回执后运行时再转发本次打印；稍等它到达。
    auto output = *done;
    for (int tries = 0; tries < 25 && output.runtimeDiagnosticSequence == diagnosticBefore; ++tries) {
        Pause(20);
        if (const auto polled = Services::QueryLuaTestReload(*luaRuntimeMailbox_); polled.Succeeded() && polled.value) output = *polled.value;
    }
    QJsonArray reads, lines;
    const auto marker = QString::fromUtf8(Services::kLuaTestEvalReadMarker.data(), static_cast<int>(Services::kLuaTestEvalReadMarker.size()));
    if (output.runtimeDiagnosticSequence != diagnosticBefore) {
        for (const auto& line : QString::fromStdString(output.runtimeMessage).split('\n', Qt::SkipEmptyParts)) {
            if (!line.startsWith(marker)) { lines.append(line); continue; }
            const auto rest = line.mid(marker.size());
            const auto tab = rest.indexOf('\t');
            const auto index = rest.left(tab).toInt();
            if (tab > 0 && index >= 0 && index < static_cast<int>(plan.value->reads.size()))
                reads.append(QJsonObject{{"name", QString::fromStdString(plan.value->reads[static_cast<std::size_t>(index)].first)},
                    {"value", rest.mid(tab + 1)}});
        }
    }
    const bool succeeded = done->resultCode == Runtime32Result::Success;
    // 失败也返回已打印的内容；运行错误时前面的步骤可能已经生效。
    return {{"ok", succeeded}, {"code", succeeded ? QString() : QStringLiteral("MAP_TEST_STEPS_RUNTIME")}, {"result", ResultName(done->resultCode)}, {"message", QString::fromStdString(done->message)},
        {"reads", reads}, {"output", lines}, {"output_truncated", output.runtimeDiagnosticTruncated},
        {"seconds", static_cast<double>(clock.elapsed()) / 1000.0}};
}
