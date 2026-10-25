#include "Test.h"
#include "aj/Host.h"
#include "aj/NativeBridge.h"
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

using namespace audastra::japi;
namespace {
struct Fixture {
    const AjHost* host = nullptr;
    bool ready = true;
    int starts = 0, stops = 0, destroys = 0, calls = 0;
    int32_t earlyEmit = -1, startStatus = AJ_OK, invokeStatus = AJ_OK;
    bool throwStop = false, throwDestroy = false;
    std::string name = "test";
    std::vector<std::string> names;
    std::vector<AjCommand> commands;
    AjModule desc{};
    static Fixture* initializing;
    static int32_t AJ_CALL Init(const AjHost* h, const AjModule** out) {
        initializing->host = h;
        *out = &initializing->desc;
        return AJ_OK;
    }
    static int32_t AJ_CALL Start(void* p) {
        auto& f = *static_cast<Fixture*>(p); ++f.starts;
        f.earlyEmit = f.host->emit(f.host->context, AJ_LOCAL, "early", "", 0);
        return f.startStatus;
    }
    static void AJ_CALL Stop(void* p) { auto& f = *static_cast<Fixture*>(p); ++f.stops; if (f.throwStop) throw 1; }
    static int32_t AJ_CALL Stopped(void* p) { return static_cast<Fixture*>(p)->ready ? 1 : 0; }
    static void AJ_CALL Destroy(void* p) { auto& f = *static_cast<Fixture*>(p); ++f.destroys; if (f.throwDestroy) throw 1; }
    static int32_t AJ_CALL Invoke(void* p, const AjCall*, const AjArg* a, uint32_t n, AjResult* out) {
        auto& f = *static_cast<Fixture*>(p); ++f.calls;
        if (f.invokeStatus != AJ_OK) return f.invokeStatus;
        if (n == 1 && a[0].kind == AJ_TEXT) {
            const std::string temporary(a[0].text, a[0].textSize);
            return AjSetText(out, temporary.data(), static_cast<uint32_t>(temporary.size()));
        }
        if (n == 1 && a[0].kind == AJ_INT) {
            out->kind = AJ_VOID;
            return f.host->emit(f.host->context, a[0].integer, "notice", "sync|payload", 12);
        }
        out->kind = AJ_INT; out->integer = -7; return AJ_OK;
    }
    static int32_t AJ_CALL InvalidResult(void*, const AjCall*, const AjArg* a, uint32_t, AjResult* out) {
        out->kind = AJ_TEXT;
        if (a[0].integer == 0) { out->textSize = AJ_MAX_OUTPUT_TEXT + 1; return AJ_OK; }
        if (a[0].integer == 1) { out->textSize = 1; out->text[0] = '\xff'; return AJ_OK; }
        if (a[0].integer == 2) { out->kind = AJ_REAL; out->real = std::numeric_limits<float>::infinity(); return AJ_OK; }
        if (a[0].integer == 3) throw 1;
        std::string big(200, '%');
        return AjSetText(out, big.data(), static_cast<uint32_t>(big.size()));
    }
    explicit Fixture(std::string moduleName = "test") : name(std::move(moduleName)) {
        names = {name + ".get", name + ".echo", name + ".emit", name + ".bad"};
        commands = {{names[0].c_str(), "", AJ_INT, AJ_SYNC_COMMAND, Invoke, this},
            {names[1].c_str(), "s", AJ_TEXT, AJ_LOCAL_COMMAND, Invoke, this},
            {names[2].c_str(), "i", AJ_VOID, AJ_SYNC_COMMAND, Invoke, this},
            {names[3].c_str(), "i", AJ_TEXT, AJ_LOCAL_COMMAND, InvalidResult, this}};
        desc = {sizeof(AjModule), AJ_ABI, name.c_str(), "2.0.0", commands.data(), 4, this, Start, Stop, Stopped, Destroy};
    }
};
Fixture* Fixture::initializing = nullptr;
LoadedModule Package(const std::shared_ptr<Fixture>& fixture) {
    Fixture::initializing = fixture.get();
    ModuleContract contract{fixture->name, "2.0.0", {}};
    for (const auto& c : fixture->commands) contract.commands.push_back({c.name, c.arguments, c.resultKind, c.domain});
    return {Fixture::Init, fixture, contract};
}
struct Client {
    Host& host;
    uint32_t epoch = 1, id = 0;
    Response Call(std::string command, std::string args = "") {
        const auto reply = host.Dispatch("ajapi:2;" + std::to_string(epoch) + ';' + std::to_string(++id) + ';' + command + args);
        Response decoded;
        Check(ParseResponse(EncodeResponse(reply), decoded) == AJ_OK, "host response always serializes");
        return decoded;
    }
};
struct Dispatcher {
    Client* client = nullptr;
    std::function<void(const std::string&)> callback;
    bool ack = true, returns = true, read = true;
    int events = 0;
    bool Run(std::string_view name) {
        Check(name == "AjDispatch", "bound function name");
        if (!read) return returns;
        auto reply = client->Call("host.event");
        Check(reply.status == AJ_OK, "dispatcher reads one event");
        const auto id = std::stoi(reply.value.text.substr(0, reply.value.text.find('|')));
        if (id != 0) { ++events; if (callback) callback(reply.value.text); }
        if (ack) Check(client->Call("host.ack", ";i:" + std::to_string(id)).status == AJ_OK, "dispatcher acknowledges after callback");
        return returns;
    }
};
void BasicTests() {
    Host host; Client c{host};
    auto f = std::make_shared<Fixture>();
    Check(!host.Dispatch("warvk:v1;module.load;s:test").consumed, "other protocol not intercepted");
    std::string bridgeOutput = "unchanged";
    Check(!HandleLocalizedString(host, "ordinary.localization.key", bridgeOutput) && bridgeOutput == "unchanged", "native adapter passthrough preserves original input/output");
    Check(HandleLocalizedString(host, "ajapi:9;0;1;host.hello", bridgeOutput) && bridgeOutput.find(";2;v:") != std::string::npos,
        "owned unsupported version produces error envelope, never fallback");
    Check(host.Dispatch("ajapi:8;0;1;host.hello").status == AJ_VERSION, "unsupported protocol consumed");
    Check(host.Dispatch("ajapi:2;1;1;module.count", false).status == AJ_CARRIER, "wrong native carrier rejected before side effect");
    c.epoch = 0;
    Check(c.Call("host.hello").value.integer == 3, "capabilities do not advertise absent adapter/loader");
    c.epoch = 1;
    Check(host.Install(Package(f)) == AJ_OK && f->starts == 1 && f->earlyEmit == AJ_STALE, "start isolated from event publication");
    Check(c.Call("test.get").value.integer == -7, "negative return distinct from errors");
    Check(c.Call("test.echo", ";s:stack%3B%E4%B8%AD%E6%96%87").value.text == "stack;中文", "stack result copied before module return");
    Check(c.Call("test.get", ";i:1").status == AJ_ARGUMENT, "signature argument count");
    Check(c.Call("test.echo", ";i:1").status == AJ_ARGUMENT, "signature argument type");
    Check(c.Call("test.bad", ";i:0").status == AJ_LIMIT, "module cannot enlarge output buffer contract");
    Check(c.Call("test.bad", ";i:1").status == AJ_ENCODING, "invalid output UTF-8");
    Check(c.Call("test.bad", ";i:2").status == AJ_BACKEND, "wrong output type");
    Check(c.Call("test.bad", ";i:3").status == AJ_INTERNAL, "C++ exception converted without poisoning host state");
    Check(c.Call("test.bad", ";i:4").status == AJ_LIMIT, "encoded output length, not just decoded length");
    Check(c.Call("module.commandcount", ";s:test").value.integer == 4, "command count");
    Check(c.Call("module.commandat", ";s:test;i:1").value.text == "test.echo|s|s|0", "exact paged command description");
    Check(c.Call("module.commandat", ";s:test;i:-1").status == AJ_ARGUMENT, "negative command index rejected");
    const int calls = f->calls;
    const auto duplicate = host.Dispatch("ajapi:2;1;" + std::to_string(c.id) + ";test.get");
    Check(duplicate.status == AJ_STALE && f->calls == calls, "duplicate request has no second side effect");
    int wrongThread = 0;
    std::thread worker([&] { wrongThread = host.Dispatch("ajapi:2;1;999;test.get").status; }); worker.join();
    Check(wrongThread == AJ_THREAD, "wrong-thread command rejected");
    Check(host.Unload("test") == AJ_OK && f->stops == 1 && f->destroys == 1, "orderly unload exactly once");
    Check(host.Close() == AJ_OK && host.Close() == AJ_OK && f->destroys == 1, "close idempotent");
}
void EventTests() {
    Dispatcher dispatch;
    HostOptions options; options.queueCapacity = 4; options.dispatchBudget = 2;
    options.execute = [&](auto name) { return dispatch.Run(name); };
    options.synchronousCallbacksVerified = true;
    Host host(options); Client c{host}; dispatch.client = &c;
    auto f = std::make_shared<Fixture>();
    Check(host.Install(Package(f)) == AJ_OK, "event fixture install");
    Check(c.Call("test.emit", ";i:0").status == AJ_UNSUPPORTED, "sync requires successful binding");
    Check(c.Call("host.bind", ";s:AjDispatch").status == AJ_OK && dispatch.events == 0, "empty handshake completed and not sent to author");
    Check(c.Call("host.bind", ";s:Other").status == AJ_STATE, "different dispatcher cannot steal binding");
    Check(c.Call("host.event").status == AJ_STATE, "map code cannot steal queued events");
    bool beforeReturn = false;
    dispatch.callback = [&](auto) {
        beforeReturn = true;
        Check(c.Call("test.get").status == AJ_REENTRANT, "same-module callback reentry blocked");
        Check(c.Call("module.unload", ";s:test").status == AJ_BUSY, "unload during callback blocked");
        Check(f->host->emit(f->host->context, AJ_SYNC, "nested", "", 0) == AJ_REENTRANT, "nested sync fails, never deferred as success");
    };
    Check(c.Call("test.emit", ";i:0").status == AJ_OK && beforeReturn, "sync callback completed before emit returns");
    int wrongThread = 0;
    std::thread worker([&] { wrongThread = f->host->emit(f->host->context, AJ_SYNC, "notice", "", 0); }); worker.join();
    Check(wrongThread == AJ_THREAD, "worker cannot emit synchronous event");
    dispatch.callback = [&](auto text) {
        Check(text.ends_with("|notice|a|b;%"), "event payload delimiters survive transport");
        Check(c.Call("test.get").status == AJ_STATE, "local callback cannot call gameplay command");
    };
    for (int i = 0; i < 4; ++i) Check(f->host->emit(f->host->context, AJ_LOCAL, "notice", "a|b;%", 5) == AJ_OK, "accept deferred event");
    Check(f->host->emit(f->host->context, AJ_ASYNC, "overflow", "", 0) == AJ_BUSY, "queue full returns busy and preserves accepted FIFO");
    Check(c.Call("module.stats", ";s:test").value.text.find("|4|1|1|0") != std::string::npos, "observable queue/reject/delivery counters");
    Check(host.Pump() == AJ_BUSY && dispatch.events == 3, "budget leaves work queued with Busy status");
    Check(host.Pump() == AJ_OK && dispatch.events == 5, "second safe point completes remaining events");
    dispatch.callback = {}; dispatch.ack = false;
    Check(c.Call("test.emit", ";i:0").status == AJ_DISPATCH, "missing ack fails synchronous completion");
    Check(c.Call("test.emit", ";i:0").status == AJ_UNSUPPORTED, "failed dispatcher is disabled");
    dispatch.ack = true; dispatch.returns = false;
    Check(c.Call("host.bind", ";s:AjDispatch").status == AJ_DISPATCH, "ack without callback return is not completion");
    dispatch.returns = true; dispatch.read = false;
    Check(c.Call("host.bind", ";s:AjDispatch").status == AJ_DISPATCH, "missing-function handshake fails closed");
    Check(host.Close() == AJ_OK, "events host close");
}
void LifecycleTests() {
    Host host; Client c{host};
    auto f = std::make_shared<Fixture>(); f->ready = false;
    Check(host.Install(Package(f)) == AJ_OK, "async fixture install");
    const auto oldId = f->host->instance;
    Check(host.Unload("test") == AJ_BUSY && f->stops == 1 && !f->destroys, "busy worker pins module");
    Check(c.Call("test.get").status == AJ_STATE, "stopping prevents new invocations");
    Check(f->host->emit(f->host->context, AJ_ASYNC, "late", "", 0) == AJ_STALE, "late old-instance emit rejected");
    Check(host.Load("test") == AJ_BUSY, "same-name reload blocked until workers are joined");
    Check(host.Unload("test") == AJ_BUSY && f->stops == 1, "stop request issued once");
    f->ready = true;
    Check(host.Pump() == AJ_OK && f->destroys == 1 && host.ModuleCount() == 0, "safe point finishes unload");
    auto next = std::make_shared<Fixture>();
    Check(host.Install(Package(next)) == AJ_OK && next->host->instance > oldId, "new load identity not reused");
    next->ready = false;
    const auto reset = host.Reset(2);
    Check(reset.status == AJ_OK && reset.applied && reset.stoppingModules == 1 && next->stops == 1,
        "reset receipt distinguishes applied epoch from pending workers");
    Check(c.Call("test.get").status == AJ_STALE, "old VM request refused");
    c.epoch = 2;
    Check(c.Call("host.version").status == AJ_OK, "new VM can diagnose pending shutdown");
    Check(host.Reset(2).status == AJ_STALE && host.Reset(1).status == AJ_STALE, "epoch never reused");
    next->ready = true;
    Check(host.Pump() == AJ_OK && next->destroys == 1, "old VM cleanup completes");
    auto failed = std::make_shared<Fixture>(); failed->startStatus = AJ_BACKEND;
    Check(host.Install(Package(failed)) == AJ_BACKEND && failed->stops == 1 && failed->destroys == 1, "failed start follows stop/quiescence/destroy");
    auto mismatch = std::make_shared<Fixture>(); auto bad = Package(mismatch); bad.contract.commands[0].arguments = "i";
    Check(host.Install(std::move(bad)) == AJ_BACKEND && mismatch->starts == 0, "manifest mismatch rejected before start");
    auto reserved = std::make_shared<Fixture>("host");
    Check(host.Install(Package(reserved)) == AJ_ARGUMENT && reserved->starts == 0, "reserved module name");
    const auto prior = Host::QuarantinedCount();
    auto blocked = std::make_shared<Fixture>(); blocked->ready = false;
    {
        Host abandoned;
        Check(abandoned.Install(Package(blocked)) == AJ_OK, "quarantine fixture install");
    }
    Check(Host::QuarantinedCount() == prior + 1 && blocked->destroys == 0, "no forced unload of unjoined worker at host destruction");
    Check(blocked->host->emit(blocked->host->context, AJ_ASYNC, "late", "", 0) == AJ_STALE, "quarantined context safely rejects after host destruction");
    auto throwing = std::make_shared<Fixture>(); throwing->throwDestroy = true;
    {
        Host h; Check(h.Install(Package(throwing)) == AJ_OK, "throwing destroy fixture install");
        Check(h.Unload("test") == AJ_BUSY && h.Pump() == AJ_BUSY && throwing->destroys == 1, "failed destroy never retried or unloaded");
    }
}
void ConcurrentEvents() {
    HostOptions o; o.queueCapacity = 256;
    Host h(o); Client c{h}; auto f = std::make_shared<Fixture>();
    Check(h.Install(Package(f)) == AJ_OK, "worker fixture install");
    std::atomic<int> accepted = 0, rejected = 0, unexpected = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        for (int j = 0; j < 100; ++j) {
            const auto status = f->host->emit(f->host->context, AJ_ASYNC, "worker", "payload", 7);
            if (status == AJ_OK) ++accepted; else if (status == AJ_BUSY) ++rejected; else ++unexpected;
        }
    });
    for (auto& worker : workers) worker.join();
    Check(accepted == 256 && rejected == 144 && unexpected == 0, "four worker producers obey exact bounded admission");
    Check(c.Call("module.stats", ";s:test").value.text.find("|256|144|0|0") != std::string::npos, "concurrent event counters");
    Check(h.Close() == AJ_OK, "close clears pending event queue");
}
}
void HostTests() { BasicTests(); EventTests(); LifecycleTests(); ConcurrentEvents(); }
