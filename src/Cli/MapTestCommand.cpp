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

// 命令行地图测试：与编辑器 F9 同一准备、预检、启动与 Lua 邮箱链路，便于脚本与 Agent 验证。
// 只写入本次新建的独占目录和游戏 Maps/Audastra 短路径副本；游戏退出后按租约清理。
namespace {
namespace Services = War3Map::Services;
namespace Script = War3Map::Script;

struct Reload { double after = 0; std::string module; std::filesystem::path file; bool sent = false; std::uint32_t sequence = 0; };
struct Shot { double after = 0; std::filesystem::path output; bool done = false; };

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
    auto engine = Services::MapTestLuaEngine::Owned;
    bool hotReload = true, stopAfter = false, hidden = false, backgroundPlay = false, pressSpace = false, cloaked = false;
    std::vector<Reload> reloads;
    std::vector<Shot> shots;
    for (int index = 5; index < argc; ++index) {
        const std::wstring option = argv[index];
        if (option == L"--ydwe" && index + 1 < argc) ydwe = argv[++index];
        else if (option == L"--seconds" && index + 1 < argc) seconds = std::stod(argv[++index]);
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
            shots.push_back({std::stod(argv[index + 1]), argv[index + 2]}); index += 2;
        }
        else if (option == L"--reload-after" && index + 3 < argc) {
            Reload reload;
            reload.after = std::stod(argv[index + 1]); reload.module = Utf8(argv[index + 2]); reload.file = argv[index + 3];
            reloads.push_back(std::move(reload)); index += 3;
        } else return 2;
    }
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
        if (pressSpace && elapsed() - lastSpace >= 2) { lastSpace = elapsed(); (void)receipt.lease->PressSpace(); }
        for (auto& shot : shots) {
            if (shot.done || elapsed() < shot.after) continue;
            shot.done = true;
            const auto taken = receipt.lease->RequestGameScreenshot(5000);
            Print(taken.diagnostics);
            std::error_code copied, ignored;
            // 游戏目录和输出可能不在同一个盘：先复制，复制成功再删游戏写的那张。
            const bool saved = taken.Succeeded() && taken.value
                && std::filesystem::copy_file(*taken.value, shot.output, std::filesystem::copy_options::overwrite_existing, copied);
            if (saved) std::filesystem::remove(*taken.value, ignored);
            std::cout << "[" << elapsed() << "s] screenshot=" << (saved ? "true" : "false") << " output=" << shot.output.string()
                << (copied ? " copy_error=" + copied.message() : std::string{}) << std::endl;
        }
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
        const auto stopped = receipt.lease->Stop();
        Print(stopped.diagnostics);
        std::cout << "stop_requested=" << (stopped.Succeeded() ? "true" : "false") << std::endl;
    }
    while (receipt.lease && receipt.lease->IsActive()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    pollRuntime();
    const auto cleaned = receipt.lease ? receipt.lease->Cleanup() : War3Map::Status::Ok();
    Print(cleaned.diagnostics);
    const auto files = (*workspace.value)->Cleanup(true);
    Print(files.diagnostics);
    // 游戏被强制结束时退出码非零，租约照常清理副本；两者分别报告。
    std::cout << "game_exited=true\ngame_exit_clean=" << (cleaned.Succeeded() ? "true" : "false")
        << "\nworkspace_cleanup=" << (files.Succeeded() ? "true" : "false") << std::endl;
    return 0;
}
