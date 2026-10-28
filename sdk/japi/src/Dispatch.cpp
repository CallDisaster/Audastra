#include "HostInternal.h"
#include <algorithm>
#include <cmath>

namespace audastra::japi {
namespace {
bool Args(const Request& request, std::string_view types) {
    if (request.arguments.size() != types.size()) return false;
    for (size_t i = 0; i < types.size(); ++i)
        if (TypeChar(request.arguments[i].kind) != types[i]) return false;
    return true;
}
const char* LifeName(Life life) {
    switch (life) {
    case Life::Starting: return "starting";
    case Life::Active: return "active";
    case Life::Stopping: return "stopping";
    default: return "unloaded";
    }
}
}
Response Host::Impl::Control(const Request& r) {
    const auto& name = r.command;
    if (name == "host.hello") {
        if (!Args(r, "") || r.epoch != 0) return Error(AJ_ARGUMENT);
        // 1: typed calls; 2: events; 4: VM callback adapter; 8: verified synchronous adapter; 16: loader.
        return Int(1 | 2 | (options.execute ? 4 : 0) | (options.execute && options.synchronousCallbacksVerified ? 8 : 0)
            | (loader ? 16 : 0));
    }
    if (name == "host.version") return Args(r, "") ? Text("2.0.0") : Error(AJ_ARGUMENT);
    if (name == "host.runtimeidentity") {
        if (!Args(r, "")) return Error(AJ_ARGUMENT);
        return options.runtimeIdentity.empty() ? Error(AJ_STATE) : Text(options.runtimeIdentity);
    }
    if (name == "host.testreport") {
        if (!Args(r, "iis")) return Error(AJ_ARGUMENT);
        if (!options.runtimeTestReport) return Error(AJ_POLICY);
        return Error(options.runtimeTestReport(epoch, r.arguments[0].integer, r.arguments[1].integer,
            r.arguments[2].text) ? AJ_OK : AJ_POLICY);
    }
    if (name == "host.bind") return Args(r, "s") ? Error(Bind(r.arguments[0].text)) : Error(AJ_ARGUMENT);
    if (name == "host.pump") return Args(r, "") ? Error(Pump()) : Error(AJ_ARGUMENT);
    if (name == "host.event") {
        if (!Args(r, "")) return Error(AJ_ARGUMENT);
        if (!dispatching) return Error(AJ_STATE);
        if (eventRead) return Text("");
        eventRead = true;
        return Text(std::to_string(current.id) + '|' + std::to_string(current.instance) + '|'
            + std::to_string(current.eventClass) + '|' + current.module + '|' + current.type + '|' + current.payload);
    }
    if (name == "host.ack") {
        if (!Args(r, "i")) return Error(AJ_ARGUMENT);
        if (!dispatching || !eventRead || acknowledged || r.arguments[0].integer < 0
            || static_cast<uint32_t>(r.arguments[0].integer) != current.id) return Error(AJ_STATE);
        acknowledged = true;
        return Error(AJ_OK);
    }
    if (name == "host.status") {
        if (!Args(r, "i")) return Error(AJ_ARGUMENT);
        return Text(StatusText(r.arguments[0].integer));
    }
    if (name == "module.load" || name == "module.unload") {
        if (!Args(r, "s")) return Error(AJ_ARGUMENT);
        return Error(name == "module.load" ? Load(r.arguments[0].text) : Unload(r.arguments[0].text));
    }
    if (name == "module.count") {
        if (!Args(r, "")) return Error(AJ_ARGUMENT);
        std::lock_guard lock(mutex);
        return Int(static_cast<int32_t>(modules.size()));
    }
    if (name == "module.info" || name == "module.stats" || name == "module.commandcount" || name == "module.commandat") {
        if (!Args(r, name == "module.commandat" ? "si" : "s")) return Error(AJ_ARGUMENT);
        const auto module = Find(r.arguments[0].text);
        if (!module) return Error(AJ_UNKNOWN);
        const auto& contract = module->loaded.contract;
        std::lock_guard lock(mutex);
        if (name == "module.info") return Text(contract.name + '|' + contract.version + '|'
            + std::to_string(module->services.instance) + '|' + LifeName(module->life) + '|'
            + std::to_string(contract.commands.size()));
        if (name == "module.commandcount") return Int(static_cast<int32_t>(contract.commands.size()));
        if (name == "module.commandat") {
            const auto index = r.arguments[1].integer;
            if (index < 0 || static_cast<size_t>(index) >= contract.commands.size()) return Error(AJ_ARGUMENT);
            const auto& command = contract.commands[index];
            return Text(command.name + '|' + command.arguments + '|' + TypeChar(command.resultKind) + '|'
                + std::to_string(command.domain));
        }
        const auto queued = std::count_if(queue.begin(), queue.end(), [&](const Event& e) {
            return e.instance == module->services.instance;
        });
        return Text(std::to_string(module->services.instance) + '|' + LifeName(module->life) + '|'
            + std::to_string(queued) + '|' + std::to_string(module->rejected) + '|'
            + std::to_string(module->delivered) + '|' + std::to_string(module->failed) + '|'
            + std::to_string(module->cancelled));
    }
    return Error(AJ_UNKNOWN);
}
Response Host::Impl::Invoke(const Request& r) {
    const auto module = Find(std::string_view(r.command).substr(0, r.command.find('.')));
    if (!module) return Error(AJ_UNKNOWN);
    { std::lock_guard lock(mutex); if (module->life != Life::Active) return Error(AJ_STATE); }
    if (module->activeCalls || callDepth >= 8) return Error(AJ_REENTRANT);
    const auto& commands = module->loaded.contract.commands;
    const auto found = std::find_if(commands.begin(), commands.end(), [&](const auto& c) { return c.name == r.command; });
    if (found == commands.end()) return Error(AJ_UNKNOWN);
    if (!Args(r, found->arguments)) return Error(AJ_ARGUMENT);
    // Local/async callbacks cannot accidentally escalate into gameplay commands.
    if (found->domain == AJ_SYNC_COMMAND && ((dispatching && current.eventClass != AJ_SYNC)
        || (callDepth && commandDomain != AJ_SYNC_COMMAND))) return Error(AJ_STATE);
    const auto& command = module->commands[static_cast<size_t>(found - commands.begin())];
    AjArg args[AJ_MAX_ARGUMENTS]{};
    for (size_t i = 0; i < r.arguments.size(); ++i) {
        const auto& arg = r.arguments[i];
        args[i] = {arg.kind, arg.integer, arg.real, arg.kind == AJ_TEXT ? arg.text.c_str() : nullptr,
            static_cast<uint32_t>(arg.text.size())};
    }
    AjCall call{sizeof(AjCall), epoch, module->services.instance, r.id};
    AjResult result{};
    const auto priorDomain = commandDomain;
    commandDomain = command.domain;
    ++callDepth; ++module->activeCalls;
    int32_t status = AJ_INTERNAL;
    try { status = command.invoke(command.user, &call, args, static_cast<uint32_t>(r.arguments.size()), &result); }
    catch (...) { diagnostic = "module invoke threw a C++ exception"; }
    --module->activeCalls; --callDepth;
    commandDomain = priorDomain;
    if (status < AJ_OK || status > AJ_INTERNAL) status = AJ_BACKEND;
    if (status != AJ_OK) return Error(status);
    if (result.kind != command.resultKind) return Error(AJ_BACKEND);
    if (result.kind == AJ_REAL && !std::isfinite(result.real)) return Error(AJ_BACKEND);
    if (result.kind == AJ_BOOL && result.integer != 0 && result.integer != 1) return Error(AJ_BACKEND);
    Response reply;
    reply.value.kind = result.kind;
    reply.value.integer = result.integer;
    reply.value.real = result.real;
    if (result.kind == AJ_TEXT) {
        if (result.textSize > AJ_MAX_OUTPUT_TEXT) return Error(AJ_LIMIT);
        reply.value.text.assign(result.text, result.textSize);
        if (!ValidText(reply.value.text)) return Error(AJ_ENCODING);
        if (EncodeText(reply.value.text).size() > AJ_MAX_OUTPUT_TEXT) return Error(AJ_LIMIT);
    }
    return reply;
}
Response Host::Impl::Dispatch(std::string_view wire, bool stringCarrier) {
    if (!wire.starts_with("ajapi:")) { Response r; r.consumed = false; return r; }
    Response reply;
    Request request;
    reply.epoch = Epoch();
    if (!OnThread()) { reply.status = AJ_THREAD; return reply; }
    reply.status = ParseRequest(wire, request);
    reply.id = request.id;
    if (reply.status != AJ_OK) return reply;
    if (!stringCarrier) { reply.status = AJ_CARRIER; return reply; }
    if (closing || changing) { reply.status = AJ_STATE; return reply; }
    if ((request.command != "host.hello" && request.epoch != epoch)
        || request.id <= lastRequest) { reply.status = AJ_STALE; return reply; }
    // Reserve before executing: reentrant requests get higher IDs, never overwritten on return.
    lastRequest = request.id;
    try {
        reply = request.command.starts_with("host.") || request.command.starts_with("module.")
            ? Control(request) : Invoke(request);
    } catch (...) { reply = Error(AJ_INTERNAL); diagnostic = "host request failed"; }
    reply.id = request.id;
    reply.epoch = epoch;
    return reply;
}
}
