#include "RuntimeNativeProviders.h"

#include "RuntimeFiles.h"
#include "War3MapCore/Runtime/TestNativeProviderAbi.h"

namespace Audastra::Runtime32 {
namespace {
bool ImageCode(HMODULE module, std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION page{};
    if (!module || !address || !VirtualQuery(reinterpret_cast<const void*>(address), &page, sizeof(page)) ||
        page.AllocationBase != module || page.State != MEM_COMMIT || page.Type != MEM_IMAGE ||
        (page.Protect & PAGE_GUARD)) return false;
    const auto protection = page.Protect & 0xff;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}
struct Registration {
    const OwnLua::RegisterObservedNative& add;
    HMODULE module;
    bool rejected = false;
    std::uintptr_t rejectedAddress = 0;
    char rejectedName[129]{};
};
thread_local Registration* active = nullptr;

void RecordRejection(Registration& registration, std::uintptr_t address, const char* name) noexcept {
    if (registration.rejected) return;
    registration.rejected = true;
    registration.rejectedAddress = address;
    __try {
        if (!name) return;
        for (std::size_t index = 0; index < sizeof(registration.rejectedName); ++index) {
            registration.rejectedName[index] = name[index];
            if (!registration.rejectedName[index]) return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    registration.rejectedName[0] = 0;
}

BOOL WINAPI Add(void* context, std::uintptr_t address, const char* name, const char* signature) noexcept {
    if (!active || active != context) return FALSE;
    if (!ImageCode(active->module, address)) {
        RecordRejection(*active, address, name);
        return FALSE;
    }
    try {
        active->add(address, name, signature);
        return TRUE;
    } catch (...) {
        RecordRejection(*active, address, name);
        return FALSE;
    }
}
BOOL Call(RegisterTestNatives entry, void* context) {
    __try { return entry(Add, context); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
}
} // namespace

std::string RuntimeNativeProviders::Load() {
    if (const auto storm = GetModuleHandleW(L"Storm.dll")) {
        const auto expected = std::filesystem::path(evidence_.gameModule.path).parent_path() / L"Storm.dll";
        std::array<wchar_t, kPathCapacity> path{};
        const auto size = GetModuleFileNameW(storm, path.data(), static_cast<DWORD>(path.size()));
        Handle lease(OpenLockedFile(expected));
        if (!size || size >= path.size() || !EqualPath(path.data(), expected.wstring()) ||
            lease.value == INVALID_HANDLE_VALUE || !IsX86Image(lease.value))
            return "已加载的 Storm.dll 不属于本次核验的游戏目录。";
    }
    for (DWORD i = 0; i < evidence_.providerModuleCount; ++i) {
        auto& record = evidence_.providerModules[i];
        auto expected = std::filesystem::path(record.file.path).lexically_normal();
        expected.make_preferred();
        if (GetModuleHandleW(expected.filename().c_str())) return "提供者 DLL 名称已被当前进程使用。";
        Handle lease(OpenLockedFile(expected));
        if (lease.value == INVALID_HANDLE_VALUE || !IsX86Image(lease.value) ||
            Sha256(lease.value, 32u * 1024u * 1024u) != record.file.expectedSha256)
            return "提供者 DLL 启动前身份变化或无法锁定。";
        // 依赖由描述文件先行载入。系统搜索不包含地图、当前目录或 DLL 邻居。
        const auto module = LoadLibraryExW(expected.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return "提供者 DLL 加载失败。Win32=" + std::to_string(GetLastError());
        std::array<wchar_t, kPathCapacity> path{};
        const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size() || !EqualPath(path.data(), expected.wstring()))
            return "加载的提供者 DLL 路径与已核验文件不同。";
        Handle mapped(OpenLockedFile(path.data()));
        const auto actual = mapped.value == INVALID_HANDLE_VALUE ? std::string{} : Sha256(mapped.value, 32u * 1024u * 1024u);
        if (actual.empty() || actual != record.file.expectedSha256) return "提供者 DLL 加载后身份不一致。";
        if (record.entry[0] && !ImageCode(module, reinterpret_cast<std::uintptr_t>(GetProcAddress(module, record.entry))))
            return "提供者登记入口不在已核验 DLL 的可执行映像中。";
        strcpy_s(record.file.actualSha256, actual.c_str());
        InterlockedExchange(&record.file.loaded, 1);
        modules_[i] = module;
    }
    return {};
}

OwnLua::NativeExtensionHooks RuntimeNativeProviders::Hooks() {
    return {
        [this](auto address, auto name, auto signature) { return Validate(address, name, signature); },
        [this](const auto& add) { return Register(add); },
        [this](const auto& registry) { return Verify(registry); }
    };
}

std::string RuntimeNativeProviders::Validate(std::uintptr_t address, std::string_view name, std::string_view signature) const {
    for (DWORD i = 0; i < evidence_.providerNativeCount; ++i) {
        const auto& expected = evidence_.providerNatives[i];
        if (name != expected.name) continue;
        if (expected.moduleIndex >= evidence_.providerModuleCount || !ImageCode(modules_[expected.moduleIndex], address))
            return "native " + std::string(name) + " 的注册地址不在声明的提供者 DLL 中"
                + DescribeNativeAddress(address) + "。";
        const auto actual = OwnLua::ParseNativeSignature(signature);
        if (!actual || actual != OwnLua::ParseNativeSignature(expected.signature))
            return "native " + std::string(name) + " 注册签名不符：声明 " + expected.signature + "，实际 "
                + std::string(signature) + DescribeNativeAddress(address) + "。";
        return {};
    }
    return "未声明的外部 native 登记：" + std::string(name) + DescribeNativeAddress(address) + "。";
}

std::string RuntimeNativeProviders::Register(const OwnLua::RegisterObservedNative& add) {
    if (active) return "提供者登记发生重入。";
    InterlockedExchange(&evidence_.providerRegistrationsVerified, 0);
    for (DWORD i = 0; i < evidence_.providerModuleCount; ++i) {
        auto& record = evidence_.providerModules[i];
        if (!record.entry[0]) continue;
        const auto entry = GetProcAddress(modules_[i], record.entry);
        if (!ImageCode(modules_[i], reinterpret_cast<std::uintptr_t>(entry))) return "提供者登记入口已失效。";
        Registration registration{add, modules_[i]};
        active = &registration;
        const auto ok = Call(reinterpret_cast<RegisterTestNatives>(entry), &registration);
        active = nullptr;
        if (!ok || registration.rejected) {
            const std::string name = OwnLua::NativeIdentifier(registration.rejectedName)
                ? registration.rejectedName : "<名称不可核验>";
            return "提供者 " + std::string(record.providerId) + " 的登记入口 " + record.entry
                + (registration.rejected ? " 拒绝 native " + name + "：回调来源不符或登记异常"
                    + DescribeNativeAddress(registration.rejectedAddress)
                    : " 返回失败或发生异常") + "。";
        }
        InterlockedExchange(&record.file.initializeCalled, 1);
    }
    return {};
}

std::string RuntimeNativeProviders::Verify(const OwnLua::NativeRegistry& registry) {
    std::string missing;
    for (DWORD i = 0; i < evidence_.providerNativeCount; ++i) {
        const auto& expected = evidence_.providerNatives[i];
        const auto* actual = registry.Find(expected.name);
        if (!actual || !actual->external) {
            if (!missing.empty()) missing += "、";
            missing += expected.name;
        }
    }
    LONG count = 0;
    for (const auto& [name, registration] : registry.Entries()) {
        if (!registration.external) continue;
        if (count >= static_cast<LONG>(kProviderNativeLimit)) return "外部实际登记数超过预算。";
        auto& text = evidence_.providerObservedNames[count++];
        strcpy_s(text.data(), text.size(), name.c_str());
    }
    InterlockedExchange(&evidence_.providerObservedCount, count);
    if (!missing.empty()) return "提供者登记不完整，缺少：" + missing;
    InterlockedExchange(&evidence_.providerRegistrationsVerified, 1);
    InterlockedIncrement(&evidence_.providerVerifiedInitializations);
    return {};
}
} // namespace Audastra::Runtime32
