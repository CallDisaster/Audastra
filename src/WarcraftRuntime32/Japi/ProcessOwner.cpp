#include "ProcessOwner.h"
#include "../RuntimeFiles.h"
#include "audastra_japi.h"
#include <tlhelp32.h>
#include <array>

namespace Audastra::Runtime32::Japi {
namespace {
std::string ModuleDigest(HMODULE module) {
    std::array<wchar_t, 32768> name{};
    const auto count = GetModuleFileNameW(module, name.data(), static_cast<DWORD>(name.size()));
    if (!count || count >= name.size()) return {};
    Handle file(OpenLockedFile(std::filesystem::path(std::wstring_view(name.data(), count))));
    return file.value == INVALID_HANDLE_VALUE ? std::string{} : Sha256(file.value);
}
bool ModuleCode(HMODULE module, const void* code) {
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(code, &page, sizeof(page)) || page.AllocationBase != module || page.State != MEM_COMMIT
        || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto protection = page.Protect & 0xff;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ
        || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}
}
ProcessOwnerLease::~ProcessOwnerLease() {
    if (locked_) ReleaseMutex(mutex_);
    if (mutex_) CloseHandle(mutex_);
}
std::unique_ptr<ProcessOwnerLease> ProcessOwnerLease::Acquire(const void* ownCode) {
    auto result = std::unique_ptr<ProcessOwnerLease>(new ProcessOwnerLease);
    result->status = AJ_POLICY;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCWSTR>(ownCode), &self)) { result->error = "cannot identify runtime image"; return result; }
    result->identity = ModuleDigest(self);
    if (result->identity.size() != 64) { result->error = "cannot verify runtime file identity"; return result; }
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exit, &kernel, &user)) { result->error = "cannot identify process lifetime"; return result; }
    const auto name = L"Local\\Audastra.RuntimeOwner.1." + std::to_wstring(GetCurrentProcessId())
        + L"." + std::to_wstring(created.dwHighDateTime) + L"." + std::to_wstring(created.dwLowDateTime);
    result->mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!result->mutex_) { result->error = "cannot acquire runtime owner lock"; return result; }
    const auto waited = WaitForSingleObject(result->mutex_, 5000);
    result->locked_ = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    if (waited != WAIT_OBJECT_0) { result->error = "runtime owner lock timeout or abandoned"; return result; }
    Handle modules(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId()));
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (modules.value == INVALID_HANDLE_VALUE || !Module32FirstW(modules.value, &entry)) { result->error = "cannot enumerate runtime images"; return result; }
    do {
        if (entry.hModule == self) continue;
        const auto query = GetProcAddress(entry.hModule, "WarVKRuntimeOwnerV1");
        if (!query) continue;
        // 先核同内容映像，再执行它的只读入口；不同版本绝不调用或替换其代码。
        if (ModuleDigest(entry.hModule) != result->identity || !ModuleCode(entry.hModule, reinterpret_cast<const void*>(query))) {
            result->error = "runtime content identity differs; restart game process"; return result;
        }
        using Query = int(__cdecl*)();
        const auto state = reinterpret_cast<Query>(query)();
        if (state == 0) continue;
        if (state != 1 || result->existing) { result->error = "runtime owner is failed or ambiguous; restart game process"; return result; }
        const auto initialize = GetProcAddress(entry.hModule, "WarVKAIInit");
        if (!initialize || !ModuleCode(entry.hModule, reinterpret_cast<const void*>(initialize))) { result->error = "runtime owner export invalid"; return result; }
        result->existing = reinterpret_cast<RuntimeInitialize>(initialize);
    } while (Module32NextW(modules.value, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) { result->error = "runtime image enumeration incomplete"; return result; }
    result->status = AJ_OK;
    return result;
}
int InitializeProcessRuntime(const void* ownCode, std::mutex& moduleLock,
    const std::function<int(std::string_view)>& install, std::string& error) {
    auto lease = ProcessOwnerLease::Acquire(ownCode);
    if (lease->status != AJ_OK) { error = std::move(lease->error); return lease->status; }
    if (lease->existing) return lease->existing();
    std::lock_guard module(moduleLock);
    return install(lease->identity);
}
}
