#include "War3MapCore/Script/MapTestToolchain.h"
#include "War3MapCore/Hash.h"
#include "../../src/ScriptBuild/MapTestLaunchInternal.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
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
        Require(argc == 2, "console fixture executable path argument");
        Services::MapTestLaunchPlan pendingRuntime;
        pendingRuntime.useOwnedLuaRuntime = true; pendingRuntime.enableLuaHotReload = true;
        Require(Code(Script::detail::LaunchVerifiedMapTestPlan(pendingRuntime), "MAP_TEST_OWN_LUA_RUNTIME_NOT_READY"),
            "internal execution entry refuses unconnected owned runtime before copying files or starting a process");
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
        const auto firstMarker = temp.root / "first-child-ended";
        plan.arguments = {L"--parent", firstMarker.wstring(), L"1200"};
        auto first = Script::detail::LaunchVerifiedMapTestPlan(plan); Success(first, "trusted console fixture launch");
        Require(first.value->lease && first.value->lease->IsActive(), "suspended launcher assigned original Job before resume");
        Require(Hash(plan.gameMapCopy) == plan.testMapSha256, "independent short game copy has frozen source SHA");
        Require(!first.value->lease->Cleanup().Succeeded() && std::filesystem::exists(plan.gameMapCopy), "active game tree cannot delete the short map copy");
        auto duplicatePlan = plan; duplicatePlan.gameMapCopy = task("duplicate");
        const auto duplicate = Script::detail::LaunchVerifiedMapTestPlan(duplicatePlan);
        Require(Code(duplicate, "MAP_TEST_ALREADY_ACTIVE"), "same game install refuses another launch even with a different short map copy");
        WaitTerminal(first.value->lease);
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
        std::cout << "Independent map test launch console Job scenarios passed; no YDWE, Warcraft, registry or GPU used\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
