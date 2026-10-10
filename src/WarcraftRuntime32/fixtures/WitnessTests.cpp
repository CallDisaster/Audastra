#include "../RuntimeFiles.h"
#include "../RuntimeProtocol.h"
#include <array>
#include <charconv>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

using namespace Audastra::Runtime32;
namespace {
void Require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
std::wstring Quote(const std::filesystem::path& path) { return L"\"" + path.wstring() + L"\""; }
std::string OriginalJsonPath(const std::filesystem::path& path) {
    // argv 保留调用者的原路径拼写；此 oracle 只做严格 UTF-8/JSON 编码。
    const auto value = path.wstring();
    Require(!value.empty() && value.size() < kPathCapacity && PlainPath(path), "original argv path bounded/plain");
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    Require(size > 0, "original argv path UTF-8");
    std::string utf8(size, '\0');
    Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), utf8.data(), size, nullptr, nullptr) == size,
        "original argv path UTF-8 exact");
    std::string json = "\"";
    for (const auto character : utf8) { if (character == '\\' || character == '"') json += '\\'; json += character; }
    return json + '"';
}
std::string LowerJsonPath(const std::filesystem::path& path) {
    // 以原文件实际句柄的规范名为 oracle；不复制产品的 GetLongPathName 算法。
    Handle original(OpenLockedFile(path)); Require(original.value != INVALID_HANDLE_VALUE, "original mixed-case file handle");
    std::array<wchar_t, kPathCapacity> finalName{};
    const auto count = GetFinalPathNameByHandleW(original.value, finalName.data(), static_cast<DWORD>(finalName.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    Require(count > 0 && count < finalName.size(), "original mixed-case file final name");
    auto value = std::wstring(finalName.data(), count);
    Require(value.starts_with(L"\\\\?\\") && value.size() >= 7 && value[5] == L':' && value[6] == L'\\', "original final name is DOS path");
    value.erase(0, 4);
    const auto finalPath = std::filesystem::path(value);
    Require(PlainPath(finalPath), "original final file path plain");
    Handle normalized(OpenLockedFile(finalPath)); Require(normalized.value != INVALID_HANDLE_VALUE, "original long-name file handle");
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    Require(GetFileInformationByHandle(original.value, &before) != FALSE && GetFileInformationByHandle(normalized.value, &after) != FALSE
        && before.nNumberOfLinks == 1 && after.nNumberOfLinks == 1 && before.dwVolumeSerialNumber == after.dwVolumeSerialNumber
        && before.nFileIndexHigh == after.nFileIndexHigh && before.nFileIndexLow == after.nFileIndexLow,
        "original alias and actual long name identify same single-link file");
    const auto originalHash = Sha256(original.value), normalizedHash = Sha256(normalized.value);
    Require(originalHash.size() == 64 && originalHash == normalizedHash, "original alias and actual long-name file SHA equal");
    for (auto& character : value) character = static_cast<wchar_t>(towlower(character));
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    Require(size > 0, "original mixed-case path UTF-8");
    std::string utf8(size, '\0');
    Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), utf8.data(), size, nullptr, nullptr) == size,
        "original mixed-case path UTF-8 exact");
    std::string json = "\"";
    for (const auto character : utf8) { if (character == '\\' || character == '"') json += '\\'; json += character; }
    return json + '"';
}
std::string Hash(const std::filesystem::path& path) {
    Handle file(OpenLockedFile(path)); Require(file.value != INVALID_HANDLE_VALUE, "original fixture hash open");
    const auto value = Sha256(file.value); Require(value.size() == 64, "original fixture actual hash"); return value;
}
std::string Read(const std::filesystem::path& path) {
    Require(std::filesystem::file_size(path) < 2 * 1024 * 1024, "bounded owned receipt");
    std::ifstream stream(path, std::ios::binary); Require(stream.good(), "owned receipt read");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::uint64_t Number(std::string_view text, std::string_view field) {
    const auto key = std::string("\"") + std::string(field) + "\":";
    const auto position = text.find(key); Require(position != text.npos, "actual numeric receipt field missing");
    const auto first = text.data() + position + key.size(); std::uint64_t value = 0;
    const auto parsed = std::from_chars(first, text.data() + text.size(), value);
    Require(parsed.ec == std::errc{}, "actual numeric receipt field invalid"); return value;
}
std::wstring Nonce() {
    std::array<UCHAR, 16> random{};
    Require(BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "nonce");
    std::wstring result; constexpr wchar_t digits[] = L"0123456789abcdef";
    for (const auto value : random) { result += digits[value >> 4]; result += digits[value & 15]; }
    return result;
}
std::wstring InputDesktopName() {
    const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    Require(input != nullptr, "input Desktop query only");
    std::array<wchar_t, 256> name{}; DWORD needed = 0;
    const auto queried = GetUserObjectInformationW(input, UOI_NAME, name.data(), static_cast<DWORD>(sizeof(name)), &needed);
    const auto closed = CloseDesktop(input);
    Require(queried && closed && name[0], "input Desktop name and query handle close"); return name.data();
}
struct Resources {
    HANDLE job = nullptr, process = nullptr, thread = nullptr;
    HDESK desktop = nullptr;
    bool assigned = false;
    ~Resources() {
        // 出错时也只结束本测试自己的 Job，尚未分配时只结束原挂起句柄。
        if (assigned && job) TerminateJobObject(job, 71);
        else if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 71);
        if (process) {
            WaitForSingleObject(process, 5000);
        }
        if (thread) CloseHandle(thread);
        if (process) CloseHandle(process);
        if (job) {
            const auto start = GetTickCount64();
            while (GetTickCount64() - start < 5000) {
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION state{};
                if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &state, sizeof(state), nullptr) || !state.ActiveProcesses) break;
                Sleep(1);
            }
            CloseHandle(job);
        }
        if (desktop) CloseDesktop(desktop);
    }
    void Finish(DWORD expectedTotal = 0) {
        Require(CloseHandle(thread) != FALSE, "close exact terminal thread"); thread = nullptr;
        Require(CloseHandle(process) != FALSE, "close exact terminal process"); process = nullptr;
        const auto start = GetTickCount64();
        while (true) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION state{};
            Require(QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &state, sizeof(state), nullptr) != FALSE, "owned Job terminal query");
            if (!state.ActiveProcesses) {
                if (expectedTotal) Require(state.TotalProcesses == expectedTotal, "invalid diagnostics flags created only original detached bridge; no Game/helper");
                break;
            }
            Require(GetTickCount64() - start < 5000, "owned Job must be empty before cleanup"); Sleep(1);
        }
        Require(CloseHandle(job) != FALSE, "close owned Job"); job = nullptr;
        Require(CloseDesktop(desktop) != FALSE, "close owned Desktop"); desktop = nullptr;
    }
};
struct WindowRow { HWND hwnd{}; DWORD pid{}, tid{}, style{}, exStyle{}; bool visible{}; std::wstring className; };
struct RowFailure { const char* api = "none"; DWORD error{}; HWND hwnd{}; };
bool Row(HWND hwnd, WindowRow& row, RowFailure* failure = nullptr) {
    const auto fail = [&](const char* api) {
        if (failure) *failure = {api, GetLastError(), hwnd};
        return false;
    };
    SetLastError(0);
    row.hwnd = hwnd; row.tid = GetWindowThreadProcessId(hwnd, &row.pid); if (!row.tid) return fail("GetWindowThreadProcessId");
    std::array<wchar_t, 256> text{};
    SetLastError(0);
    const auto count = GetClassNameW(hwnd, text.data(), static_cast<int>(text.size()));
    if (!count || count >= static_cast<int>(text.size()) - 1) return fail("GetClassNameW");
    row.className.assign(text.data(), count);
    SetLastError(0); const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!style && GetLastError()) return fail("GetWindowLongPtrW.style");
    SetLastError(0); const auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!exStyle && GetLastError()) return fail("GetWindowLongPtrW.exStyle");
    row.style = static_cast<DWORD>(style); row.exStyle = static_cast<DWORD>(exStyle);
    row.visible = IsWindowVisible(hwnd) != FALSE; return true;
}
bool Matches(HWND hwnd, const std::wstring& name, DWORD pid, DWORD tid, HANDLE process, HANDLE thread, HANDLE job) {
    if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT || WaitForSingleObject(thread, 0) != WAIT_TIMEOUT) return false;
    BOOL member = FALSE;
    if (!IsProcessInJob(process, job, &member) || !member) return false;
    WindowRow first{}, second{};
    if (!Row(hwnd, first) || first.pid != pid || first.tid != tid || first.className != name
        || first.visible || !(first.style & WS_POPUP) || (first.style & (WS_VISIBLE | WS_CHILD))
        || first.exStyle != (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) || !Row(hwnd, second)) return false;
    if (first.pid != second.pid || first.tid != second.tid || first.className != second.className
        || first.style != second.style || first.exStyle != second.exStyle || first.visible != second.visible) return false;
    member = FALSE;
    return WaitForSingleObject(process, 0) == WAIT_TIMEOUT && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT
        && IsProcessInJob(process, job, &member) && member;
}
struct Enumeration { std::vector<WindowRow> rows; bool failed = false; unsigned callbacks{}; RowFailure failure{}; };
constexpr bool InitialEnumerationPending(BOOL enumerated, DWORD error, unsigned callbacks, std::size_t rows, bool callbackFailed) {
    // 本机首轮实测的尚无证据状态；不声称 Windows 文档规定它表示空桌面。
    return enumerated == FALSE && error == ERROR_SUCCESS && callbacks == 0 && rows == 0 && !callbackFailed;
}
static_assert(InitialEnumerationPending(FALSE, 0, 0, 0, false));
static_assert(!InitialEnumerationPending(TRUE, 0, 0, 0, false));
static_assert(!InitialEnumerationPending(FALSE, ERROR_ACCESS_DENIED, 0, 0, false));
static_assert(!InitialEnumerationPending(FALSE, 0, 1, 0, false));
static_assert(!InitialEnumerationPending(FALSE, 0, 0, 1, false));
static_assert(!InitialEnumerationPending(FALSE, 0, 0, 0, true));
BOOL CALLBACK Enumerate(HWND hwnd, LPARAM parameter) {
    auto& value = *reinterpret_cast<Enumeration*>(parameter);
    ++value.callbacks;
    try {
        WindowRow row{};
        if (value.rows.size() >= 64) {
            value.failed = true; value.failure = {"row-budget", ERROR_MORE_DATA, hwnd}; SetLastError(ERROR_MORE_DATA); return FALSE;
        }
        if (!Row(hwnd, row, &value.failure)) {
            value.failed = true; SetLastError(value.failure.error); return FALSE;
        }
        value.rows.push_back(std::move(row)); return TRUE;
    } catch (...) {
        value.failed = true; value.failure = {"callback-exception", ERROR_UNHANDLED_EXCEPTION, hwnd};
        SetLastError(ERROR_UNHANDLED_EXCEPTION); return FALSE;
    }
}
void Case(const std::filesystem::path& base, const std::filesystem::path& launcher,
    const std::filesystem::path& bootstrap, const std::filesystem::path& fakeWar3,
    const std::filesystem::path& fakeGame, const std::filesystem::path& fakeRuntime, int kind) {
    const auto nonce = Nonce(), desktopName = L"AudastraRuntimeProbe-" + nonce;
    const auto inputBefore = InputDesktopName();
    const auto work = base / (L"case-" + std::to_wstring(kind));
    Require(std::filesystem::create_directory(work), "unique owned witness case");
    const auto sandbox = work / L"sandbox", runtime = work / L"runtime";
    std::filesystem::create_directories(sandbox / L"Maps" / L"Test");
    std::filesystem::create_directories(runtime / L"bin"); std::filesystem::create_directories(runtime / L"plugin" / L"warcraft3");
    std::vector<std::pair<std::filesystem::path, std::string>> files;
    const auto remember = [&](const auto& path) { files.emplace_back(path, Hash(path)); };
    std::filesystem::copy_file(fakeWar3, sandbox / L"war3.exe"); remember(sandbox / L"war3.exe");
    std::filesystem::copy_file(fakeGame, sandbox / L"Game.dll"); remember(sandbox / L"Game.dll");
    for (std::size_t index = 0; index < kModules.size(); ++index) {
        const auto path = runtime / ModuleRelativePath(RuntimeLayout::YdweTree, index);
        std::filesystem::copy_file(fakeRuntime / kModules[index], path); remember(path);
    }
    const auto map = sandbox / L"Maps" / L"Test" / L"probe-witness.w3x", script = work / L"original.lua";
    { std::ofstream output(map, std::ios::binary); output << "original witness-only fake map; not gameplay"; }
    { std::ofstream output(script, std::ios::binary); output << "return 'original fake metadata fixture'"; }
    remember(map); remember(script);
    const auto mixedCaseBlocked = work / L"MiXeDCase-Original-NoWrite.bin";
    if (kind == 0) { std::ofstream output(mixedCaseBlocked, std::ios::binary); output << "original external bytes must remain unchanged"; }
    if (kind == 0) remember(mixedCaseBlocked);
    const auto receipt = work / L"receipt.json";
    auto command = Quote(launcher) + L" --game " + Quote(sandbox / L"war3.exe") + L" --map Maps\\Test\\probe-witness.w3x"
        + L" --runtime " + Quote(runtime) + L" --runtime-layout ydwe-tree --bootstrap " + Quote(bootstrap) + L" --receipt " + Quote(receipt);
    if (kind != 4 && kind != 7 && kind != 10) {
        const auto mapHash = Hash(map), scriptHash = Hash(script);
        command += L" --lua-api-probe " + nonce + L" --probe-map-sha256 " + std::wstring(mapHash.begin(), mapHash.end())
            + L" --probe-script " + Quote(script) + L" --probe-script-sha256 " + std::wstring(scriptHash.begin(), scriptHash.end())
            + L" --desktop WinSta0\\" + desktopName + L" --probe-timeout-ms 1000";
    }
    if (kind < 5 && kind != 1) command += L" --probe-desktop-witness " + std::wstring(kind == 2 ? L"message-only" : L"hidden-window");
    if (kind == 3) command += L" --probe-desktop-witness hidden-window";
    if (kind == 0) command += L" --probe-nonfatal-blocked-path " + Quote(mixedCaseBlocked);
    if (kind == 5) command += L" --probe-window-diagnostics invalid";
    if (kind == 6) command += L" --probe-window-diagnostics bounded --probe-window-diagnostics bounded";
    if (kind == 7) command += L" --probe-window-diagnostics bounded";
    if (kind == 8) command += L" --probe-renderer invalid";
    if (kind == 9) command += L" --probe-renderer opengl --probe-renderer opengl";
    if (kind == 10 || kind == 11) command += L" --probe-renderer opengl";
    Resources resources;
    resources.desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0,
        DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_ENUMERATE, nullptr);
    Require(resources.desktop != nullptr, "owned nonce Desktop; no input switch");
    resources.job = CreateJobObjectW(nullptr, nullptr); Require(resources.job != nullptr, "own witness Job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(SetInformationJobObject(resources.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE, "kill-on-close own Job");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    auto desktopPath = L"WinSta0\\" + desktopName; startup.lpDesktop = desktopPath.data();
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | DETACHED_PROCESS,
        nullptr, sandbox.c_str(), &startup, &process) != FALSE, "own detached original bridge on private Desktop");
    resources.process = process.hProcess; resources.thread = process.hThread;
    Require(AssignProcessToJobObject(resources.job, resources.process) != FALSE, "original bridge exact Job"); resources.assigned = true;
    Require(ResumeThread(resources.thread) != static_cast<DWORD>(-1), "resume own bridge");
    WindowRow witnessed{};
    if (kind == 0) {
        const auto start = GetTickCount64(); bool found = false; unsigned pendingEnumerations = 0;
        const auto expectedClass = L"AudastraProbeDesktopWitness-" + nonce;
        while (!found && WaitForSingleObject(resources.process, 0) == WAIT_TIMEOUT && GetTickCount64() - start < 5000) {
            Enumeration enumeration;
            SetLastError(0);
            const auto enumerated = EnumDesktopWindows(resources.desktop, Enumerate, reinterpret_cast<LPARAM>(&enumeration));
            const auto enumerationError = GetLastError();
            if (InitialEnumerationPending(enumerated, enumerationError, enumeration.callbacks, enumeration.rows.size(), enumeration.failed)) {
                Require(WaitForSingleObject(resources.process, 0) == WAIT_TIMEOUT && WaitForSingleObject(resources.thread, 0) == WAIT_TIMEOUT,
                    "pending enumeration needs original process/thread alive");
                BOOL member = FALSE;
                Require(IsProcessInJob(resources.process, resources.job, &member) != FALSE && member,
                    "pending enumeration needs original process exact Job");
                if (++pendingEnumerations == 1) std::cout << "Witness enum pending return=0 Win32=0 callbacks=0 rows=0; no proof granted\n";
                Sleep(1); continue; // 共用原 start/5秒总期限，永不重置、不授予 enumerationComplete。
            }
            if (!enumerated || enumeration.failed) {
                const auto processWait = WaitForSingleObject(resources.process, 0), threadWait = WaitForSingleObject(resources.thread, 0);
                DWORD bridgeExit = STILL_ACTIVE; const auto exitQueried = GetExitCodeProcess(resources.process, &bridgeExit);
                const auto exitError = exitQueried ? ERROR_SUCCESS : GetLastError();
                std::cerr << "Witness enum failure return=" << enumerated << " Win32=" << enumerationError
                    << " callbacks=" << enumeration.callbacks << " rows=" << enumeration.rows.size()
                    << " callbackFailed=" << enumeration.failed << " callbackApi=" << enumeration.failure.api
                    << " callbackWin32=" << enumeration.failure.error << " callbackHwnd=" << reinterpret_cast<ULONG_PTR>(enumeration.failure.hwnd)
                    << " desktop=" << reinterpret_cast<ULONG_PTR>(resources.desktop) << " pid=" << process.dwProcessId << " tid=" << process.dwThreadId
                    << " processWait=" << processWait << " threadWait=" << threadWait << " exitQueried=" << exitQueried
                    << " bridgeExit=" << bridgeExit << " exitWin32=" << exitError << " elapsedMs=" << (GetTickCount64() - start) << '\n';
            }
            Require(enumerated != FALSE && !enumeration.failed, "complete bounded own Desktop enumeration");
            for (const auto& row : enumeration.rows) if (row.className == expectedClass) {
                Require(!found && Matches(row.hwnd, expectedClass, process.dwProcessId, process.dwThreadId,
                    resources.process, resources.thread, resources.job), "actual original bridge hidden mainTID witness");
                witnessed = row; found = true;
            }
            if (!found) Sleep(1); // 有界观察调度；不以固定延时授予证明。
        }
        Require(found, "must actually observe witness while original process/thread alive");
        std::cout << "Witness complete original observation after pendingEnumerations=" << pendingEnumerations << '\n';
        Require(!Matches(witnessed.hwnd, expectedClass, process.dwProcessId + 1, process.dwThreadId,
            resources.process, resources.thread, resources.job), "actual wrong owner rejected");
        Require(!Matches(witnessed.hwnd, expectedClass + L"-wrong", process.dwProcessId, process.dwThreadId,
            resources.process, resources.thread, resources.job), "actual wrong class rejected");
        Handle otherJob(CreateJobObjectW(nullptr, nullptr)); Require(otherJob.value != nullptr, "owned negative Job");
        Require(!Matches(witnessed.hwnd, expectedClass, process.dwProcessId, process.dwThreadId,
            resources.process, resources.thread, otherJob.value), "actual wrong exact Job rejected");
        Require(!Matches(nullptr, expectedClass, process.dwProcessId, process.dwThreadId,
            resources.process, resources.thread, resources.job), "absent HWND rejected");
        Require(Matches(witnessed.hwnd, expectedClass, process.dwProcessId, process.dwThreadId,
            resources.process, resources.thread, resources.job), "negative observations did not rely on an already-dead original witness");
        SetLastError(0); const auto actual = GetThreadDesktop(process.dwThreadId); const auto error = GetLastError();
        std::cout << "Witness original PID=" << process.dwProcessId << " TID=" << process.dwThreadId
            << " HWND=" << reinterpret_cast<ULONG_PTR>(witnessed.hwnd) << " directNull=" << (actual ? "false" : "true") << " error=" << error << '\n';
        if (actual) {
            std::array<wchar_t, 128> name{}; DWORD needed = 0;
            Require(GetUserObjectInformationW(actual, UOI_NAME, name.data(), static_cast<DWORD>(sizeof(name)), &needed) != FALSE
                && desktopName == name.data(), "nonNULL direct Desktop cannot contradict witness");
        }
    }
    Require(WaitForSingleObject(resources.process, 8000) == WAIT_OBJECT_0, "bounded bridge terminal");
    DWORD exit = STILL_ACTIVE; Require(GetExitCodeProcess(resources.process, &exit) != FALSE, "original terminal exit");
    if (kind >= 2 && kind <= 10) Require(exit == 64 && !std::filesystem::exists(receipt), "invalid probe-only flags reject before receipt/Game");
    else {
        const auto text = Read(receipt); Require(exit == 0 && text.find("\"gameRuntimeVerified\":false") != text.npos, "fake probe success without gameplay claim");
        Require(text.find("\"windowDiagnostics\"") == text.npos, "default metadata probe never enabled window diagnostics");
        Require(text.find("\"captureStarted\":true") != text.npos, "raw serializes existing metadata capture-start state without new IPC");
        std::string expectedArgv = "\"gameArgv\":[" + OriginalJsonPath(sandbox / L"war3.exe") + ",\"-window\",";
        if (kind == 11) expectedArgv += "\"-opengl\",";
        expectedArgv += "\"-loadfile\",\"Maps\\\\Test\\\\probe-witness.w3x\"]";
        Require(text.find(expectedArgv) != text.npos, "raw argv equals actual constructed default/OpenGL request tokens");
        Require(text.find(kind == 11 ? "\"probeGraphicsBackendRequested\":\"opengl\"" : "\"probeGraphicsBackendRequested\":\"game-default\"") != text.npos
            && text.find("\"gameplayVerified\":false") != text.npos, "fake graphics request recorded without gameplay/backend verification");
        if (kind != 0) Require(text.find("\"desktopWitness\"") == text.npos, "metadata without explicit witness never enabled GUI witness");
        else {
            Require(text.find("\"nonfatalBlockedPaths\":[" + LowerJsonPath(mixedCaseBlocked) + "]") != text.npos,
                "actual mixed-case input configured list uses exact guard Lower spelling; strict manifest membership unchanged");
            const auto position = text.find("\"desktopWitness\":"); Require(position != text.npos, "raw native witness evidence");
            const auto native = std::string_view(text).substr(position);
            Require(Number(native, "hwnd") == reinterpret_cast<ULONG_PTR>(witnessed.hwnd)
                && Number(native, "bridgePid") == process.dwProcessId && Number(native, "threadId") == process.dwThreadId
                && Number(native, "style") == witnessed.style && Number(native, "exStyle") == witnessed.exStyle, "raw/external actual witness pair");
            Require(native.find("\"created\":true") != native.npos && native.find("\"destroyed\":true") != native.npos
                && native.find("\"unregistered\":true") != native.npos && native.find("\"visible\":false") != native.npos
                && Number(native, "errorWin32") == 0, "explicit finalize succeeded before raw receipt");
        }
    }
    resources.Finish(kind >= 5 && kind <= 10 ? 1 : 0);
    Require(InputDesktopName() == inputBefore, "input Desktop unchanged; no Switch/foreground/input");
    for (const auto& [path, hash] : files) {
        const auto relative = path.lexically_relative(work);
        Require(!relative.empty() && !relative.is_absolute() && *relative.begin() != L".."
            && PlainPath(path) && Hash(path) == hash, "all owned sources unchanged before cleanup");
    }
    for (const auto& [path, hash] : files) { (void)hash; Require(std::filesystem::remove(path), "remove exact verified own fixture"); }
    for (const auto& path : {sandbox / L"Maps" / L"Test", sandbox / L"Maps", sandbox, runtime / L"plugin" / L"warcraft3",
        runtime / L"plugin", runtime / L"bin", runtime}) Require(std::filesystem::remove(path), "remove only empty own fixture directories");
    std::cout << "PASS witness focused case " << kind << " (tiny evidence retained; no real Warcraft)\n";
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 6) return 64;
    try {
        std::array<std::filesystem::path, 5> sources;
        for (std::size_t index = 0; index < sources.size(); ++index) { sources[index] = argv[index + 1]; Require(PlainPath(sources[index]), "explicit readonly original fixture path"); }
        std::array<wchar_t, kPathCapacity> temp{};
        const auto length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data()); Require(length && length < temp.size(), "bounded Temp root");
        const auto base = std::filesystem::path(temp.data()) / (L"AudastraWitnessFixture-" + Nonce());
        Require(PlainPath(base) && std::filesystem::create_directory(base), "unique original fixture root");
        for (int kind = 0; kind < 12; ++kind) Case(base, sources[0], sources[1], sources[2], sources[3], sources[4], kind);
        std::cout << "12 focused witness/diagnostic/renderer-request cases passed; default33 separate, no Warcraft/YDWE/foreground\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "Witness fixture failed: " << error.what() << '\n'; return 1; }
}
