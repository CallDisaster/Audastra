#include "MainWindow.h"
#include "MapSaveJob.h"
#include "WindowFrameCapture.h"
#include "War3MapCore/Script/MapTestToolchain.h"
#include "War3MapCore/Services/LuaTestEval.h"
#include "War3MapCore/Services/MapTestScenario.h"
#include "../QtCommon/ApplicationPaths.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>
#include <QUuid>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <future>
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
// 本次进程树里客户区最大的可见顶层窗口，即游戏窗口；后台测试在它的不可见桌面上找。
HWND GameWindow(const War3Map::Script::MapTestLaunchLease& lease) {
    const auto processes = lease.ProcessIds();
    struct Search { const std::vector<std::uint32_t>* processes; HWND best = nullptr; LONG area = 0; } search{&processes};
    const auto visit = [](HWND window, LPARAM parameter) -> BOOL {
        auto& found = *reinterpret_cast<Search*>(parameter);
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        if (!IsWindowVisible(window) || IsIconic(window)
            || std::find(found.processes->begin(), found.processes->end(), process) == found.processes->end()) return TRUE;
        RECT client{};
        if (!GetClientRect(window, &client)) return TRUE;
        const auto area = (client.right - client.left) * (client.bottom - client.top);
        if (area > found.area) { found.area = area; found.best = window; }
        return TRUE;
    };
    if (const auto desktop = static_cast<HDESK>(lease.HiddenDesktop())) EnumDesktopWindows(desktop, visit, reinterpret_cast<LPARAM>(&search));
    else EnumWindows(visit, reinterpret_cast<LPARAM>(&search));
    return search.best;
}
// 内部抽样全是同一颜色时视为没有取到画面：游戏还没绘制（显示器关闭时游戏不绘制）或绘制方式不受支持，
// 都会交回纯白或纯黑。避开边缘，系统圆角处的像素不算。
bool Uniform(const QImage& image) {
    if (image.isNull()) return true;
    const int margin = std::min({16, image.width() / 4, image.height() / 4});
    const auto first = image.pixel(margin, margin);
    for (int y = margin; y < image.height() - margin; y += 8)
        for (int x = margin; x < image.width() - margin; x += 8)
            if (image.pixel(x, y) != first) return false;
    return true;
}
// 客户区中心与四角内侧都直接露在屏幕上，才能按屏幕位置拷贝。
bool Unoccluded(HWND window, const RECT& client) {
    for (const auto [x, y] : {std::pair{client.right / 2, client.bottom / 2}, std::pair{4L, 4L}, std::pair{client.right - 5, 4L},
             std::pair{4L, client.bottom - 5}, std::pair{client.right - 5, client.bottom - 5}}) {
        POINT point{x, y};
        ClientToScreen(window, &point);
        if (GetAncestor(WindowFromPoint(point), GA_ROOT) != window) return false;
    }
    return true;
}
struct GameCapture { QImage image; QString method; QStringList problems; };
// 依次用系统图形捕获、窗口自绘、屏幕拷贝（仅窗口未被遮挡时）取一帧；都取不到时如实交回各自的原因。
// 后台测试的窗口不在屏幕上，不做屏幕拷贝。
GameCapture CaptureWindow(HWND window, bool hidden) {
    GameCapture capture;
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) {
        capture.problems << QObject::tr("窗口客户区为空");
        return capture;
    }
    auto pending = std::async(std::launch::async, [window] {
        std::string error;
        auto frame = War3Map::QtApp::CaptureWindowFrame(window, 3000, error);
        return std::pair{std::move(frame), std::move(error)};
    });
    while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) Pause(15);
    const auto [frame, error] = pending.get();
    if (frame.width > 0) {
        const QImage image(frame.bgra.data(), frame.width, frame.height, frame.width * 4, QImage::Format_RGB32);
        capture.image = image.convertToFormat(QImage::Format_RGB888);
        if (!Uniform(capture.image)) { capture.method = QStringLiteral("graphics_capture"); return capture; }
        capture.problems << QObject::tr("图形捕获：画面是单一颜色，游戏可能还没绘制（显示器关闭或锁屏时游戏不绘制）");
    } else {
        capture.problems << QObject::tr("图形捕获：%1").arg(QString::fromStdString(error));
    }

    const int width = client.right, height = client.bottom;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    const auto previous = SelectObject(memory, bitmap);
    const auto grab = [&] {
        SelectObject(memory, previous);
        capture.image = QImage::fromHBITMAP(bitmap).convertToFormat(QImage::Format_RGB888);
        SelectObject(memory, bitmap);
    };
    capture.image = {};
    if (PrintWindow(window, memory, PW_CLIENTONLY | PW_RENDERFULLCONTENT)) grab();
    if (!Uniform(capture.image)) {
        capture.method = QStringLiteral("print_window");
    } else {
        capture.problems << QObject::tr("窗口自绘：画面是单一颜色");
        capture.image = {};
        POINT origin{0, 0}; ClientToScreen(window, &origin);
        if (hidden) capture.problems << QObject::tr("后台测试的游戏不在屏幕上，要看画面请用 visible: true 重新启动测试");
        else if (!Unoccluded(window, client)) capture.problems << QObject::tr("屏幕拷贝：游戏窗口被其他窗口遮挡");
        else if (BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY)) grab();
        if (!capture.image.isNull() && !Uniform(capture.image)) capture.method = QStringLiteral("screen_copy");
        else capture.image = {};
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen);
    return capture;
}
}

