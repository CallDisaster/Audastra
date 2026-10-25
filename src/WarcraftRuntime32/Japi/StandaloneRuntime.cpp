#include "GameSession.h"
#include "../OwnLua/GameVm127.h"
#include "../GameUi/GameUiModule.h"
#include "../RuntimeFiles.h"
#include <detours.h>
#include <tlhelp32.h>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

// 仅实现 AGENT_USAGE.md 的 DLL 契约；不读取/解析引导器，不依赖启动器或邮箱。
namespace Audastra::Runtime32::Japi {
namespace {
using Localized = std::uint32_t(__cdecl*)(std::uintptr_t);
using Initialize = void(__cdecl*)();
using DestroyVm = std::uintptr_t(__thiscall*)(std::uintptr_t);
// 原版同 SHA 的 stock 登记第24项：GetLocalizedString (S)S，登记调用 RVA 1e9c30。
// 只读核验来自本项目原有 stock 登记扫描器；不是引导器实现中的地址。
constexpr std::uintptr_t kLocalized = 0x1e31d0;
constexpr std::uintptr_t kInitialize = 0x1e9a50, kDestroyVm = 0x7dd070;

void Trace(std::string_view message) noexcept {
    try {
        static std::mutex lock;
        static HANDLE file = INVALID_HANDLE_VALUE;
        static bool attempted = false;
        static unsigned lines = 0;
        std::lock_guard guard(lock);
        if (!attempted) {
            attempted = true;
            std::array<wchar_t, 32768> path{};
            const auto length = GetTempPathW(static_cast<DWORD>(path.size()), path.data());
            if (length && length < path.size()) {
                const auto name = std::filesystem::path(path.data()) / (L"audastra-japi-"
                    + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".log");
                file = CreateFileW(name.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
        }
        if (lines >= 2048) return; // 不无限记录地图调用，不输出作者参数/内存地址。
        ++lines;
        const std::string text = "[AudastraJapi] pid=" + std::to_string(GetCurrentProcessId())
            + " tid=" + std::to_string(GetCurrentThreadId()) + " " + std::string(message.substr(0, 768)) + "\n";
        OutputDebugStringA(text.c_str());
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            (void)WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
            (void)FlushFileBuffers(file);
        }
    } catch (...) { OutputDebugStringA("[AudastraJapi] diagnostic-failure\n"); }
}
struct Runtime {
    std::shared_ptr<OwnLua::GameImage127> image;
    Localized localized = nullptr;
    Initialize initialize = nullptr;
    DestroyVm destroyVm = nullptr;
    std::recursive_mutex lock;
    std::unique_ptr<NativeSession> session;
    std::shared_ptr<StringVm> vm;
    DWORD thread = 0;
    std::uint64_t epoch = 1;
    unsigned depth = 0;
    bool failed = false;

    void Fail(std::string_view message) {
        failed = true;
        if (vm) vm->Invalidate();
        Trace("failure: " + std::string(message));
    }
    void Reset() {
        if (depth) {
            if (session) session->Reset(); // 失效而不析构正在调用的 Host。
            Fail("VM reset during active JAPI call"); return;
        }
        if (session) session->Reset();
        session.reset(); vm.reset(); thread = 0;
        if (++epoch > INT32_MAX) { Fail("epoch exhausted"); return; }
        failed = false;
        Trace("vm-reset epoch=" + std::to_string(epoch));
    }
};
std::atomic<Runtime*> runtime{nullptr};
std::mutex installLock;

void TraceReply(std::string_view input, std::string_view output) {
    audastra::japi::Request request;
    audastra::japi::Response reply;
    if (audastra::japi::ParseRequest(input, request) != AJ_OK
        || audastra::japi::ParseResponse(output, reply) != AJ_OK) return;
    auto message = "epoch=" + std::to_string(reply.epoch) + " request=" + std::to_string(reply.id)
        + " command=" + request.command + " status=" + std::to_string(reply.status)
        + " kind=" + std::to_string(reply.value.kind);
    // 只记录布尔结果与句柄是否存在；不记录作者文字、实际句柄或请求参数。
    if (reply.value.kind == AJ_BOOL) message += " bool=" + std::to_string(reply.value.integer);
    if (reply.value.kind == AJ_HANDLE) message += reply.value.integer ? " handle=nonzero" : " handle=zero";
    Trace(message);
}
std::uint32_t __cdecl LocalizedHook(std::uintptr_t object) noexcept {
    auto* self = runtime.load();
    if (!self || !self->localized) return 0;
    try {
        std::array<char, AJ_MAX_FRAME + 1> bytes{};
        std::size_t count = 0;
        const auto probe = OwnLua::GameVm127::ReadPrefixedStringObject(object, "ajapi:", bytes, count);
        if (probe == OwnLua::GameVm127::StringProbe::Other) return self->localized(object);
        std::lock_guard guard(self->lock);
        if (probe == OwnLua::GameVm127::StringProbe::Unreadable) { self->Fail("unreadable native string"); return 0; }
        if (self->failed) return 0;
        const auto thread = GetCurrentThreadId();
        if (!self->thread) self->thread = thread;
        if (self->thread != thread) { self->Fail("owned request on wrong thread"); return 0; }
        if (!self->session) self->session = std::make_unique<NativeSession>(
            [self](auto error) { self->Fail(error); }, &TraceReply);
        struct Depth { unsigned& value; ~Depth() { --value; } } depth{self->depth};
        ++self->depth;
        const std::string_view input(bytes.data(), count);
        const auto result = self->session->Dispatch(self->epoch, input, [self] {
            auto attached = AttachGameStringVm(self->image, self->epoch,
                [](const OwnLua::GameImage127& image, audastra::japi::Host& host) {
                    const auto status = GameUi::InstallGameUiModule127(host, image.Base(),
                        [&image](auto address, auto size) { return image.Executable(address, size); });
                    Trace("builtin-install audastraui status=" + std::to_string(status));
                    return status;
                });
            self->vm = attached.vm;
            Trace("vm-attach epoch=" + std::to_string(self->epoch) + (attached.vm ? " ok" : " failed"));
            return attached;
        }).value_or(0);
        return result;
    } catch (...) {
        std::lock_guard guard(self->lock);
        self->Fail("native dispatch exception"); return 0;
    }
}
void __cdecl InitializeHook() noexcept {
    auto* self = runtime.load();
    if (!self || !self->initialize) return;
    try {
        std::lock_guard guard(self->lock);
        if (self->thread == GetCurrentThreadId()) self->Reset();
    } catch (...) { self->Fail("native reset exception"); }
    self->initialize();
}
std::uintptr_t __fastcall DestroyVmHook(std::uintptr_t vm, std::uintptr_t) noexcept {
    auto* self = runtime.load();
    if (!self || !self->destroyVm) return 0;
    try {
        std::lock_guard guard(self->lock);
        if (self->thread == GetCurrentThreadId() && self->vm && self->vm->IsMainVm(vm)) self->Reset();
    } catch (...) { self->Fail("VM destroy notification exception"); }
    return self->destroyVm(vm);
}

struct Threads {
    std::vector<HANDLE> handles;
    ~Threads() { for (const auto handle : handles) CloseHandle(handle); }
};
// 先枚举并打开线程，再挂起；挂起阶段不做本方的容器扩容、日志或文件操作。
LONG OpenThreads(Threads& opened) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return GetLastError();
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot.value, &entry)) return GetLastError();
    do {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
        if (opened.handles.size() >= 256) return ERROR_TOO_MANY_TCBS;
        const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE, entry.th32ThreadID);
        if (!thread) {
            const auto error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) continue; // 快照后已退出的线程。
            return error;
        }
        opened.handles.push_back(thread);
    } while (Thread32Next(snapshot.value, &entry));
    return GetLastError() == ERROR_NO_MORE_FILES ? NO_ERROR : GetLastError();
}
int Start() {
    std::lock_guard install(installLock);
    if (auto* current = runtime.load()) {
        std::lock_guard guard(current->lock);
        Trace("initialize-repeat");
        return current->failed ? AJ_STATE : AJ_OK;
    }
    Trace("initialize-enter");
    auto opened = OwnLua::GameImage127::Open(GetModuleHandleW(L"Game.dll"));
    if (!opened.image) { Trace("game-identity-rejected: " + opened.error); return AJ_POLICY; }
    const auto& image = *opened.image;
    constexpr unsigned char localized[]{0x55, 0x8b, 0xec, 0x81, 0xec, 0x04, 0x08, 0x00, 0x00};
    constexpr unsigned char initialize[]{0xe8, 0x5b, 0x96, 0x5f, 0x00, 0x68};
    constexpr unsigned char destroy[]{0x55, 0x8b, 0xec, 0x6a, 0xff};
    if (!image.Matches(kLocalized, localized) || !image.Matches(kInitialize, initialize)
        || !image.Matches(kDestroyVm, destroy)) {
        Trace("hook-sites-changed: refusing existing hooks/unsupported image"); return AJ_POLICY;
    }
    auto owned = std::make_unique<Runtime>();
    owned->image = std::move(opened.image);
    owned->localized = reinterpret_cast<Localized>(image.Base() + kLocalized);
    owned->initialize = reinterpret_cast<Initialize>(image.Base() + kInitialize);
    owned->destroyVm = reinterpret_cast<DestroyVm>(image.Base() + kDestroyVm);
    Threads threads;
    threads.handles.reserve(256);
    auto status = OpenThreads(threads);
    if (status != NO_ERROR) { Trace("thread-enumeration-failed win32=" + std::to_string(status)); return AJ_BACKEND; }
    // 成功钩子的代码必须保持可执行，不依赖外部调用方正确配对 FreeLibrary。
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&LocalizedHook), &pinned)) {
        Trace("pin-runtime-failed"); return AJ_BACKEND;
    }
    status = DetourTransactionBegin();
    if (status != NO_ERROR) { Trace("detours-begin-failed"); return AJ_BACKEND; }
    runtime.store(owned.get());
    status = DetourAttach(reinterpret_cast<PVOID*>(&owned->localized), LocalizedHook);
    if (status == NO_ERROR) status = DetourAttach(reinterpret_cast<PVOID*>(&owned->initialize), InitializeHook);
    if (status == NO_ERROR) status = DetourAttach(reinterpret_cast<PVOID*>(&owned->destroyVm), DestroyVmHook);
    if (status == NO_ERROR) {
        for (const auto thread : threads.handles) {
            status = DetourUpdateThread(thread);
            if (status != NO_ERROR) break;
        }
    }
    if (status == NO_ERROR) status = DetourTransactionCommit();
    else DetourTransactionAbort();
    if (status != NO_ERROR) {
        runtime.store(nullptr);
        Trace("hook-install-failed win32=" + std::to_string(status)); return AJ_BACKEND;
    }
    owned.release(); // 钩子与会话为进程级拥有；不在 DllMain 卸载/析构。
    Trace("initialize-ready: awaiting first JASS request; builtin=audastraui");
    return AJ_OK;
}
}
}

extern "C" __declspec(dllexport) int __cdecl WarVKAIInit(void) {
    try { return Audastra::Runtime32::Japi::Start(); }
    catch (...) { OutputDebugStringA("[AudastraJapi] initialize-exception\n"); return AJ_INTERNAL; }
}
