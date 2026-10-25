#include "OwnedLuaSession.h"
#include "GameNativeObserver.h"
#include "GameNativeProvider.h"
#include "LuaHost.h"
#include "LuaMailboxRuntime.h"
#include "LuaMapArchive.h"
#include "../Japi/GameSession.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"

#include <array>
#include <atomic>
#include <utility>

namespace Audastra::Runtime32::OwnLua {
namespace {
class PollCallback final : public NativeCallback {
public:
    explicit PollCallback(std::function<NativeCallbackResult()> poll) : poll_(std::move(poll)) {}
    NativeCallbackReturn Returns() const noexcept override { return NativeCallbackReturn::Nothing; }
    NativeCallbackResult Invoke() override { return poll_(); }
private:
    std::function<NativeCallbackResult()> poll_;
};

class Session final {
public:
    OwnedLuaSessionConfig config;
    OwnedLuaSessionEvents events;
    OwnedLuaObservation observation;
    std::shared_ptr<LuaMapArchive> archive;
    std::shared_ptr<const NativeTypeHierarchy> types;
    std::shared_ptr<GameVm127> vm;
    std::shared_ptr<GameNativeProvider> natives;
    std::unique_ptr<LuaHost> host;
    std::unique_ptr<LuaMailboxRuntime> mailbox;
    std::unique_ptr<Japi::NativeSession> japi;
    NativeValue timer;
    bool stopped = false;
    unsigned executionDepth = 0;
    unsigned callbackDepth = 0;
    std::atomic<DWORD> threadId{0};
    std::size_t consoleBytes = 0;

