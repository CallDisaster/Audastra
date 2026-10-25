#pragma once
#include "aj/Host.h"
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace audastra::japi {
enum class Life { Starting, Active, Stopping, Unloaded };
struct Host::Impl : std::enable_shared_from_this<Host::Impl> {
    struct Module;
    struct Context {
        std::weak_ptr<Impl> host;
        std::weak_ptr<Module> module;
    };
    struct Module {
        LoadedModule loaded;
        AjHost services{};
        Context context;
        const AjModule* descriptor = nullptr;
        std::vector<AjCommand> commands;
        Life life = Life::Starting; // guarded by mutex
        uint32_t activeCalls = 0; // owner-thread only
        uint64_t rejected = 0, delivered = 0, failed = 0, cancelled = 0;
        bool stopRequested = false;
        bool stopFailed = false;
    };
    struct Event {
        uint32_t id = 0;
        uint32_t instance = 0;
        uint32_t epoch = 0;
        int32_t eventClass = AJ_LOCAL;
        std::string module, type, payload;
    };
    HostOptions options;
    std::shared_ptr<Loader> loader;
    const std::thread::id thread = std::this_thread::get_id();
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<Module>, std::less<>> modules;
    std::deque<Event> queue;
    uint32_t epoch = 1, nextInstance = 0, nextEvent = 0;
    // Remaining fields are game-thread-only.
    uint32_t lastRequest = 0, callDepth = 0;
    bool closing = false, dispatching = false, changing = false;
    bool eventRead = false, acknowledged = false;
    int32_t commandDomain = AJ_SYNC_COMMAND;
    std::string dispatcher, diagnostic;
    Event current;

    Impl(HostOptions o, std::shared_ptr<Loader> l);
    bool OnThread() const { return thread == std::this_thread::get_id(); }
    uint32_t Epoch() const { std::lock_guard lock(mutex); return epoch; }
    std::shared_ptr<Module> Find(std::string_view name) const;
    int32_t Install(LoadedModule loaded);
    int32_t Load(std::string_view name);
    int32_t Unload(std::string_view name);
    void Stop(const std::shared_ptr<Module>& module);
    bool Collect(const std::shared_ptr<Module>& module);
    int32_t Close();
    ResetResult Reset(uint32_t next);
    int32_t Pump();
    static int32_t AJ_CALL Emit(void*, int32_t, const char*, const char*, uint32_t);
    int32_t Emit(const std::shared_ptr<Module>& module, int32_t eventClass,
        std::string_view type, std::string_view payload);
    int32_t Deliver(Event event);
    int32_t Bind(std::string_view function);
    Response Dispatch(std::string_view wire, bool stringCarrier);
    Response Control(const Request& request);
    Response Invoke(const Request& request);
    static Response Error(int32_t status) { Response r; r.status = status; return r; }
    static Response Int(int32_t value) { Response r; r.value.kind = AJ_INT; r.value.integer = value; return r; }
    static Response Text(std::string value) { Response r; r.value.kind = AJ_TEXT; r.value.text = std::move(value); return r; }
};
// Scope guards only restore host state; they do not claim to contain native crashes.
struct FlagScope {
    bool& flag;
    explicit FlagScope(bool& value) : flag(value) { flag = true; }
    ~FlagScope() { flag = false; }
};
inline bool BoundedString(const char* p, size_t maximum, std::string& out) {
    if (!p) return false;
    size_t n = 0;
    while (n <= maximum && p[n]) ++n;
    if (n > maximum) return false;
    out.assign(p, n);
    return true;
}
}
