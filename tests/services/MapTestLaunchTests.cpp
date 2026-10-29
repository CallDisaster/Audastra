#include "War3MapCore/Script/MapTestToolchain.h"
#include "War3MapCore/Services/LuaTestMailboxSession.h"
#include "War3MapCore/Hash.h"
#include "../../src/ScriptBuild/MapTestLaunchInternal.h"
#include "War3MapCore/Runtime/WindowTestInputProtocol.h"

#include <windows.h>

#include <filesystem>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <limits>

namespace {
using namespace War3Map;
unsigned unrelatedMessages = 0;
LRESULT CALLBACK UnrelatedWindow(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_COPYDATA) ++unrelatedMessages;
    return DefWindowProcW(window, message, wparam, lparam);
}
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> void Success(const Result<T>& result, const char* message) {
    if (result.Succeeded()) return;
    std::string text(message); for (const auto& d : result.diagnostics) text += "\n" + d.code + ": " + d.message;
    throw std::runtime_error(text);
}
void Success(const Status& result, const char* message) {
    if (result.Succeeded()) return;
    std::string text(message); for (const auto& d : result.diagnostics) text += "\n" + d.code + ": " + d.message;
    throw std::runtime_error(text);
}
std::string Hash(const std::filesystem::path& path) {
    const auto result = Sha256File(path); Success(result, "actual fixture file SHA"); return *result.value;
}
void WriteNew(const std::filesystem::path& path, std::string_view text) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(handle != INVALID_HANDLE_VALUE, "create only new owned fixture file");
    DWORD written = 0;
    const auto result = WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    const auto flushed = FlushFileBuffers(handle); CloseHandle(handle);
    Require(result && flushed && written == text.size(), "write and flush original fixture bytes");
}
bool Code(const Result<Script::MapTestLaunchReceipt>& result, std::string_view code) {
    for (const auto& d : result.diagnostics) if (d.code == code) return true; return false;
}
bool Code(const Status& result, std::string_view code) {
    for (const auto& d : result.diagnostics) if (d.code == code) return true; return false;
}
void WaitTerminal(const std::shared_ptr<Script::MapTestLaunchLease>& lease, DWORD timeout = 6000) {
    const auto start = GetTickCount64();
    while (lease->IsActive() && GetTickCount64() - start < timeout) Sleep(20);
    Require(!lease->IsActive(), "job empty and complete original process handles signaled");
}
struct Temp {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("forge-job-fixture-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Temp() { Require(std::filesystem::create_directory(root), "new owned fixture directory"); }
    ~Temp() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
}
int wmain(int argc, wchar_t** argv) {
    try {
        const bool windowOnly = argc == 3 && std::wstring_view(argv[2]) == L"--input-window";
        const bool inputContractOnly = argc == 3 && std::wstring_view(argv[2]) == L"--input-contract";
        Require(argc == 2 || windowOnly || inputContractOnly, "original fixture executable path and optional input test mode");
        const auto keys = Script::ParseMapTestKeys("Ctrl+Shift+Q"); Success(keys, "parse virtual key chord");
        Require(*keys.value == std::vector<std::uint32_t>({17,16,'Q'}), "chord preserves modifier order");
        for (const auto* bad : {"", "Ctrl+", "Ctrl+Ctrl", "F25", "Win+Q", "0x01", "0x5b", "-1", "Q+x+Q"})
            Require(!Script::ParseMapTestKeys(bad).Succeeded(), "invalid/system/mouse/duplicate key refused");
        Require(*Script::ParseMapTestKeys("F10").value == std::vector<std::uint32_t>{121}, "F10 virtual code");
        Require(*Script::ParseMapTestKeys("0x0d").value == std::vector<std::uint32_t>{13}, "numeric virtual code");
        const auto center = Script::MapTestInputPixel(.4, .3, 1920, 1080); Success(center, "widescreen stretched coordinate conversion");
        Require(*center.value == std::pair<int,int>{960,540}, "client center exact");
        Require(*Script::MapTestInputPixel(.8,0,1920,1080).value == std::pair<int,int>{1919,1079}, "edge clamped into client");
        Require(!Script::MapTestInputPixel(std::numeric_limits<double>::quiet_NaN(), .2, 640,480).Succeeded()
            && !Script::MapTestInputPixel(.9,.2,640,480).Succeeded() && !Script::MapTestInputPixel(.4,.2,0,480).Succeeded(), "unknown coordinates/geometry refused");
        if (inputContractOnly) { std::cout << "Input parser and stretched coordinate contract passed; no windows or game\n"; return 0; }
        if (windowOnly) {
            const auto desktop = OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
            POINT cursor{}; const bool available = desktop && GetCursorPos(&cursor) && GetForegroundWindow();
            if (desktop) CloseDesktop(desktop);
            if (!available) { std::cout << "INPUT_DESKTOP_UNAVAILABLE: original input-window fixture not run\n"; return 77; }
        }
        Services::MapTestLaunchPlan pendingRuntime;
        pendingRuntime.useOwnedLuaRuntime = true; pendingRuntime.enableLuaHotReload = true;
        Require(Code(Script::detail::LaunchVerifiedMapTestPlan(pendingRuntime), "MAP_TEST_OWN_LUA_PLAN"),
            "internal execution entry refuses incomplete owned component identity before copying or starting");
        Temp temp;
        const auto helper = temp.root / "fakeWarcraftConsole.exe";
        Require(CopyFileW(argv[1], helper.c_str(), TRUE) != FALSE, "copy only original tiny console fixture into owned temp directory");
        Services::MapTestLaunchPlan plan;
        plan.executable = helper; plan.workingDirectory = temp.root / "game";
        Require(std::filesystem::create_directory(plan.workingDirectory), "fixture game directory");
        plan.testMap = temp.root / "owned-input.w3x";
        WriteNew(plan.testMap, "original console lifecycle fixture; no real map or game data");
        plan.testMapSha256 = Hash(plan.testMap);
        plan.executableSha256 = Hash(helper);
        const auto task = [&](const char* token) { return plan.workingDirectory / "Maps/Audastra" / token / "test.w3x"; };
        plan.gameMapCopy = task("first");
        if (!windowOnly) {
        const auto firstMarker = temp.root / "first-child-ended";
        plan.arguments = {L"--parent", firstMarker.wstring(), L"1200"};
        auto first = Script::detail::LaunchVerifiedMapTestPlan(plan); Success(first, "trusted console fixture launch");
        Require(!first.value->lease->SendTestInput({Script::MapTestInputKind::Key,0,0,{'Q'}}).Succeeded(), "ordinary console launch cannot use test input or a foreground fallback");
        Require(first.value->lease && first.value->lease->IsActive(), "suspended launcher assigned original Job before resume");
        Require(Hash(plan.gameMapCopy) == plan.testMapSha256, "independent short game copy has frozen source SHA");
        Require(!first.value->lease->Cleanup().Succeeded() && std::filesystem::exists(plan.gameMapCopy), "active game tree cannot delete the short map copy");
        auto duplicatePlan = plan; duplicatePlan.gameMapCopy = task("duplicate");
        const auto duplicate = Script::detail::LaunchVerifiedMapTestPlan(duplicatePlan);
        Require(Code(duplicate, "MAP_TEST_ALREADY_ACTIVE"), "same game install refuses another launch even with a different short map copy");
        WaitTerminal(first.value->lease);
        const auto firstExits = first.value->lease->ProcessExits();
        Require(firstExits.size()==2 && firstExits[0].launcher && firstExits[0].known && firstExits[0].exitCode==0
            && firstExits[1].known && firstExits[1].exitCode==0, "original parent/child natural exit codes retained after handle closure");
        Require(std::filesystem::exists(firstMarker), "child completed naturally");
        Success(first.value->lease->Cleanup(), "safe short map cleanup after complete actual Job terminal");
        Require(!std::filesystem::exists(plan.gameMapCopy.parent_path()), "only first owned task directory removed");
        Require(Hash(plan.testMap) == plan.testMapSha256, "frozen input unchanged after launch and cleanup");
        // 保持first lease引用；完整终态释放命名Job，新F9不受旧UI/timer引用阻塞。
        const auto secondMarker = temp.root / "second-child-ended";
        plan.gameMapCopy = task("second");
        plan.arguments = {L"--parent", secondMarker.wstring(), L"800"};
        auto second = Script::detail::LaunchVerifiedMapTestPlan(plan); Success(second, "repeat launch after proven terminal despite old lease reference");
        WaitTerminal(second.value->lease);
        // 文件读锁保留至Cleanup；即使Job已经终态也不能让其他写者篡改地图。
        const auto writeAttempt = CreateFileW(plan.gameMapCopy.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const auto writeError = GetLastError();
        if (writeAttempt != INVALID_HANDLE_VALUE) CloseHandle(writeAttempt);
        Require(writeAttempt == INVALID_HANDLE_VALUE && writeError == ERROR_SHARING_VIOLATION, "held map read lease rejects writes until explicit Cleanup");
        // 未知新增内容使整轮停止清理，原地图和新增项均保留。
        WriteNew(plan.gameMapCopy.parent_path() / "user-note.txt", "new content must be retained");
        const auto unknownCleanup = second.value->lease->Cleanup();
        Require(!unknownCleanup.Succeeded() && Code(unknownCleanup, "MAP_TEST_CLEANUP_UNKNOWN_ENTRY"), "unexpected new file stops whole task cleanup with exact diagnostic");
        Require(Hash(plan.gameMapCopy) == plan.testMapSha256 && std::filesystem::exists(plan.gameMapCopy.parent_path() / "user-note.txt"), "original map and unexpected new file retained");
        const auto nonKillMarker = temp.root / "non-kill-child-ended";
        plan.gameMapCopy = task("nonkill");
        plan.arguments = {L"--parent", nonKillMarker.wstring(), L"1200"};
        auto nonKill = Script::detail::LaunchVerifiedMapTestPlan(plan); Success(nonKill, "non-kill fixture launch");
        Sleep(450); Require(nonKill.value->lease->IsActive(), "launcher can exit while child remains tracked");
        nonKill.value->lease.reset();
        Require(std::filesystem::exists(plan.gameMapCopy), "lease destruction retains files when complete original tree terminal is unknown");
        duplicatePlan = plan; duplicatePlan.gameMapCopy = task("after-close-active");
        Require(Code(Script::detail::LaunchVerifiedMapTestPlan(duplicatePlan), "MAP_TEST_ALREADY_ACTIVE"), "closed editor lease cannot bypass still-active game-install Job");
        const auto waitStart = GetTickCount64();
        while (!std::filesystem::exists(nonKillMarker) && GetTickCount64() - waitStart < 6000) Sleep(20);
        Require(std::filesystem::exists(nonKillMarker), "lease close did not kill child; natural terminal marker exists");
        Sleep(100); // 标记在child return之前写出；让原进程对象完成信号并关闭内核Job。
        const auto retainedCopy = plan.gameMapCopy;
        plan.gameMapCopy = task("last");
        plan.arguments = {L"--parent", (temp.root / "last-ended").wstring(), L"600"};
        auto last = Script::detail::LaunchVerifiedMapTestPlan(plan); Success(last, "closed-editor active job disappears after natural tree exit");
        WaitTerminal(last.value->lease);
        Success(last.value->lease->Cleanup(), "new task cleans after actual final tree terminal");
        Require(std::filesystem::exists(retainedCopy), "new successful task does not delete old unknown-lifecycle files");
        Require(!std::filesystem::exists(plan.gameMapCopy.parent_path()), "new task only removes its own short game directory");
        Require(Hash(plan.testMap) == plan.testMapSha256 && Hash(helper) == plan.executableSha256, "source and executable identity remain unchanged");
        plan.gameMapCopy = task("fast");
        plan.arguments = {L"--parent", (temp.root / "fast-ended").wstring(), L"0"};
        auto fast = Script::detail::LaunchVerifiedMapTestPlan(plan); Success(fast, "very fast child fixture launch");
        WaitTerminal(fast.value->lease);
        const auto fastCleanup = fast.value->lease->Cleanup();
        Require(fastCleanup.Succeeded() || Code(fastCleanup, "MAP_TEST_LIFETIME_UNCONFIRMED"), "fast child permits proven cleanup or explicit unconfirmed diagnostic");
        if (!fastCleanup.Succeeded()) Require(std::filesystem::exists(plan.gameMapCopy), "unknown fast-child evidence retains its map");
        plan.gameMapCopy = task("after-fast");
        plan.arguments = {L"--parent", (temp.root / "after-fast-ended").wstring(), L"600"};
        auto afterFast = Script::detail::LaunchVerifiedMapTestPlan(plan); Success(afterFast, "fast exited child never permanently blocks a new test task");
        WaitTerminal(afterFast.value->lease); Success(afterFast.value->lease->Cleanup(), "next slow task has complete terminal proof");
        }

        // 原创控制台接收器经过真实LaunchVerified入口/继承白名单/Job，不使用游戏或GPU。
        const auto game = plan.workingDirectory / "war3.exe";
        Require(CopyFileW(helper.c_str(), game.c_str(), TRUE) != FALSE, "owned fixture game image");
        WriteNew(plan.workingDirectory / "Game.dll", "original placeholder module identity");
        plan.runtimeBootstrap = temp.root / "bootstrap.dll";
        WriteNew(plan.runtimeBootstrap, "original placeholder bootstrap identity");
        plan.runtimeBootstrapSha256 = Hash(plan.runtimeBootstrap);
        plan.gameExecutableSha256 = Hash(game); plan.gameModuleSha256 = Hash(plan.workingDirectory / "Game.dll");
        if (windowOnly) {
            WNDCLASSW spy{}; spy.lpfnWndProc = UnrelatedWindow; spy.hInstance = GetModuleHandleW(nullptr); spy.lpszClassName = L"AudastraUnrelatedInputFixture";
            Require(RegisterClassW(&spy) != 0, "register own unrelated hidden window");
            const auto otherWindow = CreateWindowW(spy.lpszClassName, L"outside test job", WS_OVERLAPPEDWINDOW,0,0,800,600,nullptr,nullptr,spy.hInstance,nullptr);
            Require(otherWindow && SetPropW(otherWindow,Runtime::kWindowInputProperty,reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(Runtime::kWindowInputMagic))), "own unrelated process/window witness");
            auto inputPlan = plan; inputPlan.executable = game; inputPlan.cloakedWindow = true;
            inputPlan.gameMapCopy = task("input-window");
            const auto marker = temp.root / "input-natural-end.txt";
            inputPlan.arguments = {L"--input-window", marker.wstring(), L"10000"};
            POINT before{}; Require(GetCursorPos(&before), "real cursor before input fixture");
            const auto foreground = GetForegroundWindow();
            auto run = Script::detail::LaunchVerifiedMapTestPlan(inputPlan); Success(run, "owned hidden window input fixture launch");
            Result<Script::MapTestInputReceipt> clicked;
            const auto start = GetTickCount64();
            do {
                clicked = run.value->lease->SendTestInput({Script::MapTestInputKind::Click,.4,.4625,{}});
                if (clicked.Succeeded()) break;
                Sleep(10);
            } while (GetTickCount64()-start<3000);
            Success(clicked, "first click delivered only to original owned game image/window");
            Require(clicked.value->processId == run.value->launcherProcessId && clicked.value->window && clicked.value->cursorObserved && clicked.value->keyObserved,
                "receiver reports target and virtual input observations");
            Success(run.value->lease->SendTestInput({Script::MapTestInputKind::Click,.4,.4625,{}}), "second click delivers release before next click");
            Success(run.value->lease->SendTestInput({Script::MapTestInputKind::Key,0,0,{VK_CONTROL,'Q'}}), "key chord dispatch and release");
            War3Map::Runtime::WindowInputPacket spoof;
            spoof.processId = run.value->launcherProcessId; spoof.sequence = 42;
            COPYDATASTRUCT data{War3Map::Runtime::kWindowInputMagic,sizeof(spoof),&spoof}; DWORD_PTR answer = 0;
            const auto exchange = [&] {
                const auto window = reinterpret_cast<HWND>(clicked.value->window);
                if (!SendMessageTimeoutW(window,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data),SMTO_ABORTIFHUNG,1000,&answer)) return false;
                if (static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(GetPropW(window, Runtime::kWindowInputSequenceProperty))) != spoof.sequence) return false;
                answer = reinterpret_cast<ULONG_PTR>(GetPropW(window, Runtime::kWindowInputResultProperty));
                return true;
            };
            Require(exchange()
                && answer == static_cast<unsigned>(War3Map::Runtime::WindowInputState::WrongIdentity), "wrong creation time rejected inside receiver");
            const auto query = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,run.value->launcherProcessId);
            FILETIME created{}, exited{}, kernel{}, user{};
            Require(query && GetProcessTimes(query,&created,&exited,&kernel,&user), "read only original fixture process creation time"); CloseHandle(query);
            spoof.createdLow=created.dwLowDateTime; spoof.createdHigh=created.dwHighDateTime;
            Require(exchange()
                && answer == static_cast<unsigned>(Runtime::WindowInputState::UnknownSequence), "unknown sequence cannot acknowledge old action");
            spoof.kind=Runtime::WindowInputKind::Click; spoof.width=1; spoof.height=1;
            Require(exchange()
                && answer == static_cast<unsigned>(Runtime::WindowInputState::WindowChanged), "changed geometry refuses input");
            Success(run.value->lease->SendTestInput({Script::MapTestInputKind::Close}), "native WM_CLOSE request without terminating job");
            WaitTerminal(run.value->lease);
            const auto exits = run.value->lease->ProcessExits();
            Require(exits.size()==1 && exits[0].game && exits[0].known && exits[0].exitCode==0, "natural fixture exit code retained after original handle closure");
            std::ifstream output(marker); std::string line; std::getline(output,line);
            Require(line == "2,2,1,1,1,1", "original WndProc saw clicks/key; process-local pointer/key state and modifier alias observed");
            POINT after{}; Require(GetCursorPos(&after) && before.x==after.x && before.y==after.y && GetForegroundWindow()==foreground,
                "test messages never move real cursor or switch foreground");
            Require(!run.value->lease->SendTestInput({Script::MapTestInputKind::Key,0,0,{'Q'}}).Succeeded(), "ended session cannot receive input");
            Success(run.value->lease->Cleanup(), "normal-close fixture cleanup");
            Require(unrelatedMessages == 0, "larger unrelated window was never sent a message");
            RemovePropW(otherWindow,Runtime::kWindowInputProperty); DestroyWindow(otherWindow);
            inputPlan.cloakedWindow=false; inputPlan.gameMapCopy=task("input-not-cloaked");
            inputPlan.arguments={L"--child",(temp.root/"input-disabled-end").wstring(),L"500"};
            auto disabled = Script::detail::LaunchVerifiedMapTestPlan(inputPlan); Success(disabled,"ordinary process fixture");
            Require(!disabled.value->lease->SendTestInput({Script::MapTestInputKind::Key,0,0,{'Q'}}).Succeeded(), "ordinary launch has no virtual/foreground input fallback");
            WaitTerminal(disabled.value->lease); Success(disabled.value->lease->Cleanup(),"ordinary fixture cleanup");
            std::cout << "Owned hidden input-window dispatch, isolation and native close fixture passed; no game\n";
            return 0;
        }
        plan.useOwnedLuaRuntime = true;
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE forbidden = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, 4096, nullptr);
        Require(forbidden != nullptr, "unrelated inheritable mapping");
        struct Close { HANDLE value; ~Close() { CloseHandle(value); } } forbiddenGuard{forbidden};
        void* forbiddenView = MapViewOfFile(forbidden, FILE_MAP_WRITE, 0, 0, 16);
        Require(forbiddenView != nullptr, "unrelated mapping view");
        std::memcpy(forbiddenView, "not-for-child", 13); UnmapViewOfFile(forbiddenView);
        for (const bool reload : {false, true}) {
            const auto token = reload ? "owned-reload" : "owned-diagnostics";
            const auto marker = temp.root / (std::string(token) + "-ended");
            plan.enableLuaHotReload = reload; plan.gameMapCopy = task(token);
            plan.runtimeReceipt = temp.root / "receipts" / (std::string(token) + ".json");
            plan.arguments = {L"--owned-parent", L"--game", game.wstring(), L"--marker", marker.wstring(),
                L"--lua-map-sha256", std::wstring(plan.testMapSha256.begin(), plan.testMapSha256.end()),
                L"--forbidden-handle", std::to_wstring(reinterpret_cast<std::uintptr_t>(forbidden))};
            auto launched = Script::detail::LaunchVerifiedMapTestPlan(plan); Success(launched, "owned mailbox launch");
            Require(launched.value->luaMailbox != nullptr, "owned launch always returns a diagnostic mailbox");
            Result<Services::LuaTestReloadSnapshot> active;
            const auto readyStart = GetTickCount64();
            do {
                active = Services::QueryLuaTestReload(*launched.value->luaMailbox);
                if (active.Succeeded() && active.value->processActive) break;
                Sleep(10);
            } while (launched.value->lease->IsActive() && GetTickCount64() - readyStart < 5000);
            Success(active, "mailbox confirms only original captured game handle");
            Require(active.value->processActive && active.value->runtimeMessage == "original launch fixture asynchronous diagnostic",
                "diagnostic comes from original game child and unrelated inheritable mapping was not passed");
            const auto submit = Services::SubmitLuaTestReload(*launched.value->luaMailbox, {"@fixture.lua", "return true"});
            if (reload) {
                Success(submit, "enabled owned mailbox can submit");
                const auto ackStart = GetTickCount64();
                do {
                    active = Services::QueryLuaTestReload(*launched.value->luaMailbox, *submit.value);
                    if (active.Succeeded() && active.value->state == Services::LuaTestReloadState::Complete) break;
                    Sleep(10);
                } while (GetTickCount64() - ackStart < 1000);
                Require(active.Succeeded() && active.value->message == "original fixture ACK", "exact fixture ACK through product mailbox");
            } else Require(!submit.Succeeded() && submit.diagnostics.front().code == "LUA_TEST_RELOAD_DISABLED", "disabled reload rejects sending while diagnostics work");
            WaitTerminal(launched.value->lease);
            Require(std::filesystem::exists(marker), "owned child exited naturally");
            const auto ended = Services::QueryLuaTestReload(*launched.value->luaMailbox);
            Success(ended, "last diagnostic survives IsActive terminal handle closure");
            Require(!ended.value->processActive && ended.value->runtimeMessage == "original launch fixture asynchronous diagnostic",
                "retained exit identity preserves last runtime error");
            Success(launched.value->lease->Cleanup(), "owned map cleanup after original Job terminal");
            Require(Services::QueryLuaTestReload(*launched.value->luaMailbox).Succeeded(), "terminal diagnostics survive file cleanup");
        }
        plan.gameMapCopy = task("owned-spoof"); plan.runtimeReceipt = temp.root / "receipts/spoof.json";
        plan.arguments.insert(plan.arguments.end(), {L"--spoof-pid", L"yes"});
        auto spoof = Script::detail::LaunchVerifiedMapTestPlan(plan); Success(spoof, "spoofed mailbox fixture launch");
        Result<Services::LuaTestReloadSnapshot> refused;
        const auto spoofStart = GetTickCount64();
        do {
            refused = Services::QueryLuaTestReload(*spoof.value->luaMailbox);
            if (!refused.Succeeded()) break;
            Sleep(10);
        } while (GetTickCount64() - spoofStart < 3000 && spoof.value->lease->IsActive());
        Require(!refused.Succeeded() && refused.diagnostics.front().code == "LUA_TEST_PROCESS_UNCONFIRMED",
            "mailbox cannot supply an unobserved PID to gain ownership");
        WaitTerminal(spoof.value->lease); Success(spoof.value->lease->Cleanup(), "real spoof fixture tree still cleans safely");
        Require(!Services::QueryLuaTestReload(*spoof.value->luaMailbox).Succeeded(), "unknown mailbox identity does not become owned after exit");

        plan.gameMapCopy = task("owned-startup-failure"); plan.runtimeReceipt = temp.root / "receipts/startup-failure.json";
        plan.arguments = {L"--owned-parent", L"--marker", (temp.root / "must-not-exist").wstring(), L"--lua-map-sha256",
            std::wstring(plan.testMapSha256.begin(), plan.testMapSha256.end()), L"--startup-failure", L"yes", L"--receipt", plan.runtimeReceipt.wstring()};
        auto startupFailure = Script::detail::LaunchVerifiedMapTestPlan(plan); Success(startupFailure, "pre-handshake failure fixture");
        WaitTerminal(startupFailure.value->lease);
        const auto failureCleanup = startupFailure.value->lease->Cleanup();
        Require(Code(failureCleanup, "MAP_TEST_PROCESS_EXIT") && failureCleanup.diagnostics.back().message.find("original pre-handshake failure") != std::string::npos,
            "bootstrap failure before mailbox handshake retains concrete bounded failure text");
        Services::MapTestLaunchPlan evidencePlan;
        evidencePlan.executable = game; evidencePlan.executableSha256 = Hash(game);
        evidencePlan.workingDirectory = plan.workingDirectory;
        evidencePlan.testMap = plan.testMap; evidencePlan.testMapSha256 = plan.testMapSha256;
        evidencePlan.gameMapCopy = task("runtime-evidence");
        evidencePlan.arguments = {L"--evidence", (temp.root / "runtime-evidence-ended").wstring(), L"500"};
        auto observed = Script::detail::LaunchVerifiedMapTestPlan(evidencePlan); Success(observed, "test-only environment reaches original fixture process");
        WaitTerminal(observed.value->lease);
        const auto evidence = observed.value->lease->QueryRuntimeEvidence(); Success(evidence, "read original fixture evidence after process exit");
        Require(evidence.value->handshakeSuccess && evidence.value->epoch == 7
            && evidence.value->mapSha256 == evidencePlan.testMapSha256 && evidence.value->processId == observed.value->launcherProcessId,
            "original launch identity, nonce, map, VM and final acknowledgement match");
        Success(observed.value->lease->Cleanup(), "evidence run cleanup");
        Require(observed.value->lease->QueryRuntimeEvidence().Succeeded(), "own test evidence survives normal map cleanup");
        plan.gameMapCopy = task("owned-changed-bootstrap");
        { std::ofstream changed(plan.runtimeBootstrap, std::ios::binary | std::ios::trunc); changed << "changed bootstrap"; }
        Require(Code(Script::detail::LaunchVerifiedMapTestPlan(plan), "MAP_TEST_EXECUTABLE_CHANGED"),
            "bootstrap replacement after preflight cannot create a launcher");
        Require(!std::filesystem::exists(plan.gameMapCopy), "failed component identity cleans only its unstarted short copy");
        std::cout << "Independent map test launch console Job scenarios passed; no YDWE, Warcraft, registry or GPU used\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
