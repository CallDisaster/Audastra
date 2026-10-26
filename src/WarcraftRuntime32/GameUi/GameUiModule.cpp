#include "GameUiModule.h"

#include "GameUiLibrary.h"

#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

namespace Audastra::Runtime32::GameUi {
namespace {

using namespace audastra::japi;

// 地图侧每次轮询都会取空队列；正常不会积到这么多，满了丢弃新事件。
constexpr std::size_t kMaxQueued = 256;

struct Instance;
using Handler = std::int32_t (*)(Instance&, const AjCall&, const AjArg*, AjResult&);

struct CommandSpec {
    const char* name;
    const char* arguments;   // i 整数、r 实数、s 文字、b 布尔、d 框架句柄
    std::int32_t result;
    Handler handler;
};

std::int32_t StatusOf(UiError error) noexcept {
    switch (error) {
    case UiError::None: return AJ_OK;
    case UiError::NotReady: return AJ_STATE;
    case UiError::WrongThread: return AJ_THREAD;
    case UiError::BadHandle:
    case UiError::NotFound:
    case UiError::BadArgument: return AJ_ARGUMENT;
    case UiError::StaleFrame: return AJ_STALE;
    case UiError::Unsupported: return AJ_UNSUPPORTED;
    case UiError::TableFull: return AJ_LIMIT;
    case UiError::NotOwned: return AJ_STATE;
    case UiError::Faulted: return AJ_INTERNAL;
    }
    return AJ_INTERNAL;
}

struct Instance {
    struct Binding {
        Instance* self = nullptr;
        Handler handler = nullptr;
        std::uint32_t arguments = 0;
        const char* entry = nullptr;
    };

    Instance(const GameUiEntryPoints& entryPoints, std::string reason, UiFaultObserver observer);

    // 失败的请求号与原因留给 audastraui.error；状态码只说明大类。
    std::int32_t Record(const AjCall& call, UiError error) noexcept {
        if (error == UiError::None) return AJ_OK;
        failedRequest = call.request;
        failed = error;
        return StatusOf(error);
    }

