#include "MapTestCommand.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Script/MapTestToolchain.h"
#include "War3MapCore/Services/LuaTestMailboxSession.h"
#include "War3MapCore/Services/MapTestWorkspace.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>
#include <stdexcept>

// 命令行地图测试：与编辑器 F9 同一准备、预检、启动与 Lua 邮箱链路，便于脚本与 Agent 验证。
// 只写入本次新建的独占目录和游戏 Maps/Audastra 短路径副本；游戏退出后按租约清理。
namespace {
namespace Services = War3Map::Services;
namespace Script = War3Map::Script;

struct Reload { double after = 0; std::string module; std::filesystem::path file; bool sent = false; std::uint32_t sequence = 0; };
struct Event { double after = 0; Script::MapTestInput input; std::filesystem::path screenshot; bool done = false; };

double Number(const wchar_t* text) {
    std::size_t used = 0; const std::wstring value(text); const double result = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(result) || result < 0 || result > 86400) throw std::invalid_argument("invalid time/coordinate");
    return result;
}

std::string Utf8(const wchar_t* value) {
    const auto text = std::filesystem::path(value).u8string();
    return std::string(text.begin(), text.end());
}
void Print(const std::vector<War3Map::Diagnostic>& diagnostics) {
    for (const auto& d : diagnostics) std::cerr << d.subsystem << '\t' << d.code << '\t' << d.message << '\n';
}
}

