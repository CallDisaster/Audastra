#include "../src/ScriptBuild/ExternalProcess.h"

#include <Windows.h>
#include <appmodel.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace War3Map::Script::detail;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::filesystem::path Executable() {
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    Require(size > 0 && size < path.size(), "self executable path");
    path.resize(size); return path;
}
std::string Utf8(std::wstring_view text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
void Emit(std::string_view text, DWORD stream = STD_OUTPUT_HANDLE) {
    DWORD written = 0;
    Require(WriteFile(GetStdHandle(stream), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size(), "child write");
}
struct Scratch {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("forge-process-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Scratch() { Require(std::filesystem::create_directory(path), "private test directory"); path = std::filesystem::canonical(path); }
    ~Scratch() {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path, error);
        if (!error && resolved == path && path.filename().wstring().starts_with(L"forge-process-test-")) std::filesystem::remove_all(path, error);
    }
};
int Child(int argc, wchar_t** argv) {
    const std::wstring_view command(argv[1]);
    if (command == L"--echo") {
        for (int i = 2; i < argc; ++i) { const auto value = Utf8(argv[i]); Emit(std::to_string(value.size()) + ":" + value); }
        Emit("stderr", STD_ERROR_HANDLE); return 0;
    }
    if (command == L"--flood") { for (int i = 0; i < 256; ++i) Emit(std::string(4096, 'x')); return 0; }
    if (command == L"--nonzero") { Emit("bad"); return 7; }
    if (command == L"--package") {
        UINT32 length = 0;
        Emit(std::to_string(GetCurrentPackageFullName(&length, nullptr)));
        return 0;
    }
    if (command == L"--package-nested") {
        Scratch scratch;
        ProcessRequest request;
        request.executable = Executable(); request.workingDirectory = scratch.path;
        request.outputFile = scratch.path / "nested.log"; request.arguments = {L"--package"};
        const auto result = RunProcess(request);
        Require(result.Succeeded(), "ordinary desktop child can launch a supervised descendant");
        Emit(result.output); return 0;
    }
    if (command == L"--leaf") { Sleep(30000); return 0; }
    if (command == L"--spawn" && argc == 3) {
        const auto exe = Executable();
        auto line = QuoteArgument(exe.wstring()) + L" --leaf";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        Require(CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child), "spawn owned test leaf");
        HANDLE owner = OpenProcess(PROCESS_DUP_HANDLE, FALSE, static_cast<DWORD>(std::stoul(argv[2])));
        HANDLE retained = nullptr;
        Require(owner && DuplicateHandle(GetCurrentProcess(), child.hProcess, owner, &retained, SYNCHRONIZE, FALSE, 0), "retain exact leaf identity in test host");
        CloseHandle(owner);
        Emit(std::to_string(reinterpret_cast<std::uintptr_t>(retained)));
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
        return 0; // 父先退出，子仍存活；监督器不能把此exit0当作整个任务成功。
    }
    if (command == L"--memory") {
        void* memory = VirtualAlloc(nullptr, 64 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (memory) { VirtualFree(memory, 0, MEM_RELEASE); return 8; }
        Emit("memory-limited"); return 0;
    }
    if (command == L"--handle" && argc == 3) {
        const auto value = std::stoull(argv[2]);
        (void)SetEvent(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value)));
        return 0;
    }
    return 9;
}
void TestProcess() {
    Scratch scratch;
    ProcessRequest request;
    request.executable = Executable(); request.workingDirectory = scratch.path;
    request.outputFile = scratch.path / "output.log"; request.timeout = std::chrono::seconds(5);
    request.arguments = {L"--echo", L"", L"plain", L"space and \\", L"a\"b", L"中文"};
    auto result = RunProcess(request);
    std::string expected;
    for (std::size_t i = 1; i < request.arguments.size(); ++i) {
        const auto value = Utf8(request.arguments[i]); expected += std::to_string(value.size()) + ":" + value;
    }
    expected += "stderr";
    Require(result.Succeeded() && result.output == expected, "real child quoting and merged streams");
    Require(std::filesystem::file_size(request.outputFile) == expected.size(), "bounded file and returned output agree");
    request.maxOutputBytes = expected.size();
    Require(RunProcess(request).Succeeded(), "exact byte boundary succeeds");
    request.maxOutputBytes = expected.size() - 1;
    result = RunProcess(request);
    Require(result.started && result.outputLimitExceeded && !result.Succeeded() && result.output.size() == expected.size() - 1,
        "one-byte overrun is failure even if the process already exited zero");
    request.arguments = {L"--flood"}; request.maxOutputBytes = 17;
    result = RunProcess(request);
    Require(result.outputLimitExceeded && result.output == std::string(17, 'x') && std::filesystem::file_size(request.outputFile) == 17,
        "flood cannot fill disk or allocate an unbounded result");
    request.arguments = {L"--nonzero"}; request.maxOutputBytes = 1024;
    result = RunProcess(request);
    Require(result.started && !result.Succeeded() && result.exitCode == 7 && result.output == "bad", "actual nonzero exit preserved");
    request.arguments = {L"--spawn", std::to_wstring(GetCurrentProcessId())}; request.timeout = std::chrono::milliseconds(500);
    result = RunProcess(request);
    Require(result.started && result.timedOut && !result.supervisionFailed && result.exitCode == 0 && !result.Succeeded(),
        "parent exit0 cannot let a live descendant escape the timeout");
    Require(!result.output.empty(), "child handle receipt");
    HANDLE leaf = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(result.output)));
    const auto wait = WaitForSingleObject(leaf, 0); CloseHandle(leaf);
    Require(wait == WAIT_OBJECT_0, "owned leaf must already be terminated");
    request.arguments = {L"--memory"}; request.timeout = std::chrono::seconds(5); request.maxJobMemoryBytes = 32 * 1024 * 1024;
    result = RunProcess(request);
    Require(result.Succeeded() && result.output == "memory-limited", "job commit memory bound enforced");
    request.maxJobMemoryBytes = 1024ull * 1024 * 1024;
    request.arguments = {L"--package"};
    result = RunProcess(request);
    Require(result.Succeeded() && result.output == std::to_string(APPMODEL_ERROR_NO_PACKAGE),
        "external desktop tool must not inherit host MSIX DLL search semantics");
    request.arguments = {L"--package-nested"};
    result = RunProcess(request);
    Require(result.Succeeded() && result.output == std::to_string(APPMODEL_ERROR_NO_PACKAGE),
        "unpackaged child preserves ordinary execution and supervised nested tools");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE sentinel = CreateEventW(&attributes, TRUE, FALSE, nullptr);
    Require(sentinel != nullptr, "create parent inheritable sentinel");
    request.arguments = {L"--handle", std::to_wstring(reinterpret_cast<std::uintptr_t>(sentinel))};
    result = RunProcess(request);
    const auto signaled = WaitForSingleObject(sentinel, 0); CloseHandle(sentinel);
    Require(result.Succeeded() && signaled == WAIT_TIMEOUT, "unrelated parent inheritable handles do not leak to compiler");
    request.timeout = std::chrono::milliseconds(-1);
    Require(!RunProcess(request).started, "negative timeout cannot become infinite DWORD wait");
    request.timeout = std::chrono::seconds(5); request.maxOutputBytes = 64ull * 1024 * 1024 + 1;
    Require(!RunProcess(request).started, "hard output budget cannot be raised");
    request.maxOutputBytes = 1024; request.executable = scratch.path / "missing.exe";
    Require(!RunProcess(request).started, "missing executable cannot claim started");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc > 1) return Child(argc, argv);
        TestProcess();
        std::cout << "ExternalProcess: bounded output, explicit inheritance and supervised child lifetime passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
