#include "ExternalProcess.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <array>
#include <map>

#ifdef _WIN32
#include <windows.h>
#include <appmodel.h>
#endif

namespace War3Map::Script::detail {

std::wstring QuoteArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') quoted.append(backslashes * 2 + 1, L'\\');
        else quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(c);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

#ifdef _WIN32
namespace {

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

// 当前环境复制一份，PATH 前面加上指定目录。
std::wstring EnvironmentBlock(const std::vector<std::filesystem::path>& prefix) {
    std::wstring block;
    std::wstring path;
    if (const auto* strings = GetEnvironmentStringsW(); strings != nullptr) {
        for (const wchar_t* entry = strings; *entry != L'\0'; entry += wcslen(entry) + 1) {
            const std::wstring text = entry;
            if (_wcsnicmp(text.c_str(), L"PATH=", 5) == 0) {
                path = text.substr(5);
                continue;
            }
            block += text;
            block.push_back(L'\0');
        }
        FreeEnvironmentStringsW(const_cast<wchar_t*>(strings));
    }
    std::wstring joined;
    for (const auto& directory : prefix) joined += directory.wstring() + L";";
    block += L"PATH=" + joined + path;
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

struct Attributes {
    std::vector<std::byte> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    DWORD desktopPolicy = PROCESS_CREATION_DESKTOP_APP_BREAKAWAY_ENABLE_PROCESS_TREE;
    std::string error;
    ~Attributes() { if (value) DeleteProcThreadAttributeList(value); }
    bool Init(HANDLE input, HANDLE output) {
        UINT32 packageLength = 0;
        const auto packageStatus = GetCurrentPackageFullName(&packageLength, nullptr);
        const bool packaged = packageStatus == ERROR_INSUFFICIENT_BUFFER;
        if (!packaged && packageStatus != APPMODEL_ERROR_NO_PACKAGE) {
            error = "无法核实外部工具的应用包继承状态（错误 " + std::to_string(packageStatus) + "）";
            return false;
        }
        const DWORD count = packaged ? 2u : 1u;
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, count, 0, &size);
        if (size == 0) return false;
        storage.resize(size);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list, count, 0, &size)) return false;
        value = list;
        handles = {input, output};
        if (!UpdateProcThreadAttribute(value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handles.data(), sizeof(handles), nullptr, nullptr)) return false;
        // 完整桌面工具不继承调用宿主的MSIX文件/DLL搜索语义，确保显式PATH生效。
        // 只设置该子进程树的桌面策略；原Job、预算、句柄与超时监督全部保留。
        if (packaged && !UpdateProcThreadAttribute(value, 0, PROC_THREAD_ATTRIBUTE_DESKTOP_APP_POLICY,
            &desktopPolicy, sizeof(desktopPolicy), nullptr, nullptr)) {
            error = "无法为外部桌面工具设置独立包语义（错误 " + std::to_string(GetLastError()) + "）";
            return false;
        }
        return true;
    }
    std::array<HANDLE, 2> handles{};
};