int RunMapTest(int argc, wchar_t** argv) {
    if (argc < 5) return 2;
    const std::filesystem::path map = argv[2], game = argv[3], helpers = argv[4];
    std::filesystem::path ydwe;
    double seconds = 60;
    double exitTimeout = 120;
    auto engine = Services::MapTestLuaEngine::Owned;
    bool hotReload = true, stopAfter = false, hidden = false, backgroundPlay = false, pressSpace = false, cloaked = false;
    std::vector<Reload> reloads;
    std::vector<Event> events;
    bool closeRequested = false;
    try {
    for (int index = 5; index < argc; ++index) {
        const std::wstring option = argv[index];
        if (option == L"--ydwe" && index + 1 < argc) ydwe = argv[++index];
        else if (option == L"--seconds" && index + 1 < argc) seconds = Number(argv[++index]);
        else if (option == L"--exit-timeout" && index + 1 < argc) exitTimeout = Number(argv[++index]);
        else if (option == L"--lua-engine" && index + 1 < argc) {
            const std::wstring value = argv[++index];
            if (value != L"owned" && value != L"ydwe") return 2;
            engine = value == L"ydwe" ? Services::MapTestLuaEngine::Ydwe : Services::MapTestLuaEngine::Owned;
        } else if (option == L"--no-hot-reload") hotReload = false;
        else if (option == L"--stop") stopAfter = true;
        else if (option == L"--hidden") hidden = true;
        else if (option == L"--background-play") backgroundPlay = true;
        // 看不见但照常绘制（需要后台运行可用，否则退回 --hidden 的不可见桌面）。
        else if (option == L"--cloaked") { cloaked = true; hidden = true; }
        // 每 2 秒给游戏发一次空格，越过载入后的“按任意键继续”。
        else if (option == L"--press-space") pressSpace = true;
        // --screenshot-after 秒 输出.tga：让游戏自己截一张图，移到给定路径。
        else if (option == L"--screenshot-after" && index + 2 < argc) {
            if (!*argv[index + 2]) return 2;
            events.push_back({Number(argv[index + 1]), {}, argv[index + 2]}); index += 2;
        }
        else if (option == L"--click-after" && index + 3 < argc) {
            events.push_back({Number(argv[index + 1]), {Script::MapTestInputKind::Click, Number(argv[index + 2]), Number(argv[index + 3]), {}}}); index += 3;
        }
        else if (option == L"--key-after" && index + 2 < argc) {
            const auto parsed = Script::ParseMapTestKeys(Utf8(argv[index + 2]));
            if (!parsed.Succeeded() || !parsed.value) { Print(parsed.diagnostics); return 2; }
            events.push_back({Number(argv[index + 1]), {Script::MapTestInputKind::Key, 0, 0, *parsed.value}}); index += 2;
        }
        else if (option == L"--close-after" && index + 1 < argc) {
            if (closeRequested) return 2; closeRequested = true;
            events.push_back({Number(argv[++index]), {Script::MapTestInputKind::Close}});
        }
        else if (option == L"--reload-after" && index + 3 < argc) {
            Reload reload;
            reload.after = Number(argv[index + 1]); reload.module = Utf8(argv[index + 2]); reload.file = argv[index + 3];
            reloads.push_back(std::move(reload)); index += 3;
        } else return 2;
    }
    } catch (const std::exception&) { std::cerr << "MAP_TEST_INPUT_ARGUMENT: invalid numeric argument\n"; return 2; }
    if (!seconds || !exitTimeout || exitTimeout > 3600 || events.size() > 128 || (closeRequested && stopAfter)) return 2;
    double firstInput = seconds + 1;
    for (const auto& event : events) {
        if (event.after >= seconds || (!event.screenshot.empty() && event.screenshot.extension() != L".tga")) return 2;
        if (event.screenshot.empty()) {
            if (!cloaked) { std::cerr << "MAP_TEST_INPUT_UNSUPPORTED: scheduled input requires --cloaked\n"; return 2; }
            const auto valid = Script::ValidateMapTestInput(event.input); if (!valid.Succeeded()) { Print(valid.diagnostics); return 2; }
            firstInput = std::min(firstInput, event.after);
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.after < b.after; });
    const auto sha = War3Map::Sha256File(map);
    if (!sha.Succeeded()) { Print(sha.diagnostics); return 1; }
    wchar_t temp[MAX_PATH + 1]{};
    if (!GetTempPathW(MAX_PATH, temp)) return 1;
    const auto run = std::filesystem::path(temp) / (L"audastra-cli-test-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    if (!std::filesystem::create_directory(run, error) || error) { std::cerr << "cannot create test directory\n"; return 1; }
    auto workspace = Services::FreezeMapTestWorkspace(run);
    if (!workspace.Succeeded() || !workspace.value) { Print(workspace.diagnostics); return 1; }

    Services::MapTestRequest request;
    request.inputMap = std::filesystem::absolute(map); request.sourceMap = request.inputMap;
    request.expectedInputSha256 = *sha.value;
    request.gameRoot = std::filesystem::absolute(game); request.workspace = run / "prepared";
    request.runtimeYdweRoot = ydwe;
    request.runtimeBridge = std::filesystem::absolute(helpers) / "audastra-warcraft-launcher.exe";
    request.runtimeBootstrap = std::filesystem::absolute(helpers) / "audastra-warcraft-bootstrap.dll";
    request.handwrittenLuaEngine = engine; request.handwrittenLuaHotReload = hotReload;
    const auto progress = [](std::string_view stage) { std::cout << "stage=" << stage << '\n'; };
    auto artifact = Services::PrepareMapTest(request, {}, progress);
    Print(artifact.diagnostics);
    if (!artifact.Succeeded() || !artifact.value) return 1;
    const auto refreshed = (*workspace.value)->Refresh();
    if (!refreshed.Succeeded()) { Print(refreshed.diagnostics); return 1; }
    artifact.value->hiddenDesktop = hidden;
    artifact.value->backgroundPlay = backgroundPlay;
    artifact.value->cloakedWindow = cloaked;
    auto launched = Script::LaunchMapTest(*artifact.value, progress);
    Print(launched.diagnostics);
    if (!launched.Succeeded() || !launched.value) { (void)(*workspace.value)->Cleanup(true); return 1; }
    const auto& receipt = *launched.value;
    std::cout << "launched=true\nlauncher_pid=" << receipt.launcherProcessId << "\nlua_entries=" << receipt.luaEntryModules.size()
        << "\nlua_mailbox=" << (receipt.luaMailbox ? "true" : "false") << "\nlua_hot_reload=" << (receipt.luaHotReload ? "true" : "false")
        << std::endl;

    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    std::uint32_t lastDiagnostic = 0;
    std::string lastState;
    std::string lastRuntime;
    double lastSpace = -2;
    bool inputFailed = false, shotFailed = false, stopped = false;
    const auto logInput = [&](const Script::MapTestInput& input, double planned, std::string_view origin) {
        const auto result = receipt.lease->SendTestInput(input);
        nlohmann::json line{{"elapsed_seconds", elapsed()}, {"scheduled_seconds", planned}, {"origin", origin},
            {"kind", input.kind == Script::MapTestInputKind::Click ? "click" : input.kind == Script::MapTestInputKind::Key ? "key" : "close"},
            {"ui_x", input.x}, {"ui_y", input.y}, {"keys", input.keys}, {"transport_complete", result.Succeeded()},
            {"pid", nullptr}, {"hwnd", nullptr}, {"method", "none"}};
        if (result.Succeeded() && result.value) {
            const auto& value = *result.value;
            line.update({{"pid", value.processId}, {"hwnd", value.window}, {"sequence", value.sequence}, {"client_width", value.width},
                {"client_height", value.height}, {"pixel_x", value.pixelX}, {"pixel_y", value.pixelY}, {"method", value.method},
                {"virtual_cursor_observed", value.cursorObserved}, {"virtual_key_observed", value.keyObserved}, {"coordinate_space", "target_window_dpi_client"}});
        } else {
            if (origin != "startup_space") Print(result.diagnostics);
            line["error"] = result.diagnostics.empty() ? "MAP_TEST_INPUT_FAILED" : result.diagnostics.front().code;
            line["owned_job_pids"] = receipt.lease->ProcessIds();
        }
        std::cout << "test_input=" << line.dump() << std::endl;
        return result.Succeeded();
    };
    const auto pollRuntime = [&] {
        if (!receipt.lease) return;
        const auto runtime = receipt.lease->QueryRuntimeEvidence();
        if (runtime.Succeeded() && runtime.value) {
            const auto& v = *runtime.value;
            const auto json = nlohmann::json{{"state", v.state}, {"session_nonce", v.sessionNonce},
                {"map_sha256", v.mapSha256}, {"process_id", v.processId}, {"process_created", v.processCreated},
                {"epoch", v.epoch}, {"module_loaded", v.moduleLoaded}, {"handshake_success", v.handshakeSuccess},
                {"runtime_sha256", v.runtimeSha256}, {"expected_sha256", v.expectedSha256},
                {"phase_started_ms", v.loadedMs}, {"installed_ms", v.installedMs}, {"hello_ms", v.helloMs},
                {"report_ms", v.reportMs}, {"status", v.status}, {"host_deadline", v.hostDeadline}}.dump();
            if (json != lastRuntime) { lastRuntime = json; std::cout << "runtime_evidence=" << json << std::endl; }
        } else if (!runtime.diagnostics.empty()) {
            const auto& diagnostic = runtime.diagnostics.back();
            const auto key = diagnostic.code + diagnostic.message;
            if (key != lastRuntime) { lastRuntime = key; std::cout << "runtime_evidence_error=" << diagnostic.code << std::endl; }
        }
    };
    while (elapsed() < seconds && receipt.lease && receipt.lease->IsActive()) {
        pollRuntime();
        // 显式菜单输入开始后停止自动空格，避免影响随后菜单/确认框。
        if (pressSpace && elapsed() < firstInput && elapsed() - lastSpace >= 2) {
            lastSpace = elapsed();
            if (cloaked) (void)logInput({Script::MapTestInputKind::Key, 0, 0, {VK_SPACE}}, lastSpace, "startup_space");
            else (void)receipt.lease->PressSpace();
        }
        for (auto& event : events) {
            if (event.done || elapsed() < event.after) continue;
            event.done = true;
            if (event.screenshot.empty()) {
                if (!logInput(event.input, event.after, "scheduled")) { inputFailed = true; break; }
                continue;
            }
            const auto taken = receipt.lease->RequestGameScreenshot(5000);
            Print(taken.diagnostics);
            std::error_code copied, ignored;
            // 游戏目录和输出可能不在同一个盘：先复制，复制成功再删游戏写的那张。
            const bool saved = taken.Succeeded() && taken.value
                && std::filesystem::copy_file(*taken.value, event.screenshot, std::filesystem::copy_options::overwrite_existing, copied);
            if (saved) std::filesystem::remove(*taken.value, ignored);
            if (!saved) shotFailed = true;
            std::cout << "[" << elapsed() << "s] screenshot=" << (saved ? "true" : "false") << " output=" << event.screenshot.string()
                << (copied ? " copy_error=" + copied.message() : std::string{}) << std::endl;
        }
        if (inputFailed) break;
        if (receipt.luaMailbox) {
            const auto query = Services::QueryLuaTestReload(*receipt.luaMailbox);
            if (query.Succeeded() && query.value) {
                const auto& snapshot = *query.value;
                if (snapshot.runtimeDiagnosticSequence && snapshot.runtimeDiagnosticSequence != lastDiagnostic) {
                    lastDiagnostic = snapshot.runtimeDiagnosticSequence;
                    std::cout << "[" << elapsed() << "s] lua: " << snapshot.runtimeMessage << std::endl;
                }
                const auto state = std::to_string(static_cast<int>(snapshot.state)) + "/" + std::to_string(snapshot.sequence);
                if (state != lastState) {
                    lastState = state;
                    std::cout << "[" << elapsed() << "s] mailbox_state=" << static_cast<int>(snapshot.state)
                        << " sequence=" << snapshot.sequence << " result=" << static_cast<int>(snapshot.resultCode)
                        << (snapshot.message.empty() ? "" : " message=" + snapshot.message) << std::endl;
                }
                for (auto& reload : reloads) {
                    if (reload.sent || elapsed() < reload.after || snapshot.state == Services::LuaTestReloadState::Starting
                        || snapshot.state == Services::LuaTestReloadState::Queued || snapshot.state == Services::LuaTestReloadState::Running)
                        continue;
                    const auto bytes = War3Map::IO::ReadFileBytes(reload.file, 1024 * 1024);
                    if (!bytes.Succeeded()) { Print(bytes.diagnostics); reload.sent = true; continue; }
                    const std::string source(reinterpret_cast<const char*>(bytes.value->data()), bytes.value->size());
                    const auto sent = Services::SubmitLuaTestReload(*receipt.luaMailbox, {"module:" + reload.module, source});
                    reload.sent = true;
                    if (!sent.Succeeded() || !sent.value) { Print(sent.diagnostics); continue; }
                    reload.sequence = *sent.value;
                    std::cout << "[" << elapsed() << "s] reload_sent module=" << reload.module << " sequence=" << reload.sequence << std::endl;
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cout << "observation_done=true\nwaiting_for_game_exit=true" << std::endl;
    // --stop：观察结束后结束本次启动的进程树（与编辑器 map_test_stop 相同）。
    if (stopAfter && receipt.lease && receipt.lease->IsActive()) {
        const auto stopResult = receipt.lease->Stop();
        Print(stopResult.diagnostics);
        std::cout << "stop_requested=" << (stopResult.Succeeded() ? "true" : "false") << std::endl;
    }
    stopped = stopAfter;
    if (inputFailed && !stopAfter) { std::cout << "test_failed=true\nactive_game_retained=true\n"; return 1; }
    const auto waitStart = std::chrono::steady_clock::now();
    while (receipt.lease && receipt.lease->IsActive()) {
        pollRuntime();
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - waitStart).count() >= exitTimeout) {
            std::cerr << "MAP_TEST_EXIT_TIMEOUT: no forced termination; test process and copies retained\n"; return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    pollRuntime();
    bool exitKnown = false, gameExitZero = true;
    if (receipt.lease) for (const auto& process : receipt.lease->ProcessExits()) {
        std::cout << "process_exit=" << nlohmann::json{{"pid",process.processId},{"launcher",process.launcher},{"game",process.game},
            {"known",process.known},{"exit_code",process.exitCode}}.dump() << std::endl;
        if (process.game) { exitKnown = true; gameExitZero &= process.known && process.exitCode == 0; }
    }
    const auto cleaned = receipt.lease ? receipt.lease->Cleanup() : War3Map::Status::Ok();
    Print(cleaned.diagnostics);
    const auto files = (*workspace.value)->Cleanup(true);
    Print(files.diagnostics);
    // 游戏被强制结束时退出码非零，租约照常清理副本；两者分别报告。
    std::cout << "game_exited=true\ngame_exit_clean=" << (cleaned.Succeeded() ? "true" : "false")
        << "\nunforced_process_exit=" << (!stopped ? "true" : "false")
        << "\ngame_exit_code_zero=" << (exitKnown && gameExitZero ? "true" : "false")
        << "\nworkspace_cleanup=" << (files.Succeeded() ? "true" : "false") << std::endl;
    const bool missed = std::any_of(events.begin(), events.end(), [](const auto& event) { return !event.done; });
    if (missed) std::cout << "scheduled_events_incomplete=true\n";
    return !inputFailed && !shotFailed && !missed && cleaned.Succeeded() && files.Succeeded() && exitKnown && gameExitZero ? 0 : 1;
}
