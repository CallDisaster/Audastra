#include "UpdateInstallerInternal.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Update/SemanticVersion.h"
#include "../Serialization/StrictJson.h"

#include <Psapi.h>
#include <TlHelp32.h>
#include <sodium.h>

#include <algorithm>
#include <array>

namespace War3Map::Update {
namespace {

Status Failure(std::string code, std::string message) {
    return Status::Failure("Update", std::move(code), std::move(message));
}
Result<UpdateProcessIdentity> Identity(HANDLE process) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user))
        return Result<UpdateProcessIdentity>::Failure("Update", "UPDATE_PROCESS_IDENTITY_FAILED", "无法确认进程创建时间。");
    return Result<UpdateProcessIdentity>::Success({GetProcessId(process),
        (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime});
}
bool Hex(std::string_view text, std::size_t size) {
    return text.size() == size && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::wstring Wide(std::string_view text) {
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size && !text.empty()) throw std::runtime_error("invalid UTF8 argument");
    std::wstring value(size, L'\0');
    if (size && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(), size) != size)
        throw std::runtime_error("invalid UTF8 argument");
    return value;
}
std::wstring Quote(std::wstring_view value) {
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        output.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"') output += L'\\';
        output += c; slashes = 0;
    }
    output.append(slashes * 2, L'\\'); output += L'"';
    return output;
}
std::wstring Command(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
    auto command = Quote(executable.native());
    for (const auto& argument : arguments) { command += L' '; command += Quote(Wide(argument)); }
    return command;
}
Result<std::filesystem::path> VerifyNativeExecutable(const Detail::FileReadLease& file) {
    using R = Result<std::filesystem::path>;
    const auto checked = Detail::CheckOpenedPath(file.file.Get(), file.path, false);
    if (!checked.Succeeded()) return {std::nullopt, checked.diagnostics};
    std::array<wchar_t, 32768> value{};
    const auto count = GetFinalPathNameByHandleW(file.file.Get(), value.data(), static_cast<DWORD>(value.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    const std::wstring actual(value.data(), count < value.size() ? count : 0);
    if (!count || count >= value.size() || !actual.starts_with(L"\\\\?\\") || actual.starts_with(L"\\\\?\\UNC\\"))
        return R::Failure("Update", "UPDATE_EXECUTABLE_PATH_UNKNOWN", "无法确定签名入口的实际本机长路径，拒绝启动。");
    // 已持有签名文件及祖先租约；沿原生实际长路径检查，文本.exe可能被识别为DOS而返回成功。
    DWORD type = 0;
    if (!GetBinaryTypeW(actual.c_str(), &type) || type != SCS_64BIT_BINARY)
        return R::Failure("Update", "UPDATE_EXECUTABLE_TYPE_INVALID", "签名入口不是本windows-x64产品的64位Windows程序，拒绝进入原生启动。");
    return R::Success(std::filesystem::path(actual));
}
Result<std::filesystem::path> CanonicalFile(const std::filesystem::path& file) {
    Detail::NativeHandle handle(CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    std::array<wchar_t, 32768> value{};
    const auto count = handle.Valid() ? GetFinalPathNameByHandleW(handle.Get(), value.data(), static_cast<DWORD>(value.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
    std::wstring path(value.data(), count < value.size() ? count : 0);
    if (!count || count >= value.size() || !path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\?\\UNC\\"))
        return Result<std::filesystem::path>::Failure("Update", "UPDATE_MODULE_IDENTITY_FAILED", "无法确定进程模块的实际本机路径。");
    return Result<std::filesystem::path>::Success(std::filesystem::path(path.substr(4)).lexically_normal());
}

} // namespace

Result<UpdateProcessIdentity> ReadUpdateProcessIdentity(std::uint32_t processId) {
    Detail::NativeHandle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
        processId ? processId : GetCurrentProcessId()));
    if (!process.Valid()) return Result<UpdateProcessIdentity>::Failure("Update", "UPDATE_PROCESS_OPEN_FAILED", "无法只读打开进程身份。");
    return Identity(process.Get());
}

Status ReplyUpdateHealth(std::uintptr_t pipeWriteHandle, std::string_view transactionId, std::string_view challengeHex,
    std::string_view compiledVersion, std::string_view compiledCommit) {
    try {
        const auto handle = reinterpret_cast<HANDLE>(pipeWriteHandle);
        if (!Hex(transactionId, 32) || !Hex(challengeHex, 64) || !Hex(compiledCommit, 40)
            || !ParseSemanticVersion(compiledVersion).Succeeded() || GetFileType(handle) != FILE_TYPE_PIPE)
            return Failure("UPDATE_HEALTH_ARGUMENT_INVALID", "更新健康回执参数或继承管道无效。");
        DWORD flags = 0;
        if (!GetHandleInformation(handle, &flags) || !(flags & HANDLE_FLAG_INHERIT))
            return Failure("UPDATE_HEALTH_HANDLE_INVALID", "健康回执只能写入更新程序显式继承的管道。");
        Detail::NativeHandle pipe(handle);
        const auto identity = Identity(GetCurrentProcess());
        if (!identity.Succeeded()) return {identity.diagnostics};
        nlohmann::json reply{{"format", "audastra-update-health"}, {"transaction", transactionId}, {"challenge", challengeHex},
            {"pid", identity.value->processId}, {"creation_time_100ns", identity.value->creationTime100ns},
            {"version", compiledVersion}, {"commit", compiledCommit}};
        const auto bytes = reply.dump() + "\n";
        DWORD written = 0;
        if (!WriteFile(pipe.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size())
            return Failure("UPDATE_HEALTH_WRITE_FAILED", "未能向更新程序完整回复真实启动健康身份。");
        return Status::Ok();
    } catch (const std::exception&) { return Failure("UPDATE_HEALTH_WRITE_FAILED", "无法构造启动健康回执。"); }
}

namespace Detail {

Status WaitForParent(const UpdateProcessIdentity& expected, std::uint32_t timeoutMs) {
    if (!expected.processId || !expected.creationTime100ns || expected.processId == GetCurrentProcessId())
        return Failure("UPDATE_PARENT_IDENTITY_INVALID", "主程序PID/创建时间无效或指向更新程序自身。");
    NativeHandle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, expected.processId));
    if (!process.Valid()) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return Status::Ok();
        return Failure("UPDATE_PARENT_OPEN_FAILED", "无法确认主程序自然退出，拒绝替换。");
    }
    const auto actual = Identity(process.Get());
    if (!actual.Succeeded()) return {actual.diagnostics};
    if (*actual.value != expected) return Failure("UPDATE_PARENT_IDENTITY_MISMATCH", "PID已重用或创建时间不符，拒绝等待其他进程。");
    const auto wait = WaitForSingleObject(process.Get(), timeoutMs);
    if (wait == WAIT_OBJECT_0) return Status::Ok();
    return Failure(wait == WAIT_TIMEOUT ? "UPDATE_PARENT_EXIT_TIMEOUT" : "UPDATE_PARENT_WAIT_FAILED",
        "主程序未在限定时间自然退出；未结束任何进程，安装目录保持。");
}

Result<bool> ProcessStillRunning(const UpdateProcessIdentity& expected) {
    NativeHandle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, expected.processId));
    if (!process.Valid()) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return Result<bool>::Success(false);
        return Result<bool>::Failure("Update", "UPDATE_PROCESS_OPEN_FAILED", "无法确认已启动新版本是否仍运行。");
    }
    const auto actual = Identity(process.Get());
    if (!actual.Succeeded()) return {std::nullopt, actual.diagnostics};
    if (*actual.value != expected) return Result<bool>::Success(false); // 原身份已退出，新的同PID进程不受影响。
    const auto wait = WaitForSingleObject(process.Get(), 0);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) return Result<bool>::Success(wait == WAIT_TIMEOUT);
    return Result<bool>::Failure("Update", "UPDATE_PROCESS_WAIT_FAILED", "新版本进程运行状态未知。");
}