// Job 计数归零早于最后一个进程对象变为 signaled。保留真实句柄并逐个等待；
// completion 通知不保证送达，最后还须以 TotalProcesses 校验观察覆盖，缺失则拒绝成功。
struct ProcessTree {
    std::map<DWORD, HANDLE> observed;
    ~ProcessTree() { for (const auto& entry : observed) if (entry.second) CloseHandle(entry.second); }
    bool Observe(HANDLE job, DWORD pid) {
        if (const auto found = observed.find(pid); found != observed.end()) return found->second != nullptr;
        if (observed.size() >= 256) return false;
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) {
            if (GetLastError() != ERROR_INVALID_PARAMETER) return false;
            observed.emplace(pid, nullptr); // 对象已不存在，不能再执行或持有文件。
            return true;
        }
        BOOL member = FALSE;
        if (!IsProcessInJob(process, job, &member) || !member) { CloseHandle(process); return false; }
        observed.emplace(pid, process);
        return true;
    }
    bool Drain(HANDLE port, HANDLE job) {
        for (unsigned count = 0; count < 1024; ++count) {
            DWORD message = 0; ULONG_PTR key = 0; LPOVERLAPPED value = nullptr;
            if (!GetQueuedCompletionStatus(port, &message, &key, &value, 0)) return GetLastError() == WAIT_TIMEOUT;
            if (key != reinterpret_cast<ULONG_PTR>(job)) return false;
            if (message == JOB_OBJECT_MSG_NEW_PROCESS && !Observe(job, static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(value)))) return false;
            if (message == JOB_OBJECT_MSG_ACTIVE_PROCESS_LIMIT) return false;
        }
        return false;
    }
    bool WaitUntil(std::chrono::steady_clock::time_point deadline) const {
        for (const auto& entry : observed) {
            if (!entry.second) continue;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (WaitForSingleObject(entry.second, static_cast<DWORD>(std::max<std::int64_t>(0, remaining))) != WAIT_OBJECT_0) return false;
        }
        return true;
    }
};

} // namespace

