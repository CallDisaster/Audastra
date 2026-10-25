#include "../Japi/GameSession.h"
#include "../GameUi/GameUiModule.h"
#include "../OwnLua/GameVm127.h"
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace Audastra::Runtime32;
using audastra::japi::Response;
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
class FakeVm final : public Japi::StringVm {
public:
    std::uint64_t epoch = 7;
    std::uintptr_t mainVm = 0x1000;
    bool alive = true;
    bool matchesCurrent = true;
    unsigned returned = 0, invalidations = 0;
    int failAllocation = 0;
    std::function<void()> duringReturn;
    std::map<std::uint32_t, std::string> strings;
    std::uint64_t Generation() const noexcept override { return alive ? epoch : 0; }
    bool IsMainVm(std::uintptr_t vm) const noexcept override { return vm == mainVm; }
    bool MatchesCurrentMainVm() const noexcept override { return matchesCurrent; }
    std::optional<std::uint32_t> ReturnString(std::string_view text) override {
        if (duringReturn) duringReturn();
        if (failAllocation == 1) return {};
        if (failAllocation == 2) return 0;
        if (failAllocation == 3) throw std::runtime_error("fixture allocator failure");
        const auto id = ++returned;
        strings[id] = text; // 模拟 VM 复制持有；独立于调用栈的临时 std::string。
        return id;
    }
    void Invalidate() noexcept override { alive = false; ++invalidations; }
};
struct Fixture {
    std::vector<std::string> errors;
    Japi::NativeSession session{[this](auto error) { errors.emplace_back(error); }};
    std::shared_ptr<FakeVm> vm = std::make_shared<FakeVm>();
    unsigned attaches = 0;
    Japi::NativeSession::Attach Attach() {
        return [this] { ++attaches; return Japi::AttachResult{vm, {}}; };
    }
    std::optional<std::uint32_t> Send(std::string_view input) {
        return session.Dispatch(vm->epoch, input, Attach());
    }
    Response Reply(std::string_view input) {
        const auto id = Send(input);
        Check(id && *id && vm->strings.contains(*id), "response is VM-owned nonzero ID");
        Response reply;
        Check(audastra::japi::ParseResponse(vm->strings.at(*id), reply) == AJ_OK, "response parses");
        return reply;
    }
};
void ReplyObservation() {
    auto vm = std::make_shared<FakeVm>();
    unsigned replies = 0, errors = 0;
    Japi::NativeSession session([&](auto) { ++errors; }, [&](auto request, auto wire) {
        Check(vm->returned != 0, "observe only after VM owns the string");
        Response reply;
        Check(request.starts_with("ajapi:") && audastra::japi::ParseResponse(wire, reply) == AJ_OK,
            "observer sees exact protocol response");
        Check(reply.status == AJ_OK && reply.id == 1, "observer records actual status and request identity");
        ++replies;
        throw std::runtime_error("diagnostic sink failed");
    });
    const auto attach = [&] { return Japi::AttachResult{vm, {}, {}}; };
    Check(!session.Dispatch(7, "TRIGSTR_001", attach) && replies == 0, "ordinary localization not observed");
    Check(session.Dispatch(7, "ajapi:2;0;1;host.hello", attach) == 1u && replies == 1 && errors == 0,
        "diagnostic exception does not change successful request");
    vm->failAllocation = 1;
    Check(session.Dispatch(7, "ajapi:2;7;2;host.version", attach) == 0u && replies == 1 && errors == 1,
        "failed return never logs successful delivery");
}
void BasicCalls() {
    Fixture f;
    for (const std::string input : {"", "TRIGSTR_001", "warvk:v1;x", "ajapi", "AJAPI:2;0;1;host.hello", "x\xff"})
        Check(!f.Send(input), "unowned bytes pass through without VM attach");
    Check(f.attaches == 0 && f.errors.empty(), "ordinary localization untouched");
    auto r = f.Reply("ajapi:2;0;1;host.hello");
    Check(r.status == AJ_OK && r.epoch == 7 && r.id == 1 && r.value.integer == 3,
        "hello reports protocol only, no loader/execute/sync capability");
    const auto first = f.vm->strings.at(1);
    r = f.Reply("ajapi:2;7;2;host.version");
    Check(r.status == AJ_OK && r.value.text == "2.0.0", "version through string carrier");
    Check(f.vm->strings.at(1) == first, "older return survives later allocation");
    Check(f.Reply("ajapi:2;7;2;host.version").status == AJ_STALE, "replay rejected");
    Check(f.Reply("ajapi:2;6;3;host.version").status == AJ_STALE, "wrong epoch rejected");
    Check(f.Reply("ajapi:2;7;3;module.load;s:demo").status == AJ_UNSUPPORTED, "map cannot self-authorize DLL");
    Check(f.Reply("ajapi:2;7;4;host.bind;s:AjDispatch").status == AJ_UNSUPPORTED, "callback adapter absent");
    Check(f.Reply("ajapi:2;7;5;module.count").value.integer == 0, "no implicit modules");
    Check(f.Reply("ajapi:2;7;6;host.pump").status == AJ_OK, "explicit empty safe-point pump");
    Check(f.Reply("ajapi:99;0;7;host.hello").status != AJ_OK, "owned unsupported version never falls through");
    Check(f.Reply("ajapi:").status != AJ_OK, "owned malformed frame consumed");
    Check(f.Reply("ajapi:2;7;8;host.status;s:%FF").status == AJ_ENCODING, "owned invalid UTF-8 returns protocol error");
    Check(f.Reply("ajapi:" + std::string(AJ_MAX_FRAME, 'x')).status == AJ_LIMIT, "oversize gets bounded error");
    Check(f.attaches == 1 && f.errors.empty(), "protocol errors do not kill healthy VM");
}
// 内置模块随每个新 Host 装入；装入失败按运行时故障处理，不留半个会话。
void BuiltinModules() {
    Fixture f;
    unsigned installs = 0;
    const auto attach = [&] {
        ++f.attaches;
        Japi::AttachResult result{f.vm, {}, {}};
        result.install = [&](audastra::japi::Host& host) {
            ++installs;
            return GameUi::InstallGameUiModule(host, GameUi::GameUiEntryPoints{}, "fixture: no game UI");
        };
        return result;
    };
    const auto reply = [&](std::string_view input) {
        const auto id = f.session.Dispatch(f.vm->epoch, input, attach);
        Check(id && *id && f.vm->strings.contains(*id), "built-in response is VM-owned");
        Response parsed;
        Check(audastra::japi::ParseResponse(f.vm->strings.at(*id), parsed) == AJ_OK, "built-in response parses");
        return parsed;
    };
    Check(reply("ajapi:2;0;1;host.hello").value.integer == 3, "built-in module adds no loader capability");
    Check(installs == 1 && reply("ajapi:2;7;2;module.count").value.integer == 1, "installed once with the Host");
    Check(reply("ajapi:2;7;3;module.load;s:audastraui").status == AJ_OK, "map-side load of the built-in module");
    auto r = reply("ajapi:2;7;4;audastraui.ready");
    Check(r.status == AJ_OK && r.value.integer == 0, "fixture without game UI is not ready");
    Check(reply("ajapi:2;7;5;audastraui.error;i:4").value.text == "fixture: no game UI", "reason reaches the map");
    Check(installs == 1 && f.errors.empty(), "later calls reuse the installed module");
    f.session.Reset();
    f.vm = std::make_shared<FakeVm>(); f.vm->epoch = 8;
    Check(reply("ajapi:2;0;1;host.hello").epoch == 8 && installs == 2, "next VM gets a fresh module");

    Fixture broken;
    const auto failed = broken.session.Dispatch(7, "ajapi:2;0;1;host.hello", [&] {
        Japi::AttachResult result{broken.vm, {}, {}};
        result.install = [](audastra::japi::Host&) { return std::int32_t{AJ_BACKEND}; };
        return result;
    });
    Check(failed == 0u && !broken.vm->alive && broken.errors.size() == 1, "failed built-in install is fatal");
}
void Lifetimes() {
    Fixture f;
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        f.vm = std::make_shared<FakeVm>(); f.vm->epoch = cycle + 1;
        const auto r = f.Reply("ajapi:2;0;1;host.hello");
        Check(r.epoch == cycle + 1, "reset obtains new epoch with fresh request IDs");
        if (cycle) Check(f.Reply("ajapi:2;" + std::to_string(cycle) + ";2;host.version").status == AJ_STALE,
            "old epoch cannot be replayed in new VM at reused address");
        f.session.BeforeVmDestroy(0x2000);
        Check(f.vm->alive, "child VM destroy is not main VM teardown");
        std::thread other([&] { f.session.Reset(); f.session.BeforeVmDestroy(f.vm->mainVm); });
        other.join();
        Check(f.vm->alive, "other thread lifecycle does not touch owner VM");
        if (cycle % 2) f.session.Reset(); else f.session.BeforeVmDestroy(f.vm->mainVm);
        Check(!f.vm->alive && f.vm->invalidations == 1, "owner lifecycle invalidates exactly once");
        f.session.BeforeVmDestroy(f.vm->mainVm); f.session.Reset();
        Check(f.vm->invalidations == 1, "duplicate notifications are harmless");
    }
    Check(f.attaches == 20 && f.errors.empty(), "20 fake VM cycles; not game-runtime proof");
    f.vm = std::make_shared<FakeVm>(); f.vm->epoch = 20;
    Check(f.Send("ajapi:2;0;1;host.hello") == 0u && f.attaches == 20 && f.errors.size() == 1,
        "destroyed registration cannot resurrect even at identical VM address");
    auto vm = std::make_shared<FakeVm>();
    {
        Japi::NativeSession session([](auto) {});
        Check(session.Dispatch(7, "ajapi:2;0;1;host.hello", [&] { return Japi::AttachResult{vm, {}}; }).value_or(0) != 0,
            "separate session attaches");
    }
    Check(!vm->alive, "session destruction invalidates external VM lease");
}
void NativeInitializationBoundary() {
    Fixture f;
    Check(!f.session.VmIdentityChanged(), "unattached native initialization is not a map reset");
    f.Reply("ajapi:2;0;1;host.hello");
    for (unsigned i = 0; i < 8; ++i)
        Check(!f.session.VmIdentityChanged(), "same-map native reinitialization preserves the connected Host");
    Check(f.Reply("ajapi:2;7;2;host.version").status == AJ_OK && f.attaches == 1,
        "native notifications do not consume epoch or reconnect");
    f.vm->alive = false;
    Check(!f.session.VmIdentityChanged(), "a failed VM lease alone cannot masquerade as a new map");
    f.vm->matchesCurrent = false;
    bool otherChanged = true;
    std::thread other([&] { otherChanged = f.session.VmIdentityChanged(); }); other.join();
    Check(!otherChanged, "another thread cannot infer the owner VM lifecycle");
    Check(f.session.VmIdentityChanged(), "actual main VM identity change requires reset");
    f.session.Reset();
    Check(!f.session.VmIdentityChanged(), "duplicate native notifications after teardown are harmless");
    f.vm = std::make_shared<FakeVm>(); f.vm->epoch = 8;
    Check(f.Reply("ajapi:2;0;1;host.hello").epoch == 8 && f.attaches == 2,
        "actual next map establishes one fresh epoch");
}
void FailClosed() {
    for (const auto generation : {std::uint64_t{0}, std::uint64_t{INT32_MAX} + 1}) {
        Fixture f;
        Check(f.session.Dispatch(generation, "ajapi:2;0;1;host.hello", f.Attach()) == 0u
            && f.attaches == 0 && f.errors.size() == 1, "invalid generation fails before attach");
    }
    for (int failure = 1; failure <= 3; ++failure) {
        Fixture f; f.vm->failAllocation = failure;
        Check(f.Send("ajapi:2;0;1;host.hello") == 0u && !f.vm->alive && f.errors.size() == 1,
            "missing, zero, throwing result allocation fails closed");
        Check(f.Send("ajapi:2;0;2;host.hello") == 0u && f.attaches == 1 && f.errors.size() == 1,
            "failed session never auto-recovers");
    }
    {
        Fixture f;
        Check(f.session.Dispatch(8, "ajapi:2;0;1;host.hello", f.Attach()) == 0u
            && !f.vm->alive && f.errors.size() == 1, "VM must match registration generation at attach");
    }
    {
        Fixture f;
        Check(f.session.Dispatch(7, "ajapi:2;0;1;host.hello", []() -> Japi::AttachResult {
            throw std::runtime_error("fixture attach failure");
        }) == 0u && f.errors.size() == 1, "attach exception contained");
    }
    {
        Fixture f;
        Check(f.session.Dispatch(7, "ajapi:2;0;1;host.hello", [] { return Japi::AttachResult{{}, "no verified VM"}; }) == 0u
            && f.errors == std::vector<std::string>{"no verified VM"}, "attach diagnostic retained");
    }
    {
        Fixture f; f.Reply("ajapi:2;0;1;host.hello"); f.vm->alive = false;
        Check(f.Send("ajapi:2;7;2;host.version") == 0u && f.vm->returned == 1 && f.attaches == 1,
            "invalid VM never allocates a return or silently reattaches");
    }
    {
        Fixture f; f.Reply("ajapi:2;0;1;host.hello"); f.vm->epoch = 8;
        Check(f.Send("ajapi:2;8;2;host.version") == 0u && f.errors.size() == 1,
            "registration change needs lifecycle boundary");
    }
    {
        Fixture f; f.Reply("ajapi:2;0;1;host.hello");
        std::optional<std::uint32_t> result;
        std::thread other([&] { result = f.Send("ajapi:2;7;2;host.version"); }); other.join();
        Check(result == 0u && f.vm->returned == 1 && !f.vm->alive && f.errors.size() == 1,
            "wrong-thread request never calls game allocator");
    }
    for (int path = 0; path < 3; ++path) {
        Fixture f;
        f.vm->duringReturn = [&] {
            if (path == 0) f.session.Reset();
            if (path == 1) f.session.BeforeVmDestroy(f.vm->mainVm);
            if (path == 2) f.vm->alive = false;
        };
        Check(f.Send("ajapi:2;0;1;host.hello") == 0u && f.errors.size() == 1,
            "reset/destroy/invalidation during result copy is never success");
    }
    {
        Fixture f;
        Check(f.session.Dispatch(7, "ajapi:2;0;1;host.hello", [&] {
            f.session.Reset(); return Japi::AttachResult{f.vm, {}};
        }) == 0u && !f.vm->alive && f.errors.size() == 1, "reset during attachment cannot publish VM");
    }
    {
        Fixture f;
        Check(Japi::DispatchGameString(f.session, {}, "ajapi:2;0;1;host.hello") == 0
            && f.errors.size() == 1, "production adapter rejects absent image without ABI access");
    }
}
// 原创内存布局 fixture，只验证既有 VM 字符串读边界；不加载 Game.dll、不绕过 SHA。
struct StringObject {
    std::array<std::uint32_t, 4> object{};
    std::array<std::uint32_t, 8> link{};
    explicit StringObject(const char* text) {
        object[2] = reinterpret_cast<std::uint32_t>(link.data());
        link[7] = reinterpret_cast<std::uint32_t>(text);
    }
    std::uintptr_t Address() const { return reinterpret_cast<std::uintptr_t>(object.data()); }
};
void ProbeBounds() {
    using Vm = OwnLua::GameVm127;
    using Kind = Vm::StringProbe;
    std::array<char, AJ_MAX_FRAME + 1> bytes{};
    std::size_t size = 99;
    Check(Vm::ReadPrefixedStringObject(0, "ajapi:", bytes, size) == Kind::Other && !size, "null string goes to stock native");
    Check(Vm::ReadPrefixedStringObject(1, "ajapi:", bytes, size) == Kind::Unreadable, "bad object guarded by SEH");
    for (const std::string input : {"", "ajap", "ajapi", "ajapx:", "TRIGSTR_001", "\xff\xfe", "x\xff", "warvk:v1;ignored"}) {
        StringObject object(input.c_str());
        Check(Vm::ReadPrefixedStringObject(object.Address(), "ajapi:", bytes, size) == Kind::Other && !size,
            "ordinary/legacy/nonUTF8 input is not decoded");
    }
    for (const auto length : {6u, AJ_MAX_FRAME, AJ_MAX_FRAME + 1, 4096u}) {
        const auto input = "ajapi:" + std::string(length - 6, 'x'); StringObject object(input.c_str());
        const auto probe = Vm::ReadPrefixedStringObject(object.Address(), "ajapi:", bytes, size);
        Check(probe == (length <= AJ_MAX_FRAME ? Kind::Matched : Kind::TooLong), "exact native frame limit");
        Check(size == (std::min)(length, AJ_MAX_FRAME + 1) && std::string_view(bytes.data(), size) == input.substr(0, size),
            "probe only copies bounded raw input");
    }
    const std::string invalid = "ajapi:\xff"; StringObject object(invalid.c_str());
    Check(Vm::ReadPrefixedStringObject(object.Address(), "ajapi:", bytes, size) == Kind::Matched
        && std::string_view(bytes.data(), size) == invalid, "owned UTF8 validation belongs to SDK");
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    auto* pages = static_cast<char*>(VirtualAlloc(nullptr, system.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(pages != nullptr, "guard-page fixture allocation");
    struct Release { char* pages; ~Release() { VirtualFree(pages, 0, MEM_RELEASE); } } release{pages};
    DWORD old = 0;
    Check(VirtualProtect(pages + system.dwPageSize, system.dwPageSize, PAGE_NOACCESS, &old) != 0, "guard page installed");
    auto* last = pages + system.dwPageSize - 1; *last = 'x'; StringObject other(last);
    Check(Vm::ReadPrefixedStringObject(other.Address(), "ajapi:", bytes, size) == Kind::Other,
        "nonmatching prefix never scans unreadable tail");
    auto* owned = pages + system.dwPageSize - 6; std::memcpy(owned, "ajapi:", 6); StringObject truncated(owned);
    Check(Vm::ReadPrefixedStringObject(truncated.Address(), "ajapi:", bytes, size) == Kind::Unreadable,
        "owned unterminated memory fails closed");
    auto* oversized = pages + system.dwPageSize - bytes.size(); std::memset(oversized, 'x', bytes.size());
    std::memcpy(oversized, "ajapi:", 6); StringObject longFrame(oversized);
    Check(Vm::ReadPrefixedStringObject(longFrame.Address(), "ajapi:", bytes, size) == Kind::TooLong && size == bytes.size(),
        "oversize stops before inaccessible tail");
}
}
int main() {
    try {
        ReplyObservation(); BasicCalls(); BuiltinModules(); Lifetimes(); NativeInitializationBoundary(); FailClosed(); ProbeBounds();
        std::cout << "JAPI runtime adapter: " << checks << " checks passed; 20 fake-VM cycles; no game launched\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "JAPI runtime adapter failure after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
