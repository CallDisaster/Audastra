#include "GameSession.h"
#include "ProcessOwner.h"
#include "RuntimeTestWriter.h"
#include "../GameUi/GameUiMemory.h"
#include "HookSite.h"
#include "../OwnLua/GameVm127.h"
#include "../GameUi/GameUiModule.h"
#include "../GameUi/GameUiMemory.h"
#include "../RuntimeFiles.h"
#include <detours.h>
#include <tlhelp32.h>
#include <array>
#include <atomic>
#include <charconv>
#include <bit>
#include <cstdio>
#include <mutex>
#include <vector>

// 仅实现 AGENT_USAGE.md 的 DLL 契约；不读取/解析引导器，不依赖启动器或邮箱。
namespace Audastra::Runtime32::Japi {
namespace {
using Localized = std::uint32_t(__cdecl*)(std::uintptr_t);
using Initialize = void(__cdecl*)();
using DestroyVm = std::uintptr_t(__thiscall*)(std::uintptr_t);
using SaveReal = void(__cdecl*)(std::int32_t, std::int32_t, std::int32_t, const float*);
using SaveStr = std::int32_t(__cdecl*)(std::int32_t, std::int32_t, std::int32_t, std::uintptr_t);
using SaveBoolean = void(__cdecl*)(std::int32_t, std::int32_t, std::int32_t, std::int32_t);
using SaveInteger = void(__cdecl*)(std::int32_t, std::int32_t, std::int32_t, std::int32_t);
// 原版同 SHA 的 stock 登记第24项：GetLocalizedString (S)S，登记调用 RVA 1e9c30。
// 只读核验来自本项目原有 stock 登记扫描器；不是引导器实现中的地址。
constexpr std::uintptr_t kLocalized = 0x1e31d0;
constexpr std::uintptr_t kInitialize = 0x1e9a50, kDestroyVm = 0x7dd070;
// 来自固定映像的 stock native 登记与对应实现；real 是指针，string 是 VM object。
constexpr std::uintptr_t kSaveReal = 0x1f3a10, kSaveStr = 0x1f3af0, kSaveBoolean = 0x1f3570;
// 固定映像 stock 登记 RVA 1ed0c9..1ed0d8：SaveInteger (Hhashtable;III)V。
constexpr std::uintptr_t kSaveInteger = 0x1f3890;
constexpr std::uintptr_t kResolveTable = 0x1cf9d0;
RuntimeTestWriter testEvidence;

void Trace(std::string_view message, bool fault = false, bool counter = false, bool observation = false, bool lifecycle = false) noexcept {
    try {
        static std::mutex lock;
        static HANDLE file = INVALID_HANDLE_VALUE;
        static bool attempted = false;
        static unsigned lines = 0;
        static unsigned faults = 0;
        static unsigned counters = 0;
        static unsigned observations = 0;
        static unsigned lifecycles = 0;
        std::lock_guard guard(lock);
        if (!attempted) {
            attempted = true;
            std::array<wchar_t, 32768> path{};
            const auto length = GetTempPathW(static_cast<DWORD>(path.size()), path.data());
            if (length && length < path.size() && testEvidence.Enabled()) {
                const auto name = std::filesystem::path(path.data()) / (L"audastra-japi-"
                    + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".log");
                file = CreateFileW(name.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
        }
        // 故障保留独立预算，普通轮询刷满后也能记录；两者都不无限增长。
        if (lifecycle ? lifecycles >= 256 : observation ? observations >= 2048 : counter ? counters >= 32 : fault ? faults >= 64 : lines >= 2048) return;
        if (lifecycle) ++lifecycles; else if (observation) ++observations; else if (counter) ++counters; else if (fault) ++faults; else ++lines;
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
// 高频轮询和文字观察不能挤掉换图、会话销毁的有限诊断记录。
void TraceLifecycle(std::string_view message) { Trace(message, false, false, false, true); }
struct Runtime {
    std::shared_ptr<OwnLua::GameImage127> image;
    Localized localized = nullptr;
    Initialize initialize = nullptr;
    DestroyVm destroyVm = nullptr;
    SaveReal saveReal = nullptr;
    SaveStr saveStr = nullptr;
    SaveBoolean saveBoolean = nullptr;
    SaveInteger saveInteger = nullptr;
    bool dataHooks = false;
    std::atomic<std::int32_t> boundTable{0};
    std::recursive_mutex lock;
    std::unique_ptr<NativeSession> session;
    std::shared_ptr<StringVm> vm;
    std::shared_ptr<GameUi::GameUiDataPlane> data;
    DWORD thread = 0;
    std::uint64_t epoch = 1;
    unsigned depth = 0;
    bool failed = false;
    std::string identity;
    std::array<std::unique_ptr<HookSite>, 8> sites;

    void Fail(std::string_view message) {
        failed = true;
        if (vm) vm->Invalidate();
        Trace("failure: " + std::string(message));
    }
    void Reset() {
        boundTable.store(0, std::memory_order_release);
        if (data) data->Stop();
        if (depth) {
            if (session) session->Reset(); // 失效而不析构正在调用的 Host。
            Fail("VM reset during active JAPI call"); return;
        }
        if (session) session->Reset();
        session.reset(); data.reset(); vm.reset(); thread = 0;
        if (++epoch > INT32_MAX) { Fail("epoch exhausted"); return; }
        testEvidence.Reset(static_cast<std::uint32_t>(epoch));
        failed = false;
        TraceLifecycle("vm-reset epoch=" + std::to_string(epoch));
    }
};
std::atomic<Runtime*> runtime{nullptr};
std::mutex installLock;
std::atomic<int> ownerState{0};

void TraceUiFault(const GameUi::UiFault& fault) noexcept {
    try {
        auto* self = runtime.load();
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08X", fault.code);
        std::string location = "outside_verified_game";
        if (self && self->image && self->image->Executable(fault.address, 1)) {
            char offset[24]{};
            std::snprintf(offset, sizeof(offset), "0x%08X", static_cast<unsigned>(fault.address - self->image->Base()));
            location = offset;
        }
        Trace("ui-fault epoch=" + std::to_string(fault.epoch) + " request=" + std::to_string(fault.request)
            + " entry=" + (fault.entry ? fault.entry : "internal") + " code=" + code + " game_rva=" + location, true);
    } catch (...) { Trace("ui-fault diagnostic-failure", true); }
}

void TraceReply(std::string_view input, std::string_view output) {
    audastra::japi::Request request;
    audastra::japi::Response reply;
    if (audastra::japi::ParseRequest(input, request) != AJ_OK
        || audastra::japi::ParseResponse(output, reply) != AJ_OK) return;
    if (request.command == "host.hello" && reply.status == AJ_OK) testEvidence.Hello(reply.epoch);
    auto message = "epoch=" + std::to_string(reply.epoch) + " request=" + std::to_string(reply.id)
        + " command=" + request.command + " status=" + std::to_string(reply.status)
        + " kind=" + std::to_string(reply.value.kind);
    // 只记录布尔结果与句柄是否存在；不记录作者文字、实际句柄或请求参数。
    if (reply.value.kind == AJ_BOOL) message += " bool=" + std::to_string(reply.value.integer);
    if (reply.value.kind == AJ_HANDLE) message += reply.value.integer ? " handle=nonzero" : " handle=zero";
    if (testEvidence.Enabled() && reply.value.kind == AJ_HANDLE)
        message += " handle_value=" + std::to_string(reply.value.integer);
    if (testEvidence.Enabled() && request.command == "audastraui.clickunit" && reply.value.kind == AJ_INT)
        message += " input_result=" + std::to_string(reply.value.integer);
    if (request.command == "audastraui.dataerrors") {
        // 只记录自有计数，不记录作者文字/资源路径；独立预算不会挤掉真正的异常日志。
        std::uint32_t count = 0;
        const auto separator = reply.value.text.find('|');
        bool valid = reply.status == AJ_OK && reply.value.kind == AJ_TEXT && separator != std::string::npos && separator > 0;
        if (valid) {
            const auto begin = reply.value.text.data();
            const auto parsed = std::from_chars(begin, begin + separator, count);
            valid = parsed.ec == std::errc{} && parsed.ptr == begin + separator && count <= INT32_MAX;
        }
        Trace("ui-data-errors epoch=" + std::to_string(reply.epoch) + " request=" + std::to_string(reply.id)
            + " status=" + std::to_string(reply.status) + (valid ? " count=" + std::to_string(count) : " count=unavailable"), false, true);
    }
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
            [self](auto error) { self->Fail(error); }, &TraceReply, self->identity,
            [self](auto epoch, auto state, auto status, auto expected) {
                testEvidence.Installed(AJ_OK, self->identity);
                return testEvidence.Report(epoch, state, status, expected);
            });
        struct Depth { unsigned& value; ~Depth() { --value; } } depth{self->depth};
        ++self->depth;
        const std::string_view input(bytes.data(), count);
        const auto result = self->session->Dispatch(self->epoch, input, [self] {
            auto attached = AttachGameStringVm(self->image, self->epoch,
                [self](const OwnLua::GameImage127& image, audastra::japi::Host& host) {
                    if (self->dataHooks) {
                        const std::weak_ptr<StringVm> vm = self->vm;
                        GameUi::GameUiDataContext context{
                            reinterpret_cast<GameUi::FnResolveUiTable>(image.Base() + kResolveTable),
                            [vm] { const auto current = vm.lock(); return current && current->Generation() && current->MatchesCurrentMainVm(); }, &self->boundTable};
                        context.storeInteger = [self](std::int32_t table, std::int32_t parent, std::int32_t child, std::int32_t value) {
                            self->saveInteger(table, parent, child, value);
                        };
                        if (testEvidence.Enabled()) {
                            context.observeTextArgument = [self](std::uint32_t ordinal, GameUi::FrameHandle frame,
                                int index, bool integer, std::uint32_t bits) {
                                Trace("ui-text-argument epoch=" + std::to_string(self->epoch) + " ordinal=" + std::to_string(ordinal) + " frame=" + std::to_string(frame)
                                    + " index=" + std::to_string(index) + " type=" + (integer ? "integer" : "real")
                                    + " bits=" + std::to_string(bits), false, false, true);
                            };
                            context.observeTextCommit = [self](std::uint32_t ordinal, GameUi::FrameHandle frame, std::string_view text) {
                                Trace("ui-text-commit epoch=" + std::to_string(self->epoch) + " ordinal=" + std::to_string(ordinal) + " frame=" + std::to_string(frame)
                                    + " text=" + audastra::japi::EncodeText(text), false, false, true);
                            };
                        }
                        self->data = std::make_shared<GameUi::GameUiDataPlane>(std::move(context), &TraceUiFault);
                    }
                    std::function<void(GameUi::Address, GameUi::Address, bool, float, float)> pointerObserver;
                    if (testEvidence.Enabled()) {
                        const auto window = GetForegroundWindow();
                        DWORD process = 0;
                        const auto thread = GetWindowThreadProcessId(window, &process);
                        char name[80]{}; RECT client{};
                        if (process == GetCurrentProcessId() && GetClassNameA(window, name, sizeof(name)) && GetClientRect(window, &client))
                            Trace("ui-window class=" + std::string(name) + " thread=" + std::to_string(thread)
                                + " caller=" + std::to_string(GetCurrentThreadId()) + " width=" + std::to_string(client.right)
                                + " height=" + std::to_string(client.bottom), false, false, true);
                        pointerObserver = [self, base = image.Base(), lastHit = GameUi::Address{}, lastDown = false,
                            samples = 0u](GameUi::Address active, GameUi::Address hit, bool down, float queryX, float queryY) mutable {
                            if (samples >= 64 || (hit == lastHit && down == lastDown)) return;
                            lastHit = hit; lastDown = down; ++samples;
                            GameUi::Address x = 0, y = 0;
                            const bool event = GameUi::Detail::ReadWord(base + GameUi::Rva127::MouseEvent + 0x24, x)
                                && GameUi::Detail::ReadWord(base + GameUi::Rva127::MouseEvent + 0x28, y);
                            POINT cursor{};
                            const bool cursorOk = GetCursorPos(&cursor) != FALSE;
                            Trace("ui-pointer epoch=" + std::to_string(self->epoch) + " active=" + std::to_string(active)
                                + " hit=" + std::to_string(hit) + " down=" + std::to_string(down)
                                + " event=" + std::to_string(event) + " cached_x_bits=" + std::to_string(x)
                                + " cached_y_bits=" + std::to_string(y)
                                + " query_x_bits=" + std::to_string(std::bit_cast<std::uint32_t>(queryX))
                                + " query_y_bits=" + std::to_string(std::bit_cast<std::uint32_t>(queryY)) + " cursor=" + std::to_string(cursorOk)
                                + " cursor_x=" + std::to_string(cursor.x) + " cursor_y=" + std::to_string(cursor.y), false, false, true);
                        };
                    }
                    wchar_t legacy[2]{};
                    const bool legacyUi = testEvidence.Enabled()
                        && GetEnvironmentVariableW(L"AUDASTRA_RUNTIME_TEST_LEGACY_UI", legacy, 2) == 1 && legacy[0] == L'1';
                    if (legacyUi) TraceLifecycle("test-ui-legacy textformat=unsupported eventtable=unsupported");
                    const auto status = GameUi::InstallGameUiModule127(host, image.Base(),
                        [&image](auto address, auto size) { return image.Executable(address, size); }, &TraceUiFault, self->data,
                        std::move(pointerObserver), !legacyUi);
                    Trace("builtin-install audastraui status=" + std::to_string(status));
                    return status;
                });
            self->vm = attached.vm;
            TraceLifecycle("vm-attach epoch=" + std::to_string(self->epoch) + (attached.vm ? " ok" : " failed"));
            return attached;
        }).value_or(0);
        return result;
    } catch (...) {
        std::lock_guard guard(self->lock);
        self->Fail("native dispatch exception"); return 0;
    }
}
template<class Write>
GameUi::UiDataWrite WriteData(Runtime& self, Write&& write, const char* kind, std::int32_t frame, std::int32_t property,
    std::uintptr_t stringObject = 0) {
    // 与字符串入口共用串行/活动深度，写入中发生 VM 清理时只失效，不销毁在栈上的消费者。
    std::lock_guard guard(self.lock);
    const auto data = self.data;
    if (!data) return {};
    struct Depth { unsigned& value; ~Depth() { --value; } } depth{self.depth};
    ++self.depth;
    const auto result = write(*data);
    static std::atomic<unsigned> rejected{0};
    if (result.intercepted && !result.applied && testEvidence.Enabled() && rejected.fetch_add(1) < 16) {
        auto detail = "ui-data-rejected kind=" + std::string(kind) + " frame=" + std::to_string(frame)
            + " property=" + std::to_string(property) + " errors=" + data->Errors();
        if (std::string_view(kind) == "string") {
            GameUi::Address linked = 0, text = 0;
            const bool linkRead = stringObject && GameUi::Detail::ReadWord(stringObject + 8, linked);
            const bool textRead = linkRead && linked && GameUi::Detail::ReadWord(linked + 0x1c, text);
            detail += " object_zero=" + std::to_string(!stringObject) + " link_read=" + std::to_string(linkRead)
                + " link_zero=" + std::to_string(!linked) + " text_read=" + std::to_string(textRead)
                + " text_zero=" + std::to_string(!text);
        }
        Trace(detail, true);
    }
    return result;
}
void __cdecl SaveRealHook(std::int32_t table, std::int32_t parent, std::int32_t child, const float* value) noexcept {
    auto* self = runtime.load();
    if (!self || !self->saveReal) return;
    if (!table || table != self->boundTable.load(std::memory_order_acquire)) {
        self->saveReal(table, parent, child, value); return;
    }
    try {
        if (WriteData(*self, [&](auto& data) { return data.SaveReal(table, parent, child, value); }, "real", parent, child).intercepted) return;
        self->saveReal(table, parent, child, value);
    } catch (...) { Trace("data-write dispatch exception", true); }
}
std::int32_t __cdecl SaveStrHook(std::int32_t table, std::int32_t parent, std::int32_t child, std::uintptr_t value) noexcept {
    auto* self = runtime.load();
    if (!self || !self->saveStr) return 0;
    if (!table || table != self->boundTable.load(std::memory_order_acquire)) return self->saveStr(table, parent, child, value);
    try {
        const auto result = WriteData(*self, [&](auto& data) { return data.SaveStr(table, parent, child, value); }, "string", parent, child, value);
        if (result.intercepted) return result.applied ? 1 : 0;
        return self->saveStr(table, parent, child, value);
    } catch (...) { Trace("data-write dispatch exception", true); return 0; }
}
void __cdecl SaveBooleanHook(std::int32_t table, std::int32_t parent, std::int32_t child, std::int32_t value) noexcept {
    auto* self = runtime.load();
    if (!self || !self->saveBoolean) return;
    if (!table || table != self->boundTable.load(std::memory_order_acquire)) {
        self->saveBoolean(table, parent, child, value); return;
    }
    try {
        if (WriteData(*self, [&](auto& data) { return data.SaveBoolean(table, parent, child, value); }, "boolean", parent, child).intercepted) return;
        self->saveBoolean(table, parent, child, value);
    } catch (...) { Trace("data-write dispatch exception", true); }
}
void __cdecl SaveIntegerHook(std::int32_t table, std::int32_t parent, std::int32_t child, std::int32_t value) noexcept {
    auto* self = runtime.load();
    if (!self || !self->saveInteger) return;
    if (!table || table != self->boundTable.load(std::memory_order_acquire)) {
        self->saveInteger(table, parent, child, value); return;
    }
    try {
        if (WriteData(*self, [&](auto& data) { return data.SaveInteger(table, parent, child, value); }, "integer", parent, child).intercepted) return;
        self->saveInteger(table, parent, child, value);
    } catch (...) { Trace("data-write dispatch exception", true); }
}
void __cdecl InitializeHook() noexcept {
    auto* self = runtime.load();
    if (!self || !self->initialize) return;
    try {
        std::lock_guard guard(self->lock);
        // native登记也可能来自同局的其他VM；仅线程相同不足以认定换图。
        // 主VM销毁由独立接点处理，此处只补获真实身份已变化的情况。
        if (self->session && self->session->VmIdentityChanged()) self->Reset();
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
int Install(std::string_view identity) {
    if (auto* current = runtime.load()) {
        std::lock_guard guard(current->lock);
        Trace("initialize-repeat");
        return current->failed ? AJ_STATE : AJ_OK;
    }
    Trace("initialize-enter");
    auto opened = OwnLua::GameImage127::Open(GetModuleHandleW(L"Game.dll"));
    if (!opened.image) { Trace("game-identity-rejected: " + opened.error); return AJ_POLICY; }
    const auto& image = *opened.image;
    auto owned = std::make_unique<Runtime>();
    owned->identity = identity;
    owned->image = std::move(opened.image);
    owned->localized = reinterpret_cast<Localized>(image.Base() + kLocalized);
    owned->initialize = reinterpret_cast<Initialize>(image.Base() + kInitialize);
    owned->destroyVm = reinterpret_cast<DestroyVm>(image.Base() + kDestroyVm);
    constexpr std::array<std::uintptr_t, 8> rvas{kLocalized, kInitialize, kDestroyVm, kSaveReal, kSaveStr, kSaveBoolean, kResolveTable, kSaveInteger};
    constexpr std::array<const char*, 8> names{"localized", "initialize", "destroy_vm", "save_real", "save_str", "save_boolean", "resolve_table", "save_integer"};
    const auto reportSite = [&](std::size_t i, std::string_view reason) {
        auto message = "hook-site site=" + std::string(names[i]) + " reason=" + std::string(reason);
        if (testEvidence.Enabled()) message += " " + DescribeHookSite(image.Base() + rvas[i]);
        Trace(message, reason != "original" && reason != "verified_rel32_chain");
        if (testEvidence.Enabled() && owned->sites[i]) for (std::size_t hop = 1; hop < owned->sites[i]->ObservationCount(); ++hop)
            Trace("hook-chain-hop site=" + std::string(names[i]) + " hop=" + std::to_string(hop) + " "
                + DescribeHookSite(owned->sites[i]->ObservationAddress(hop)));
    };
    for (std::size_t i = 0; i < rvas.size(); ++i) {
        std::array<unsigned char, 32> expected{};
        if (!image.OriginalCode(static_cast<std::uint32_t>(rvas[i]), expected)) {
            reportSite(i, "original_relocation_unavailable");
        } else {
            owned->sites[i] = std::make_unique<HookSite>(image.Base() + rvas[i], expected, i == 1);
            reportSite(i, owned->sites[i]->Reason());
        }
    }
    const auto accepted = [&](std::size_t i) { return owned->sites[i] && owned->sites[i]->Approved(); };
    if (!accepted(0) || !accepted(1) || !accepted(2)) {
        Trace("hook-sites-changed: required site qualification refused"); return AJ_POLICY;
    }
    owned->dataHooks = accepted(3) && accepted(4) && accepted(5) && accepted(6) && accepted(7);
    if (owned->dataHooks) {
        owned->saveReal = reinterpret_cast<SaveReal>(image.Base() + kSaveReal);
        owned->saveStr = reinterpret_cast<SaveStr>(image.Base() + kSaveStr);
        owned->saveBoolean = reinterpret_cast<SaveBoolean>(image.Base() + kSaveBoolean);
        owned->saveInteger = reinterpret_cast<SaveInteger>(image.Base() + kSaveInteger);
    } else {
        for (std::size_t i = 3; i < owned->sites.size(); ++i) owned->sites[i].reset();
        Trace("data-plane unavailable: native sites changed; preserving original natives");
    }
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
    struct Attachment { std::size_t site; PVOID* next; PVOID callback; };
    const std::array<Attachment, 7> attachments{{
        {0, reinterpret_cast<PVOID*>(&owned->localized), reinterpret_cast<PVOID>(&LocalizedHook)},
        {1, reinterpret_cast<PVOID*>(&owned->initialize), reinterpret_cast<PVOID>(&InitializeHook)},
        {2, reinterpret_cast<PVOID*>(&owned->destroyVm), reinterpret_cast<PVOID>(&DestroyVmHook)},
        {3, reinterpret_cast<PVOID*>(&owned->saveReal), reinterpret_cast<PVOID>(&SaveRealHook)},
        {4, reinterpret_cast<PVOID*>(&owned->saveStr), reinterpret_cast<PVOID>(&SaveStrHook)},
        {5, reinterpret_cast<PVOID*>(&owned->saveBoolean), reinterpret_cast<PVOID>(&SaveBooleanHook)},
        {7, reinterpret_cast<PVOID*>(&owned->saveInteger), reinterpret_cast<PVOID>(&SaveIntegerHook)}}};
    std::size_t changedSite = owned->sites.size(); bool snapshotChanged = false;
    for (const auto& step : attachments) {
        if (step.site >= 3 && !owned->dataHooks) continue;
        status = AttachQualifiedHook(*owned->sites[step.site], step.next, step.callback);
        if (status != NO_ERROR) { changedSite = step.site; break; }
    }
    if (status == NO_ERROR) {
        for (const auto thread : threads.handles) {
            status = DetourUpdateThread(thread);
            if (status != NO_ERROR) break;
        }
    }
    if (status == NO_ERROR) for (std::size_t i = 0; i < owned->sites.size(); ++i) {
        if (owned->sites[i] && !owned->sites[i]->StillMatches()) { changedSite = i; snapshotChanged = true; status = ERROR_INVALID_DATA; break; }
    }
    if (status == NO_ERROR) status = DetourTransactionCommit();
    else DetourTransactionAbort();
    if (status != NO_ERROR) {
        runtime.store(nullptr);
        if (changedSite != owned->sites.size()) reportSite(changedSite, snapshotChanged ? "changed_before_commit" : "attach_rejected");
        Trace("hook-install-failed win32=" + std::to_string(status)); return status == ERROR_INVALID_DATA ? AJ_POLICY : AJ_BACKEND;
    }
    owned.release(); // 钩子与会话为进程级拥有；不在 DllMain 卸载/析构。
    ownerState.store(1);
    Trace(std::string("initialize-ready: awaiting first JASS request; builtin=audastraui; data-plane=") + (runtime.load()->dataHooks ? "1" : "0"));
    return AJ_OK;
}
int Start() {
    testEvidence.Loaded();
    std::string error;
    std::string identity;
    const auto result = InitializeProcessRuntime(reinterpret_cast<const void*>(&Start), installLock, &Install, error);
    if (const auto* current = runtime.load()) identity = current->identity;
    testEvidence.Installed(result, identity);
    if (!error.empty()) Trace("owner-rejected: " + error);
    return result;
}
}
}

extern "C" __declspec(dllexport) int __cdecl WarVKAIInit(void) {
    try { return Audastra::Runtime32::Japi::Start(); }
    catch (...) { OutputDebugStringA("[AudastraJapi] initialize-exception\n"); return AJ_INTERNAL; }
}

extern "C" __declspec(dllexport) int __cdecl WarVKRuntimeOwnerV1(void) {
    return Audastra::Runtime32::Japi::ownerState.load();
}