Status CheckUpdaterOutsideInstall(const std::filesystem::path& install) {
    std::array<HMODULE, 512> modules{};
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(sizeof(modules)), &needed)
        || needed > sizeof(modules)) return Failure("UPDATE_MODULE_INSPECTION_FAILED", "无法完整检查更新程序已加载模块。");
    std::array<wchar_t, 32768> path{};
    for (std::size_t index = 0; index < needed / sizeof(HMODULE); ++index) {
        const auto count = GetModuleFileNameW(modules[index], path.data(), static_cast<DWORD>(path.size()));
        if (!count || count >= path.size()) return Failure("UPDATE_MODULE_INSPECTION_FAILED", "模块路径未能完整读取。");
        const auto actual = CanonicalFile(std::filesystem::path(std::wstring(path.data(), count)));
        if (!actual.Succeeded()) return {actual.diagnostics};
        if (Within(*actual.value, install)) return Failure("UPDATE_UPDATER_INSIDE_INSTALL",
            "更新程序或DLL仍位于待替换安装树内，需要先在外部副本运行。");
    }
    return Status::Ok();
}

Result<HealthChild> LaunchHealthChild(const InstallJournal& journal, const VerifiedUpdateManifest& target) {
    using R = Result<HealthChild>;
    try {
        const auto entry = std::find_if(target.Manifest().files.begin(), target.Manifest().files.end(),
            [&](const auto& file) { return file.path == journal.executable; });
        if (entry == target.Manifest().files.end()) return R::Failure("Update", "UPDATE_EXECUTABLE_UNSIGNED", "健康入口不在签名allowlist。");
        const auto executable = journal.install / std::filesystem::path(std::wstring(entry->path.begin(), entry->path.end()));
        auto file = OpenSafeReadFile(executable, kMaxUpdateFileBytes);
        if (!file.Succeeded()) return {std::nullopt, std::move(file.diagnostics)};
        const auto digest = Sha256Handle(file.value->file.Get());
        if (!digest.Succeeded() || file.value->bytes != entry->bytes || *digest.value != entry->sha256)
            return R::Failure("Update", "UPDATE_EXECUTABLE_SHA_MISMATCH", "真实健康入口SHA/长度与签名不符。");
        const auto nativeExecutable = VerifyNativeExecutable(*file.value);
        if (!nativeExecutable.Succeeded()) return {std::nullopt, nativeExecutable.diagnostics};
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &security, 4096)) return R::Failure("Update", "UPDATE_HEALTH_PIPE_FAILED", "无法创建受限健康管道。");
        NativeHandle pipeRead(read), pipeWrite(write);
        if (!SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0)) return R::Failure("Update", "UPDATE_HEALTH_PIPE_FAILED", "无法禁止读端继承。");
        std::array<unsigned char, 32> challengeBytes{};
        if (sodium_init() < 0) return R::Failure("Update", "UPDATE_RANDOM_FAILED", "无法初始化健康challenge随机源。");
        randombytes_buf(challengeBytes.data(), challengeBytes.size());
        std::array<char, 65> challenge{};
        sodium_bin2hex(challenge.data(), challenge.size(), challengeBytes.data(), challengeBytes.size());
        auto arguments = journal.startupArguments;
        arguments.insert(arguments.end(), {"--update-health-transaction", journal.transactionId,
            "--update-health-challenge", challenge.data(), "--update-health-pipe", std::to_string(reinterpret_cast<std::uintptr_t>(write))});
        auto command = Command(*nativeExecutable.value, arguments);
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<std::byte> attributes(attributeBytes);
        auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes))
            return R::Failure("Update", "UPDATE_HANDLE_LIST_FAILED", "无法初始化受限继承句柄列表。");
        struct AttributeGuard { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeGuard() { DeleteProcThreadAttributeList(value); } } guard{attributeList};
        if (!UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write, sizeof(write), nullptr, nullptr))
            return R::Failure("Update", "UPDATE_HANDLE_LIST_FAILED", "无法限制新版本继承句柄。");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributeList;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nativeExecutable.value->c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
            nullptr, journal.install.c_str(), &startup.StartupInfo, &process))
            return R::Failure("Update", "UPDATE_NEW_VERSION_START_FAILED", "新版本进程未能启动，未假报健康。");
        NativeHandle childProcess(process.hProcess), childThread(process.hThread);
        const auto identity = Identity(childProcess.Get());
        if (!identity.Succeeded()) return {std::nullopt, identity.diagnostics};
        return R::Success({std::move(childProcess), std::move(childThread), std::move(pipeRead), *identity.value, challenge.data()});
    } catch (const std::exception&) { return R::Failure("Update", "UPDATE_NEW_VERSION_START_FAILED", "无法完整启动签名新版本。"); }
}