    GameUiEntryPoints entries;
    UiFaultObserver observer = nullptr;
    std::string unavailable;
    std::optional<GameUiLibrary> ui;   // start 时在游戏线程建
    std::deque<UiFrameEvent> events;
    std::uint32_t failedRequest = 0;
    UiError failed = UiError::None;
    std::vector<Binding> bindings;
    std::vector<AjCommand> commands;
    AjModule descriptor{};
};

std::string_view Text(const AjArg& arg) noexcept { return {arg.text ? arg.text : "", arg.textSize}; }
FrameHandle Frame(const AjArg& arg) noexcept { return arg.integer; }
bool Flag(const AjArg& arg) noexcept { return arg.integer != 0; }

std::int32_t Void(Instance& s, const AjCall& call, AjResult& out, UiError error) noexcept {
    out.kind = AJ_VOID;
    return s.Record(call, error);
}

std::int32_t Created(Instance& s, const AjCall& call, AjResult& out, UiCreated created) noexcept {
    out.kind = AJ_HANDLE;
    out.integer = created.handle;
    return s.Record(call, created.error);
}

std::int32_t ReplyText(AjResult& out, std::string_view text) noexcept {
    return AjSetText(&out, text.data(), static_cast<std::uint32_t>(text.size()));
}

// 顺序即契约顺序；改动须同步 AudastraUI.j。全部是本机界面命令，不进同步域。
const CommandSpec kCommands[] = {
    {"audastraui.ready", "", AJ_BOOL, [](Instance& s, const AjCall& c, const AjArg*, AjResult& out) -> std::int32_t {
        // 回答本身总是成功；不可用时把原因记在这次请求上，地图侧随后可用 error 查询。
        auto error = s.ui->Status();
        if (error == UiError::None) error = s.ui->Root().error;
        out.kind = AJ_BOOL;
        out.integer = error == UiError::None ? 1 : 0;
        static_cast<void>(s.Record(c, error));
        return AJ_OK;
    }},
    {"audastraui.error", "i", AJ_TEXT, [](Instance& s, const AjCall&, const AjArg* a, AjResult& out) -> std::int32_t {
        // 只回答这个模块自己记下的那次失败；宿主层面的失败（未连接、过期请求）返回空。
        if (s.failed == UiError::None || a[0].integer <= 0 || static_cast<std::uint32_t>(a[0].integer) != s.failedRequest)
            return ReplyText(out, {});
        if (s.failed == UiError::NotReady && !s.unavailable.empty()
            && EncodeText(s.unavailable).size() <= AJ_MAX_OUTPUT_TEXT)
            return ReplyText(out, s.unavailable);
        return ReplyText(out, UiErrorText(s.failed));
    }},
    {"audastraui.root", "", AJ_HANDLE, [](Instance& s, const AjCall& c, const AjArg*, AjResult& out) -> std::int32_t {
        return Created(s, c, out, s.ui->Root());
    }},
    {"audastraui.world", "", AJ_HANDLE, [](Instance& s, const AjCall& c, const AjArg*, AjResult& out) -> std::int32_t {
        return Created(s, c, out, s.ui->World());
    }},
    {"audastraui.find", "si", AJ_HANDLE, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Created(s, c, out, s.ui->Find(Text(a[0]), a[1].integer));
    }},
    {"audastraui.create", "sdii", AJ_HANDLE, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Created(s, c, out, s.ui->Create(Text(a[0]), Frame(a[1]), a[2].integer, a[3].integer));
    }},
    {"audastraui.createsimple", "sdi", AJ_HANDLE, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Created(s, c, out, s.ui->CreateSimple(Text(a[0]), Frame(a[1]), a[2].integer));
    }},
    {"audastraui.loadtoc", "s", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->LoadToc(Text(a[0])));
    }},
    {"audastraui.setpoint", "didirr", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetPoint(Frame(a[0]), a[1].integer, Frame(a[2]), a[3].integer, a[4].real, a[5].real));
    }},
    {"audastraui.setabspoint", "dirr", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetAbsPoint(Frame(a[0]), a[1].integer, a[2].real, a[3].real));
    }},
    {"audastraui.setallpoints", "dd", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetAllPoints(Frame(a[0]), Frame(a[1])));
    }},
    {"audastraui.clearpoints", "d", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->ClearPoints(Frame(a[0])));
    }},
    {"audastraui.setsize", "drr", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        // 两个值都先检查，不留下只改了宽度的半成品。
        if (!(a[1].real >= 0.0f) || !(a[2].real >= 0.0f)) return Void(s, c, out, UiError::BadArgument);
        auto error = s.ui->SetWidth(Frame(a[0]), a[1].real);
        if (error == UiError::None) error = s.ui->SetHeight(Frame(a[0]), a[2].real);
        return Void(s, c, out, error);
    }},
    {"audastraui.setwidth", "dr", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetWidth(Frame(a[0]), a[1].real));
    }},
    {"audastraui.setheight", "dr", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetHeight(Frame(a[0]), a[1].real));
    }},
    {"audastraui.setscale", "dr", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetScale(Frame(a[0]), a[1].real));
    }},
    {"audastraui.show", "db", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetVisible(Frame(a[0]), Flag(a[1])));
    }},
    {"audastraui.setalpha", "di", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetAlpha(Frame(a[0]), a[1].integer));
    }},
    {"audastraui.setlevel", "di", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetLevel(Frame(a[0]), a[1].integer));
    }},
    {"audastraui.setparent", "dd", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetParent(Frame(a[0]), Frame(a[1])));
    }},
    {"audastraui.settext", "ds", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetText(Frame(a[0]), Text(a[1])));
    }},
    {"audastraui.setfont", "dsri", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetFont(Frame(a[0]), Text(a[1]), a[2].real, a[3].integer));
    }},
    {"audastraui.settextalign", "dii", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetTextAlign(Frame(a[0]), a[1].integer, a[2].integer));
    }},
    {"audastraui.setcolor", "diiii", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        // 红、绿、蓝、不透明度，各 0..255，在这里拼成游戏用的 ARGB。
        for (int index = 1; index <= 4; ++index) {
            if (a[index].integer < 0 || a[index].integer > 255) return Void(s, c, out, UiError::BadArgument);
        }
        const auto channel = [&](int index) { return static_cast<std::uint32_t>(a[index].integer); };
        const auto argb = channel(4) << 24 | channel(1) << 16 | channel(2) << 8 | channel(3);
        return Void(s, c, out, s.ui->SetColor(Frame(a[0]), argb));
    }},
    {"audastraui.settexture", "dsb", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetTexture(Frame(a[0]), Text(a[1]), Flag(a[2])));
    }},
    {"audastraui.setvalue", "dr", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetValue(Frame(a[0]), a[1].real));
    }},
    {"audastraui.setlimits", "drr", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetLimits(Frame(a[0]), a[1].real, a[2].real));
    }},
    {"audastraui.setmodel", "dsi", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetModel(Frame(a[0]), Text(a[1]), a[2].integer));
    }},
    {"audastraui.setenabled", "db", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->SetEnabled(Frame(a[0]), Flag(a[1])));
    }},
    {"audastraui.destroy", "d", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        return Void(s, c, out, s.ui->Destroy(Frame(a[0])));
    }},
    {"audastraui.watch", "di", AJ_VOID, [](Instance& s, const AjCall& c, const AjArg* a, AjResult& out) -> std::int32_t {
        // 掩码：1 进入、2 离开、4 点击；0 取消监听。
        if (a[1].integer < 0 || a[1].integer > (kUiEventEnter | kUiEventLeave | kUiEventClick))
            return Void(s, c, out, UiError::BadArgument);
        return Void(s, c, out, s.ui->Watch(Frame(a[0]), static_cast<std::uint8_t>(a[1].integer)));
    }},
    {"audastraui.poll", "", AJ_INT, [](Instance& s, const AjCall& c, const AjArg*, AjResult& out) -> std::int32_t {
        // 读一次鼠标状态，返回排队等取的事件数；地图侧计时器按返回数逐个取。
        out.kind = AJ_INT;
        if (const auto ready = s.ui->Status(); ready != UiError::None) return s.Record(c, ready);
        s.ui->Poll(s.ui->ReadPointer());
        for (const auto& event : s.ui->TakeEvents()) {
            if (s.events.size() < kMaxQueued) s.events.push_back(event);
        }
        out.integer = static_cast<std::int32_t>(s.events.size());
        return AJ_OK;
    }},
    {"audastraui.event", "", AJ_TEXT, [](Instance& s, const AjCall&, const AjArg*, AjResult& out) -> std::int32_t {
        // “框架句柄|事件类型”；没有事件时为空文字。
        if (s.events.empty()) return ReplyText(out, {});
        const auto event = s.events.front();
        s.events.pop_front();
        return ReplyText(out, std::to_string(event.frame) + '|' + std::to_string(event.kind));
    }},
};

