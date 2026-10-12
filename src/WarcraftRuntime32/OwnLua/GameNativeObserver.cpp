#include "GameNativeObserver.h"
#include "GameVm127.h"
#include "LuaEntryCommand.h"

#include <detours.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <mutex>

namespace Audastra::Runtime32::OwnLua {
namespace {
using RegisterFunction = int(__fastcall*)(std::uintptr_t, const char*, const char*);
using InitializeFunction = void(__cdecl*)();
using SupplementFunction = int(__cdecl*)();
using CheatFunction = std::uint32_t(__cdecl*)(std::uintptr_t);
using RunFunction = int(__fastcall*)(std::uint32_t, std::uint32_t, std::uintptr_t, std::uint32_t*, int, int, int);
using DestroyVmFunction = std::uintptr_t(__thiscall*)(std::uintptr_t);
constexpr std::uintptr_t kRegisterRva = 0x7e3710, kInitializeRva = 0x1e9a50;
constexpr std::uintptr_t kSupplementRva = 0x892d80;
constexpr std::uintptr_t kRunRva = 0x7e2fa0, kDestroyVmRva = 0x7dd070;
struct ThreadRegistration {
    std::shared_ptr<NativeRegistry> registry;
    std::uint64_t generation = 0;
    CheatFunction cheat = nullptr;
};
struct Observer {
    std::shared_ptr<GameImage127> image;
    GameNativeHandlers handlers;
    RegisterFunction registerNative = nullptr;
    InitializeFunction initialize = nullptr;
    SupplementFunction supplement = nullptr;
    RunFunction run = nullptr;
    DestroyVmFunction destroyVm = nullptr;
    std::mutex mutex;
    std::map<DWORD, ThreadRegistration> threads;
    std::uint64_t generation = 0;
    std::atomic<bool> failed{false};
    void Fail(std::string_view error) noexcept {
        failed = true;
        try { if (handlers.failure) handlers.failure(error); } catch (...) {}
    }
};
std::atomic<Observer*> observer{nullptr};

template<std::size_t N> bool CopyText(const char* source, std::array<char, N>& text) noexcept {
    __try {
        if (!source) return false;
        for (std::size_t index = 0; index < N; ++index) {
            text[index] = source[index];
            if (!text[index]) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::uint32_t __cdecl ObserveCheat(std::uintptr_t stringObject) noexcept {
    auto* self = observer.load();
    if (!self) return 0;
    try {
        const auto command = GameVm127::ReadStringObject(stringObject);
        if (!command) { self->Fail("Cheat 收到无效或超预算的 VM 字符串。"); return 0; }
        const auto parsed = ParseLuaEntryCommand(*command);
        ThreadRegistration current;
        std::string failure;
        {
            std::lock_guard lock(self->mutex);
            const auto found = self->threads.find(GetCurrentThreadId());
            if (found != self->threads.end()) current = found->second;
            if (parsed.recognized && parsed.error.empty()) {
                if (self->failed || !current.registry || !current.registry->Ready() || !self->handlers.module)
                    failure = "Lua 入口的实际 native 注册快照尚未就绪或已经失效。";
                else if (!current.registry->Seal()) failure = current.registry->Error();
            }
        }
        // 保留游戏本来处理的普通命令。Lua 入口失败不落回作弊解释器。
        if (!parsed.recognized) return current.cheat ? current.cheat(stringObject) : 0;
        if (!parsed.error.empty()) { self->Fail(parsed.error); return 0; }
        if (!failure.empty()) { self->Fail(failure); return 0; }
        self->handlers.module({self->image, current.registry, current.generation, true}, parsed.module);
    } catch (...) { self->Fail("处理游戏 Lua 入口时发生异常。"); }
    return 0;
}
int __fastcall ObserveRegistration(std::uintptr_t function, const char* name, const char* signature) noexcept {
    auto* self = observer.load();
    if (!self || !self->registerNative) return 0;
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    auto registered = function;
    try {
        std::array<char, 256> copiedName{};
        std::array<char, 4096> copiedSignature{};
        const auto executable = self->image->Executable(function);
        const auto nameReadable = CopyText(name, copiedName);
        const auto signatureReadable = CopyText(signature, copiedSignature);
        if (!executable || !nameReadable || !signatureReadable) {
            self->Fail("native 登记输入不能读取或不属于已核验映像：address-in-image=" + std::to_string(executable)
                + "; name-readable=" + std::to_string(nameReadable) + "; signature-readable=" + std::to_string(signatureReadable)
                + "; address=" + std::to_string(function));
        } else {
            std::string failure;
            {
                std::lock_guard lock(self->mutex);
                const auto found = self->threads.find(GetCurrentThreadId());
                if (found == self->threads.end() || !found->second.registry || !found->second.registry->Collecting()) {
                    const auto* current = found == self->threads.end() ? nullptr : &found->second;
                    failure = "native 登记发生在完整初始化观察范围之外。 gate=initialization-scope; "
                        + DescribeNativeRegistration(current && current->registry ? current->registry->Size() : 0,
                            copiedName.data(), copiedSignature.data(), function)
                        + "; thread=" + std::to_string(GetCurrentThreadId())
                        + "; generation=" + std::to_string(current ? current->generation : 0)
                        + "; collecting=" + std::to_string(current && current->registry && current->registry->Collecting())
                        + "; ready=" + std::to_string(current && current->registry && current->registry->Ready())
                        + "; caller-in-image=" + std::to_string(self->image->Executable(caller))
                        + "; caller-rva=" + std::to_string(self->image->Executable(caller) ? caller - self->image->Base() : 0);
                } else {
                    auto& current = found->second;
                    if (!current.registry->Observe(copiedName.data(), copiedSignature.data(), function)) {
                        failure = std::string(current.registry->Error()) + "; thread=" + std::to_string(GetCurrentThreadId())
                            + "; generation=" + std::to_string(current.generation) + "; collecting=true";
                    } else if (std::string_view(copiedName.data()) == "Cheat") {
                        if (std::string_view(copiedSignature.data()) != "(S)V") failure = "Cheat 实际签名与已核对的地图入口不符。";
                        else {
                            current.cheat = reinterpret_cast<CheatFunction>(function);
                            registered = reinterpret_cast<std::uintptr_t>(&ObserveCheat);
                        }
                    }
                }
            }
            if (!failure.empty()) self->Fail(failure);
        }
    } catch (...) { self->Fail("读取实际 native 登记时发生异常。"); }
    return self->registerNative(registered, name, signature);
}
void __cdecl ObserveInitialize() noexcept {
    auto* self = observer.load();
    if (!self || !self->initialize) return;
    try {
        if (self->handlers.reset) self->handlers.reset();
        bool accepted = false;
        {
            std::lock_guard lock(self->mutex);
            const auto id = GetCurrentThreadId();
            if (!self->failed && (self->threads.contains(id) || self->threads.size() < 16) && self->generation != UINT64_MAX) {
                auto& current = self->threads[id];
                if (!current.registry || !current.registry->Collecting()) {
                    current = {std::make_shared<NativeRegistry>(), ++self->generation, nullptr};
                    accepted = true;
                }
            }
        }
        if (!accepted) self->Fail("native 初始化嵌套、线程数或代际超出范围。");
    } catch (...) { self->Fail("准备实际 native 登记观察时发生异常。"); }
    self->initialize();
    try {
        bool ready = false;
        {
            std::lock_guard lock(self->mutex);
            const auto found = self->threads.find(GetCurrentThreadId());
            if (found != self->threads.end()) {
                auto& current = found->second;
                ready = !self->failed && current.cheat && current.registry && current.registry->CompleteStock();
            }
        }
        if (!ready) self->Fail("完整 native 初始化未产生有效的 Lua 入口登记。");
    } catch (...) { self->Fail("完成实际 native 登记观察时发生异常。"); }
}
int __cdecl ObserveSupplement() noexcept {
    auto* self = observer.load();
    if (!self || !self->supplement) return 0;
    bool accepted = false;
    try {
        std::lock_guard lock(self->mutex);
        const auto found = self->threads.find(GetCurrentThreadId());
        if (!self->failed && found != self->threads.end() && found->second.cheat && found->second.registry)
            accepted = found->second.registry->BeginSupplement();
    } catch (...) { self->Fail("准备原版补充 native 登记时发生异常。"); }
    if (!accepted) self->Fail("原版补充 native 登记不在同线程完整 stock 初始化之后，或已交给 Lua。");
    // 原版先做辅助初始化，再尾跳转到124条补充登记；保留其调用约定、返回值和原始转发。
    const auto result = self->supplement();
    try {
        bool ready = false;
        {
            std::lock_guard lock(self->mutex);
            const auto found = self->threads.find(GetCurrentThreadId());
            ready = accepted && !self->failed && found != self->threads.end() && found->second.registry
                && found->second.registry->CompleteSupplement();
        }
        if (!ready) self->Fail("原版补充 native 登记未完整完成，不能交给 Lua 使用。");
    } catch (...) { self->Fail("完成原版补充 native 登记时发生异常。"); }
    return result;
}
bool WriteCodeResult(std::uint32_t* destination, std::uint32_t value) noexcept {
    if (!destination) return true;
    __try { *destination = value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int __fastcall ObserveRun(std::uint32_t vmIndex, std::uint32_t code, std::uintptr_t parameters,
    std::uint32_t* result, int budget, int option1, int option2) noexcept {
    auto* self = observer.load();
    if (!self) return 6;
    try {
        const auto own = self->handlers.code(vmIndex, code, parameters != 0);
        if (own) {
            if (!WriteCodeResult(result, own->succeeded && own->booleanValue ? 1 : 0)) {
                self->Fail("Lua code 返回缓冲无效。"); return 6;
            }
            // 上层游戏入口已建立 GetExpiredTimer/GetTriggerUnit 等上下文。
            // 没有构造 JASS 解释栈，因此不修改它；正常结束/错误由上层按原协议清理子 VM。
            return own->succeeded ? 1 : 6;
        }
    } catch (...) { self->Fail("分派游戏 Lua code 时发生异常。"); return 6; }
    return self->run(vmIndex, code, parameters, result, budget, option1, option2);
}
std::uintptr_t __fastcall ObserveVmDestroy(std::uintptr_t vm, std::uintptr_t) noexcept {
    auto* self = observer.load();
    if (!self) return 0;
    try { self->handlers.beforeVmDestroy(vm); }
    catch (...) { self->Fail("清理 Lua 所属游戏 VM 时发生异常。"); }
    return self->destroyVm(vm);
}
}

bool InstallGameNativeObserver(std::shared_ptr<GameImage127> image, GameNativeHandlers handlers, std::string& error) {
    if (!image || !handlers.module || !handlers.reset || !handlers.failure || !handlers.code || !handlers.beforeVmDestroy) {
        error = "游戏 native 观察器缺少来源或生命周期处理器。"; return false;
    }
    // 入口固定字节均已在命中 SHA 的映像中只读核对；不覆盖未知第三方已有 hook。
    constexpr unsigned char registration[]{0x55, 0x8b, 0xec, 0x81, 0xec, 0x0c, 0x01, 0x00, 0x00};
    constexpr unsigned char initialization[]{0xe8, 0x5b, 0x96, 0x5f, 0x00, 0x68};
    constexpr unsigned char supplement[]{0xe8, 0x0b, 0x00, 0x00, 0x00, 0xe9, 0x36, 0x00, 0x00, 0x00};
    constexpr unsigned char run[]{0x55, 0x8b, 0xec, 0x56, 0x57, 0x8b, 0xf2};
    constexpr unsigned char destroy[]{0x55, 0x8b, 0xec, 0x6a, 0xff};
    if (!image->Matches(kRegisterRva, registration) || !image->Matches(kInitializeRva, initialization)
        || !image->Matches(kSupplementRva, supplement)
        || !image->Matches(kRunRva, run) || !image->Matches(kDestroyVmRva, destroy)) {
        error = "Game.dll 的 native 观察入口已变化，拒绝覆盖。"; return false;
    }
    auto owned = std::make_unique<Observer>();
    owned->registerNative = reinterpret_cast<RegisterFunction>(image->Base() + kRegisterRva);
    owned->initialize = reinterpret_cast<InitializeFunction>(image->Base() + kInitializeRva);
    owned->supplement = reinterpret_cast<SupplementFunction>(image->Base() + kSupplementRva);
    owned->run = reinterpret_cast<RunFunction>(image->Base() + kRunRva);
    owned->destroyVm = reinterpret_cast<DestroyVmFunction>(image->Base() + kDestroyVmRva);
    owned->image = std::move(image); owned->handlers = std::move(handlers);
    Observer* absent = nullptr;
    if (!observer.compare_exchange_strong(absent, owned.get())) { error = "游戏 native 观察器已经安装。"; return false; }
    LONG code = DetourTransactionBegin();
    if (code == NO_ERROR) {
        code = DetourUpdateThread(GetCurrentThread());
        if (code == NO_ERROR) code = DetourAttach(reinterpret_cast<PVOID*>(&owned->registerNative), ObserveRegistration);
        if (code == NO_ERROR) code = DetourAttach(reinterpret_cast<PVOID*>(&owned->initialize), ObserveInitialize);
        if (code == NO_ERROR) code = DetourAttach(reinterpret_cast<PVOID*>(&owned->supplement), ObserveSupplement);
        if (code == NO_ERROR) code = DetourAttach(reinterpret_cast<PVOID*>(&owned->run), ObserveRun);
        if (code == NO_ERROR) code = DetourAttach(reinterpret_cast<PVOID*>(&owned->destroyVm), ObserveVmDestroy);
        if (code == NO_ERROR) code = DetourTransactionCommit();
        else DetourTransactionAbort();
    }
    if (code != NO_ERROR) {
        observer = nullptr; error = "安装游戏 native 观察器失败：" + std::to_string(code); return false;
    }
    // 明确的进程级拥有者；随测试进程释放，避免 loader-lock 下的 hook / Lua 析构。
    owned.release();
    return true;
}
} // namespace Audastra::Runtime32::OwnLua
