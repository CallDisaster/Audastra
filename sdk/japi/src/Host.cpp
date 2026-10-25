#include "HostInternal.h"
#include <algorithm>
#include <stdexcept>

namespace audastra::japi {
namespace {
struct Quarantine {
    std::mutex mutex;
    std::vector<std::shared_ptr<void>> entries;
};
Quarantine& Quarantined() {
    // Deliberately never destructed: process shutdown must not unload a live worker's code.
    static auto* storage = new Quarantine;
    return *storage;
}
bool ValidContract(const ModuleContract& contract) {
    if (!ValidName(contract.name) || contract.name == "host" || contract.name == "module"
        || !ValidVersion(contract.version) || contract.commands.size() > AJ_MAX_COMMANDS) return false;
    std::vector<std::string> names;
    for (const auto& command : contract.commands) {
        if (!ValidCommand(command.name) || !command.name.starts_with(contract.name + '.')
            || command.arguments.size() > AJ_MAX_ARGUMENTS || command.resultKind < AJ_VOID
            || command.resultKind > AJ_HANDLE || (command.domain != AJ_LOCAL_COMMAND && command.domain != AJ_SYNC_COMMAND)) return false;
        for (char type : command.arguments) if (TypeKind(type) <= AJ_VOID) return false;
        if (std::find(names.begin(), names.end(), command.name) != names.end()) return false;
        names.push_back(command.name);
    }
    return true;
}
}
Host::Impl::Impl(HostOptions o, std::shared_ptr<Loader> l) : options(std::move(o)), loader(std::move(l)) {
    if (!options.initialEpoch || options.initialEpoch > INT32_MAX || !options.maxModules
        || options.maxModules > 256 || !options.queueCapacity || options.queueCapacity > 4096
        || !options.dispatchBudget || options.dispatchBudget > 256)
        throw std::invalid_argument("JAPI host limits/epoch invalid");
    epoch = options.initialEpoch;
}
std::shared_ptr<Host::Impl::Module> Host::Impl::Find(std::string_view name) const {
    std::lock_guard lock(mutex);
    const auto found = modules.find(name);
    return found == modules.end() ? nullptr : found->second;
}
int32_t Host::Impl::Install(LoadedModule loaded) {
    if (!OnThread()) return AJ_THREAD;
    if (closing) return AJ_STATE;
    if (callDepth || dispatching || changing) return AJ_BUSY;
    if (!loaded.init || !ValidContract(loaded.contract)) return AJ_ARGUMENT;
    if (Find(loaded.contract.name)) return AJ_STATE;
    {
        std::lock_guard lock(mutex);
        if (modules.size() >= options.maxModules || nextInstance == INT32_MAX) return AJ_LIMIT;
    }
    FlagScope guard(changing);
    auto module = std::make_shared<Module>();
    module->loaded = std::move(loaded);
    module->context = {shared_from_this(), module};
    module->services = {sizeof(AjHost), AJ_ABI, epoch, ++nextInstance, &module->context, &Impl::Emit};
    const AjModule* descriptor = nullptr;
    int32_t status = AJ_INTERNAL;
    try { status = module->loaded.init(&module->services, &descriptor); }
    catch (...) { diagnostic = "module Init threw a C++ exception"; }
    // Init is metadata-only. Failure must leave no resources/workers behind.
    if (status != AJ_OK) return status >= AJ_OK && status <= AJ_INTERNAL ? status : AJ_BACKEND;
    if (!descriptor || descriptor->structSize != sizeof(AjModule) || descriptor->abi != AJ_ABI)
        return AJ_VERSION;
    const auto& expected = module->loaded.contract;
    std::string name, version;
    if (!BoundedString(descriptor->name, 32, name) || name != expected.name
        || !BoundedString(descriptor->version, 63, version) || version != expected.version
        || descriptor->commandCount != expected.commands.size()
        || (descriptor->commandCount && !descriptor->commands)
        || !descriptor->start || !descriptor->requestStop || !descriptor->pollStopped || !descriptor->destroy)
        return AJ_BACKEND;
    for (uint32_t i = 0; i < descriptor->commandCount; ++i) {
        const auto& command = descriptor->commands[i];
        CommandContract actual;
        if (!BoundedString(command.name, 63, actual.name) || !BoundedString(command.arguments, 16, actual.arguments)
            || !command.invoke) return AJ_BACKEND;
        actual.resultKind = command.resultKind;
        actual.domain = command.domain;
        if (actual != expected.commands[i]) return AJ_BACKEND;
        module->commands.push_back(command);
    }
    module->descriptor = descriptor;
    {
        std::lock_guard lock(mutex);
        modules.emplace(expected.name, module); // reserve name even while starting/stopping
    }
    try { status = descriptor->start(descriptor->user); }
    catch (...) { status = AJ_INTERNAL; diagnostic = "module start threw a C++ exception"; }
    if (status != AJ_OK) {
        Stop(module);
        Collect(module); // busy stays pinned and diagnosable
        return status >= AJ_OK && status <= AJ_INTERNAL ? status : AJ_BACKEND;
    }
    { std::lock_guard lock(mutex); module->life = Life::Active; }
    return AJ_OK;
}
int32_t Host::Impl::Load(std::string_view name) {
    if (!OnThread()) return AJ_THREAD;
    if (closing) return AJ_STATE;
    if (changing || dispatching || callDepth) return AJ_BUSY;
    if (!ValidName(name) || name == "host" || name == "module") return AJ_ARGUMENT;
    if (const auto existing = Find(name)) {
        std::lock_guard lock(mutex);
        return existing->life == Life::Active ? AJ_OK : AJ_BUSY;
    }
    if (!loader) return AJ_UNSUPPORTED;
    {
        std::lock_guard lock(mutex);
        if (modules.size() >= options.maxModules || nextInstance == INT32_MAX) return AJ_LIMIT;
    }
    LoadResult result;
    {
        FlagScope guard(changing);
        try { result = loader->Open(name); }
        catch (...) { diagnostic = "module loader threw a C++ exception"; return AJ_INTERNAL; }
    }
    diagnostic = result.diagnostic;
    if (result.status != AJ_OK) return result.status >= AJ_OK && result.status <= AJ_INTERNAL ? result.status : AJ_BACKEND;
    if (result.module.contract.name != name) return AJ_POLICY;
    return Install(std::move(result.module));
}
void Host::Impl::Stop(const std::shared_ptr<Module>& module) {
    if (module->stopRequested) return;
    {
        std::lock_guard lock(mutex);
        module->life = Life::Stopping;
        module->cancelled += std::erase_if(queue, [&](const Event& event) { return event.instance == module->services.instance; });
    }
    module->stopRequested = true;
    try { module->descriptor->requestStop(module->descriptor->user); }
    catch (...) { module->stopFailed = true; diagnostic = "requestStop failed; DLL retained"; }
}
bool Host::Impl::Collect(const std::shared_ptr<Module>& module) {
    if (!module->stopRequested || module->stopFailed || module->activeCalls) return false;
    int32_t stopped = 0;
    try { stopped = module->descriptor->pollStopped(module->descriptor->user); }
    catch (...) { module->stopFailed = true; diagnostic = "pollStopped failed; DLL retained"; return false; }
    if (stopped == 0) return false;
    if (stopped != 1) { module->stopFailed = true; diagnostic = "invalid pollStopped result; DLL retained"; return false; }
    try { module->descriptor->destroy(module->descriptor->user); }
    catch (...) { module->stopFailed = true; diagnostic = "destroy failed; DLL retained"; return false; }
    {
        std::lock_guard lock(mutex);
        module->life = Life::Unloaded;
        modules.erase(module->loaded.contract.name);
    }
    // Never let a shared_ptr's final release on a worker decide the FreeLibrary thread.
    module->loaded.owner.reset();
    module->descriptor = nullptr;
    module->commands.clear();
    return true;
}
int32_t Host::Impl::Unload(std::string_view name) {
    if (!OnThread()) return AJ_THREAD;
    if (changing || dispatching || callDepth) return AJ_BUSY;
    auto module = Find(name);
    if (!module) return AJ_UNKNOWN;
    FlagScope guard(changing);
    Stop(module);
    return Collect(module) ? AJ_OK : AJ_BUSY;
}
int32_t Host::Impl::Close() {
    if (!OnThread()) return AJ_THREAD;
    if (changing || dispatching || callDepth) return AJ_BUSY;
    closing = true;
    dispatcher.clear();
    std::vector<std::shared_ptr<Module>> pending;
    {
        std::lock_guard lock(mutex);
        for (auto& [name, module] : modules) pending.push_back(module);
    }
    FlagScope guard(changing);
    bool busy = false;
    for (const auto& module : pending) { Stop(module); if (!Collect(module)) busy = true; }
    return busy ? AJ_BUSY : AJ_OK;
}
ResetResult Host::Impl::Reset(uint32_t next) {
    if (!OnThread()) return {AJ_THREAD, false, 0};
    if (changing || dispatching || callDepth) return {AJ_BUSY, false, 0};
    if (closing || !next || next > INT32_MAX || next <= epoch) return {AJ_STALE, false, 0};
    dispatcher.clear();
    lastRequest = 0;
    std::vector<std::shared_ptr<Module>> pending;
    {
        std::lock_guard lock(mutex);
        epoch = next;
        for (auto& [name, module] : modules) pending.push_back(module);
    }
    FlagScope guard(changing);
    for (const auto& module : pending) { Stop(module); Collect(module); }
    std::lock_guard lock(mutex);
    return {AJ_OK, true, static_cast<uint32_t>(modules.size())};
}
Host::Host(HostOptions options, std::shared_ptr<Loader> loader)
    : impl_(std::make_shared<Impl>(std::move(options), std::move(loader))) {}
Host::~Host() {
    if (impl_->Close() != AJ_OK) {
        auto& quarantine = Quarantined();
        std::scoped_lock lock(quarantine.mutex, impl_->mutex);
        for (auto& [name, module] : impl_->modules) {
            module->life = Life::Stopping;
            quarantine.entries.push_back(module);
        }
        impl_->modules.clear();
    }
}
Response Host::Dispatch(std::string_view wire, bool stringCarrier) {
    auto lifetime = impl_;
    return lifetime->Dispatch(wire, stringCarrier);
}
int32_t Host::Install(LoadedModule module) { return impl_->Install(std::move(module)); }
int32_t Host::Load(std::string_view name) { return impl_->Load(name); }
int32_t Host::Unload(std::string_view name) { return impl_->Unload(name); }
int32_t Host::Pump() { return impl_->Pump(); }
ResetResult Host::Reset(uint32_t epoch) { return impl_->Reset(epoch); }
int32_t Host::Close() { return impl_->Close(); }
uint32_t Host::ModuleCount() const { std::lock_guard lock(impl_->mutex); return static_cast<uint32_t>(impl_->modules.size()); }
std::string Host::Diagnostic() const { return impl_->OnThread() ? impl_->diagnostic : "wrong thread"; }
size_t Host::QuarantinedCount() {
    auto& quarantine = Quarantined();
    std::lock_guard lock(quarantine.mutex);
    return quarantine.entries.size();
}
}
