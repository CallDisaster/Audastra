#pragma once
#include "Protocol.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace audastra::japi {
struct CommandContract {
    std::string name;
    std::string arguments;
    int32_t resultKind = AJ_VOID;
    int32_t domain = AJ_LOCAL_COMMAND;
    bool operator==(const CommandContract&) const = default;
};
struct ModuleContract {
    std::string name;
    std::string version;
    std::vector<CommandContract> commands;
};
// The loader must authenticate bytes BEFORE executing any DLL code. The owner is
// released on the host thread only after the lifecycle has proved quiescence.
struct LoadedModule {
    AjInit init = nullptr;
    std::shared_ptr<void> owner;
    ModuleContract contract;
};
struct LoadResult {
    int32_t status = AJ_POLICY;
    std::string diagnostic;
    LoadedModule module;
};
class Loader {
public:
    virtual ~Loader() = default;
    virtual LoadResult Open(std::string_view name) = 0;
};
struct HostOptions {
    uint32_t initialEpoch = 1;
    uint32_t maxModules = 32;
    uint32_t queueCapacity = 256;
    uint32_t dispatchBudget = 64;
    // Supplied by a verified VM adapter. Must return AFTER the named JASS function
    // finishes. A suspended/yielded function must never be reported as completed.
    std::function<bool(std::string_view)> execute;
    bool synchronousCallbacksVerified = false;
    // Set only by a verified embedding adapter, never by a map request.
    std::string runtimeIdentity;
    // Optional test-only observer. Absent in ordinary hosts; cannot execute map code.
    std::function<bool(uint32_t, int32_t, int32_t, std::string_view)> runtimeTestReport;
};
struct ResetResult {
    int32_t status = AJ_OK;
    bool applied = false;
    uint32_t stoppingModules = 0;
};
class Host {
public:
    explicit Host(HostOptions options = {}, std::shared_ptr<Loader> loader = {});
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    Response Dispatch(std::string_view wire, bool stringCarrier = true);
    // Explicit trusted embedding API, NOT exposed as arbitrary function pointers to maps.
    int32_t Install(LoadedModule module);
    int32_t Load(std::string_view name);
    int32_t Unload(std::string_view name); // OK = actually unloaded, BUSY = stopping
    int32_t Pump(); // safe point; bounded dispatch; also progresses stopping modules
    ResetResult Reset(uint32_t newEpoch); // applied is distinct from pending asynchronous cleanup
    int32_t Close(); // OK only when all modules are destroyed
    uint32_t ModuleCount() const;
    std::string Diagnostic() const;
    // Number quarantined at teardown because a module did not prove stopped.
    // Quarantine intentionally retains DLL/context for process lifetime, never force-unloads.
    static size_t QuarantinedCount();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