QJsonObject MainWindow::MapTestTool(const QString& name, const QJsonObject& arguments) {
    if (name == QLatin1String("map_test_start")) {
        pendingMapTestScenario_.reset();
        if (arguments.contains("scenario")) {
            if (project_->luaMapTarget_)
                return Failure("MAP_TEST_SCENARIO_LUA", tr("Lua 地图请在启动后用 map_test_eval 执行步骤；测试场景用于 JASS 地图。"));
            Services::MapTestScenarioRequest request;
            request.actions = arguments.value("scenario").toString().toStdString();
            for (const auto& read : arguments.value("reads").toArray()) request.reads.push_back(read.toString().toStdString());
            request.startSeconds = arguments.value("start_seconds").toDouble(1.0);
            request.resultFile = "Maps\\Audastra\\scenario-"
                + QUuid::createUuid().toString(QUuid::Id128).left(16).toStdString() + ".txt";
            pendingMapTestScenario_ = std::move(request);
        }
        // 默认开在看不见的独立桌面上，不打扰正在用电脑的人；visible 才在屏幕上显示（要截画面时用）。
        const bool visible = arguments.value("visible").toBool(false);
        const auto rejected = StartMapTest(false, !visible);
        pendingMapTestScenario_.reset();
        if (!rejected.isEmpty()) return Failure("MAP_TEST_REJECTED", rejected);
        QJsonObject result{{"ok", true}, {"started", true}, {"game_verified", false},
            {"steps_supported", static_cast<bool>(luaReloadController_)}, {"visible", visible}};
        if (mapTestScenario_)
            result.insert("scenario", QJsonObject{{"result_file", QString::fromStdWString(mapTestScenario_->resultPath.wstring())},
                {"reads", static_cast<int>(mapTestScenario_->plan.reads.size())}});
        return result;
    }
    if (name == QLatin1String("map_test_result")) return MapTestScenarioResult(arguments);
    if (name == QLatin1String("map_test_capture")) return MapTestCapture();
    if (name == QLatin1String("map_test_stop")) {
        if (!mapTestLease_ || !mapTestLease_->IsActive())
            return {{"ok", true}, {"stopped", false}, {"message", tr("没有正在运行的测试游戏。")}};
        const auto stopped = mapTestLease_->Stop();
        if (!stopped.Succeeded()) return Failure(stopped.diagnostics, "MAP_TEST_STOP_FAILED");
        return {{"ok", true}, {"stopped", true}};
    }
    return MapTestSteps(arguments);
}