Status WaitForHealth(HealthChild& child, const InstallJournal& journal, std::uint32_t timeoutMs) {
    try {
        const auto deadline = GetTickCount64() + timeoutMs;
        std::string received;
        std::array<char, 1024> bytes{};
        while (GetTickCount64() <= deadline) {
            DWORD available = 0;
            if (!PeekNamedPipe(child.pipeRead.Get(), nullptr, 0, nullptr, &available, nullptr))
                return Failure("UPDATE_HEALTH_PIPE_CLOSED", "新版本未回复健康身份便关闭管道。");
            if (available) {
                DWORD read = 0;
                const auto wanted = static_cast<DWORD>((std::min)(bytes.size(), std::size_t{available}));
                if (!ReadFile(child.pipeRead.Get(), bytes.data(), wanted, &read, nullptr) || !read)
                    return Failure("UPDATE_HEALTH_READ_FAILED", "健康身份管道读取失败。");
                received.append(bytes.data(), read);
                if (received.size() > 2048) return Failure("UPDATE_HEALTH_INVALID", "健康回执超过有界长度。");
                if (received.ends_with('\n')) break;
            }
            if (WaitForSingleObject(child.process.Get(), 0) == WAIT_OBJECT_0 && !available)
                return Failure("UPDATE_HEALTH_CHILD_EXITED", "新版本在真实就绪回复前退出。");
            Sleep(10);
        }
        if (!received.ends_with('\n')) return Failure("UPDATE_HEALTH_TIMEOUT", "限定时间内未收到真实主窗口就绪回执。");
        received.pop_back();
        Serialization::StrictJsonLimits limits; limits.maxBytes = 2048; limits.maxDepth = 4; limits.maxNodes = 32;
        limits.maxStringBytes = 512; limits.rejectFloatingPoint = true;
        const auto parsed = Serialization::ParseStrictJson(std::as_bytes(std::span(received.data(), received.size())), limits);
        if (!parsed.Succeeded() || !parsed.value->is_object() || parsed.value->size() != 7)
            return Failure("UPDATE_HEALTH_INVALID", "健康回执不是完整严格协议。");
        const auto& reply = *parsed.value;
        if (reply.at("format") != "audastra-update-health" || reply.at("transaction") != journal.transactionId
            || reply.at("challenge") != child.challenge || !reply.at("pid").is_number_unsigned()
            || !reply.at("creation_time_100ns").is_number_unsigned()
            || reply.at("pid").get<std::uint64_t>() != child.identity.processId
            || reply.at("creation_time_100ns").get<std::uint64_t>() != child.identity.creationTime100ns
            || reply.at("version") != journal.targetVersion || reply.at("commit") != journal.sourceCommit)
            return Failure("UPDATE_HEALTH_IDENTITY_MISMATCH", "真实健康回复的challenge/进程/version/commit绑定不符。");
        return Status::Ok();
    } catch (const std::exception&) { return Failure("UPDATE_HEALTH_INVALID", "健康回执字段无效，未授予commit。"); }
}

