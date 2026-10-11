#include "GameNativeObserver.h"
#include "GameVm127.h"
#include "LuaEntryCommand.h"

#include <detours.h>
#include <array>
#include <atomic>
#include <mutex>

namespace Audastra::Runtime32::OwnLua {
namespace {
using RegisterFunction = int(__fastcall*)(std::uintptr_t, const char*, const char*);
using InitializeFunction = void(__cdecl*)();
using CheatFunction = std::uint32_t(__cdecl*)(std::uintptr_t);
constexpr std::uintptr_t kRegisterRva = 0x7e3710, kInitializeRva = 0x1e9a50;
struct ThreadRegistration {
    std::shared_ptr<NativeRegistry> registry;
    std::uint64_t generation = 0;
    CheatFunction cheat = nullptr;
    bool collecting = false, ready = false;
};
struct Observer {
    std::shared_ptr<GameImage127> image;
    GameNativeHandlers handlers;
    RegisterFunction registerNative = nullptr;
    InitializeFunction initialize = nullptr;
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
        {
            std::lock_guard lock(self->mutex);
            const auto found = self->threads.find(GetCurrentThreadId());
            if (found != self->threads.end()) current = found->second;
        }
        // 保留游戏本来处理的普通命令。Lua 入口失败不落回作弊解释器。
        if (!parsed.recognized) return current.cheat ? current.cheat(stringObject) : 0;
        if (!parsed.error.empty()) { self->Fail(parsed.error); return 0; }
        if (self->failed || !current.ready || !current.registry || !current.registry->Valid() || !self->handlers.module) {
            self->Fail("Lua 入口的实际 native 注册快照尚未就绪或已经失效。"); return 0;
        }
        self->handlers.module({self->image, current.registry, current.generation}, parsed.module);
    } catch (...) { self->Fail("处理游戏 Lua 入口时发生异常。"); }
    return 0;
}
int __fastcall ObserveRegistration(std::uintptr_t function, const char* name, const char* signature) noexcept {
    auto* self = observer.load();
    if (!self || !self->registerNative) return 0;
    auto registered = function;
    try {
        std::array<char, 256> copiedName{};
        std::array<char, 4096> copiedSignature{};
        if (!self->image->Executable(function) || !CopyText(name, copiedName) || !CopyText(signature, copiedSignature)) {
            self->Fail("native 登记地址不属于已核验映像，或名字／签名超过预算。");
        } else {
            std::string failure;
            {
                std::lock_guard lock(self->mutex);
                const auto found = self->threads.find(GetCurrentThreadId());
                if (found == self->threads.end() || !found->second.collecting || !found->second.registry) {
                    failure = "native 登记发生在完整初始化观察范围之外。";
                } else {
                    auto& current = found->second;
                    if (!current.registry->Observe(copiedName.data(), copiedSignature.data(), function)) {
                        failure = current.registry->Error();
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
            if ((self->threads.contains(id) || self->threads.size() < 16) && self->generation != UINT64_MAX) {
                auto& current = self->threads[id];
                if (!current.collecting) {
                    current = {std::make_shared<NativeRegistry>(), ++self->generation, nullptr, true, false};
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
                ready = current.collecting && current.cheat && current.registry && current.registry->Valid() && !self->failed;
                current.collecting = false; current.ready = ready;
            }
        }
        if (!ready) self->Fail("完整 native 初始化未产生有效的 Lua 入口登记。");
    } catch (...) { self->Fail("完成实际 native 登记观察时发生异常。"); }
}
}

bool InstallGameNativeObserver(std::shared_ptr<GameImage127> image, GameNativeHandlers handlers, std::string& error) {
    if (!image || !handlers.module || !handlers.reset || !handlers.failure) { error = "游戏 native 观察器缺少来源或生命周期处理器。"; return false; }
    // 入口固定字节均已在命中 SHA 的映像中只读核对；不覆盖未知第三方已有 hook。
    constexpr unsigned char registration[]{0x55, 0x8b, 0xec, 0x81, 0xec, 0x0c, 0x01, 0x00, 0x00};
    constexpr unsigned char initialization[]{0xe8, 0x5b, 0x96, 0x5f, 0x00, 0x68};
    if (!image->Matches(kRegisterRva, registration) || !image->Matches(kInitializeRva, initialization)) {
        error = "Game.dll 的 native 观察入口已变化，拒绝覆盖。"; return false;
    }
    auto owned = std::make_unique<Observer>();
    owned->registerNative = reinterpret_cast<RegisterFunction>(image->Base() + kRegisterRva);
    owned->initialize = reinterpret_cast<InitializeFunction>(image->Base() + kInitializeRva);
    owned->image = std::move(image); owned->handlers = std::move(handlers);
    Observer* absent = nullptr;
    if (!observer.compare_exchange_strong(absent, owned.get())) { error = "游戏 native 观察器已经安装。"; return false; }
    LONG code = DetourTransactionBegin();
    if (code == NO_ERROR) {
        code = DetourUpdateThread(GetCurrentThread());
        if (code == NO_ERROR) code = DetourAttach(reinterpret_cast<PVOID*>(&owned->registerNative), ObserveRegistration);
        if (code == NO_ERROR) code = DetourAttach(reinterpret_cast<PVOID*>(&owned->initialize), ObserveInitialize);
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