std::int32_t AJ_CALL Invoke(void* user, const AjCall* call, const AjArg* args, std::uint32_t count, AjResult* out) {
    const auto* binding = static_cast<const Instance::Binding*>(user);
    if (!binding || !binding->self || !binding->self->ui || !call || !out) return AJ_STATE;
    if (count != binding->arguments || (count && !args)) return AJ_ARGUMENT;
    auto& ui = *binding->self->ui;
    ui.SetDiagnosticContext(call->epoch, call->request, binding->entry);
    struct ClearContext {
        GameUiLibrary& ui;
        ~ClearContext() { ui.SetDiagnosticContext(0, 0, nullptr); }
    } clear{ui};
    try {
        return binding->handler(*binding->self, *call, args, *out);
    } catch (...) {
        return AJ_INTERNAL;
    }
}

std::int32_t AJ_CALL Start(void* user) {
    auto& self = *static_cast<Instance*>(user);
    self.ui.emplace(self.entries, self.observer);
    return AJ_OK;
}

void AJ_CALL RequestStop(void*) {}

// 没有工作线程，停止请求后即可销毁。
std::int32_t AJ_CALL PollStopped(void*) { return 1; }

void AJ_CALL Destroy(void* user) {
    auto& self = *static_cast<Instance*>(user);
    self.events.clear();
    self.ui.reset();
}

// AjInit 不带上下文；装入时把这次的实例交给同一线程上紧接着的 Init。
thread_local Instance* t_installing = nullptr;

std::int32_t AJ_CALL Init(const AjHost* host, const AjModule** out) {
    if (!host || !out) return AJ_ARGUMENT;
    if (host->structSize != sizeof(AjHost) || host->abi != AJ_ABI) return AJ_VERSION;
    if (!t_installing) return AJ_STATE;
    *out = &t_installing->descriptor;
    t_installing = nullptr;
    return AJ_OK;
}

Instance::Instance(const GameUiEntryPoints& entryPoints, std::string reason, UiFaultObserver faultObserver)
    : entries(entryPoints), observer(faultObserver), unavailable(std::move(reason)) {
    bindings.reserve(std::size(kCommands));
    commands.reserve(std::size(kCommands));
    for (const auto& spec : kCommands)
        bindings.push_back({this, spec.handler, static_cast<std::uint32_t>(std::strlen(spec.arguments)), spec.name});
    for (std::size_t index = 0; index < std::size(kCommands); ++index) {
        const auto& spec = kCommands[index];
        commands.push_back({spec.name, spec.arguments, spec.result, AJ_LOCAL_COMMAND, &Invoke, &bindings[index]});
    }
    descriptor = {sizeof(AjModule), AJ_ABI, kGameUiModuleName.data(), kGameUiModuleVersion.data(), commands.data(),
        static_cast<std::uint32_t>(commands.size()), this, &Start, &RequestStop, &PollStopped, &Destroy};
}

} // namespace

ModuleContract GameUiModuleContract() {
    ModuleContract contract{std::string(kGameUiModuleName), std::string(kGameUiModuleVersion), {}};
    for (const auto& spec : kCommands) contract.commands.push_back({spec.name, spec.arguments, spec.result, AJ_LOCAL_COMMAND});
    return contract;
}

std::int32_t InstallGameUiModule(Host& host, const GameUiEntryPoints& entries, std::string unavailable, UiFaultObserver observer) {
    auto instance = std::make_shared<Instance>(entries, std::move(unavailable), observer);
    t_installing = instance.get();
    const auto status = host.Install({&Init, instance, GameUiModuleContract()});
    t_installing = nullptr;
    return status;
}

std::int32_t InstallGameUiModule127(Host& host, Address base, const std::function<bool(Address, std::size_t)>& executable,
    UiFaultObserver observer) {
    GameUiEntryPoints entries;
    std::string error;
    if (!ResolveGameUiEntryPoints127(base, executable, entries, error)) entries = {};
    return InstallGameUiModule(host, entries, std::move(error), observer);
}

} // namespace Audastra::Runtime32::GameUi