Result<bool> InstallExecutableStillRunning(const InstallJournal& journal) {
    NativeHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.Valid()) return Result<bool>::Failure("Update", "UPDATE_PROCESS_ENUMERATION_FAILED", "无法枚举启动/崩溃间隙的可能新进程。");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.Get(), &entry)) return Result<bool>::Failure("Update", "UPDATE_PROCESS_ENUMERATION_FAILED", "无法完整枚举进程。");
    const auto name = std::filesystem::path(std::wstring(journal.executable.begin(), journal.executable.end())).filename().native();
    WindowsPathLess less;
    do {
        if (less(name, entry.szExeFile) || less(entry.szExeFile, name)) continue;
        NativeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (!process.Valid()) {
            if (GetLastError() == ERROR_INVALID_PARAMETER) continue;
            return Result<bool>::Failure("Update", "UPDATE_PROCESS_IDENTITY_UNKNOWN", "同名进程身份不可确认，暂不回滚。");
        }
        std::array<wchar_t, 32768> path{};
        DWORD size = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process.Get(), 0, path.data(), &size))
            return Result<bool>::Failure("Update", "UPDATE_PROCESS_IDENTITY_UNKNOWN", "同名进程路径不可确认，暂不回滚。");
        const auto actual = CanonicalFile(std::filesystem::path(std::wstring(path.data(), size)));
        if (!actual.Succeeded()) return {std::nullopt, actual.diagnostics};
        if (Within(*actual.value, journal.install) || Within(*actual.value, journal.backup) || Within(*actual.value, journal.failed))
            return Result<bool>::Success(true);
    } while (Process32NextW(snapshot.Get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) return Result<bool>::Failure("Update", "UPDATE_PROCESS_ENUMERATION_FAILED", "进程枚举中断。");
    return Result<bool>::Success(false);
}

