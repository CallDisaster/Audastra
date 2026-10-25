#include "HostInternal.h"
#include <algorithm>

namespace audastra::japi {
int32_t AJ_CALL Host::Impl::Emit(void* opaque, int32_t eventClass, const char* type,
    const char* payload, uint32_t size) {
    if (!opaque || !type || (!payload && size)) return AJ_ARGUMENT;
    if (size > AJ_MAX_EVENT_TEXT) return AJ_LIMIT;
    auto* context = static_cast<Context*>(opaque);
    auto host = context->host.lock();
    auto module = context->module.lock();
    if (!host || !module) return AJ_STALE;
    try {
        std::string eventType;
        if (!BoundedString(type, 31, eventType)) return AJ_ARGUMENT;
        return host->Emit(module, eventClass, eventType, std::string_view(payload ? payload : "", size));
    } catch (...) { return AJ_INTERNAL; }
}
int32_t Host::Impl::Emit(const std::shared_ptr<Module>& module, int32_t eventClass,
    std::string_view type, std::string_view payload) {
    // A dotted event name has the same safe segment rules as a command; bare names are valid too.
    if (type.size() > 31 || (!ValidName(type, 31) && !ValidCommand(type))) return AJ_ARGUMENT;
    if (eventClass < AJ_SYNC || eventClass > AJ_ASYNC) return AJ_ARGUMENT;
    if (!ValidText(payload)) return AJ_ENCODING;
    if (EncodeText(payload).size() > AJ_MAX_EVENT_TEXT) return AJ_LIMIT;
    if (eventClass == AJ_SYNC) {
        if (!OnThread()) return AJ_THREAD;
        if (dispatching) return AJ_REENTRANT; // never queue and falsely report synchronous completion
        if (commandDomain != AJ_SYNC_COMMAND) return AJ_STATE;
        if (!options.synchronousCallbacksVerified || dispatcher.empty()) return AJ_UNSUPPORTED;
    }
    Event event;
    {
        std::lock_guard lock(mutex);
        if (module->life != Life::Active || module->services.epoch != epoch) return AJ_STALE;
        if (nextEvent == INT32_MAX) return AJ_LIMIT;
        if (eventClass != AJ_SYNC && queue.size() >= options.queueCapacity) {
            ++module->rejected;
            return AJ_BUSY; // reject newest; never evict an accepted event
        }
        event = {++nextEvent, module->services.instance, epoch, eventClass,
            module->loaded.contract.name, std::string(type), std::string(payload)};
        if (eventClass != AJ_SYNC) { queue.push_back(std::move(event)); return AJ_OK; }
    }
    // Synchronous events bypass the deferred FIFO and complete on this stack.
    return Deliver(std::move(event));
}
int32_t Host::Impl::Deliver(Event event) {
    if (dispatcher.empty() || !options.execute) return AJ_UNSUPPORTED;
    if (dispatching) return AJ_REENTRANT;
    FlagScope guard(dispatching);
    current = std::move(event);
    eventRead = false;
    acknowledged = false;
    bool returned = false;
    try { returned = options.execute(dispatcher); }
    catch (...) { diagnostic = "dispatcher adapter threw a C++ exception"; }
    const bool completed = returned && eventRead && acknowledged;
    if (current.instance) {
        if (const auto module = Find(current.module)) {
            std::lock_guard lock(mutex);
            if (completed) ++module->delivered;
            else ++module->failed;
        }
    }
    current = {};
    if (!completed) { dispatcher.clear(); diagnostic = "dispatcher did not read, acknowledge and return"; }
    return completed ? AJ_OK : AJ_DISPATCH;
}
int32_t Host::Impl::Bind(std::string_view function) {
    if (!ValidFunction(function)) return AJ_ARGUMENT;
    if (closing || changing || callDepth || dispatching) return AJ_BUSY;
    if (!options.execute) return AJ_UNSUPPORTED;
    if (!dispatcher.empty()) return dispatcher == function ? AJ_OK : AJ_STATE;
    dispatcher = function;
    // Reserved event zero: the JASS library must acknowledge it without invoking author callbacks.
    return Deliver({0, 0, epoch, AJ_LOCAL, "host", "ready", ""});
}
int32_t Host::Impl::Pump() {
    if (!OnThread()) return AJ_THREAD;
    if (changing || dispatching || callDepth) return AJ_BUSY;
    std::vector<std::shared_ptr<Module>> pending;
    {
        std::lock_guard lock(mutex);
        for (auto& [name, module] : modules)
            if (module->life == Life::Stopping) pending.push_back(module);
    }
    bool stoppingBusy = false;
    {
        FlagScope guard(changing);
        for (auto& module : pending) if (!Collect(module)) stoppingBusy = true;
    }
    if (closing) return modules.empty() ? AJ_OK : AJ_BUSY;
    for (uint32_t i = 0; i < options.dispatchBudget && !dispatcher.empty(); ++i) {
        Event event;
        {
            std::lock_guard lock(mutex);
            if (queue.empty()) return stoppingBusy ? AJ_BUSY : AJ_OK;
            event = std::move(queue.front());
            queue.pop_front();
        }
        const auto status = Deliver(std::move(event));
        if (status != AJ_OK) return status;
    }
    std::lock_guard lock(mutex);
    return queue.empty() && !stoppingBusy ? AJ_OK : AJ_BUSY;
}
}
