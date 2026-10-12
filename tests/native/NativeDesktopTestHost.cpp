// 原创 Win32 合同测试的隔离宿主；不链接产品、不进入输入桌面、不操作作者窗口。
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

void Require(bool condition, const char* operation) {
    if (!condition) throw std::runtime_error(std::string(operation) + " (Win32=" + std::to_string(GetLastError()) + ")");
}
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    void Close() {
        if (value && value != INVALID_HANDLE_VALUE) Require(CloseHandle(value) != FALSE, "close owned handle");
        value = nullptr;
    }
};
std::uint64_t FileTimeValue(FILETIME value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
std::uint64_t ProcessCreated(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    Require(GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE, "query process creation time");
    return FileTimeValue(created);
}
std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    Require(count > 0, "encode identity text");
    std::string result(static_cast<std::size_t>(count), '\0');
    Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) == count,
        "encode complete identity text");
    return result;
}
std::string Json(std::string_view value) {
    std::string result = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\') { result += '\\'; result += character; }
        else if (byte < 0x20) { result += "\\u00"; result += digits[byte >> 4]; result += digits[byte & 15]; }
        else result += character;
    }
    return result + '"';
}
std::wstring Quote(std::wstring_view value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(slashes * (character == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (character == L'"') result += L'\\';
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
std::wstring Nonce() {
    std::array<UCHAR, 16> bytes{};
    Require(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "create test nonce");
    std::wstring result;
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (const auto value : bytes) { result += digits[value >> 4]; result += digits[value & 15]; }
    return result;
}
std::wstring UserObjectName(HANDLE object) {
    Require(object != nullptr, "query user object handle");
    std::array<wchar_t, 256> name{};
    DWORD needed = 0;
    Require(GetUserObjectInformationW(object, UOI_NAME, name.data(), static_cast<DWORD>(sizeof(name)), &needed) != FALSE && name[0],
        "query user object name");
    return name.data();
}
std::wstring InputDesktopName() {
    const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    Require(desktop != nullptr, "open input Desktop for query");
    std::wstring name;
    try { name = UserObjectName(desktop); }
    catch (...) { CloseDesktop(desktop); throw; }
    Require(CloseDesktop(desktop) != FALSE, "close input Desktop query handle");
    return name;
}

// 某些外部进程的线程桌面句柄无法查询；改用输入桌面的窗口枚举取得独立归属证据。
// 只读桌面和窗口身份，不切换桌面、不设置焦点，也不把查询失败判成外部窗口。
std::wstring InputDesktopWindowName(HWND window, DWORD pid, DWORD tid) {
    struct Evidence {
        HWND window;
        DWORD pid, tid;
        std::size_t count = 0;
        bool found = false, budgetExceeded = false;
    } evidence{window, pid, tid};
    const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    Require(desktop != nullptr, "open input Desktop for window attribution");
    try {
        const auto name = UserObjectName(desktop);
        const auto enumerated = EnumDesktopWindows(desktop, [](HWND candidate, LPARAM state) -> BOOL {
            auto& item = *reinterpret_cast<Evidence*>(state);
            if (++item.count > 16384) {
                item.budgetExceeded = true;
                SetLastError(ERROR_NOT_ENOUGH_QUOTA);
                return FALSE;
            }
            if (candidate == item.window) {
                DWORD candidatePid = 0;
                const auto candidateTid = GetWindowThreadProcessId(candidate, &candidatePid);
                item.found = candidateTid == item.tid && candidatePid == item.pid;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&evidence));
        Require(enumerated != FALSE && !evidence.budgetExceeded, "enumerate input Desktop windows for attribution");
        Require(evidence.found, "foreground HWND/thread not proven on input Desktop");
        Require(InputDesktopName() == name, "input Desktop changed during window attribution");
        Require(CloseDesktop(desktop) != FALSE, "close window attribution Desktop query handle");
        return name;
    } catch (...) { CloseDesktop(desktop); throw; }
}

enum class Ownership { External, Owned, Unknown };
struct IdentityEvidence {
    DWORD pid = 0, tid = 0;
    std::uint64_t created = 0, retainedCreated = 0;
    bool queryComplete = false, windowStable = false, inputDesktop = false, everInJob = false;
};
Ownership Classify(const IdentityEvidence& evidence, DWORD hostPid) {
    if (!evidence.queryComplete || !evidence.pid || !evidence.tid || !evidence.created ||
        evidence.created != evidence.retainedCreated || !evidence.windowStable || !evidence.inputDesktop)
        return Ownership::Unknown;
    return evidence.everInJob || evidence.pid == hostPid ? Ownership::Owned : Ownership::External;
}
struct Attribution {
    std::size_t external = 0, owned = 0, unknown = 0;
    void Add(Ownership ownership) {
        switch (ownership) {
        case Ownership::External: ++external; break;
        case Ownership::Owned: ++owned; break;
        case Ownership::Unknown: ++unknown; break;
        }
    }
    bool Safe() const { return owned == 0 && unknown == 0; }
};
unsigned CheckAttributionContract() {
    IdentityEvidence identity{100, 101, 12345, 12345, true, true, true, true};
    Require(Classify(identity, 1) == Ownership::Owned, "complete owned fixture identity must be owned");
    identity.everInJob = false;
    Require(Classify(identity, 1) == Ownership::External, "complete external identity must be external");
    Require(Classify(identity, 100) == Ownership::Owned, "host identity must be owned");
    identity.queryComplete = false;
    Require(Classify(identity, 1) == Ownership::Unknown, "exited child without query evidence must be unknown");
    identity.queryComplete = true;
    ++identity.created;
    Require(Classify(identity, 1) == Ownership::Unknown, "PID creation mismatch must be unknown");
    --identity.created;
    identity.windowStable = false;
    Require(Classify(identity, 1) == Ownership::Unknown, "destroyed or reused HWND must be unknown");
    identity.windowStable = true;
    identity.inputDesktop = false;
    Require(Classify(identity, 1) == Ownership::Unknown, "unproven Desktop must be unknown");
    // 瞬时抢焦点后恢复、未知归属后恢复，都不得被末尾的外部窗口覆盖。
    Attribution transient;
    transient.Add(Ownership::Owned); transient.Add(Ownership::External);
    Require(!transient.Safe() && transient.owned == 1, "transient owned foreground must remain a failure");
    Attribution unknown;
    unknown.Add(Ownership::Unknown); unknown.Add(Ownership::External);
    Require(!unknown.Safe() && unknown.unknown == 1, "unknown foreground must remain a failure");
    Attribution external;
    external.Add(Ownership::External); external.Add(Ownership::External);
    Require(external.Safe() && external.external == 2, "external foreground must retain external attribution");
    return 10;
}

class ForegroundObserver {
public:
    Attribution attribution;
    bool hookInstalled = false, stopped = false, pumpHealthy = true;
    std::size_t events = 0;

    ForegroundObserver(HANDLE job, std::wstring inputDesktop, const fs::path& path)
        : job_(job), inputDesktop_(std::move(inputDesktop)), log_(path, std::ios::binary) {
        Require(log_.good(), "create foreground identity log");
    }
    ~ForegroundObserver() { if (hook_) UnhookWinEvent(hook_); current_ = nullptr; }
    void Start() {
        Require(current_ == nullptr, "single foreground observer");
        // Hook 与消息循环都在仍属于输入 Desktop 的这个线程；不创建可见窗口。
        Require(UserObjectName(GetThreadDesktop(GetCurrentThreadId())) == inputDesktop_, "observer must remain on original input Desktop");
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        current_ = this;
        hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, Callback, 0, 0, WINEVENT_OUTOFCONTEXT);
        Require(hook_ != nullptr, "install foreground WinEvent hook");
        hookInstalled = true;
    }
    void Sample(const char* stage) { Record(stage, GetForegroundWindow(), 0, 0); }
    void RetainOwnedRoot(DWORD pid) {
        const auto& identity = Retain(pid);
        Require(identity.everInJob, "retain assigned root process identity before resume");
    }
    std::size_t RetainedProcesses() const { return processes_.size(); }
    bool Pump(DWORD milliseconds, HANDLE process = nullptr) {
        const auto result = MsgWaitForMultipleObjects(process ? 1 : 0, process ? &process : nullptr, FALSE, milliseconds, QS_ALLINPUT);
        if (result == WAIT_FAILED) { pumpHealthy = false; throw std::runtime_error("foreground message wait failed"); }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { pumpHealthy = false; attribution.Add(Ownership::Unknown); continue; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return process && result == WAIT_OBJECT_0;
    }
    void DrainAndStop() {
        // Job 已清空后仍处理已排队的 out-of-context 事件，再关闭观察。
        const auto started = GetTickCount64();
        while (GetTickCount64() - started < 250) Pump(25);
        Sample("after-job-empty");
        Pump(0);
        Require(UnhookWinEvent(hook_) != FALSE, "remove foreground WinEvent hook");
        hook_ = nullptr;
        current_ = nullptr;
        log_.flush();
        Require(log_.good(), "write complete foreground identity log");
        stopped = true;
    }

private:
    HANDLE job_;
    std::wstring inputDesktop_;
    std::ofstream log_;
    HWINEVENTHOOK hook_ = nullptr;
    struct ProcessIdentity {
        Handle process;
        std::uint64_t created = 0;
        bool everInJob = false;
        explicit ProcessIdentity(HANDLE value) : process(value) {}
    };
    // 保留实际查询句柄至观察结束；不会把曾属于本 Job 的已退出进程再判成外部进程。
    std::map<DWORD, std::unique_ptr<ProcessIdentity>> processes_;
    inline static ForegroundObserver* current_ = nullptr;

    ProcessIdentity& Retain(DWORD pid) {
        const auto found = processes_.find(pid);
        if (found != processes_.end()) return *found->second;
        auto identity = std::make_unique<ProcessIdentity>(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        Require(identity->process.value != nullptr, "open foreground owner for query");
        identity->created = ProcessCreated(identity->process.value);
        BOOL member = FALSE;
        Require(IsProcessInJob(identity->process.value, job_, &member) != FALSE, "query foreground owner Job membership");
        identity->everInJob = member != FALSE;
        return *processes_.emplace(pid, std::move(identity)).first->second;
    }

    static void CALLBACK Callback(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD eventThread, DWORD eventTime) noexcept {
        if (!current_) return;
        ++current_->events;
        try { current_->Record("foreground-event", window, eventThread, eventTime); }
        catch (...) { current_->attribution.Add(Ownership::Unknown); current_->pumpHealthy = false; }
    }
    void Record(const char* stage, HWND window, DWORD eventThread, DWORD eventTime) {
        Require(events <= 4096, "bounded foreground identity event log");
        DWORD pid = 0, tid = 0;
        std::uint64_t created = 0;
        std::wstring desktop;
        const char* desktopEvidence = "unavailable";
        Ownership ownership = Ownership::Unknown;
        IdentityEvidence evidence;
        std::string reason;
        BOOL inJob = FALSE;
        try {
            Require(window != nullptr, "foreground HWND unavailable");
            tid = GetWindowThreadProcessId(window, &pid);
            Require(tid != 0 && pid != 0 && (!eventThread || tid == eventThread), "foreground HWND/thread identity unavailable or changed");
            auto& identity = Retain(pid);
            created = ProcessCreated(identity.process.value);
            Require(IsProcessInJob(identity.process.value, job_, &inJob) != FALSE, "recheck foreground owner Job membership");
            identity.everInJob = identity.everInJob || inJob != FALSE;
            const auto threadDesktop = GetThreadDesktop(tid);
            if (threadDesktop) {
                desktop = UserObjectName(threadDesktop);
                desktopEvidence = "thread-desktop-handle";
            } else {
                desktop = InputDesktopWindowName(window, pid, tid);
                desktopEvidence = "input-desktop-window-enumeration";
            }
            DWORD confirmedPid = 0;
            Require(GetWindowThreadProcessId(window, &confirmedPid) == tid && confirmedPid == pid,
                "foreground window changed while reading identity");
            Require(ProcessCreated(identity.process.value) == created, "foreground process identity changed during Desktop query");
            Require(desktop == inputDesktop_, "foreground event Desktop differs from observed input Desktop");
            evidence = {pid, tid, created, identity.created, true, true, true, identity.everInJob};
            ownership = Classify(evidence, GetCurrentProcessId());
            if (ownership == Ownership::Unknown) reason = "foreground identity is incomplete or reused";
        } catch (const std::exception& error) { reason = error.what(); }
        attribution.Add(ownership);
        const auto kind = ownership == Ownership::Owned ? "owned" : ownership == Ownership::External ? "external" : "unknown";
        log_ << "{\"stage\":" << Json(stage) << ",\"observedTick\":" << GetTickCount64()
             << ",\"eventTime\":" << eventTime << ",\"hwnd\":" << reinterpret_cast<std::uintptr_t>(window)
             << ",\"eventThread\":" << eventThread << ",\"pid\":" << pid << ",\"tid\":" << tid
             << ",\"processCreated\":" << created << ",\"desktop\":" << Json(Utf8(desktop))
             << ",\"desktopEvidence\":" << Json(desktopEvidence)
             << ",\"inExpectedJob\":" << (inJob ? "true" : "false") << ",\"ownership\":" << Json(kind)
             << ",\"everInExpectedJob\":" << (evidence.everInJob ? "true" : "false")
             << ",\"reason\":" << Json(reason) << "}\n";
        Require(log_.good(), "append foreground identity record");
    }
};

struct Resources {
    Handle job, process, thread;
    HDESK desktop = nullptr;
    bool assigned = false;
    ~Resources() {
        // 兜底回收也只操作本次持有的 Job/原始挂起进程句柄，不按名称或 PID 杀进程。
        if (assigned && job.value) TerminateJobObject(job.value, 71);
        else if (process.value && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) TerminateProcess(process.value, 71);
        if (process.value) WaitForSingleObject(process.value, 5000);
        if (desktop) CloseDesktop(desktop);
    }
    bool Empty() const {
        if (!job.value) return true;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION state{};
        Require(QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &state, sizeof(state), nullptr) != FALSE,
            "query owned Job active process count");
        return state.ActiveProcesses == 0;
    }
    void StopOwned() {
        if (assigned) Require(TerminateJobObject(job.value, 71) != FALSE, "terminate only owned Job");
        else if (process.value && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT)
            Require(TerminateProcess(process.value, 71) != FALSE, "terminate only original unassigned suspended process");
    }
    void Close() {
        thread.Close(); process.Close(); job.Close();
        if (desktop) { Require(CloseDesktop(desktop) != FALSE, "close private Desktop"); desktop = nullptr; }
    }
};
struct Attributes {
    std::vector<std::byte> buffer;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    explicit Attributes(const std::array<HANDLE, 2>& handles) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        Require(bytes != 0, "size explicit inherited handle list");
        buffer.resize(bytes);
        value = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
        Require(InitializeProcThreadAttributeList(value, 1, 0, &bytes) != FALSE, "initialize inherited handle list");
        if (!UpdateProcThreadAttribute(value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, const_cast<HANDLE*>(handles.data()), sizeof(handles), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(value); value = nullptr;
            throw std::runtime_error("set explicit inherited handle list failed");
        }
    }
    ~Attributes() { if (value) DeleteProcThreadAttributeList(value); }
};

fs::path LongPath(const fs::path& path) {
    const auto text = path.wstring();
    if (text.starts_with(L"\\\\?\\")) return path;
    if (text.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + text.substr(2);
    return L"\\\\?\\" + text;
}
void RemoveOwnedEvidence(const fs::path& root, const fs::path& temp, const std::wstring& token) {
    // 验证最终绝对路径、一次性目录身份及整个树没有重解析点，才进行递归删除。
    Require(root.is_absolute() && fs::weakly_canonical(root) == fs::weakly_canonical(temp) / (L"AudastraSynthetic-" + token), "owned TEMP cleanup path");
    const auto plain = [](const fs::path& path) {
        const auto attributes = GetFileAttributesW(LongPath(path).c_str());
        Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "cleanup path must not be a reparse point");
    };
    plain(root);
    std::ifstream owner(root / "owner.txt", std::ios::binary);
    std::string recorded;
    std::getline(owner, recorded);
    Require(recorded == Utf8(token), "owned TEMP cleanup nonce");
    owner.close();
    for (const auto& entry : fs::recursive_directory_iterator(LongPath(root))) plain(entry.path());
    Require(fs::remove_all(LongPath(root)) > 0, "remove only completed owned evidence");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 5 || std::wstring_view(argv[1]) != L"--powershell" || std::wstring_view(argv[3]) != L"--script") {
        std::cerr << "usage: native-desktop-test-host --powershell <pwsh> --script <synthetic-test.ps1>\n";
        return 2;
    }
    fs::path root, temp;
    std::wstring token, desktopName, jobName, inputBefore, inputAfter;
    std::vector<std::string> failures;
    DWORD childExit = STILL_ACTIVE, childPid = 0, childTid = 0;
    std::uint64_t childCreated = 0;
    bool jobEmpty = false, desktopStable = false, assigned = false;
    bool evidenceOwned = false, evidenceRemoved = false;
    unsigned attributionContractChecks = 0;
    Attribution observed;
    std::size_t events = 0, retainedProcesses = 0;
    bool observationComplete = false;
    try {
        attributionContractChecks = CheckAttributionContract();
        const auto powershell = fs::absolute(argv[2]).lexically_normal();
        const auto script = fs::absolute(argv[4]).lexically_normal();
        Require(fs::is_regular_file(powershell) && fs::is_regular_file(script), "explicit synthetic command inputs");
        temp = fs::canonical(fs::temp_directory_path());
        token = Nonce();
        root = temp / (L"AudastraSynthetic-" + token);
        Require(fs::create_directory(root), "create new evidence directory");
        evidenceOwned = true;
        { std::ofstream owner(root / "owner.txt", std::ios::binary); owner << Utf8(token); Require(owner.good(), "write evidence owner nonce"); }
        desktopName = L"AudastraSynthetic-" + token;
        jobName = L"Local\\AudastraSynthetic-" + token;
        inputBefore = InputDesktopName();
        Require(UserObjectName(GetThreadDesktop(GetCurrentThreadId())) == inputBefore, "host thread must observe active input Desktop");
        const auto station = UserObjectName(GetProcessWindowStation());
        Resources resources;
        resources.desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0,
            DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_ENUMERATE, nullptr);
        Require(resources.desktop != nullptr && UserObjectName(resources.desktop) == desktopName && desktopName != inputBefore, "create private nonce Desktop");
        SetLastError(ERROR_SUCCESS);
        resources.job.value = CreateJobObjectW(nullptr, jobName.c_str());
        const auto jobError = GetLastError();
        Require(resources.job.value != nullptr && jobError != ERROR_ALREADY_EXISTS, "create fresh owned Job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Require(SetInformationJobObject(resources.job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE, "owned Job kill-on-close without breakaway");
        ForegroundObserver observer(resources.job.value, inputBefore, root / "foreground.jsonl");
        observer.Start();
        observer.Sample("before-launch");
        try {
            SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            Handle output(CreateFileW((root / "child.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr));
            Require(output.value != INVALID_HANDLE_VALUE && input.value != INVALID_HANDLE_VALUE, "create owned child standard handles");
            const std::array<HANDLE, 2> inherited{input.value, output.value};
            Attributes attributes(inherited);
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            startup.StartupInfo.wShowWindow = SW_HIDE;
            startup.StartupInfo.hStdInput = input.value;
            startup.StartupInfo.hStdOutput = output.value;
            startup.StartupInfo.hStdError = output.value;
            auto desktopPath = station + L"\\" + desktopName;
            startup.StartupInfo.lpDesktop = desktopPath.data();
            startup.lpAttributeList = attributes.value;
            auto command = Quote(powershell.wstring()) + L" -NoLogo -NoProfile -NonInteractive -File " + Quote(script.wstring())
                + L" -WorkDirectory " + Quote((root / "fixtures").wstring())
                + L" -ExpectedDesktop " + Quote(desktopName) + L" -ExpectedJobName " + Quote(jobName);
            PROCESS_INFORMATION process{};
            Require(CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, root.c_str(), &startup.StartupInfo, &process) != FALSE,
                "create suspended synthetic process on private Desktop");
            resources.process.value = process.hProcess; resources.thread.value = process.hThread;
            childPid = process.dwProcessId; childTid = process.dwThreadId;
            childCreated = ProcessCreated(resources.process.value);
            Require(AssignProcessToJobObject(resources.job.value, resources.process.value) != FALSE, "assign original suspended process to owned Job");
            resources.assigned = assigned = true;
            observer.RetainOwnedRoot(childPid);
            Require(ResumeThread(resources.thread.value) != static_cast<DWORD>(-1), "resume only assigned synthetic process");
            output.Close(); input.Close();
            const auto started = GetTickCount64();
            while (!observer.Pump(50, resources.process.value)) {
                Require(GetTickCount64() - started < 100000, "synthetic test timeout");
            }
            Require(GetExitCodeProcess(resources.process.value, &childExit) != FALSE, "query original synthetic process exit");
            if (childExit != 0) failures.emplace_back("synthetic contract child returned " + std::to_string(childExit));
        } catch (const std::exception& error) { failures.emplace_back(error.what()); }
        try {
            if (!failures.empty()) resources.StopOwned();
            const auto started = GetTickCount64();
            while (!resources.Empty()) {
                if (GetTickCount64() - started >= 5000) {
                    failures.emplace_back("owned Job did not empty after child completion");
                    resources.StopOwned();
                    break;
                }
                observer.Pump(25);
            }
            const auto cleanupStart = GetTickCount64();
            while (!resources.Empty() && GetTickCount64() - cleanupStart < 5000) observer.Pump(25);
            jobEmpty = resources.Empty();
            if (!jobEmpty) failures.emplace_back("owned Job cleanup incomplete");
            if (!assigned && resources.process.value)
                Require(WaitForSingleObject(resources.process.value, 5000) == WAIT_OBJECT_0, "unassigned suspended process cleanup complete");
            inputAfter = InputDesktopName();
            desktopStable = inputAfter == inputBefore && UserObjectName(GetThreadDesktop(GetCurrentThreadId())) == inputBefore;
            if (!desktopStable) failures.emplace_back("input Desktop identity changed during synthetic contract");
            observer.DrainAndStop();
            observed = observer.attribution;
            events = observer.events;
            retainedProcesses = observer.RetainedProcesses();
            observationComplete = observer.hookInstalled && observer.stopped && observer.pumpHealthy;
            if (!observationComplete || !observed.Safe()) failures.emplace_back("foreground observation is owned, unknown, or incomplete");
            resources.Close();
        } catch (const std::exception& error) {
            observed = observer.attribution; events = observer.events;
            retainedProcesses = observer.RetainedProcesses();
            failures.emplace_back(error.what());
        }
        const auto childLog = root / "child.log";
        if (fs::exists(childLog)) {
            Require(fs::file_size(childLog) <= 4 * 1024 * 1024, "bounded synthetic output log");
            std::ifstream log(childLog, std::ios::binary);
            std::cout << log.rdbuf();
        }
    } catch (const std::exception& error) { failures.emplace_back(error.what()); }

    if (evidenceOwned && failures.empty()) {
        try {
            RemoveOwnedEvidence(root, temp, token);
            evidenceRemoved = true;
        } catch (const std::exception& error) { failures.emplace_back(error.what()); }
    }
    try {
        std::ostringstream summary;
        summary << "{\"schema\":\"audastra-synthetic-desktop-host-v1\",\"passed\":" << (failures.empty() ? "true" : "false")
            << ",\"attributionContractChecks\":" << attributionContractChecks
            << ",\"childPid\":" << childPid << ",\"childTid\":" << childTid << ",\"childCreated\":" << childCreated
            << ",\"childExit\":" << childExit << ",\"privateDesktop\":" << Json(Utf8(desktopName))
            << ",\"jobName\":" << Json(Utf8(jobName)) << ",\"assignedBeforeResume\":" << (assigned ? "true" : "false")
            << ",\"inputDesktopBefore\":" << Json(Utf8(inputBefore)) << ",\"inputDesktopAfter\":" << Json(Utf8(inputAfter))
            << ",\"inputDesktopStable\":" << (desktopStable ? "true" : "false") << ",\"jobEmpty\":" << (jobEmpty ? "true" : "false")
            << ",\"observationComplete\":" << (observationComplete ? "true" : "false") << ",\"foregroundEvents\":" << events
            << ",\"retainedProcessIdentities\":" << retainedProcesses
            << ",\"externalObservations\":" << observed.external << ",\"ownedObservations\":" << observed.owned
            << ",\"unknownObservations\":" << observed.unknown << ",\"evidenceRemoved\":" << (evidenceRemoved ? "true" : "false")
            << ",\"failures\":[";
        for (std::size_t i = 0; i < failures.size(); ++i) { if (i) summary << ','; summary << Json(failures[i]); }
        summary << "]}";
        const auto text = summary.str();
        Require(text.size() <= 16384, "bounded host identity summary");
        if (evidenceOwned && !evidenceRemoved) {
            std::ofstream file(root / "summary.local.json", std::ios::binary);
            file << text << '\n';
            Require(file.good(), "write preserved host summary");
        }
        // 成功时删除本次临时树，CTest 仍保留身份和观察计数，不仅是一行 PASS。
        std::cout << "NativeDesktopHostSummary " << text << '\n';
    } catch (const std::exception& error) { failures.emplace_back(error.what()); }
    for (const auto& failure : failures) std::cerr << "FAIL native Desktop host: " << failure << '\n';
    if (!failures.empty() && evidenceOwned && !evidenceRemoved) std::cerr << "Preserved owned evidence: " << Utf8(root.wstring()) << '\n';
    if (failures.empty()) std::cout << "PASS isolated Desktop host; foreground owned=0 unknown=0; owned Job empty\n";
    return failures.empty() ? 0 : 1;
}
