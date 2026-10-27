#include "../RuntimeProtocol.h"
#include "../RuntimeFiles.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <memory>
#include <tlhelp32.h>

using namespace Audastra::Runtime32;
namespace {
std::wstring Quote(std::wstring value) { return L"\"" + value + L"\""; }
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
std::string FileHash(const std::filesystem::path& path) {
    Handle file(OpenLockedFile(path));
    Require(file.value != INVALID_HANDLE_VALUE, "owned fixture ordinary file lease");
    const auto hash = Sha256(file.value);
    Require(hash.size() == 64, "owned fixture actual SHA");
    return hash;
}
void ReleaseTerminalProcessHandles(Handle& process, Handle& thread) {
    // 调用者已经等到原进程终态并读取 exit code。先释放本测试自己的引用再核 Job。
    Require(CloseHandle(thread.value) != FALSE, "close terminal owned fixture thread handle");
    thread.value = INVALID_HANDLE_VALUE;
    Require(CloseHandle(process.value) != FALSE, "close terminal owned fixture process handle");
    process.value = INVALID_HANDLE_VALUE;
}
std::unique_ptr<Handle> OpenAndRecordOwnedJobMember(HANDLE job, DWORD pid, const char* phase) {
    auto process = std::make_unique<Handle>(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    const auto processError = GetLastError();
    std::cout << "Owned fixture Job member phase=" << phase << " pid=" << pid;
    if (process->value == nullptr) { std::cout << " identity-unavailable-error=" << processError << '\n'; return {}; }
    BOOL belongs = FALSE;
    Require(IsProcessInJob(process->value, job, &belongs) != FALSE && belongs, "queried process remains member of exact owned Job");
    // 先固定原查询 HANDLE 并读取 image/exit；Toolhelp 放在之后，避免错过短命成员。
    std::array<wchar_t, kPathCapacity> image{}; DWORD length = static_cast<DWORD>(image.size()), exit = STILL_ACTIVE;
    if (QueryFullProcessImageNameW(process->value, 0, image.data(), &length)) {
        const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, image.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        Require(bytes > 0, "owned Job member image UTF-8 identity");
        std::string utf8(static_cast<std::size_t>(bytes), '\0');
        Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, image.data(), static_cast<int>(length), utf8.data(), bytes, nullptr, nullptr) == bytes,
            "owned Job member image identity conversion");
        std::cout << " image=" << utf8;
    } else { const auto imageError = GetLastError(); std::cout << " image-unavailable-error=" << imageError; }
    if (GetExitCodeProcess(process->value, &exit)) std::cout << " exit=" << exit;
    std::cout << '\n';
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    Require(snapshot.value != INVALID_HANDLE_VALUE, "readonly process snapshot for owned Job member parent identity");
    DWORD parent = 0; bool parentFound = false; PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.value, &entry)) {
        do { if (entry.th32ProcessID == pid) { parent = entry.th32ParentProcessID; parentFound = true; break; } } while (Process32NextW(snapshot.value, &entry));
    }
    std::cout << "Owned fixture Job member parent pid=" << pid << " parent=" << (parentFound ? std::to_string(parent) : "unknown") << '\n';
    return process;
}
void RecordOwnedJobMembers(HANDLE job, const char* phase) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    Require(QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) != FALSE,
        "owned fixture Job identity accounting query");
    struct Members { DWORD assigned = 0, listed = 0; ULONG_PTR ids[16]{}; } members;
    const auto queryStarted = GetTickCount64();
    while (true) {
        members = {}; DWORD returnedBytes = 0;
        SetLastError(ERROR_SUCCESS);
        const auto queried = QueryInformationJobObject(job, JobObjectBasicProcessIdList, &members, sizeof(members), &returnedBytes);
        const auto error = queried ? ERROR_SUCCESS : GetLastError();
        if (queried && members.assigned <= 16 && members.listed <= 16 && members.assigned == members.listed) break;
        std::cout << "Owned fixture Job member query incomplete phase=" << phase << " api=" << (queried ? "true" : "false")
            << " error=" << error << " bytes=" << returnedBytes << " assigned=" << members.assigned << " listed=" << members.listed << '\n';
        Require(members.assigned <= 16 && members.listed <= 16 && members.listed <= members.assigned,
            "owned fixture Job member query stays inside fixed identity budget");
        Require(queried != FALSE || error == ERROR_MORE_DATA, "owned fixture Job member API error is not an incomplete-list retry");
        Require(GetTickCount64() - queryStarted < 500, "bounded complete owned fixture Job member query retry deadline");
        // 只重取不完整快照；不会把 partial 或查询失败转为成功。
        Sleep(10);
    }
    std::cout << "Owned fixture Job members phase=" << phase << " active=" << accounting.ActiveProcesses
        << " total=" << accounting.TotalProcesses << " listed=" << members.listed << '\n';
    if (members.listed == 0) return;
    for (DWORD index = 0; index < members.listed; ++index) {
        const auto pid = static_cast<DWORD>(members.ids[index]);
        (void)OpenAndRecordOwnedJobMember(job, pid, phase);
    }
}
struct OwnedJobNotificationCapture {
    HANDLE job;
    Handle port;
    std::vector<std::pair<DWORD, std::unique_ptr<Handle>>> processes;
    unsigned notifications = 0;
    explicit OwnedJobNotificationCapture(HANDLE ownedJob) : job(ownedJob), port(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)) {
        Require(port.value != nullptr, "owned invalid-case completion port creation");
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
        association.CompletionKey = job; association.CompletionPort = port.value;
        Require(SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &association, sizeof(association)) != FALSE,
            "associate only fresh owned invalid-case Job with own completion port");
    }
    bool Poll(DWORD waitMs) {
        DWORD message = 0; ULONG_PTR key = 0; LPOVERLAPPED payload = nullptr;
        const auto received = GetQueuedCompletionStatus(port.value, &message, &key, &payload, waitMs);
        const auto error = GetLastError();
        if (!received && !payload && error == WAIT_TIMEOUT) return false;
        Require(received != FALSE && key == reinterpret_cast<ULONG_PTR>(job), "notification belongs to exact owned Job completion key");
        Require(++notifications <= 128, "owned Job diagnostic notification budget");
        const auto pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(payload));
        if (message == JOB_OBJECT_MSG_NEW_PROCESS) {
            Require(pid != 0 && processes.size() < 16, "bounded owned Job NEW_PROCESS identity capture");
            const auto found = std::find_if(processes.begin(), processes.end(), [&](const auto& item) { return item.first == pid; });
            if (found == processes.end()) processes.emplace_back(pid, OpenAndRecordOwnedJobMember(job, pid, "completion-new-process"));
        }
        std::cout << "Owned fixture Job notification message=" << message << " pid=" << pid << '\n';
        return true;
    }
    void Drain() {
        for (unsigned index = 0; index < 64; ++index) if (!Poll(0)) return;
        throw std::runtime_error("owned Job diagnostic queue drain budget exceeded");
    }
    void WaitTerminal(HANDLE bridge) {
        const auto started = GetTickCount64();
        while (true) {
            (void)Poll(1);
            const auto status = WaitForSingleObject(bridge, 0);
            if (status == WAIT_OBJECT_0) { Drain(); return; }
            Require(status == WAIT_TIMEOUT && GetTickCount64() - started < 8000, "bounded diagnostic bridge terminal wait with live owned Job notifications");
        }
    }
    void ReleaseObservedHandles() {
        for (const auto& [pid, process] : processes) {
            DWORD exit = STILL_ACTIVE;
            if (process && GetExitCodeProcess(process->value, &exit)) std::cout << "Owned fixture retained member pid=" << pid << " exit-before-release=" << exit << '\n';
        }
        // 不让诊断持有的查询引用影响后续 ActiveProcesses==0 的硬条件。
        processes.clear();
    }
};
void RequireFreshJobEmpty(HANDLE job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    Require(QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) != FALSE
        && accounting.ActiveProcesses == 0 && accounting.TotalProcesses == 0, "fresh owned Job starts with zero active and total processes");
}
JOBOBJECT_BASIC_ACCOUNTING_INFORMATION WaitOwnedJobEmpty(HANDLE job) {
    const auto started = GetTickCount64();
    bool delayReported = false;
    while (true) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        Require(QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) != FALSE,
            "owned fixture Job accounting query must succeed");
        if (accounting.ActiveProcesses == 0) return accounting;
        if (!delayReported) {
            std::cout << "Owned fixture Job not yet empty: active=" << accounting.ActiveProcesses << " total=" << accounting.TotalProcesses << '\n';
            RecordOwnedJobMembers(job, "terminal-accounting-nonzero");
            delayReported = true;
        }
        if (GetTickCount64() - started >= 5000) {
            std::cout << "Owned fixture Job terminal deadline: active=" << accounting.ActiveProcesses << " total=" << accounting.TotalProcesses << '\n';
            throw std::runtime_error("owned fixture Job must contain zero processes within terminal deadline before cleanup");
        }
        Sleep(10);
    }
}
void CleanupOwned(const std::filesystem::path& base, const std::filesystem::path& work,
    const std::vector<std::pair<std::filesystem::path, std::string>>& files, RuntimeLayout layout) {
    const auto baseResolved = std::filesystem::weakly_canonical(base);
    const auto workResolved = std::filesystem::weakly_canonical(work);
    Require(PlainPath(baseResolved) && PlainPath(workResolved) && workResolved.parent_path() == baseResolved,
        "cleanup target is the exact newly created unique Temp case directory");
    // 先核完全部本轮文件身份，再逐项删除；不用递归删除，不触碰源fixture产品或回执。
    for (const auto& [path, hash] : files) {
        const auto relative = path.lexically_relative(work);
        Require(!relative.empty() && !relative.is_absolute() && *relative.begin() != L".." && PlainPath(path), "cleanup member stays inside owned case");
        Require(FileHash(path) == hash, "cleanup member still has its creation SHA");
    }
    for (const auto& [path, hash] : files) { (void)hash; Require(std::filesystem::remove(path), "remove only verified created fixture file"); }
    if (layout == RuntimeLayout::YdweTree) {
        Require(std::filesystem::remove(work / L"runtime" / L"plugin" / L"warcraft3"), "remove empty owned YDWE plugin directory");
        Require(std::filesystem::remove(work / L"runtime" / L"plugin"), "remove empty owned plugin parent");
        Require(std::filesystem::remove(work / L"runtime" / L"bin"), "remove empty owned bin directory");
    }
    Require(std::filesystem::remove(work / L"runtime"), "remove empty owned runtime directory");
    Require(std::filesystem::remove(work / L"Maps" / L"Test"), "remove empty owned map directory");
    Require(std::filesystem::remove(work / L"Maps"), "remove empty owned Maps directory");
    Require(std::filesystem::exists(work / L"receipt.json"), "small JSON evidence retained");
}
void Case(const std::filesystem::path& base, const std::filesystem::path& launcher,
    const std::filesystem::path& bootstrap, const std::filesystem::path& fakeWar3,
    const std::filesystem::path& fakeGame, const std::filesystem::path& fakeRuntime,
    const char* name, bool expectSuccess, bool missingPlugin, bool missingInitialize, bool noGame,
    std::wstring_view loadFlavor = {}, bool unicodeDirectory = false,
    RuntimeLayout layout = RuntimeLayout::Flat, bool explicitLayout = false) {
    std::wstring directoryName(name, name + std::strlen(name));
    if (unicodeDirectory) directoryName = L"宽字符路径-" + directoryName;
    const auto work = base / directoryName;
    Require(std::filesystem::create_directory(work), "unique case directory");
    const auto runtime = work / L"runtime";
    std::vector<std::pair<std::filesystem::path, std::string>> ownedFiles;
    const auto remember = [&](const std::filesystem::path& path) { ownedFiles.emplace_back(path, FileHash(path)); };
    std::filesystem::create_directory(runtime);
    if (layout == RuntimeLayout::YdweTree) {
        std::filesystem::create_directory(runtime / L"bin");
        std::filesystem::create_directories(runtime / L"plugin" / L"warcraft3");
    }
    std::filesystem::create_directories(work / L"Maps" / L"Test");
    std::filesystem::copy_file(fakeWar3, work / L"war3.exe");
    remember(work / L"war3.exe");
    std::filesystem::copy_file(fakeGame, work / L"Game.dll");
    remember(work / L"Game.dll");
    for (std::size_t index = 0; index < kModules.size(); ++index) {
        const auto* module = kModules[index];
        if (missingPlugin && std::wstring(module) == L"yd_lua_engine.dll") continue;
        const auto source = missingInitialize && std::wstring(module) == L"yd_jass_api.dll" ? fakeRuntime / L"ydbase.dll" : fakeRuntime / module;
        const auto target = runtime / ModuleRelativePath(layout, index);
        std::filesystem::copy_file(source, target);
        remember(target);
    }
    const std::wstring relative = noGame ? L"Maps\\Test\\nogame.w3x"
        : loadFlavor.empty() ? L"Maps\\Test\\smoke.w3x" : L"Maps\\Test\\" + std::wstring(loadFlavor) + L".w3x";
    std::ofstream(work / relative, std::ios::binary) << "original-fixture-no-warcraft-map";
    remember(work / relative);
    const auto receipt = work / L"receipt.json";
    auto command = Quote(launcher.wstring()) + L" --game " + Quote((work / L"war3.exe").wstring())
        + L" --map " + Quote(relative) + L" --runtime " + Quote(runtime.wstring())
        + L" --bootstrap " + Quote(bootstrap.wstring()) + L" --receipt " + Quote(receipt.wstring());
    if (layout == RuntimeLayout::YdweTree || explicitLayout)
        command += layout == RuntimeLayout::YdweTree ? L" --runtime-layout ydwe-tree" : L" --runtime-layout flat";
    // Job只拥有本测试新建的原创控制台进程；不启动/捕获作者游戏。
    Handle job(CreateJobObjectW(nullptr, nullptr));
    Require(job.value != nullptr, "fixture job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE, "fixture job limits");
    RequireFreshJobEmpty(job.value);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, work.c_str(), &startup, &process) != FALSE, "fixture launcher create");
    Handle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job.value, child.value)) {
        TerminateProcess(child.value, 1); WaitForSingleObject(child.value, 5000); throw std::runtime_error("fixture job assign");
    }
    Require(ResumeThread(thread.value) != static_cast<DWORD>(-1), "fixture resume");
    Require(WaitForSingleObject(child.value, 22000) == WAIT_OBJECT_0, "fixture terminal timeout");
    DWORD exit = STILL_ACTIVE;
    Require(GetExitCodeProcess(child.value, &exit) != FALSE, "fixture exit");
    ReleaseTerminalProcessHandles(child, thread);
    const auto text = Read(receipt);
    Require(text.find("\"schema\":\"audastra-warcraft-launch-v1\"") != std::string::npos, "receipt schema");
    Require(text.find("\"gameRuntimeVerified\":false") != std::string::npos, "fixture cannot claim gameplay");
    Require(text.find(layout == RuntimeLayout::YdweTree ? "\"runtimeLayout\":\"ydwe-tree\"" : "\"runtimeLayout\":\"flat\"")
        != std::string::npos, "receipt reports the exact selected runtime layout");
    if (expectSuccess) {
        Require(exit == 0, "valid runtime bridge exit");
        Require(text.find("\"runtimeInitialized\":true") != std::string::npos, "runtime initialized");
        Require(text.find("\"fixturePluginBits\":3") != std::string::npos, "both Initialize actually executed");
        Require(text.find("\"hookInstalled\":true") != std::string::npos, "early import hook");
        Require(text.find("\"gameObserved\":true") != std::string::npos, "Game.dll observed");
        if (layout == RuntimeLayout::YdweTree) {
            Require(text.find("bin\\\\ydbase.dll") != std::string::npos && text.find("plugin\\\\warcraft3\\\\yd_jass_api.dll") != std::string::npos,
                "actual loaded module paths retain the two readonly YDWE directories");
            Require(!std::filesystem::exists(runtime / L"ydbase.dll") && !std::filesystem::exists(runtime / L"yd_jass_api.dll"),
                "bootstrap loads the original two-level paths without flattening or copying DLLs");
        }
        if (loadFlavor == L"concurrent") {
            Require(text.find("\"fixtureConcurrentPass\":2") != std::string::npos, "both concurrent Game enters see fully initialized runtime");
            Require(text.find("\"fixtureConcurrentWaitObserved\":1") != std::string::npos, "second Game loader really raced inside first Initialize");
        }
    } else {
        Require(exit != 0, "unconfirmed runtime must fail");
        Require(text.find("\"runtimeInitialized\":false") != std::string::npos, "no false runtime success");
        if (missingPlugin) Require(text.find("runtime-preflight") != std::string::npos, "missing plugin preflight");
        if (missingInitialize) Require(text.find("runtime-initialize-export") != std::string::npos, "missing export caught in child");
        if (noGame) Require(text.find("runtime-initialization-unconfirmed") != std::string::npos, "missing Game.dll observation fails closed");
        if (loadFlavor == L"hang") {
            Require(text.find("runtime-initialization-timeout") != std::string::npos, "unobserved live process has bounded handshake");
            Require(text.find("\"gameExitCode\":" + std::to_string(kRuntimeFailure)) != std::string::npos, "timed out original target is confirmed terminated");
        }
    }
    (void)WaitOwnedJobEmpty(job.value);
    CleanupOwned(base, work, ownedFiles, layout);
    std::cout << "PASS " << name << " (receipt in owned task directory)\n";
}
// 裸模式：不加载扩展运行库，只装测试钩子。原创 Game.dll 不是 1.27a，后台运行钩子必须拒绝安装；
// 窗口隐身与游戏版本无关，原创游戏建的顶层窗口必须被隐身、不进任务栏、不激活。
// 静音同样与版本无关：要求静音时 Miles 替身打开声卡必须失败，没要求时必须照常成功。
enum class BareHook { None, BackgroundPlay, WindowCloak };
void BareCase(const std::filesystem::path& base, const std::filesystem::path& launcher,
    const std::filesystem::path& bootstrap, const std::filesystem::path& fakeWar3,
    const std::filesystem::path& fakeGame, const char* name, BareHook hook) {
    const auto work = base / std::wstring(name, name + std::strlen(name));
    Require(std::filesystem::create_directory(work), "unique bare case directory");
    std::vector<std::pair<std::filesystem::path, std::string>> ownedFiles;
    const auto remember = [&](const std::filesystem::path& path) { ownedFiles.emplace_back(path, FileHash(path)); };
    std::filesystem::create_directory(work / L"runtime");
    std::filesystem::create_directories(work / L"Maps" / L"Test");
    std::filesystem::copy_file(fakeWar3, work / L"war3.exe");
    remember(work / L"war3.exe");
    std::filesystem::copy_file(fakeGame, work / L"Game.dll");
    remember(work / L"Game.dll");
    std::filesystem::copy_file(fakeWar3.parent_path() / L"Mss32.dll", work / L"Mss32.dll");
    remember(work / L"Mss32.dll");
    const std::wstring relative = hook == BareHook::WindowCloak ? L"Maps\\Test\\cloak.w3x" : L"Maps\\Test\\smoke.w3x";
    std::ofstream(work / relative, std::ios::binary) << "original-fixture-no-warcraft-map";
    remember(work / relative);
    const auto receipt = work / L"receipt.json";
    const auto prefix = Quote(launcher.wstring()) + L" --game " + Quote((work / L"war3.exe").wstring())
        + L" --map " + Quote(relative) + L" --bootstrap " + Quote(bootstrap.wstring()) + L" --receipt " + Quote(receipt.wstring())
        + L" --runtime-mode bare";
    const auto run = [&](std::wstring command, DWORD timeout) {
        Handle job(CreateJobObjectW(nullptr, nullptr));
        Require(job.value != nullptr, "bare fixture job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE, "bare fixture job limits");
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        Require(CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, work.c_str(), &startup, &process) != FALSE, "bare launcher create");
        Handle child(process.hProcess), thread(process.hThread);
        if (!AssignProcessToJobObject(job.value, child.value)) {
            TerminateProcess(child.value, 1); WaitForSingleObject(child.value, 5000); throw std::runtime_error("bare fixture job assign");
        }
        Require(ResumeThread(thread.value) != static_cast<DWORD>(-1), "bare fixture resume");
        Require(WaitForSingleObject(child.value, timeout) == WAIT_OBJECT_0, "bare fixture terminal timeout");
        DWORD exit = STILL_ACTIVE;
        Require(GetExitCodeProcess(child.value, &exit) != FALSE, "bare fixture exit");
        ReleaseTerminalProcessHandles(child, thread);
        (void)WaitOwnedJobEmpty(job.value);
        return exit;
    };
    // 裸模式不接受扩展运行库目录或布局；参数错误不建回执。
    Require(run(prefix + L" --runtime " + Quote((work / L"runtime").wstring()), 5000) == 64 && !std::filesystem::exists(receipt),
        "bare mode rejects an extension runtime directory");
    Require(run(prefix + L" --runtime-layout flat", 5000) == 64 && !std::filesystem::exists(receipt), "bare mode rejects a runtime layout");
    Require(run(prefix + L" --background-play on", 5000) == 64 && !std::filesystem::exists(receipt), "background play accepts only explicit enabled");
    Require(run(prefix + L" --game-nosound on", 5000) == 64 && !std::filesystem::exists(receipt), "nosound accepts only explicit enabled");
    // 静音不走游戏命令行（1.27a 不认 -nosound，会连带丢掉 -loadfile），由 bootstrap 让 Miles 替身打不开声卡。
    const bool muted = hook != BareHook::None;
    auto command = prefix;
    if (muted) command += L" --game-nosound enabled";
    if (hook == BareHook::BackgroundPlay) command += L" --background-play enabled";
    if (hook == BareHook::WindowCloak) command += L" --window-cloak enabled";
    const auto exit = run(command, 22000);
    const auto text = Read(receipt);
    Require(text.find("\"runtimeMode\":\"bare\"") != std::string::npos, "receipt reports bare mode");
    Require(text.find("\"gameRuntimeVerified\":false") != std::string::npos, "bare fixture cannot claim gameplay");
    Require(text.find("\"fixturePluginBits\":0") != std::string::npos, "bare mode loads no extension runtime");
    Require(text.find("\"-window\",\"-loadfile\"") != std::string::npos && text.find("-nosound") == std::string::npos,
        "the game argv never carries -nosound");
    if (hook != BareHook::BackgroundPlay) {
        Require(exit == 0, "bare launcher exit (the fake game checked the sound driver state itself)");
        Require(text.find(muted ? "\"gameNoSound\":true,\"gameNoSoundInstalled\":true" : "\"gameNoSound\":false,\"gameNoSoundInstalled\":false")
            != std::string::npos, "mute is installed exactly when requested");
        Require(text.find("\"runtimeInitialized\":true") != std::string::npos && text.find("\"gameObserved\":true") != std::string::npos,
            "bare mode still observes Game.dll before reporting ready");
        Require(text.find("\"backgroundPlay\":false") != std::string::npos && text.find("\"backgroundPlayInstalled\":false") != std::string::npos,
            "background play stays off unless requested");
        if (hook == BareHook::WindowCloak)
            Require(text.find("\"windowCloak\":true") != std::string::npos && text.find("\"windowCloakInstalled\":true") != std::string::npos
                && text.find("\"cloakedWindows\":1") != std::string::npos, "the game's top-level window was cloaked before it showed");
        else
            Require(text.find("\"windowCloak\":false") != std::string::npos && text.find("\"cloakedWindows\":0") != std::string::npos,
                "window cloak stays off unless requested");
    } else {
        Require(exit != 0, "background play on a non-1.27a Game.dll must fail");
        Require(text.find("background-play-game-identity") != std::string::npos, "identity mismatch is reported at its own stage");
        Require(text.find("\"backgroundPlay\":true") != std::string::npos && text.find("\"backgroundPlayInstalled\":false") != std::string::npos,
            "no false background play success");
        Require(text.find("\"runtimeInitialized\":false") != std::string::npos, "failed hook keeps the runtime unconfirmed");
    }
    CleanupOwned(base, work, ownedFiles, RuntimeLayout::Flat);
    std::cout << "PASS " << name << " (receipt in owned task directory)\n";
}
void ProbeCase(const std::filesystem::path& base, const std::filesystem::path& launcher,
    const std::filesystem::path& bootstrap, const std::filesystem::path& fakeWar3,
    const std::filesystem::path& fakeGame, const std::filesystem::path& fakeRuntime, const wchar_t* flavor,
    bool succeeds, const char* expectedFailure = nullptr) {
    const auto work = base / flavor;
    std::string label;
    for (const auto c : std::wstring_view(flavor)) label.push_back(static_cast<char>(c));
    std::cout << "START isolated Lua probe fixture " << label << '\n';
    Require(std::filesystem::create_directory(work), "unique probe case directory");
    const auto sandbox = work / L"sandbox", runtime = work / L"readonly-installation";
    std::filesystem::create_directories(sandbox / L"Maps" / L"Test");
    std::filesystem::create_directories(runtime / L"bin");
    std::filesystem::create_directories(runtime / L"plugin" / L"warcraft3");
    std::filesystem::create_directory(runtime / L"logs");
    std::vector<std::pair<std::filesystem::path, std::string>> files;
    const auto remember = [&](const std::filesystem::path& file) { files.emplace_back(file, FileHash(file)); };
    std::filesystem::copy_file(fakeWar3, sandbox / L"war3.exe"); remember(sandbox / L"war3.exe");
    std::filesystem::copy_file(fakeGame, sandbox / L"Game.dll"); remember(sandbox / L"Game.dll");
    for (std::size_t index = 0; index < kModules.size(); ++index) {
        const auto target = runtime / ModuleRelativePath(RuntimeLayout::YdweTree, index);
        std::filesystem::copy_file(fakeRuntime / kModules[index], target); remember(target);
    }
    const std::wstring relative = L"Maps\\Test\\" + std::wstring(flavor) + L".w3x";
    std::ofstream(sandbox / relative, std::ios::binary) << "original-no-game-probe-fixture"; remember(sandbox / relative);
    const auto script = work / L"original-probe.lua";
    std::ofstream(script, std::ios::binary) << "return 'original fixture; no Lua execution in this console test'"; remember(script);
    const auto external = work / L"external-protected.txt";
    const auto alias = sandbox / L"Maps" / L"Test" / L"aliased-write.txt";
    const bool hardlink = std::wstring_view(flavor) == L"probe-hardlink" || std::wstring_view(flavor) == L"probe-blocked-alias";
    const bool blocked = std::wstring_view(flavor).starts_with(L"probe-blocked-");
    const bool two = std::wstring_view(flavor) == L"probe-blocked-two" || std::wstring_view(flavor) == L"probe-blocked-third";
    const bool invalid = std::wstring_view(flavor) == L"probe-blocked-duplicate" || std::wstring_view(flavor) == L"probe-blocked-slash-duplicate"
        || std::wstring_view(flavor) == L"probe-blocked-short-duplicate" || std::wstring_view(flavor) == L"probe-blocked-capacity"
        || std::wstring_view(flavor) == L"probe-blocked-invalid";
    const auto externalSecond = work / L"external-second.txt";
    const std::string externalBytes = "original external fixture bytes must never be truncated";
    if (hardlink || blocked) {
        std::ofstream(external, std::ios::binary) << externalBytes; remember(external);
        if (hardlink) Require(CreateHardLinkW(alias.c_str(), external.c_str(), nullptr) != FALSE, "original alias inside allowed sandbox to external owned fixture");
    }
    if (two) { std::ofstream(externalSecond, std::ios::binary) << externalBytes; remember(externalSecond); }
    std::array<UCHAR, 16> random{};
    Require(BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "probe nonce generation");
    constexpr wchar_t hex[] = L"0123456789abcdef"; std::wstring nonce;
    for (const auto byte : random) { nonce += hex[byte >> 4]; nonce += hex[byte & 15]; }
    const auto desktopName = L"AudastraRuntimeProbe-" + nonce;
    const auto desktopPath = L"WinSta0\\" + desktopName;
    struct Desktop { HDESK value; ~Desktop() { if (value) CloseDesktop(value); } } desktop{
        CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_ENUMERATE, nullptr)};
    Require(desktop.value != nullptr, "owned isolated probe desktop; never switched input desktop");
    const auto receipt = work / L"receipt.json";
    const auto wideHash = [](const std::string& value) { return std::wstring(value.begin(), value.end()); };
    auto command = Quote(launcher.wstring()) + L" --game " + Quote((sandbox / L"war3.exe").wstring())
        + L" --map " + Quote(relative) + L" --runtime " + Quote(runtime.wstring()) + L" --runtime-layout ydwe-tree"
        + L" --bootstrap " + Quote(bootstrap.wstring()) + L" --receipt " + Quote(receipt.wstring())
        + L" --lua-api-probe " + nonce + L" --probe-map-sha256 " + wideHash(FileHash(sandbox / relative))
        + L" --probe-script " + Quote(script.wstring()) + L" --probe-script-sha256 " + wideHash(FileHash(script))
        + L" --desktop " + desktopPath + L" --probe-timeout-ms 1000";
    if (blocked) command += L" --probe-nonfatal-blocked-path " + Quote(external.wstring());
    if (two) command += L" --probe-nonfatal-blocked-path " + Quote(externalSecond.wstring());
    if (std::wstring_view(flavor) == L"probe-blocked-duplicate") command += L" --probe-nonfatal-blocked-path " + Quote(external.wstring());
    if (std::wstring_view(flavor) == L"probe-blocked-slash-duplicate") {
        auto slashPath = external.wstring(); std::replace(slashPath.begin(), slashPath.end(), L'\\', L'/');
        Require(slashPath != external.wstring(), "slash spelling is different text for the same original owned file");
        command += L" --probe-nonfatal-blocked-path " + Quote(slashPath);
    }
    if (std::wstring_view(flavor) == L"probe-blocked-short-duplicate") {
        std::array<wchar_t, kPathCapacity> shortPath{};
        const auto length = GetShortPathNameW(external.c_str(), shortPath.data(), static_cast<DWORD>(shortPath.size()));
        Require(length > 0 && length < shortPath.size(), "query original owned fixture short-name spelling; volume may preserve long name");
        const std::wstring spelling(shortPath.data(), length);
        std::cout << "Owned fixture 8.3 spelling distinct=" << (EqualPath(external.wstring(), spelling) ? "false" : "true") << '\n';
        if (EqualPath(external.wstring(), spelling)) std::cout << "No distinct 8.3 alias supplied by this volume; short-name alias branch is not covered.\n";
        command += L" --probe-nonfatal-blocked-path " + Quote(spelling);
    }
    if (std::wstring_view(flavor) == L"probe-blocked-capacity")
        for (int index = 0; index < 4; ++index) command += L" --probe-nonfatal-blocked-path " + Quote((work / (L"extra-" + std::to_wstring(index))).wstring());
    if (std::wstring_view(flavor) == L"probe-blocked-invalid") command += L" --probe-nonfatal-blocked-path relative-invalid.bin";
    Handle job(CreateJobObjectW(nullptr, nullptr)); Require(job.value != nullptr, "owned probe fixture job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE, "probe fixture kill-on-close job");
    RequireFreshJobEmpty(job.value);
    if (invalid) RecordOwnedJobMembers(job.value, "fresh-invalid-before-create");
    // 通知只用于固定的异常成员身份诊断；完整 suite 的 33 项和硬门禁不变。
    std::unique_ptr<OwnedJobNotificationCapture> notifications;
    if (std::wstring_view(flavor) == L"probe-blocked-duplicate") notifications = std::make_unique<OwnedJobNotificationCapture>(job.value);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.lpDesktop = const_cast<wchar_t*>(desktopPath.c_str());
    PROCESS_INFORMATION process{};
    // 非法参数只验证 bridge 前置拒绝；避免控制台宿主成为额外的 Job 成员。
    // 普通有效夹具继续采用原 CREATE_NO_WINDOW；两个控制台标志不组合。
    const DWORD consoleFlags = invalid ? DETACHED_PROCESS : CREATE_NO_WINDOW;
    Require(CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | consoleFlags | BELOW_NORMAL_PRIORITY_CLASS, nullptr, sandbox.c_str(), &startup, &process) != FALSE, "probe fixture isolated bridge create");
    Handle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job.value, child.value)) {
        TerminateProcess(child.value, 1); WaitForSingleObject(child.value, 5000); throw std::runtime_error("probe fixture job assign");
    }
    if (invalid) RecordOwnedJobMembers(job.value, "invalid-before-resume");
    if (notifications) notifications->Drain();
    Require(ResumeThread(thread.value) != static_cast<DWORD>(-1), "probe fixture resume");
    if (notifications) notifications->WaitTerminal(child.value);
    else Require(WaitForSingleObject(child.value, 8000) == WAIT_OBJECT_0, "bounded probe fixture terminal");
    DWORD exit = STILL_ACTIVE; Require(GetExitCodeProcess(child.value, &exit) != FALSE, "probe fixture exit query");
    if (invalid) RecordOwnedJobMembers(job.value, "invalid-bridge-terminal-before-release");
    ReleaseTerminalProcessHandles(child, thread);
    if (notifications) notifications->ReleaseObservedHandles();
    const auto text = std::filesystem::exists(receipt) ? Read(receipt) : std::string{};
    if (invalid) {
        Require(exit == 64 && text.empty(), "duplicate/overcapacity/illegal opt-in rejects before game/receipt creation");
    } else {
        Require(text.find("\"gameRuntimeVerified\":false") != std::string::npos, "console probe fixture is not gameplay evidence");
        Require(text.find("\"writeGuardInstalled\":true") != std::string::npos, "actual native write guard installed in child");
        if (succeeds) {
            Require(exit == 0 && text.find("\"captured\":true") != std::string::npos, "actual cross-process probe capture");
            Require(text.find("OriginalFixtureFunction") != std::string::npos && text.find("enumerationComplete\\\":false") != std::string::npos,
                "fixture observations stay partial and retain actual returned fields");
            if (blocked) {
                Require(text.find("\"writeDenied\":true") != std::string::npos && text.find(two ? "\"blockedWriteCount\":2" : "\"blockedWriteCount\":1") != std::string::npos
                    && text.find("\"fatalWriteCount\":0") != std::string::npos && text.find("\"ntStatus\":3221225506") != std::string::npos
                    && text.find("\"originalNtDispatched\":false") != std::string::npos && text.find("\"nonfatal\":true") != std::string::npos,
                    "exact explicit request stayed denied before original dispatch while capture continued");
            } else Require(text.find("\"writeDenied\":false") != std::string::npos, "valid probe did not request external writes");
        } else {
            Require(exit != 0 && text.find("\"captured\":false") != std::string::npos, "failed probe never publishes an API inventory");
            Require(expectedFailure && text.find(expectedFailure) != std::string::npos, "specific probe failure stage retained");
        }
    }
    Require(!std::filesystem::exists(work / L"forbidden-write.txt") && !std::filesystem::exists(runtime / L"malicious-dllmain.txt"),
        "outside-sandbox creation was blocked before the filesystem changed");
    Require(!std::filesystem::exists(runtime / L"logs" / L"war3.log"), "original runtime installation log was not created");
    Require(!std::filesystem::exists(runtime / L"forbidden-author.txt"), "author installation path remains rejected despite exact external opt-in");
    if (blocked) {
        Require(Read(external) == externalBytes, "nonfatal-blocked original external bytes unchanged before any cleanup");
        if (two) Require(Read(externalSecond) == externalBytes, "second exact blocked path retained original bytes");
        if (std::wstring_view(flavor) == L"probe-blocked-overflow")
            Require(text.find("\"blockedWriteOverflow\":true") != std::string::npos, "bounded blocked ledger overflow is fatal");
    }
    if (std::wstring_view(flavor) == L"probe-redirect") {
        const auto log = (sandbox / relative).parent_path() / L"audastra-runtime-probe-war3.log";
        Require(Read(log) == "original isolated fixture log" && text.find("\"logRedirects\":1") != std::string::npos, "YDWE log was redirected into owned sandbox");
        remember(log);
    }
    if (std::wstring_view(flavor) == L"probe-sandbox") {
        const auto log = (sandbox / relative).parent_path() / L"owned-write.txt";
        Require(Read(log) == "original isolated fixture log", "owned sandbox writes remain usable"); remember(log);
    }
    const auto accounting = WaitOwnedJobEmpty(job.value);
    if (invalid) Require(accounting.TotalProcesses == 1, "invalid native parameter fixture never created a game child; only bridge joined this Job");
    if (hardlink) {
        Require(Read(external) == externalBytes, "external hardlink bytes unchanged before any cleanup; CREATE_ALWAYS was rejected before truncation");
        Require((blocked || text.find("aliased-write.txt") != std::string::npos) && text.find("\"writeDenied\":true") != std::string::npos,
            "hardlink target is named in actual write guard failure");
        Require(std::filesystem::remove(alias), "remove exact owned alias name after Job terminal, never its external target");
    }
    for (const auto& [file, sha] : files) Require(FileHash(file) == sha, "all source fixture files retained creation SHA");
    for (const auto& [file, sha] : files) { (void)sha; Require(std::filesystem::remove(file), "remove exact owned probe fixture file"); }
    for (const auto& directory : {sandbox / L"Maps" / L"Test", sandbox / L"Maps", sandbox,
        runtime / L"plugin" / L"warcraft3", runtime / L"plugin", runtime / L"bin", runtime / L"logs", runtime}) {
        Require(std::filesystem::remove(directory), "remove only empty newly created probe fixture directories");
    }
    std::cout << "PASS isolated Lua probe fixture " << label << " (small receipt retained when created)\n";
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 6 && argc != 8) return 64;
    const bool onlyProbeCase = argc == 8;
    if (onlyProbeCase && (std::wstring_view(argv[6]) != L"--only-probe-case" || std::wstring_view(argv[7]) != L"probe-blocked-duplicate")) return 64;
    try {
        std::array<wchar_t, 4096> temp{};
        const auto length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
        Require(length > 0 && length < temp.size(), "temp directory");
        const auto base = std::filesystem::path(temp.data()) / (L"Audastra-runtime32-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        Require(std::filesystem::create_directory(base), "unique root");
        const std::filesystem::path launcher(argv[1]), bootstrap(argv[2]), war3(argv[3]), game(argv[4]), runtime(argv[5]);
        if (onlyProbeCase) {
            std::cout << "FILTERED diagnostic 1 case: probe-blocked-duplicate; this is not the default 33-case suite.\n";
            ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-duplicate", false);
            std::cout << "FILTERED 1/1 original diagnostic fixture passed; not a full-suite result.\n";
            return 0;
        }
        Case(base, launcher, bootstrap, war3, game, runtime, "early-hook-and-initialize", true, false, false, false);
        Case(base, launcher, bootstrap, war3, game, runtime, "missing-runtime-dll", false, true, false, false);
        Case(base, launcher, bootstrap, war3, game, runtime, "missing-initialize-export", false, false, true, false);
        Case(base, launcher, bootstrap, war3, game, runtime, "missing-game-load", false, false, false, true);
        Case(base, launcher, bootstrap, war3, game, runtime, "absolute-ansi-game-load", true, false, false, false, L"absolute-a");
        Case(base, launcher, bootstrap, war3, game, runtime, "absolute-unicode-game-load", true, false, false, false, L"absolute-w", true);
        Case(base, launcher, bootstrap, war3, game, runtime, "relative-wide-game-load", true, false, false, false, L"relative-w");
        Case(base, launcher, bootstrap, war3, game, runtime, "bounded-unobserved-game", false, false, false, false, L"hang");
        Case(base, launcher, bootstrap, war3, game, runtime, "extended-ansi-game-load", true, false, false, false, L"extended-a");
        Case(base, launcher, bootstrap, war3, game, runtime, "concurrent-game-load", true, false, false, false, L"concurrent");
        Case(base, launcher, bootstrap, war3, game, runtime, "explicit-flat-layout", true, false, false, false, {}, false, RuntimeLayout::Flat, true);
        Case(base, launcher, bootstrap, war3, game, runtime, "ydwe-tree-hook-and-initialize", true, false, false, false, {}, true, RuntimeLayout::YdweTree);
        Case(base, launcher, bootstrap, war3, game, runtime, "ydwe-tree-missing-plugin", false, true, false, false, {}, false, RuntimeLayout::YdweTree);
        Case(base, launcher, bootstrap, war3, game, runtime, "ydwe-tree-missing-initialize", false, false, true, false, {}, false, RuntimeLayout::YdweTree);
        BareCase(base, launcher, bootstrap, war3, game, "bare-mode", BareHook::None);
        BareCase(base, launcher, bootstrap, war3, game, "bare-background-play-identity", BareHook::BackgroundPlay);
        BareCase(base, launcher, bootstrap, war3, game, "bare-window-cloak", BareHook::WindowCloak);
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-valid", true);
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-sandbox", true);
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-redirect", true);
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-outside", false, "lua-probe-write-outside-sandbox");
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-init-outside", false, "lua-probe-write-outside-sandbox");
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-nonce", false, "lua-probe-result-timeout");
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-hardlink", false, "lua-probe-write-outside-sandbox");
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-continue", true);
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-other", false, "lua-probe-write-outside-sandbox");
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-author", false, "lua-probe-write-outside-sandbox");
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-overflow", false, "lua-probe-write-outside-sandbox");
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-alias", false, "lua-probe-write-outside-sandbox");
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-two", true);
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-third", false, "lua-probe-write-outside-sandbox");
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-duplicate", false);
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-slash-duplicate", false);
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-short-duplicate", false);
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-capacity", false);
        ProbeCase(base, launcher, bootstrap, war3, game, runtime, L"probe-blocked-invalid", false);
        std::cout << "33/33 original console fixtures passed; no Warcraft/YDWE/GUI/GPU executed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
