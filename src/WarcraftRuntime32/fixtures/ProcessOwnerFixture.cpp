#include "../Japi/ProcessOwner.h"
#include <atomic>
namespace { std::atomic<int> state{0}, installs{0}; std::mutex initialization; }
extern "C" __declspec(dllexport) int __cdecl WarVKRuntimeOwnerV1() { return state.load(); }
extern "C" __declspec(dllexport) int __cdecl WarVKAIInit() {
    std::string error;
    return Audastra::Runtime32::Japi::InitializeProcessRuntime(reinterpret_cast<const void*>(&WarVKAIInit),initialization,
        [](std::string_view) {
            if (state.load() == 0) { ++installs; state.store(1); }
            return 123;
        },error);
}
extern "C" __declspec(dllexport) int __cdecl OwnerFixtureInstallCount() { return installs.load(); }
extern "C" __declspec(dllexport) void __cdecl SetOwnerFixtureState(int value) { state.store(value); }
extern "C" __declspec(dllexport) int __cdecl ProbeOwnerFixture() {
    auto lease = Audastra::Runtime32::Japi::ProcessOwnerLease::Acquire(reinterpret_cast<const void*>(&ProbeOwnerFixture));
    if (lease->status) return -lease->status;
    return lease->existing ? lease->existing() : 0;
}