QJsonObject MainWindow::MapTestScenarioResult(const QJsonObject& arguments) {
    if (!mapTestScenario_) return Failure("MAP_TEST_SCENARIO_NONE", tr("本次测试没有带测试场景；请用 map_test_start 的 scenario 启动。"));
    const int wait = std::clamp(arguments.value("wait_ms").toInt(30000), 0, 600000);
    // 等待期间照常处理事件，别的请求可能结束或重开测试：只看开始等待时的这一局。
    const auto scenario = *mapTestScenario_;
    const auto lease = mapTestLease_;
    QElapsedTimer clock; clock.start();
    std::optional<Services::MapTestScenarioReport> report;
    bool running = true;
    qint64 lastKey = -10000;
    for (;;) {
        running = lease && lease->IsActive();
        QFile file(QString::fromStdWString(scenario.resultPath.wstring()));
        if (file.exists() && file.size() <= 4 * 1024 * 1024 && file.open(QIODevice::ReadOnly)) {
            const auto bytes = file.readAll();
            auto parsed = Services::ParseMapTestScenarioResult(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                scenario.plan);
            if (parsed.Succeeded() && parsed.value) report = std::move(*parsed.value);
        }
        // 载入完成后游戏可能停在“按任意键继续”：场景开始前每 2 秒给本局测试游戏发一次空格（载入中会被忽略）。
        // 先发再判断是否返回，wait_ms 为 0 的轮询也能越过这一步。
        if (running && (!report || !report->started) && clock.elapsed() - lastKey >= 2000) {
            lastKey = clock.elapsed();
            if (const auto window = GameWindow(*lease)) {
                PostMessageW(window, WM_KEYDOWN, VK_SPACE, 0x00390001);
                PostMessageW(window, WM_KEYUP, VK_SPACE, static_cast<LPARAM>(0xC0390001u));
            }
        }
        if ((report && report->finished) || !running || clock.elapsed() >= wait) break;
        Pause(250);
    }
    QJsonObject result{{"ok", true}, {"game_running", running},
        {"result_file", QString::fromStdWString(scenario.resultPath.wstring())}};
    if (!report) {
        result.insert("started", false); result.insert("finished", false); result.insert("written", false);
        result.insert("message", running ? tr("场景还没有写出结果；游戏可能仍在载入或场景在等待。") : tr("游戏已退出，场景没有写出结果。"));
        return result;
    }
    QJsonArray lines, reads;
    for (const auto& line : report->lines) { if (lines.size() >= 400) break; lines.append(QString::fromStdString(line)); }
    for (const auto& [read, value] : report->reads)
        reads.append(QJsonObject{{"name", QString::fromStdString(read)}, {"value", QString::fromStdString(value)}});
    result.insert("written", true); result.insert("started", report->started); result.insert("finished", report->finished);
    result.insert("passed", static_cast<int>(report->passed)); result.insert("failed", static_cast<int>(report->failed));
    result.insert("lines", lines); result.insert("reads", reads);
    if (static_cast<int>(report->lines.size()) > lines.size()) result.insert("lines_total", static_cast<int>(report->lines.size()));
    // 游戏在记录期间写入的资源路径只给个数，排查载入问题时可直接打开结果文件。
    if (!report->files.empty()) result.insert("loaded_files", static_cast<int>(report->files.size()));
    return result;
}

QJsonObject MainWindow::MapTestCapture() {
    if (!mapTestLease_ || !mapTestLease_->IsActive()) return Failure("MAP_TEST_NOT_RUNNING", tr("没有正在运行的测试游戏。"));
    const auto window = GameWindow(*mapTestLease_);
    if (!window) return Failure("MAP_TEST_CAPTURE_WINDOW", tr("没有找到本次测试游戏的可见窗口（可能仍在启动或已最小化）。"));
    const auto capture = CaptureWindow(window, mapTestLease_->HiddenDesktop() != nullptr);
    if (capture.image.isNull())
        return Failure("MAP_TEST_CAPTURE_FAILED", tr("游戏窗口画面没有取到：%1。").arg(capture.problems.join(QStringLiteral("；"))));
    const auto& image = capture.image;
    const QDir root(QDir(War3Map::QtCommon::InstallationRoot()).filePath(QStringLiteral("test-captures/") + mapTestRunName_));
    if (!QDir().mkpath(root.absolutePath())) return Failure("MAP_TEST_CAPTURE_FAILED", tr("无法创建截图目录。"));
    const auto path = root.filePath(QStringLiteral("capture-%1.png").arg(++mapTestCaptures_, 3, 10, QLatin1Char('0')));
    if (!image.save(path, "PNG")) return Failure("MAP_TEST_CAPTURE_FAILED", tr("截图没有写出。"));
    return {{"ok", true}, {"path", QDir::toNativeSeparators(path)}, {"width", image.width()}, {"height", image.height()},
        {"method", capture.method}};
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
