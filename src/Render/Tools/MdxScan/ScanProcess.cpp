#include "ScanTypes.h"
#include <algorithm>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif
namespace War3Map::Render::Scan {
std::vector<std::string> LimitArguments(const Limits& v) {
    return {"--model-ms",std::to_string(v.modelMs),"--total-seconds",std::to_string(v.totalSeconds),
        "--memory-mib",std::to_string(v.memoryMiB),"--max-models",std::to_string(v.maxModels),
        "--file-mib",std::to_string(v.fileMiB),"--max-samples",std::to_string(v.maxSamples),
        "--max-nodes",std::to_string(v.maxNodes),"--max-vertices",std::to_string(v.maxVertices)};
}
#ifdef _WIN32
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring Quote(const std::wstring& text) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const auto c : text) {
        if (c == L'\\') { ++backslashes; continue; }
        result.append(c == L'"' ? backslashes*2+1 : backslashes,L'\\');
        result += c; backslashes = 0;
    }
    result.append(backslashes*2,L'\\'); result += L'"'; return result;
}
}
#endif
std::string RunChild(const Options& options, const std::vector<std::string>& arguments, std::uint32_t timeoutMs) {
#ifdef _WIN32
    Handle job{CreateJobObjectW(nullptr,nullptr)};
    if (!job.value) return "SCAN_JOB_CREATE_FAILED";
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limit.BasicLimitInformation.ActiveProcessLimit = 1;
    limit.ProcessMemoryLimit = static_cast<SIZE_T>(options.limits.memoryMiB)*1024*1024;
    if (!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limit,sizeof(limit))) return "SCAN_JOB_LIMIT_FAILED";
    auto command = Quote(options.executable.wstring());
    for (const auto& arg : arguments) { command += L' '; command += Quote(FromUtf8(arg).wstring()); }
    STARTUPINFOW start{}; start.cb = sizeof(start);
    PROCESS_INFORMATION process{};
    // 挂起创建，先成功附加受限job再运行；无法进入job时拒绝，绝不无界降级。
    if (!CreateProcessW(options.executable.c_str(),command.data(),nullptr,nullptr,FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW,nullptr,nullptr,&start,&process)) return "SCAN_CHILD_CREATE_FAILED";
    Handle owner{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job.value,owner.value)) {
        TerminateProcess(owner.value,1); WaitForSingleObject(owner.value,5000); return "SCAN_JOB_ASSIGN_FAILED";
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) return "SCAN_CHILD_RESUME_FAILED";
    const auto result = WaitForSingleObject(owner.value,timeoutMs);
    if (result != WAIT_OBJECT_0) {
        TerminateJobObject(job.value,1); WaitForSingleObject(owner.value,5000);
        return result == WAIT_TIMEOUT ? "SCAN_MODEL_TIMEOUT" : "SCAN_CHILD_WAIT_FAILED";
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(owner.value,&exitCode)) return "SCAN_CHILD_EXIT_QUERY_FAILED";
    return exitCode == 0 ? "" : "SCAN_CHILD_EXIT_" + std::to_string(exitCode);
#else
    std::vector<std::string> owned{Utf8(options.executable)}; owned.insert(owned.end(),arguments.begin(),arguments.end());
    std::vector<char*> argv; argv.reserve(owned.size()+1);
    for (auto& arg : owned) argv.push_back(arg.data());
    argv.push_back(nullptr);
    const auto process = fork();
    if (process < 0) return "SCAN_CHILD_CREATE_FAILED";
    if (process == 0) {
        const rlim_t bytes = static_cast<rlim_t>(options.limits.memoryMiB)*1024*1024;
        const rlimit memory{bytes,bytes}; const rlimit core{0,0};
        if (setrlimit(RLIMIT_AS,&memory) != 0 || setrlimit(RLIMIT_CORE,&core) != 0) _exit(125);
        execv(options.executable.c_str(),argv.data()); _exit(126);
    }
    const auto deadline = Clock::now()+std::chrono::milliseconds(timeoutMs);
    int status = 0;
    while (true) {
        const auto waited = waitpid(process,&status,WNOHANG);
        if (waited == process) break;
        if (waited < 0 && errno != EINTR) return "SCAN_CHILD_WAIT_FAILED";
        if (Clock::now() >= deadline) {
            kill(process,SIGKILL); while (waitpid(process,&status,0) < 0 && errno == EINTR) {}
            return "SCAN_MODEL_TIMEOUT";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return "";
    return WIFSIGNALED(status) ? "SCAN_CHILD_SIGNAL_" + std::to_string(WTERMSIG(status)) : "SCAN_CHILD_EXIT_" + std::to_string(WEXITSTATUS(status));
#endif
}
}