Status RestartOldVersion(const InstallJournal& journal, const VerifiedUpdateManifest* baseline,
    const std::function<Result<bool>(const InstallJournal&)>& processProbe) {
    if (!baseline) return Failure("UPDATE_OLD_RESTART_UNVERIFIED", "缺少旧版本可信签名，不自动重启。");
    const auto entry = std::find_if(baseline->Manifest().files.begin(), baseline->Manifest().files.end(),
        [&](const auto& file) { return file.path == journal.executable; });
    if (entry == baseline->Manifest().files.end())
        return Failure("UPDATE_OLD_RESTART_UNVERIFIED", "旧可执行程序不在可信签名清单内，不自动重启。");
    const auto executable = journal.install / std::filesystem::path(std::wstring(entry->path.begin(), entry->path.end()));
    auto file = OpenSafeReadFile(executable, kMaxUpdateFileBytes);
    if (!file.Succeeded()) return Failure("UPDATE_OLD_RESTART_UNVERIFIED", "无法锁定旧可执行程序，不自动重启。");
    const auto digest = Sha256Handle(file.value->file.Get());
    if (!digest.Succeeded() || file.value->bytes != entry->bytes || *digest.value != entry->sha256)
        return Failure("UPDATE_OLD_RESTART_UNVERIFIED", "旧可执行程序的大小或SHA不符，不自动重启。");
    const auto nativeExecutable = VerifyNativeExecutable(*file.value);
    if (!nativeExecutable.Succeeded())
        return Failure("UPDATE_OLD_RESTART_EXECUTABLE_INVALID", "旧签名入口的实际路径或64位Windows程序格式无法确认，跳过自动重启。");
    auto arguments = journal.startupArguments;
    arguments.insert(arguments.end(), {"--update-error", journal.reason.empty() ? "更新未成功，已恢复旧版本。" : journal.reason});
    auto command = Command(*nativeExecutable.value, arguments);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // 可能已有作者自行重开的实例；紧邻启动前复核，未知也不允许重复启动。
    const auto running = processProbe ? processProbe(journal) : InstallExecutableStillRunning(journal);
    if (!running.Succeeded())
        return Failure("UPDATE_OLD_RESTART_RUNNING_UNKNOWN", "现有实例身份或完整进程枚举未知，保留窗口并跳过自动重启。");
    if (*running.value)
        return Failure("UPDATE_OLD_RESTART_ALREADY_RUNNING", "同一安装根已有实例运行，保留该窗口并跳过自动重启。");
    if (!CreateProcessW(nativeExecutable.value->c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        journal.install.c_str(), &startup, &process))
        return Failure("UPDATE_OLD_RESTART_CREATE_FAILED", "恢复已完成，但旧版本进程未能启动。");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return Status::Ok(); // 仅表示原版本进程已启动，不宣称其窗口健康。
}

} // namespace Detail
} // namespace War3Map::Update