ProcessOutcome RunProcess(const ProcessRequest& request) {
    ProcessOutcome outcome;
    if (request.timeout.count() <= 0 || request.timeout > std::chrono::hours(24) ||
        request.maxOutputBytes > 64ull * 1024 * 1024 || request.maxJobMemoryBytes == 0 ||
        request.maxJobMemoryBytes > 2ull * 1024 * 1024 * 1024) {
        outcome.error = "外部进程预算无效";
        return outcome;
    }
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    std::ofstream log(request.outputFile, std::ios::binary | std::ios::trunc);
    Handle readPipe, writePipe;
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
    if (!log || input.value == INVALID_HANDLE_VALUE || !CreatePipe(&readPipe.value, &writePipe.value, &inherit, 0) ||
        !SetHandleInformation(readPipe.value, HANDLE_FLAG_INHERIT, 0)) {
        outcome.error = "无法创建输出文件：" + request.outputFile.string();
        return outcome;
    }
    std::wstring commandLine = QuoteArgument(request.executable.wstring());
    for (const auto& argument : request.arguments) commandLine += L" " + QuoteArgument(argument);
    std::wstring environment = EnvironmentBlock(request.pathPrefix);

    Handle job(CreateJobObjectW(nullptr, nullptr));
    Handle completion(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{job.value, completion.value};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 32;
    limits.JobMemoryLimit = request.maxJobMemoryBytes;
    Attributes attributes;
    if (!job.value || !completion.value || !SetInformationJobObject(job.value, JobObjectAssociateCompletionPortInformation, &association, sizeof(association)) ||
        !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !attributes.Init(input.value, writePipe.value)) {
        outcome.error = attributes.error.empty() ? "无法建立外部进程Job/句柄隔离，未启动工具" : attributes.error;
        return outcome;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = writePipe.value;
    startup.StartupInfo.hStdError = writePipe.value;
    startup.lpAttributeList = attributes.value;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(request.executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, environment.data(),
        request.workingDirectory.empty() ? nullptr : request.workingDirectory.c_str(), &startup.StartupInfo, &process);
    if (!created) {
        outcome.error = "无法启动 " + request.executable.string() + "（错误 " + std::to_string(GetLastError()) + "）";
        return outcome;
    }
    Handle processHandle(process.hProcess);
    Handle threadHandle(process.hThread);
    ProcessTree tree;
    if (!AssignProcessToJobObject(job.value, process.hProcess) || !tree.Observe(job.value, process.dwProcessId) ||
        ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        outcome.error = "外部进程未能进入受监督状态（错误 " + std::to_string(error) + "）";
        return outcome;
    }
    outcome.started = true;
    CloseHandle(writePipe.value); writePipe.value = nullptr;
    const auto started = std::chrono::steady_clock::now();
    bool stopped = false;
    while (!stopped) {
        if (!tree.Drain(completion.value, job.value)) {
            outcome.supervisionFailed = true; outcome.error = "外部进程树观察或资源预算失败"; break;
        }
        // 每轮最多64KiB，持续写日志的子进程也必须让出时间核对超时/整个Job状态。
        for (unsigned chunk = 0; chunk < 16; ++chunk) {
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() != ERROR_BROKEN_PIPE) {
                    outcome.supervisionFailed = true; outcome.error = "无法读取外部进程输出"; stopped = true;
                }
                break;
            }
            if (available == 0) break;
            std::array<char, 4096> buffer{};
            DWORD received = 0;
            if (!::ReadFile(readPipe.value, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &received, nullptr)) {
                outcome.supervisionFailed = true; outcome.error = "外部进程输出读取失败"; stopped = true; break;
            }
            const auto kept = std::min<std::size_t>(received, request.maxOutputBytes - outcome.output.size());
            log.write(buffer.data(), static_cast<std::streamsize>(kept));
            outcome.output.append(buffer.data(), kept);
            if (!log) { outcome.supervisionFailed = true; outcome.error = "外部进程日志写入失败"; stopped = true; break; }
            if (kept != received) {
                outcome.outputLimitExceeded = true; outcome.error = "外部进程输出超过字节预算，已终止整个进程树"; stopped = true; break;
            }
        }
        if (stopped) break;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
            outcome.supervisionFailed = true; outcome.error = "无法核对外部进程树是否结束"; break;
        }
        DWORD available = 0;
        const bool readable = PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &available, nullptr) != FALSE;
        if (accounting.ActiveProcesses == 0 && (!readable || available == 0)) break;
        if (std::chrono::steady_clock::now() - started >= request.timeout) {
            outcome.timedOut = true; outcome.error = "外部进程树执行超时"; break;
        }
        Sleep(5);
    }
    const auto cleanupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (!tree.Drain(completion.value, job.value)) outcome.supervisionFailed = true;
    if (outcome.timedOut || outcome.outputLimitExceeded || outcome.supervisionFailed) {
        if (!TerminateJobObject(job.value, 1)) outcome.supervisionFailed = true;
        for (;;) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if (!QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
                outcome.supervisionFailed = true; break;
            }
            if (accounting.ActiveProcesses == 0) break;
            if (std::chrono::steady_clock::now() >= cleanupDeadline) {
                outcome.supervisionFailed = true; outcome.error += "；进程树终止未在期限内确认"; break;
            }
            Sleep(5);
        }
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION finalAccounting{};
    bool coverage = false;
    for (;;) {
        if (!tree.Drain(completion.value, job.value)) outcome.supervisionFailed = true;
        if (!QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &finalAccounting, sizeof(finalAccounting), nullptr)) break;
        coverage = finalAccounting.ActiveProcesses == 0 && finalAccounting.TotalProcesses == tree.observed.size();
        if (coverage || std::chrono::steady_clock::now() >= cleanupDeadline) break;
        Sleep(5); // 给已退出进程的晚到通知同一期限，不以计数归零替代句柄退出。
    }
    const bool handlesExited = tree.WaitUntil(cleanupDeadline); // 覆盖不全也必须等待已取得的句柄。
    if (!coverage || !handlesExited) {
        outcome.supervisionFailed = true; outcome.error += "；未完整确认受监督进程树退出";
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.hProcess, &exitCode) || exitCode == STILL_ACTIVE) {
        outcome.supervisionFailed = true; outcome.error = "未取得外部进程最终退出状态";
    }
    outcome.exitCode = static_cast<int>(exitCode);
    log.flush();
    if (!log) { outcome.supervisionFailed = true; outcome.error = "外部进程日志未完整写入"; }
    return outcome;
}
#else
ProcessOutcome RunProcess(const ProcessRequest& request) {
    ProcessOutcome outcome;
    outcome.error = "外部编译器只支持 Windows：" + request.executable.string();
    return outcome;
}
#endif

} // namespace War3Map::Script::detail
