#include "../OwnLua/NativeCodeRegistry.h"

#include <windows.h>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace Audastra::Runtime32::OwnLua;
void Check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
class Backend final : public NativeCodeBackend {
public:
    std::uint64_t generation = 1;
    std::uint32_t next = 1, released = 0;
    bool refuse = false;
    std::map<std::uint32_t, std::uintptr_t> table;
    std::set<NativeCodeOwner> alive;
    std::uint64_t Generation() const noexcept override { return generation; }
    std::optional<std::uint32_t> Register(std::uintptr_t token) override {
        if (refuse) return {};
        const auto id = next++; table.emplace(id, token); return id;
    }
    void Release(std::uint32_t id) noexcept override { released += static_cast<std::uint32_t>(table.erase(id)); }
    std::uintptr_t Resolve(std::uint32_t vm, std::uint32_t id) const noexcept override {
        const auto found = table.find(id); return vm == 77 && found != table.end() ? found->second : 0;
    }
    bool OwnerAlive(const NativeCodeOwner& owner) const noexcept override { return alive.contains(owner); }
};
class Callback final : public NativeCallback {
public:
    std::function<NativeCallbackResult()> run;
    NativeCallbackReturn Returns() const noexcept override { return NativeCallbackReturn::Boolean; }
    NativeCallbackResult Invoke() override { return run(); }
};
std::shared_ptr<Callback> CallbackWith(std::function<NativeCallbackResult()> run) {
    auto result = std::make_shared<Callback>(); result->run = std::move(run); return result;
}
void OwnershipAndReentry() {
    auto backend = std::make_shared<Backend>(); NativeCodeRegistry codes(backend);
    const NativeCodeOwner timer{123, 45, 67}; backend->alive.insert(timer);
    auto captured = codes.Create(CallbackWith([] { return NativeCallbackResult{true, true, {}}; }));
    Check(captured.lease && codes.CodeCount() == 1, "actual backend ID retained");
    auto first = captured.lease; const auto id = first->Id(); captured.lease.reset();
    Check(!codes.Dispatch(78, id, false) && !codes.Dispatch(77, 99, false), "foreign VM / code goes to original JASS");
    Check(!codes.Dispatch(77, id, true)->succeeded, "unknown parameters never enter callback");
    Check(codes.Dispatch(77, id, false)->booleanValue, "registered boolean callback");
    Check(codes.RetainFor(timer, first), "timer holds callback"); first.reset();
    Check(codes.CodeCount() == 1 && codes.OwnerCount() == 1, "Lua caller can release its temporary reference");
    auto replacement = codes.Create(CallbackWith([&] {
        codes.Forget(timer);
        Check(codes.CodeCount() == 1, "executing callback lives through self-destruction");
        return NativeCallbackResult{true, false, {}};
    }));
    const auto replacementId = replacement.lease->Id();
    Check(codes.RetainFor(timer, replacement.lease), "timer replacement"); replacement.lease.reset();
    Check(!backend->table.contains(id) && codes.CodeCount() == 1, "replaced callback code released");
    Check(codes.Dispatch(77, replacementId, false)->succeeded && codes.CodeCount() == 0 && backend->table.empty(),
        "self-destroyed callback releases after return");
    auto outside = codes.Create(CallbackWith([] { return NativeCallbackResult{true, false, {}}; }));
    Check(codes.RetainFor(timer, outside.lease), "persisted owner"); outside.lease.reset();
    backend->alive.erase(timer); codes.Sweep();
    Check(codes.OwnerCount() == 0 && codes.CodeCount() == 0, "engine-side destruction collected using weak identity");
    const NativeCodeOwner reused{123, 45, 68}; backend->alive.insert(reused);
    auto newGeneration = codes.Create(CallbackWith([] { return NativeCallbackResult{true, false, {}}; }));
    Check(!codes.RetainFor(timer, newGeneration.lease) && codes.RetainFor(reused, newGeneration.lease), "object-address reuse does not revive old identity");
}
void FailureThreadAndBudget() {
    auto backend = std::make_shared<Backend>(); NativeCodeRegistry codes(backend);
    auto callback = CallbackWith([]() -> NativeCallbackResult { throw std::runtime_error("fixture callback failure"); });
    backend->refuse = true;
    Check(!codes.Create(callback).lease && codes.CodeCount() == 0, "failed registration leaves no closure");
    backend->refuse = false;
    auto created = codes.Create(callback);
    Check(codes.Dispatch(77, created.lease->Id(), false)->error == "fixture callback failure", "callback exception contained");
    const auto id = created.lease->Id();
    std::thread other([&] {
        Check(!codes.Create(callback).lease, "background cannot register code");
        Check(!codes.Dispatch(77, id, false)->succeeded, "background cannot execute Lua code");
        created.lease.reset();
    });
    other.join();
    Check(backend->table.contains(id), "background destructor defers VM operation");
    codes.Sweep(); Check(backend->table.empty(), "VM release drains on owning thread");
    std::vector<std::shared_ptr<NativeCodeLease>> limit;
    for (std::size_t i = 0; i < 4096; ++i) {
        auto item = codes.Create(callback); Check(item.lease != nullptr, "bounded code allocation"); limit.push_back(std::move(item.lease));
    }
    Check(!codes.Create(callback).lease, "live code limit enforced");
    limit.clear(); Check(codes.CodeCount() == 0 && backend->table.empty(), "all bounded codes reclaimed");
    auto stale = codes.Create(callback); const auto staleId = stale.lease->Id();
    ++backend->generation;
    Check(!stale.lease->Id() && !codes.Dispatch(77, staleId, false)->succeeded, "old generation cannot run or fall into original bytecode interpreter");
    const auto before = backend->released; stale.lease.reset();
    Check(backend->released == before, "old generation release does not touch new VM");
}
}
int main() {
    try { OwnershipAndReentry(); FailureThreadAndBudget(); std::cout << "original code identity/lifetime fixtures passed; game_evidence=0\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