    void Publish() {
        if (natives) observation.nativeCalls = natives->InvocationCount();
        events.observe(observation);
    }
    void Report(const LuaHostResult& result) {
        if (!result.Succeeded()) ++observation.errors;
        std::string message = result.message;
        if (host) {
            const auto console = host->ConsoleText();
            if (console.size() > consoleBytes) {
                if (!message.empty()) message += '\n';
                message.append(console.substr(consoleBytes));
                host->ClearConsole();
                consoleBytes = 0;
            }
        }
        if (!message.empty() && mailbox) (void)mailbox->Report(result.code, message);
        Publish();
    }
    void Fatal(std::string_view error) {
        ++observation.errors;
        if (mailbox) (void)mailbox->Report(LuaReloadResult::HostFailure, error);
        if (vm) vm->Invalidate();
        stopped = true;
        if (mailbox) mailbox->Stop();
        observation.ready = false;
        Publish();
        events.fatal(error);
    }
    bool Start(const GameNativeContext& context) {
        if (stopped || !context.codeDispatchReady) { Fatal("自有 Lua 会话已停止或 code 桥未就绪。"); return false; }
        auto opened = GameVm127::OpenAtScriptEntry(context.image, context.registrationGeneration);
        if (!opened.vm) { Fatal(opened.error); return false; }
        vm = std::move(opened.vm);
        observation.threadId = GetCurrentThreadId();
        threadId.store(observation.threadId);
        observation.nativeCount = static_cast<std::uint32_t>(context.registry->Entries().size());
        mailbox = std::make_unique<LuaMailboxRuntime>(*config.mailbox);
        if (!mailbox->Attach(config.nonce, config.mapSha256)) { Fatal("自有 Lua 邮箱无法与原测试身份附着。"); return false; }
        host = std::make_unique<LuaHost>([source = archive](std::string_view member, std::size_t maxBytes) {
            return source->Read(member, maxBytes);
        });
        auto initialized = host->Initialize();
        if (!initialized.Succeeded()) { Fatal(initialized.message); return false; }
        natives = std::make_shared<GameNativeProvider>(vm, context.registry, types, context.codeDispatchReady);
        initialized = host->BindNatives(natives);
        if (!initialized.Succeeded()) { Fatal(initialized.message); return false; }
        {
            auto created = natives->Invoke("CreateTimer", {});
            if (!created.succeeded || !std::holds_alternative<NativeHandle>(created.value)) {
                Fatal("无法创建已核对身份的热重载游戏 timer：" + created.error); return false;
            }
            timer = std::move(created.value);
            const std::array<NativeValue, 4> arguments{timer, 0.05f, true,
                std::shared_ptr<NativeCallback>(std::make_shared<PollCallback>([this] { return Poll(); }))};
            const auto started = natives->Invoke("TimerStart", arguments);
            if (!started.succeeded) { Fatal("无法启动主线程热重载轮询：" + started.error); return false; }
        }
        observation.ready = true;
        Publish();
        return true;
    }
    void Module(const GameNativeContext& context, std::string_view name) {
        DWORD absent = 0;
        (void)threadId.compare_exchange_strong(absent, GetCurrentThreadId());
        const auto owner = threadId.load();
        if (owner && owner != GetCurrentThreadId()) { events.fatal("Lua 地图入口不在原脚本线程。"); return; }
        if (!host && !Start(context)) return;
        if (stopped || !vm || !vm->Alive() || GetCurrentThreadId() != observation.threadId
            || context.registrationGeneration != vm->Generation() || executionDepth) {
            Fatal("Lua 模块入口发生重入、线程变化或地图代际变化。"); return;
        }
        ++executionDepth;
        const auto result = host->ExecuteModule(name);
        --executionDepth;
        if (result.Succeeded()) ++observation.modules;
        Report(result);
        if (!vm->Alive()) Fatal("Lua 模块返回时游戏 VM 已失效，已停止本次测试。");
    }
    NativeCallbackResult Poll() {
        ++observation.polls;
        if (stopped || !vm || !vm->Alive()) return {false, false, "热重载轮询所属 VM 已失效。"};
        // native 可能同步回调 Lua；若时间推进发生嵌套，等待下一次 timer，不重入外层执行。
        if (executionDepth || callbackDepth > 1) return {true, false, {}};
        auto request = mailbox->Take();
        if (!request) { Publish(); return {true, false, {}}; }
        if (!config.hotReload) {
            if (!mailbox->Complete(request->sequence, LuaReloadResult::NotReady, "本次测试没有开启 Lua 热重载。"))
                Fatal("无法回执未启用的热重载请求。");
            return {true, false, {}};
        }
        ++executionDepth;
        // “module:<模块名>” 是作者手写 Lua 文件的模块热重载；其余名称按一段独立代码执行。
        constexpr std::string_view modulePrefix = "module:";
        const auto result = std::string_view(request->chunkName).starts_with(modulePrefix)
            ? host->ReloadModule(std::string_view(request->chunkName).substr(modulePrefix.size()), request->source)
            : host->Execute(request->source, request->chunkName);
        --executionDepth;
        if (!mailbox->Complete(request->sequence, result.code, result.message)) {
            Fatal("热重载已经执行，但无法提交本次请求的独立回执。");
            return {false, false, "热重载回执身份已失效。"};
        }
        // 错误正文只进入请求 ACK；这里只转发本次打印，避免 UI 重复展示同一错误。
        Report({LuaReloadResult::Success, {}});
        if (!vm->Alive()) Fatal("热重载返回时游戏 VM 已失效，已停止本次测试。");
        return {true, false, {}};
    }
    std::optional<NativeCallbackResult> Code(std::uint32_t vmIndex, std::uint32_t code, bool parameters) {
        // native登记及code表属于线程TLS；尚未附着或其它线程的JASS不属于本会话。
        if (GetCurrentThreadId() != threadId.load()) return {};
        if (!natives) return {};
        ++callbackDepth;
        auto result = natives->DispatchCode(vmIndex, code, parameters);
        --callbackDepth;
        if (result) {
            ++observation.callbacks;
            Report({result->succeeded ? LuaReloadResult::Success : LuaReloadResult::RuntimeError, result->error});
            if (!vm->Alive()) Fatal("Lua code 返回时游戏 VM 已失效，已停止本次测试。");
        }
        return result;
    }
    void Reset() {
        if (japi) japi->Reset();
        if (GetCurrentThreadId() == threadId.load() && host)
            Fatal("测试过程中重新登记了 native；本次 Lua 会话不跨地图或 VM 代际复用。");
    }
    void BeforeVmDestroy(std::uintptr_t destroying) {
        if (japi) japi->BeforeVmDestroy(destroying);
        if (GetCurrentThreadId() != threadId.load()) return;
        if (!vm || !vm->IsMainVm(destroying)) return;
        if (executionDepth || callbackDepth) { Fatal("游戏在 Lua 执行期间销毁其所属 VM。"); return; }
        // VM 尚未析构，先让 Lua 释放自己持有的原句柄，再使 provider/code 失效。
        stopped = true;
        if (mailbox) mailbox->Stop();
        host.reset();
        timer = {};
        natives->BeforeVmDestroy(destroying);
        observation.ready = false;
        Publish();
    }
};
}

bool InstallOwnedLuaSession(std::shared_ptr<GameImage127> image, OwnedLuaSessionConfig config,
    OwnedLuaSessionEvents events, std::string& error) {
    if (!image || !config.mailbox || !LuaMailboxHeader(*config.mailbox)
        || !LuaMailboxHex(config.nonce, 32) || !LuaMailboxHex(config.mapSha256, 64)
        || std::string_view(config.mailbox->sessionNonce, 32) != config.nonce
        || std::string_view(config.mailbox->mapSha256, 64) != config.mapSha256
        || !events.observe || !events.fatal) {
        error = "自有 Lua 会话缺少已核对的地图、邮箱身份或进程终止处理器。"; return false;
    }
    auto archive = LuaMapArchive::Open(config.mapPath, config.mapSha256);
    if (!archive.archive) { error = std::move(archive.error); return false; }
    auto types = NativeTypeHierarchy::ReadGame(image->Path().parent_path());
    if (!types.types) { error = std::move(types.error); return false; }
    auto owned = std::make_unique<Session>();
    owned->config = std::move(config); owned->events = std::move(events);
    owned->archive = std::move(archive.archive); owned->types = std::move(types.types);
    auto* self = owned.get();
    if (self->config.experimentalJapi)
        self->japi = std::make_unique<Japi::NativeSession>([self](auto error) { self->events.fatal(error); });
    GameNativeHandlers handlers;
    handlers.reset = [self] { self->Reset(); };
    handlers.module = [self](const auto& context, auto module) { self->Module(context, module); };
    handlers.code = [self](auto vm, auto code, auto parameters) { return self->Code(vm, code, parameters); };
    handlers.beforeVmDestroy = [self](auto vm) { self->BeforeVmDestroy(vm); };
    handlers.failure = [self](auto message) { self->Fatal(message); };
    if (self->japi) handlers.localizedString = [self](const auto& context, auto input) {
        return Japi::DispatchGameString(*self->japi, context, input, self->config.japiModules);
    };
    if (!InstallGameNativeObserver(std::move(image), std::move(handlers), error)) return false;
    owned.release();
    return true;
}

} // namespace Audastra::Runtime32::OwnLua
